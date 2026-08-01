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
/* The bus addresses and the bit-banging itself are in smc.s. */
extern uint8_t smc_getkey_raw(void);
extern void    x816_exec_init(void);

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

    /* ---- the I2C bus ------------------------------------------------------
     *
     * ORA = 0, so DDRA alone decides drive-low vs release, and DDRA = 0 to
     * leave both lines released for the pull-ups. smc.s sets ORA again on
     * every transaction, so this is belt and braces rather than load-bearing.
     *
     * It is worth saying what this is NOT, because the wrong answer was
     * committed once already: a dead keyboard here was blamed on ORA being
     * undefined at power-up. It is not -- rtl/via65c22.sv resets ora_r to 0
     * explicitly, so that reasoning applies to a physical 65C22 and not to
     * this core. The real fault was a miscompiled shift; see smc.s. */
    VIA1_PA   = 0;
    VIA1_DDRA = 0;

    /* Prime the program relocator into bank $00 while bank $01 is still
       intact. It has to be somewhere the copy does not erase -- see exec.s. */
    x816_exec_init();

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

/* ---- keyboard ---------------------------------------------------------- *
 *
 * The bit-banging lives in smc.s, NOT here, and that is not a style choice.
 * This was C, and Calypsi 5.18 compiled `b = (uint8_t)(b << 1)` on an 8-bit
 * local into a 16-BIT read-modify-write, which also shifted the loop counter
 * in the adjacent stack slot. Bytes went out three or four bits wide, the SMC
 * NACKed every transaction, and the keyboard was dead while the screen above
 * kept working. See smc.s for the full diagnosis. */

/* IBM System/2 keycode -> ASCII. Defined alongside the font, and not const,
   for the same reason -- see font8x8.c. */
extern uint8_t keymap[64];
extern uint8_t keymap_shift[64];

/* Positions in the IBM System/2 numbering the SMC emits; see the ps2_to_ibm
   table in X816_Core rtl/smc_x16.sv. */
#define KEY_LSHIFT 44
#define KEY_RSHIFT 57

/* Shift is the first thing here with STATE, and that is why releases can no
   longer be discarded on sight: a modifier is defined entirely by the gap
   between its press and its release. Everything else still ignores them. */
static bool shift_l, shift_r;

uint8_t
con_smc_raw(void)
{
    return smc_getkey_raw();
}

char
con_getkey(void)
{
    uint8_t code = smc_getkey_raw();
    uint8_t key, release;

    if (code == 0)                      /* FIFO empty */
        return 0;

    key     = (uint8_t)(code & 0x7F);
    release = (uint8_t)(code & 0x80);

    /* Modifiers are tracked on BOTH edges and never produce a character.
       Handled before the release test below, which would otherwise throw away
       the key-up that clears the state and leave shift stuck on for ever. */
    if (key == KEY_LSHIFT) {
        shift_l = (release == 0);
        return 0;
    }
    if (key == KEY_RSHIFT) {
        shift_r = (release == 0);
        return 0;
    }

    if (release)                        /* every other key-up is noise */
        return 0;
    if (key >= 64)
        return 0;

    return (char)((shift_l || shift_r) ? keymap_shift[key] : keymap[key]);
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
