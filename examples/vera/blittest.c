/* blittest -- blit816 conformance, plus the firmware write-protect.
 *
 * The contract is X816_core/doc/BLIT816.md (the DCSEL=33 blitter bank at
 * $9F29-$9F2C); the firmware region is doc/KERNEL.md section 3. Every
 * assertion below is an assertion about those documents, so a failure here is
 * a conformance failure, not a style opinion.
 *
 * 2026-08-02: retargeted from the 19-bit VERA816 address space to stock
 * VERA's 17 bits. Three things changed and each mattered:
 *   - every test address moved below $20000, and they now sit ABOVE the
 *     $00000-$12BFF framebuffer so a blit cannot silently repaint the screen
 *     it is judged by;
 *   - test 6 tested wrap THROUGH the unpopulated hole. 128 KB is fully
 *     populated, so it now checks that all four bytes LAND -- two at the top
 *     of VRAM and two wrapped to $00000 -- which is a stronger assertion than
 *     the old one (that version passed if the writes simply vanished);
 *   - test 8, the sprite reach above 128 KB, was DELETED with the feature.
 *
 * Judgement is by screen colour, run-write.sh style: GREEN means every test
 * passed, any other full-screen colour names the test that failed (the map
 * lives in run-blit.sh).
 *
 * Deliberately not reusing console.c: sharing code with the device under
 * test is how two broken halves get to agree with each other. Everything
 * here goes straight at the registers, greentest.c style.
 *
 * BUILD AT -O0. Calypsi 5.18 eliminates volatile reads above -O0, and
 * widens in-place shifts/RMW on 8-bit locals (see the project README);
 * every local that gets arithmetic below is 16 or 32 bits wide for that
 * reason, and MMIO is only ever touched through volatile pointers.
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

/* ---- blitter bank, DCSEL=33 (BLIT816.md) ------------------------------ */
#define VERA_BLT_IDX    (*(volatile uint8_t *)0x9F29)   /* DCSEL 33 */
#define VERA_BLT_DATA   (*(volatile uint8_t *)0x9F2A)
#define VERA_BLT_CTRL   (*(volatile uint8_t *)0x9F2B)
#define VERA_BLT_ID     (*(volatile uint8_t *)0x9F2C)

#define DCSEL(n)        ((uint8_t)((n) << 1))           /* CTRL = dcsel<<1|addrsel */

#define BLT_ID_VALUE    0xB6
#define BLT_START_COPY  0x01
#define BLT_START_FILL  0x02

/* The fill byte for test 2, and what the verification expects to read back.
 * run-blit.sh --negative rewrites FILL_EXPECT (and only it) to a wrong
 * value, proving this test can fail rather than always painting green. */
#define FILL_VAL        0x5A
#define FILL_EXPECT     FILL_VAL

/* Failure colours, VERA default palette. The runner decodes them by name. */
#define COL_GREEN       0x05    /* (  0,204, 85)  all tests passed        */
#define COL_RED         0x02    /* (136,  0,  0)  1: BLT_ID / busy        */
#define COL_YELLOW      0x07    /* (238,238,119)  2: FILL                 */
#define COL_BLUE        0x06    /* (  0,  0,170)  3: misaligned COPY      */
#define COL_PURPLE      0x04    /* (204, 68,204)  4: doubling idiom       */
#define COL_CYAN        0x03    /* (170,255,238)  5: LEN=0 no-op          */
#define COL_WHITE       0x01    /* (255,255,255)  6: wrap at top of VRAM  */
#define COL_ORANGE      0x08    /* (221,136, 85)  7: firmware protect     */

/* ---- 17-bit data-port access ------------------------------------------ */

/* Set ADDR0 to a 17-bit byte address with auto-increment 1. This is plain
 * stock VERA: ADDR_H bit 0 is address bit 16 and bits [7:4] are the
 * increment. (The VERA816 build needed an ADDRX window here for bits
 * [18:17]; stock has no such bits and needs no such register.) */
static void
vset(uint32_t a)
{
    VERA_CTRL   = 0;
    VERA_ADDR_L = (uint8_t)a;
    VERA_ADDR_M = (uint8_t)(a >> 8);
    VERA_ADDR_H = (uint8_t)(0x10 | ((a >> 16) & 1));
}

static uint8_t
vpeek(uint32_t a)
{
    vset(a);
    return VERA_DATA0;
}

static void
vpoke(uint32_t a, uint8_t v)
{
    vset(a);
    VERA_DATA0 = v;
}

/* ---- blitter access ---------------------------------------------------- */

/* Load SRC/DST/LEN into the parameter file through BLT_IDX/BLT_DATA. Each
 * BLT_DATA write auto-increments BLT_IDX, so this is nine data writes after
 * one index write. */
static void
blt_params(uint32_t src, uint32_t dst, uint32_t len)
{
    VERA_CTRL     = DCSEL(33);
    VERA_BLT_IDX  = 0;
    VERA_BLT_DATA = (uint8_t)src;                   /* 0: SRC[7:0]   */
    VERA_BLT_DATA = (uint8_t)(src >> 8);            /* 1: SRC[15:8]  */
    VERA_BLT_DATA = (uint8_t)((src >> 16) & 1);     /* 2: SRC[16]    */
    VERA_BLT_DATA = (uint8_t)dst;                   /* 3: DST[7:0]   */
    VERA_BLT_DATA = (uint8_t)(dst >> 8);            /* 4: DST[15:8]  */
    VERA_BLT_DATA = (uint8_t)((dst >> 16) & 1);     /* 5: DST[16]    */
    VERA_BLT_DATA = (uint8_t)len;                   /* 6: LEN[7:0]   */
    VERA_BLT_DATA = (uint8_t)(len >> 8);            /* 7: LEN[15:8]  */
    VERA_BLT_DATA = (uint8_t)((len >> 16) & 1);     /* 8: LEN[16]    */
}

/* Start an operation and poll busy down. The emulator completes a blit
 * instantaneously so the loop exits on its first read; on hardware it spins
 * for real -- either way a terminating loop here is itself the busy-polling
 * conformance check. */
static void
blt_go(uint8_t start_bits)
{
    VERA_CTRL     = DCSEL(33);
    VERA_BLT_CTRL = start_bits;
    while ((VERA_BLT_CTRL & 0x01) != 0) {
    }
}

static void
blt_fill(uint32_t dst, uint32_t len, uint8_t val)
{
    blt_params(0, dst, len);
    VERA_BLT_IDX  = 9;
    VERA_BLT_DATA = val;
    blt_go(BLT_START_FILL);
}

static void
blt_copy(uint32_t src, uint32_t dst, uint32_t len)
{
    blt_params(src, dst, len);
    blt_go(BLT_START_COPY);
}

/* Read one parameter byte back. BLT_DATA reads do NOT auto-increment. */
static uint8_t
blt_rd(uint8_t idx)
{
    VERA_CTRL    = DCSEL(33);
    VERA_BLT_IDX = idx;
    return VERA_BLT_DATA;
}

/* Read a 17-bit parameter (SRC at base 0, DST at 3, LEN at 6) back. */
static uint32_t
blt_rd17(uint8_t base)
{
    uint32_t v;
    v  = (uint32_t)blt_rd(base);
    v |= (uint32_t)blt_rd((uint8_t)(base + 1)) << 8;
    v |= (uint32_t)blt_rd((uint8_t)(base + 2)) << 16;
    return v;
}

/* ---- verdict ----------------------------------------------------------- */

/* 320x240 8bpp bitmap, painted through the DATA PORT on purpose: the paint
 * path must not depend on the engine under test, or a broken blitter could
 * paint its own acquittal. */
static void
paint(uint8_t colour)
{
    uint32_t i;
    vset(0);
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

/* ---- the tests ---------------------------------------------------------- */

int
main(void)
{
    uint32_t i;

    /* greentest.c's display setup: VGA, layer 0, 320x240 8bpp bitmap at
     * VRAM $00000, so a paint() is a full-screen colour. The framebuffer
     * occupies $00000-$12BFF; every test below works at $13000 or above so a
     * stray blit cannot repaint the screen that judges it. */
    VERA_CTRL      = 0;
    VERA_DC_VIDEO  = 0x11;
    VERA_DC_HSCALE = 0x40;
    VERA_DC_VSCALE = 0x40;
    VERA_L0_CONFIG = 0x07;
    VERA_L0_TILEB  = 0;

    /* --- 1: feature detect ---------------------------------------------
     * BLT_ID reads $B6 (stock VERA has the version byte here), and BLT_CTRL
     * reads busy=0 while idle. Nothing else may run before this: on a stock
     * VERA every later register write would land in the version bank. */
    VERA_CTRL = DCSEL(33);
    if (VERA_BLT_ID != BLT_ID_VALUE) {
        fail(COL_RED);
    }
    if ((VERA_BLT_CTRL & 0x01) != 0) {
        fail(COL_RED);
    }

    /* --- 2: FILL, odd address, odd length ------------------------------
     * 999 bytes at $13000. Sentinels are placed by THIS test on both sides
     * first (a fresh emulator zeroes that region; asserting on unset memory
     * would pass by luck). Afterwards the parameter file must read back as
     * the engine left it: LEN=0, DST one-past-end (BLIT816.md: "SRC/DST/LEN
     * read back as the engine left them"). */
    vpoke(0x12FFFUL, 0xA1);
    vpoke(0x13000UL + 999UL, 0xA2);
    blt_fill(0x13000UL, 999UL, FILL_VAL);
    if (blt_rd17(6) != 0UL) {
        fail(COL_YELLOW);
    }
    if (blt_rd17(3) != 0x13000UL + 999UL) {
        fail(COL_YELLOW);
    }
    vset(0x13000UL);
    for (i = 0; i < 999UL; i++) {
        if (VERA_DATA0 != FILL_EXPECT) {
            fail(COL_YELLOW);
        }
    }
    if (vpeek(0x12FFFUL) != 0xA1 || vpeek(0x13000UL + 999UL) != 0xA2) {
        fail(COL_YELLOW);        /* the fill leaked past its bounds */
    }

    /* --- 3: COPY, misaligned src and dst -------------------------------
     * SRC odd, DST even-but-not-word-aligned, LEN=257: every alignment
     * combination the RTL's word fast path must fall back from. The source
     * pattern is seeded here, through the data port. Readback: SRC and DST
     * both one-past-end, LEN=0. */
    vset(0x13401UL);
    for (i = 0; i < 257UL; i++) {
        VERA_DATA0 = (uint8_t)((i * 7UL + 13UL) & 0xFFUL);
    }
    vpoke(0x13701UL, 0xB1);
    vpoke(0x13702UL + 257UL, 0xB2);
    blt_copy(0x13401UL, 0x13702UL, 257UL);
    if (blt_rd17(0) != 0x13401UL + 257UL) {
        fail(COL_BLUE);
    }
    if (blt_rd17(3) != 0x13702UL + 257UL) {
        fail(COL_BLUE);
    }
    if (blt_rd17(6) != 0UL) {
        fail(COL_BLUE);
    }
    vset(0x13702UL);
    for (i = 0; i < 257UL; i++) {
        if (VERA_DATA0 != (uint8_t)((i * 7UL + 13UL) & 0xFFUL)) {
            fail(COL_BLUE);
        }
    }
    if (vpeek(0x13701UL) != 0xB1 || vpeek(0x13702UL + 257UL) != 0xB2) {
        fail(COL_BLUE);
    }

    /* --- 4: the doubling idiom ------------------------------------------
     * Seed 16 bytes, then DST = SRC + LEN twice: 16 -> 32 -> 64. This is
     * the disjoint-overlap pattern BLIT816.md names as the intended fast
     * fill, and ascending order is what makes it work. Base is odd. */
    vset(0x14001UL);
    for (i = 0; i < 16UL; i++) {
        VERA_DATA0 = (uint8_t)((i * 11UL + 5UL) & 0xFFUL);
    }
    blt_copy(0x14001UL, 0x14001UL + 16UL, 16UL);
    blt_copy(0x14001UL, 0x14001UL + 32UL, 32UL);
    vset(0x14001UL);
    for (i = 0; i < 64UL; i++) {
        if (VERA_DATA0 != (uint8_t)(((i & 15UL) * 11UL + 5UL) & 0xFFUL)) {
            fail(COL_PURPLE);
        }
    }

    /* --- 5: LEN=0 is a no-op --------------------------------------------
     * "LEN = 0 starts nothing (busy never rises)": memory untouched and the
     * parameter file left exactly as programmed, for both COPY and FILL.
     * The target bytes are set by this test first. */
    vset(0x15000UL);
    for (i = 0; i < 16UL; i++) {
        VERA_DATA0 = 0xEE;
    }
    blt_params(0x13000UL, 0x15000UL, 0UL);
    blt_go(BLT_START_COPY);
    blt_params(0x13000UL, 0x15000UL, 0UL);
    VERA_BLT_IDX  = 9;
    VERA_BLT_DATA = 0x11;
    blt_go(BLT_START_FILL);
    if (blt_rd17(0) != 0x13000UL || blt_rd17(3) != 0x15000UL
        || blt_rd17(6) != 0UL) {
        fail(COL_CYAN);
    }
    vset(0x15000UL);
    for (i = 0; i < 16UL; i++) {
        if (VERA_DATA0 != 0xEE) {
            fail(COL_CYAN);
        }
    }

    /* --- 6: wrap at the top of VRAM --------------------------------------
     * FILL 4 bytes at $1FFFE. All four LAND: two at $1FFFE/$1FFFF, then the
     * address wraps modulo 128 KB and the tail lands at $00000/$00001.
     * $00002 must be untouched, and DST must read back as $00002 -- one-
     * past-end THROUGH the wrap.
     *
     * BOTH ENDS ARE CHECKED. A truncating adder would put all four bytes at
     * the top and still pass a test that only looked at $00000; a wrap that
     * fired one byte early would pass one that only looked at the top.
     *
     * (Under VERA816 this region was the unpopulated hole and the first two
     * bytes were required to VANISH. Requiring them to land is the stronger
     * assertion -- the old form passed if the writes simply went nowhere.)
     *
     * $1FFFE/$1FFFF are inside the sprite-attribute window, so this also
     * writes sprite 127's last two attribute bytes. Harmless: sprites are
     * disabled, and the VRAM shadow underneath is what vpeek reads. */
    vpoke(0x00000UL, 0x00);
    vpoke(0x00001UL, 0x00);
    vpoke(0x00002UL, 0x24);
    vpoke(0x1FFFEUL, 0x00);
    vpoke(0x1FFFFUL, 0x00);
    blt_fill(0x1FFFEUL, 4UL, 0x77);
    if (vpeek(0x1FFFEUL) != 0x77 || vpeek(0x1FFFFUL) != 0x77) {
        fail(COL_WHITE);         /* the top-of-VRAM bytes did not land */
    }
    if (vpeek(0x00000UL) != 0x77 || vpeek(0x00001UL) != 0x77) {
        fail(COL_WHITE);         /* the tail did not wrap to $00000 */
    }
    if (vpeek(0x00002UL) != 0x24) {
        fail(COL_WHITE);         /* the fill overshot the wrap */
    }
    if (blt_rd17(3) != 0x00002UL || blt_rd17(6) != 0UL) {
        fail(COL_WHITE);
    }

    /* --- 7: the firmware region -----------------------------------------
     * doc/KERNEL.md section 3: banks $F0-$FF are write-protected against
     * CPU stores, reads unrestricted, and the loader path bypasses the
     * protection.
     *
     * WHAT IS AT $F0:0000 DEPENDS ON WHERE THIS RUNS, so the test must not
     * care: under run-blit.sh it is a pattern placed with -load, on a card
     * it is the resident kernel placed by the HPS. Both are the loader
     * bypass, which is the half of the contract a read cannot distinguish
     * anyway. So: read whatever is there, try to change it, and require it
     * unchanged. That assertion is identical in both worlds and would fail
     * the moment the protection stopped working in either.
     *
     * The $EF:0000 store is the control -- one bank below the region, so it
     * proves far stores work at all and a dropped $F0 store is protection
     * rather than a store path that never worked. */
    {
        volatile uint8_t __far *fw = (volatile uint8_t __far *)0xF00000UL;
        volatile uint8_t __far *ctl = (volatile uint8_t __far *)0xEF0000UL;
        uint16_t was;

        ctl[0] = 0x77;
        if (ctl[0] != 0x77) {
            fail(COL_ORANGE);    /* far stores broken -- test would be vacuous */
        }
        was = fw[0];
        fw[0] = (uint8_t)(was ^ 0xFF);   /* guaranteed different from `was` */
        if (fw[0] != (uint8_t)was) {
            fail(COL_ORANGE);    /* the CPU store was not dropped */
        }
    }

    /* --- all green ------------------------------------------------------ */
    paint(COL_GREEN);

    for (;;) {
    }
}
