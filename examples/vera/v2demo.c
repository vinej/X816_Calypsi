/* v2demo -- the VERA2 bitmap layer, 640x480 4bpp, judged by eye.
 *
 * The contract is X816_core/doc/VERA2.md. This is the first thing that puts
 * a VERA2 picture on a screen, so it is built to be DIAGNOSTIC rather than
 * merely pretty: each feature of the picture fails in a distinguishable way.
 *
 *   16 vertical colour bands, 40 px each   -- tests the 4bpp nibble order and
 *                                             the within-line unpack. Wrong
 *                                             nibble order swaps the halves of
 *                                             every band pair; a broken unpack
 *                                             gives 40-px stripes of one colour
 *                                             where two are expected.
 *   a WHITE bar across the top 16 lines    -- tests the line stride. If the
 *   a  BLUE bar across the bottom 16 lines    stride is wrong these repeat down
 *                                             the screen or land in the middle.
 *   a diagonal white line, top-left to     -- tests line addressing across the
 *   bottom-right                              whole frame at once: any stride
 *                                             error bends or breaks it.
 *
 * So: bands + two bars + a straight diagonal = the layer is right. Anything
 * else names roughly which part is wrong.
 *
 * WHAT THIS DEMONSTRATES BESIDES WORKING. The framebuffer is ORDINARY MEMORY
 * at $E0:0000 (doc/VERA2.md 2) -- everything below is plain far stores through
 * a C pointer. There is no data port, no blitter and no address register. On
 * the upstream core this same picture needs a register-programmed DMA.
 *
 * BUILD AT -O0. Calypsi 5.18 eliminates volatile reads above -O0.
 *
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

#define WIDTH           640u
#define HEIGHT          480u
#define BPL             (WIDTH / 2u)          /* 4bpp: 320 bytes per line */

/* NOT `const`: with --data-model=small a const array is placed in the far code
 * section but referenced with 16-bit addressing, and the linker rejects it
 * ("value 66533 is out of range"). Plain initialised data lands in near RAM.
 *
 * The 16 entries, RGB444. Deliberately distinguishable from each other on a
 * real display -- a ramp would make an off-by-one in the palette index
 * invisible, which is the opposite of what this program is for. */
static uint8_t pal_r[16] = {0,15, 8, 0, 0, 0,15,15, 4, 8,15, 0, 8,15, 6,12};
static uint8_t pal_g[16] = {0,15, 8, 0,10,10, 0,10, 4, 0, 6,15,12, 4, 6, 8};
static uint8_t pal_b[16] = {0,15, 8,12, 0,12, 0, 0, 4, 8, 6,12, 0, 8,15, 0};

static void
palette_load(void)
{
    uint16_t i;

    V2_PALADR = 0;                    /* index auto-increments after PALHI */
    for (i = 0; i < 16u; i++) {
        V2_PALLO = (uint8_t)((pal_g[i] << 4) | pal_b[i]);
        V2_PALHI = pal_r[i];          /* commits {R,G,B}, steps the index */
    }
}

/* One pixel. Slow by design -- only the diagonal uses it. */
static void
plot(uint16_t x, uint16_t y, uint8_t colour)
{
    volatile uint8_t __far *fb = (volatile uint8_t __far *)X816_VFB_BASE;
    uint32_t off = (uint32_t)y * BPL + (x >> 1);
    uint8_t  b   = fb[off];

    /* High nibble is the LEFT pixel (doc/VERA2.md 4). */
    if ((x & 1u) == 0u) {
        fb[off] = (uint8_t)((b & 0x0Fu) | (colour << 4));
    } else {
        fb[off] = (uint8_t)((b & 0xF0u) | (colour & 0x0Fu));
    }
}

int
main(void)
{
    volatile uint8_t __far *fb = (volatile uint8_t __far *)X816_VFB_BASE;
    uint16_t y, i;
    uint32_t off;

    /* ---- feature detect, and say so if it is not there -----------------
     * $B5 means present AND the OSD switch is on. Anything else means the
     * layer is unavailable -- most likely the OSD master switch is Off, which
     * is the default. Leave VERA's text-mode screen alone in that case so the
     * machine still shows something rather than going black. */
    if (V2_ID != X816_VERA2_ID_VALUE) {
        for (;;) {
        }
    }

    palette_load();

    /* ---- 16 vertical bands, 40 px each ---------------------------------
     * A band is 40 px = 20 bytes, and both nibbles of a byte are the same
     * colour, so one line is built with plain byte stores. */
    for (i = 0; i < BPL; i++) {
        uint8_t c = (uint8_t)(i / 20u);          /* 20 bytes = 40 px */
        fb[i] = (uint8_t)((c << 4) | c);
    }
    /* ...and copied down the frame. Ordinary memory, so this is a plain copy;
     * no blitter and no DMA registers are involved. */
    for (y = 1; y < HEIGHT; y++) {
        off = (uint32_t)y * BPL;
        for (i = 0; i < BPL; i++) {
            fb[off + i] = fb[i];
        }
    }

    /* ---- top and bottom bars: the line-stride check --------------------- */
    for (y = 0; y < 16u; y++) {
        off = (uint32_t)y * BPL;
        for (i = 0; i < BPL; i++) {
            fb[off + i] = 0x11;                  /* white */
        }
    }
    for (y = HEIGHT - 16u; y < HEIGHT; y++) {
        off = (uint32_t)y * BPL;
        for (i = 0; i < BPL; i++) {
            fb[off + i] = 0x33;                  /* blue */
        }
    }

    /* ---- the diagonal: line addressing across the whole frame ----------- */
    for (y = 0; y < HEIGHT; y++) {
        plot((uint16_t)((uint32_t)y * WIDTH / HEIGHT), y, 1u);
    }

    /* ---- show it -------------------------------------------------------
     * Display base 0. VERA keeps producing the raster; the bitmap replaces
     * the active area. passthru is left off, so this is the bitmap alone. */
    V2_DISPL = 0;
    V2_DISPM = 0;
    V2_DISPH = 0;

    VERA_CTRL     = 0;
    VERA_DC_VIDEO = 0x01;                        /* VGA output, layers off */

    V2_CTRL = (uint8_t)((X816_VERA2_MODE_4BPP << 1) | 1u);   /* 4bpp, enable */

    for (;;) {
    }
}
