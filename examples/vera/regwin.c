/* regwin -- VERA816 section 8 test 8: CTRL816.REGWIN window relocation.
 *
 * The contract is X816_core/doc/VERA816.md section 4.4: setting REGWIN moves
 * the PSG/palette/sprite-attribute windows from $1F9C0-$1FFFF to
 * $7F9C0-$7FFFF, which frees the whole 352 KB as plain VRAM -- the reason the
 * bit exists is section 2.2: no 640x480 framebuffer fits without crossing the
 * stock window position, so without REGWIN the blitter is a *dependency* of
 * high-resolution graphics rather than a speed-up.
 *
 * JUDGEMENT. CPU-checkable claims fail() with their own colour; the two
 * claims only a screen can settle are arranged so each failure mode ends in
 * a different final picture:
 *
 *   palette entry 1 is written BLUE through the NEW window first, then RED
 *   through the OLD address second. The screen is painted in entry 1, so:
 *
 *     BLUE   both writes went where section 4.4 says     -- PASS
 *     RED    the OLD address still reaches the palette   -- the relocation
 *            leaked (this is also what --negative expects, since with REGWIN
 *            clear the old address SHOULD repaint the screen)
 *     WHITE  the NEW window reaches nothing              -- relocation dead
 *
 *   On a pass the screen goes WHITE -> BLUE -> BLUE plus one yellow sprite
 *   near the top left, and shows nothing else on the way. Every other colour
 *   it can display is a failure from the list below, held forever.
 *
 *   sprite 1 is programmed entirely through the NEW window and must render;
 *   sprite 2's slot is poisoned through the OLD address and must NOT (those
 *   bytes are plain VRAM now). The runner probes one pixel of each.
 *
 * Deliberately not reusing console.c, and everything below goes straight at
 * the registers -- blittest.c explains why.
 *
 * BUILD AT -O0. Calypsi 5.18 eliminates volatile reads above -O0; every
 * local that gets arithmetic is 16 or 32 bits wide (project README).
 */

#include <stdint.h>

/* ---- VERA, stock registers -------------------------------------------- */
#define VERA_ADDR_L     (*(volatile uint8_t *)0x9F20)
#define VERA_ADDR_M     (*(volatile uint8_t *)0x9F21)
#define VERA_ADDR_H     (*(volatile uint8_t *)0x9F22)
#define VERA_DATA0      (*(volatile uint8_t *)0x9F23)
#define VERA_CTRL       (*(volatile uint8_t *)0x9F25)
#define VERA_DC_VIDEO   (*(volatile uint8_t *)0x9F29)   /* DCSEL 0 */
#define VERA_DC_HSCALE  (*(volatile uint8_t *)0x9F2A)
#define VERA_DC_VSCALE  (*(volatile uint8_t *)0x9F2B)
#define VERA_L0_CONFIG  (*(volatile uint8_t *)0x9F2D)
#define VERA_L0_TILEB   (*(volatile uint8_t *)0x9F2F)

/* ---- VERA816 banks (VERA816.md 4.1, 4.4) ------------------------------ */
#define VERA_ADDRX      (*(volatile uint8_t *)0x9F29)   /* DCSEL 32 */
#define VERA_VRAMCAP    (*(volatile uint8_t *)0x9F2C)
#define VERA_CTRL816    (*(volatile uint8_t *)0x9F29)   /* DCSEL 34 */

#define DCSEL(n)        ((uint8_t)((n) << 1))
#define VRAMCAP_VALUE   22
#define REGWIN_BIT      0x01

/* Set to 0 by run-regwin.sh --negative: REGWIN stays clear, so the OLD
 * palette address must repaint the screen red and the NEW window must do
 * nothing -- the exact opposite verdict, proving these checks can fail. */
#define SET_REGWIN      1

/* The windows, stock and relocated (VERA816.md 2.2 / 4.4). */
#define OLD_PAL1        0x1FA02UL   /* palette entry 1, stock position     */
#define NEW_PAL1        0x7FA02UL   /* palette entry 1, relocated          */
#define OLD_SPR2        0x1FC10UL   /* sprite 2 attributes, stock position */
#define NEW_SPR1        0x7FC08UL   /* sprite 1 attributes, relocated      */

/* The write-only probe scribbles on palette entry 15, which nothing here
 * displays. It used to use entry 1 -- the entry the whole verdict is painted
 * in -- so the screen passed through a meaningless pink ($0F5A) on its way to
 * the verdict, and an observer could not tell that transient apart from the
 * light-red COL_NOTWO failure. Now the picture goes white -> blue -> blue
 * plus sprite, and any reddish screen means exactly one thing. */
#define NEW_PAL15       0x7FA1EUL   /* palette entry 15, relocated         */

#define SPR_DATA        0x14000UL   /* 64 bytes of sprite pixels           */
#define SPR_COL         7           /* yellow -- untouched palette entry   */

/* Pre-verdict failure colours: palette entries this test never rewrites,
 * and none of them white/red/blue/yellow. */
#define COL_NOCAP       11          /* dark grey    VRAMCAP is not 22       */
#define COL_NOCTRL      13          /* light green  CTRL816 absent (reads
                                                    the version byte)       */
#define COL_STUCK       9           /* brown        REGWIN write won't read
                                                    back                    */
#define COL_NOTRAM      12          /* grey         freed $1FA02 is not
                                                    plain VRAM              */
#define COL_NOTWO       10          /* light red    relocated window is not
                                                    write-only              */

/* ---- 19-bit data-port access (ADDRX first -- VERA816.md 4.1) ----------- */
static void
vset19(uint32_t a)
{
    VERA_CTRL   = DCSEL(32);
    VERA_ADDRX  = (uint8_t)((a >> 17) & 3);
    VERA_CTRL   = 0;
    VERA_ADDR_L = (uint8_t)a;
    VERA_ADDR_M = (uint8_t)(a >> 8);
    VERA_ADDR_H = (uint8_t)(0x10 | ((a >> 16) & 1));
}

static uint8_t
vpeek(uint32_t a)
{
    vset19(a);
    return VERA_DATA0;
}

static void
vpoke(uint32_t a, uint8_t v)
{
    vset19(a);
    VERA_DATA0 = v;
}

/* ---- verdict ----------------------------------------------------------- */
static void
paint(uint8_t colour)
{
    uint32_t i;
    vset19(0);
    for (i = 0; i < 76800UL; i++) {
        VERA_DATA0 = colour;
    }
}

static void
fail(uint8_t colour)
{
    paint(colour);
    for (;;) {
    }
}

/* Write one sprite attribute record (8 bytes) starting at `a`. */
static void
spr_attr(uint32_t a, uint16_t x, uint16_t y)
{
    vset19(a);
    VERA_DATA0 = 0x00;                      /* addr[12:5] = 0              */
    VERA_DATA0 = 0x8A;                      /* 8bpp, addr[16:13] = $A      */
    VERA_DATA0 = (uint8_t)x;
    VERA_DATA0 = (uint8_t)(x >> 8);
    VERA_DATA0 = (uint8_t)y;
    VERA_DATA0 = (uint8_t)(y >> 8);
    VERA_DATA0 = 0x0C;                      /* z-depth 3, no flips         */
    VERA_DATA0 = 0x00;                      /* 8x8, palette offset 0       */
}

int
main(void)
{
    uint32_t i;
    uint16_t v, was;    /* 16-bit on purpose: Calypsi 5.18 widens RMW on
                         * 8-bit locals and corrupts the neighbour (README) */

    /* 320x240 8bpp bitmap at $00000, greentest.c's setup; screen = entry 1. */
    VERA_CTRL      = 0;
    VERA_DC_VIDEO  = 0x11;
    VERA_DC_HSCALE = 0x40;
    VERA_DC_VSCALE = 0x40;
    VERA_L0_CONFIG = 0x07;
    VERA_L0_TILEB  = 0;

    /* --- preflight: VERA816 at all ------------------------------------- */
    VERA_CTRL = DCSEL(32);
    v = VERA_VRAMCAP;
    VERA_CTRL = 0;
    if (v != VRAMCAP_VALUE) {
        fail(COL_NOCAP);
    }

#if SET_REGWIN
    /* --- preflight: CTRL816 exists and takes the bit ---------------------
     * At reset it must read $00; an older VERA816 falls through to the
     * version string and reads $56 ('V') -- VERA816.md 4.4's detection
     * recipe, asserted rather than assumed. */
    VERA_CTRL = DCSEL(34);
    v = VERA_CTRL816;
    if (v != 0x00) {
        VERA_CTRL = 0;
        fail(COL_NOCTRL);
    }
    VERA_CTRL816 = REGWIN_BIT;
    v = VERA_CTRL816;
    VERA_CTRL = 0;
    if (v != REGWIN_BIT) {
        fail(COL_STUCK);
    }

    /* --- the freed range is plain VRAM, CPU-checkable --------------------
     * In stock mode this readback also passes (the shadow), so it is not the
     * decisive test -- the screen below is -- but it does catch the freed
     * range turning into a hole. */
    was = vpeek(OLD_PAL1);
    vpoke(OLD_PAL1, (uint8_t)(was ^ 0xFF));
    if (vpeek(OLD_PAL1) != (uint8_t)(was ^ 0xFF)) {
        fail(COL_NOTRAM);
    }

    /* --- the relocated window is write-only, CPU-checkable ---------------
     * VERA816.md 4.4: reads at $7F9C0-$7FFFF follow the section 3 hole rule.
     * Entry 15, not entry 1: this probe leaves the byte it wrote behind, and
     * entry 1 is what the verdict is painted in. (The write's effect on the
     * palette is judged on the screen below.) */
    vpoke(NEW_PAL15, 0x5A);
    if (vpeek(NEW_PAL15) != 0x00) {
        fail(COL_NOTWO);
    }
#endif

    /* --- the screen-judged half ------------------------------------------
     * Paint in entry 1, then write the entry BLUE through the NEW window
     * FIRST and RED through the OLD address SECOND. Any decode that lets the
     * old address reach the palette -- relocation ineffective, or leaking
     * both ways -- ends with the red write winning. Palette entry format:
     * +0 = green<<4|blue, +1 = red. */
    paint(1);

    vpoke(NEW_PAL1,       0x0F);    /* blue $00F ... */
    vpoke(NEW_PAL1 + 1UL, 0x00);
    vpoke(OLD_PAL1,       0x00);    /* ... then red $F00 at the old address */
    vpoke(OLD_PAL1 + 1UL, 0x0F);

    /* --- sprites: one through each position ------------------------------
     * Sprite 1 exists only if the NEW window writes reach the attribute RAM;
     * sprite 2 exists only if the OLD address still does. The runner expects
     * sprite 1's pixel and sprite 2's absence -- and --negative, the
     * opposite. */
    vset19(SPR_DATA);
    for (i = 0; i < 64UL; i++) {
        VERA_DATA0 = SPR_COL;
    }
    spr_attr(NEW_SPR1, 32, 32);
    spr_attr(OLD_SPR2, 48, 32);

    VERA_CTRL     = 0;
    VERA_DC_VIDEO = 0x51;           /* VGA + layer 0 + sprites */

    for (;;) {
    }
}
