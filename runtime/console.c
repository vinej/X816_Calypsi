/* The X816 console. See console.h. BUILD AT -O0. */

#include "console.h"

/* ---- VERA ------------------------------------------------------------- */
#define VERA_ADDR_L     (*(volatile uint8_t *)0x9F20)
#define VERA_ADDR_M     (*(volatile uint8_t *)0x9F21)
#define VERA_ADDR_H     (*(volatile uint8_t *)0x9F22)
#define VERA_DATA0      (*(volatile uint8_t *)0x9F23)
#define VERA_DATA1      (*(volatile uint8_t *)0x9F24)
#define VERA_CTRL       (*(volatile uint8_t *)0x9F25)
#define VERA_DC_VIDEO   (*(volatile uint8_t *)0x9F29)
#define VERA_DC_HSCALE  (*(volatile uint8_t *)0x9F2A)
#define VERA_DC_VSCALE  (*(volatile uint8_t *)0x9F2B)
#define VERA_L0_CONFIG  (*(volatile uint8_t *)0x9F2D)
#define VERA_L0_MAPBASE (*(volatile uint8_t *)0x9F2E)
#define VERA_L0_TILEB   (*(volatile uint8_t *)0x9F2F)

/* ---- VIA 1, the SMC's I2C bus ----------------------------------------- */
#define VIA1_PA         (*(volatile uint8_t *)0x9F01)
#define VIA1_DDRA       (*(volatile uint8_t *)0x9F03)
#define I2C_SDA         0x01u
#define I2C_SCL         0x02u
#define SMC_ADDR        0x42u
#define SMC_GETKEY      0x07u

/* The map is 128 cells wide, so a cell address is (y*128 + x)*2 = y*256 + x*2
   -- ADDR_M is the row and ADDR_L the doubled column, with no multiply. That
   is why the map is 128 wide and not 80. */
#define MAP_W       128
#define TILE_VRAM   0x04000UL
#define FONT_FIRST  0x20
#define FONT_LAST   0x5F
#define ATTR        0x01u        /* white on black, VERA default palette */

/* In its own translation unit, and NOT const -- see font8x8.c for why. */
extern uint8_t font8x8[512];

static uint8_t curx, cury;

/* ---- VERA text mode ---------------------------------------------------- */

static void
set_addr(uint32_t a)
{
    VERA_CTRL   = 0;
    VERA_ADDR_L = (uint8_t)(a);
    VERA_ADDR_M = (uint8_t)(a >> 8);
    VERA_ADDR_H = (uint8_t)(0x10 | ((a >> 16) & 0x0F));   /* increment 1 */
}

void
con_init(void)
{
    uint16_t i;

    VERA_CTRL      = 0;
    VERA_DC_VIDEO  = 0x11;      /* VGA output + layer 0 enable */
    VERA_DC_HSCALE = 0x80;      /* 1:1 -> 640x480 active */
    VERA_DC_VSCALE = 0x80;
    /* map height 64 (1<<6) | map width 128 (2<<4); bitmap off, 1bpp */
    VERA_L0_CONFIG  = 0x60;
    VERA_L0_MAPBASE = 0;                            /* tilemap at $00000 */
    VERA_L0_TILEB   = (uint8_t)((TILE_VRAM >> 11) << 2);

    /* Tile N lives at TILE_VRAM + N*8, so glyph $20 goes to TILE_VRAM+$100. */
    set_addr(TILE_VRAM + FONT_FIRST * 8);
    for (i = 0; i < 512; i++)
        VERA_DATA0 = font8x8[i];

    /* ---- the I2C bus, and this is NOT optional ----------------------------
     *
     * ORA must be zeroed ONCE, here. The whole open-drain scheme below rests
     * on it: a line is driven low by switching the pin to an OUTPUT, which
     * then drives whatever ORA holds. With ORA undefined at power-up, "drive
     * low" can drive the line HIGH, and the bus never works at all.
     *
     * This was missed when the routines were ported from boot/kbd.s, because
     * the initialisation lives in kbd.s's caller rather than in its I2C
     * helpers -- porting the functions did not bring it along. The symptom was
     * exact: output fine, and con_getc() blocking forever on hardware. */
    VIA1_PA   = 0;      /* from here DDRA alone decides drive-low vs release */
    VIA1_DDRA = 0;      /* both lines released, pull-ups take them high */

    con_cls();
}

void
con_cls(void)
{
    uint16_t i;

    set_addr(0);
    /* The whole 128x64 map, not just the visible 80x60, so nothing stale
       shows at the edges if the display size ever changes. */
    for (i = 0; i < MAP_W * 64; i++) {
        VERA_DATA0 = ' ';
        VERA_DATA0 = ATTR;
    }
    curx = 0;
    cury = 0;
}

/* Scroll one line using BOTH data ports: port 0 reads row n+1 while port 1
   writes row n. VERA has no block move, and a single port would mean
   re-addressing for every byte. */
static void
scroll(void)
{
    uint16_t i;
    uint8_t  row;

    for (row = 0; row < CON_ROWS - 1; row++) {
        VERA_CTRL   = 0;                       /* ADDRSEL 0 -- source */
        VERA_ADDR_L = 0;
        VERA_ADDR_M = (uint8_t)(row + 1);
        VERA_ADDR_H = 0x10;
        VERA_CTRL   = 1;                       /* ADDRSEL 1 -- destination */
        VERA_ADDR_L = 0;
        VERA_ADDR_M = row;
        VERA_ADDR_H = 0x10;
        VERA_CTRL   = 0;
        for (i = 0; i < CON_COLS * 2; i++)
            VERA_DATA1 = VERA_DATA0;
    }

    /* Blank the last line. */
    VERA_CTRL   = 0;
    VERA_ADDR_L = 0;
    VERA_ADDR_M = CON_ROWS - 1;
    VERA_ADDR_H = 0x10;
    for (i = 0; i < CON_COLS; i++) {
        VERA_DATA0 = ' ';
        VERA_DATA0 = ATTR;
    }
}

static void
newline(void)
{
    curx = 0;
    if (cury + 1 >= CON_ROWS)
        scroll();
    else
        cury++;
}

void
con_putc(char c)
{
    uint8_t ch = (uint8_t)c;

    if (ch == 0x0A) { newline(); return; }      /* \n */
    if (ch == 0x0D) { curx = 0;  return; }      /* \r */
    if (ch == 0x08) {                           /* \b, stops at column 0 */
        if (curx > 0)
            curx--;
        return;
    }
    /* Fold lower case up rather than dropping it.
     *
     * The font holds $20-$5F, so 'a'-'z' is outside it. Filtering them to
     * spaces would make any lowercase output silently invisible -- a shell
     * echoing a mistyped command would print a row of blanks and look broken.
     * Folding is what every uppercase-only machine has always done, and it
     * costs one comparison. */
    if (ch >= 'a' && ch <= 'z')
        ch = (uint8_t)(ch - 32);
    if (ch < FONT_FIRST || ch > FONT_LAST)
        ch = ' ';

    VERA_CTRL   = 0;
    VERA_ADDR_L = (uint8_t)(curx << 1);
    VERA_ADDR_M = cury;
    VERA_ADDR_H = 0x10;
    VERA_DATA0  = ch;
    VERA_DATA0  = ATTR;

    if (++curx >= CON_COLS)
        newline();
}

void
con_puts(const char *s)
{
    while (*s)
        con_putc(*s++);
}

void
con_gotoxy(uint8_t x, uint8_t y)
{
    if (x < CON_COLS) curx = x;
    if (y < CON_ROWS) cury = y;
}

uint8_t con_getx(void) { return curx; }
uint8_t con_gety(void) { return cury; }

/* ---- keyboard: bit-banged I2C to the SMC ------------------------------- *
 *
 * PA0 = SDA, PA1 = SCL, both open drain. ORA stays 0, so a line is driven LOW
 * by making it an output and RELEASED by making it an input, where the pull-up
 * takes it high. Never drive a line high -- that is what open drain means, and
 * the SMC drives SDA itself during ACK and reads.
 */

static void sda_low(void) { VIA1_DDRA |= I2C_SDA; }
static void sda_rel(void) { VIA1_DDRA &= (uint8_t)~I2C_SDA; }
static void scl_low(void) { VIA1_DDRA |= I2C_SCL; }
static void scl_rel(void) { VIA1_DDRA &= (uint8_t)~I2C_SCL; }

static void i2c_start(void) { sda_rel(); scl_rel(); sda_low(); scl_low(); }
static void i2c_stop(void)  { sda_low(); scl_rel(); sda_rel(); }

static void
i2c_write(uint8_t b)
{
    uint8_t i;
    for (i = 0; i < 8; i++) {
        if (b & 0x80) sda_rel(); else sda_low();
        b = (uint8_t)(b << 1);
        scl_rel();
        scl_low();
    }
    sda_rel();                      /* ACK slot: let the slave pull SDA low */
    scl_rel();
    scl_low();
}

static uint8_t
i2c_read_nak(void)
{
    uint8_t i, v = 0;
    sda_rel();                      /* let the slave drive SDA */
    for (i = 0; i < 8; i++) {
        v = (uint8_t)(v << 1);
        scl_rel();
        if (VIA1_PA & I2C_SDA)
            v |= 1;
        scl_low();
    }
    sda_rel();                      /* NACK */
    scl_rel();
    scl_low();
    return v;
}

/* IBM System/2 keycode -> ASCII. Defined alongside the font, and not const,
   for the same reason -- see font8x8.c. */
extern uint8_t keymap[64];

char
con_getkey(void)
{
    uint8_t code;

    /* NOTE the full STOP between the command and the read, NOT a repeated
       START. X816_Core rtl/smc_x16.sv documents why: the real SMC firmware
       early-returns for one-byte writes, leaving the command armed for a
       separate read transaction. A repeated START never arms it, and every
       read comes back $FE. */
    i2c_start();
    i2c_write((uint8_t)(SMC_ADDR << 1));
    i2c_write(SMC_GETKEY);
    i2c_stop();

    i2c_start();
    i2c_write((uint8_t)((SMC_ADDR << 1) | 1));
    code = i2c_read_nak();
    i2c_stop();

    if (code == 0 || (code & 0x80))     /* FIFO empty, or a key release */
        return 0;
    if (code >= 64)
        return 0;
    return (char)keymap[code];
}

char
con_getc(void)
{
    char c;
    do {
        c = con_getkey();
    } while (c == 0);
    return c;
}
