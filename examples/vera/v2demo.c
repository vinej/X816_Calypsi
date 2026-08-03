/* v2demo -- the VERA2 bitmap layer, 640x480 4bpp, judged by eye.
 *
 * The contract is X816_core/doc/VERA2.md. This is the first thing that puts
 * a VERA2 picture on a screen, so it is built to be DIAGNOSTIC rather than
 * merely pretty: each feature of the picture fails in a distinguishable way.
 *
 *   16 vertical colour bands, 40 px each   -- the 4bpp nibble order and the
 *                                             within-line unpack
 *   a WHITE bar across the top 16 lines    -- the line stride: if it is wrong
 *   a  BLUE bar across the bottom 16 lines    these repeat or land mid-screen
 *   a diagonal white line, corner to       -- line addressing across the whole
 *   corner                                    frame at once
 *
 * bands + bars + a straight diagonal = the layer is right.
 *
 * ============================================================================
 * HOW THIS FILE WRITES FAR MEMORY, AND WHY -- READ BEFORE EDITING.
 *
 * The first version of this demo produced a scrambled picture on the emulator
 * while the emulator's own model test displayed a -loaded framebuffer
 * pixel-perfectly. Eight probe programs later, the cause was pinned to
 * Calypsi 5.18 at -O0 (run-v2.sh is the regression test for all of it):
 *
 *   fb[i32]        BROKEN: a 32-bit array index on a __far pointer is
 *                  TRUNCATED TO 16 BITS -- fb[0x10400] lands on fb[0x0400].
 *                  Loads truncate the same way, so read-back "verifies".
 *   *p++ crossing  BROKEN: walking a __far pointer across a 64 KB boundary
 *                  wraps within the bank -- $E0:FFFF steps to $E0:0000.
 *   RMW via far    BROKEN: uint8_t b = p[i]; p[i] = f(b); reads the wrong
 *                  byte (the documented 8-bit-local family).
 *   (far *)(addr)  CORRECT: casting a 32-bit address -- compile-time or
 *                  runtime-computed -- yields a good 24-bit pointer, in
 *                  every bank.
 *
 * So: all drawing goes through fill_far(), which takes a 32-bit address,
 * makes a fresh cast pointer, and SPLITS AT BANK BOUNDARIES so no walk ever
 * crosses one. The same split, for the same architectural reason, that
 * kern_block_move does around MVN.
 * ============================================================================
 *
 * BUILD AT -O0. Calypsi 5.18 eliminates volatile reads above -O0.
 * The screen stays up; there is no console linked, so reset to return.
 */

#include <stdint.h>
#include "x816_contract.h"

/* ---- VERA, only what is needed to hand the display over ---------------- */
#define VERA_CTRL       (*(volatile uint8_t *)0x9F25)
#define VERA_DC_VIDEO   (*(volatile uint8_t *)0x9F29)   /* DCSEL 0 */

/* ---- VERA2 ------------------------------------------------------------- */
#define V2_CTRL         (*(volatile uint8_t *)X816_VERA2_BASE)
#define V2_ID           (*(volatile uint8_t *)X816_VERA2_ID)
#define V2_DISPL        (*(volatile uint8_t *)X816_VERA2_DISPL)
#define V2_DISPM        (*(volatile uint8_t *)X816_VERA2_DISPM)
#define V2_DISPH        (*(volatile uint8_t *)X816_VERA2_DISPH)
#define V2_PALADR       (*(volatile uint8_t *)X816_VERA2_PALADR)
#define V2_PALLO        (*(volatile uint8_t *)X816_VERA2_PALLO)
#define V2_PALHI        (*(volatile uint8_t *)X816_VERA2_PALHI)

#define BPL             320u                  /* 4bpp: bytes per line */

/* NOT `const`: with --data-model=small a const array is placed in the far
 * code section but referenced with 16-bit addressing, and the linker rejects
 * it. Plain initialised data lands in near RAM.
 *
 * The 16 entries, RGB444, deliberately distinguishable from each other. */
static uint8_t pal_r[16] = {0,15, 8, 0, 0, 0,15,15, 4, 8,15, 0, 8,15, 6,12};
static uint8_t pal_g[16] = {0,15, 8, 0,10,10, 0,10, 4, 0, 6,15,12, 4, 6, 8};
static uint8_t pal_b[16] = {0,15, 8,12, 0,12, 0, 0, 4, 8, 6,12, 0, 8,15, 0};

/* Fill len bytes at a 32-bit address. Fresh cast pointer per bank segment,
 * never walking across a 64 KB boundary -- see the header. */
static void
fill_far(uint32_t addr, uint16_t len, uint8_t v)
{
    while (len != 0u) {
        volatile uint8_t __far *q = (volatile uint8_t __far *)addr;
        uint32_t room32 = 0x10000UL - (addr & 0xFFFFUL);
        uint16_t room   = (room32 > (uint32_t)len) ? len : (uint16_t)room32;
        uint16_t i;
        for (i = 0; i < room; i++) {
            *q++ = v;
        }
        addr += room;
        len  -= room;
    }
}

/* One byte, by cast pointer. No RMW: callers supply the WHOLE byte. */
static void
poke_far(uint32_t addr, uint8_t v)
{
    *(volatile uint8_t __far *)addr = v;
}

int
main(void)
{
    uint32_t base;                /* line start, ACCUMULATED (no multiply) */
    uint16_t y, x;
    uint16_t err;                 /* diagonal Bresenham accumulator */
    uint8_t  c, other;
    uint16_t i;

    /* ---- feature detect, and say so if it is not there -----------------
     * $B5 means present AND the OSD switch is on. Anything else: leave
     * VERA's screen alone so the machine shows something rather than black.
     * On the emulator the switch is the -vera2 flag. */
    if (V2_ID != X816_VERA2_ID_VALUE) {
        for (;;) {
        }
    }

    V2_PALADR = 0;                /* index auto-increments after PALHI */
    for (i = 0; i < 16u; i++) {
        V2_PALLO = (uint8_t)((pal_g[i] << 4) | pal_b[i]);
        V2_PALHI = pal_r[i];
    }

    /* ---- the frame: bars top and bottom, bands between ------------------ */
    base = X816_VFB_BASE;
    for (y = 0; y < 480u; y++) {
        if (y < 16u) {
            fill_far(base, BPL, 0x11);                   /* white bar */
        } else if (y >= 464u) {
            fill_far(base, BPL, 0x33);                   /* blue bar */
        } else {
            for (c = 0; c < 16u; c++) {                  /* 16 bands, 20 B each */
                fill_far(base + (uint32_t)(c * 20u), 20u,
                         (uint8_t)((c << 4) | c));
            }
        }
        base += BPL;
    }

    /* ---- the diagonal ----------------------------------------------------
     * x = y * 640/480, by accumulator (x += 1, plus 1 more every third line)
     * -- no multiply, no divide. Each plotted byte is written WHOLE: the
     * diagonal nibble plus whatever the other pixel of that byte should be
     * (bar colour in the bars, band colour between), so no read-modify-write
     * is needed anywhere. */
    base = X816_VFB_BASE;
    x = 0;
    err = 0;
    for (y = 0; y < 480u; y++) {
        if (y < 16u) {
            other = 0x1;                                 /* inside the white bar */
        } else if (y >= 464u) {
            other = 0x3;                                 /* inside the blue bar */
        } else {
            other = (uint8_t)((x / 40u) & 0x0Fu);        /* the band under it */
        }
        if ((x & 1u) == 0u) {
            poke_far(base + (x >> 1), (uint8_t)(0x10u | other));   /* left px */
        } else {
            poke_far(base + (x >> 1), (uint8_t)((other << 4) | 1u)); /* right */
        }
        base += BPL;
        x++;
        err += 160u;              /* 640/480 = 1 + 160/480 */
        if (err >= 480u) {
            err -= 480u;
            x++;
        }
    }

    /* ---- show it -------------------------------------------------------- */
    V2_DISPL = 0;
    V2_DISPM = 0;
    V2_DISPH = 0;

    VERA_CTRL     = 0;
    VERA_DC_VIDEO = 0x01;                                /* VGA, layers off */

    V2_CTRL = (uint8_t)((X816_VERA2_MODE_4BPP << 1) | 1u);   /* 4bpp, enable */

    for (;;) {
    }
}
