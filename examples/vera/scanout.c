/* scanout -- VERA816 section 8 test 5: 640x480 8bpp bitmap scanout.
 *
 * THIS IS THE TEST THAT WAS MISSING. X816_core/doc/AUDIT.md H-3 records what
 * its absence cost: the three renderer address wires in top.v stayed 15 bits
 * while both ends were widened to 17, so every renderer was truncated to the
 * first 128 KB, and four green conformance tests said nothing because all four
 * reach VRAM through the CPU data port -- a physically different path from the
 * display's. Only the SPRITE renderer has since been proved above 128 KB
 * (run-blit.sh test 8). This is the test that covers the TILE LAYERS, and it
 * is the reason the 352 KB exists at all.
 *
 * It asserts two independent widenings at once:
 *
 *   VERA816.md section 5   layer_renderer.v `bm_line_addr_tmp` -- stock VERA
 *                          keeps only line_idx_mul5[9:0] for 640-wide 8bpp,
 *                          so the line address wraps after line 204 EVEN WITH
 *                          UNLIMITED VRAM. This is the wall VERA2 was built
 *                          to climb.
 *   AUDIT.md H-3           top.v `l0_addr` -- 17 bits, or line 205 fetches
 *                          from $00080 instead of $20080.
 *
 * Both fail the same way and the runner names either: line 205 shows line 0.
 *
 * HOW IT JUDGES. The framebuffer is painted as eight 60-line colour bands, so
 * screen line y must show band y/60. run-scanout.sh probes every one of the
 * 480 lines in the emulator's GIF and checks it against that rule -- the
 * normative "line 205 must differ from line 0" is one case of it, and the
 * rest turn a pass into a statement about the whole 307,200-byte framebuffer.
 * A failure BEFORE the display comes up instead paints the screen one colour
 * and the runner names it (map at the top of run-scanout.sh).
 *
 * The paint goes through the CPU data port on purpose, and the program reads
 * the framebuffer back and requires the ramp before it trusts the display.
 * The point of this test is the RENDERER, so everything upstream of it has to
 * be an established fact rather than an assumption -- and the blitter, though
 * proved and much faster, is used for exactly one thing here (see below) for
 * the same reason blittest.c refuses to paint its verdict with the engine
 * under test.
 *
 * THE REGISTER WINDOWS. A 640x480 8bpp framebuffer is 307,200 bytes and VERA
 * puts the PSG, palette and sprite-attribute windows at $1F9C0-$1FFFF, inside
 * the CPU data port's view of VRAM. No 307,200-byte run inside the 352 KB can
 * avoid them: the largest clear stretches are $00000-$1F9BF (129,472 B) and
 * $20000-$57FFF (229,376 B). So the framebuffer necessarily covers them, and
 * lines 202-204 of any 640x480 picture based at $00000 cross the windows.
 *
 * That has two consequences this test has to respect, and VERA816.md section
 * 2.2 now states:
 *
 *   - The CPU data port must NOT be used to paint those 1,600 bytes. A store
 *     there lands in the palette (and PSG, and sprite attributes) as well as
 *     in VRAM, which repaints the picture in colours of its own choosing --
 *     it is how this test failed the first time it was ever run.
 *   - The BLITTER must, because its traffic goes through its own VRAM port
 *     and never touches those shadows (blit816.v; the emulator's blt_start
 *     says the same). It is the only way to put pixels there.
 *
 * WATCHING IT PAINT, this is visible and is NOT a fault: a ragged black gap
 * about two and a half rows tall opens in the middle of the purple band while
 * the ramp goes down the screen, and closes at the end. That is the window
 * band — line 202 from x=192, all of line 203, line 204 up to x=511 — left
 * alone by the data port and filled afterwards by the three blitter fills
 * below. It appears mid-purple because band 3 is lines 180-239, and it sits
 * exactly on the 128 KB boundary (line 204.8), so the gap marks where stock
 * VERA's address space would have ended. Confirmed on a DE10-Nano.
 *
 * BUILD AT -O0. Calypsi 5.18 eliminates volatile reads above -O0, and widens
 * in-place RMW on 8-bit locals (see the project README); every local that gets
 * arithmetic below is 16 or 32 bits wide for that reason.
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
#define VERA_L0_MAPB    (*(volatile uint8_t *)0x9F2E)
#define VERA_L0_TILEB   (*(volatile uint8_t *)0x9F2F)
#define VERA_L0_HSCR_L  (*(volatile uint8_t *)0x9F30)
#define VERA_L0_HSCR_H  (*(volatile uint8_t *)0x9F31)
#define VERA_L0_VSCR_L  (*(volatile uint8_t *)0x9F32)
#define VERA_L0_VSCR_H  (*(volatile uint8_t *)0x9F33)

/* ---- VERA816 extension bank, DCSEL=32 (VERA816.md 4.1) ---------------- */
#define VERA_ADDRX      (*(volatile uint8_t *)0x9F29)
#define VERA_L0_BASEX   (*(volatile uint8_t *)0x9F2A)
#define VERA_VRAMCAP    (*(volatile uint8_t *)0x9F2C)

/* ---- VERA816 control bank, DCSEL=34 (VERA816.md 4.4) ------------------ */
#define VERA_CTRL816    (*(volatile uint8_t *)0x9F29)
#define REGWIN_BIT      0x01

/* ---- VERA816 blitter bank, DCSEL=33 (VERA816.md 4.3) ------------------ */
#define VERA_BLT_IDX    (*(volatile uint8_t *)0x9F29)
#define VERA_BLT_DATA   (*(volatile uint8_t *)0x9F2A)
#define VERA_BLT_CTRL   (*(volatile uint8_t *)0x9F2B)
#define VERA_BLT_ID     (*(volatile uint8_t *)0x9F2C)

#define DCSEL(n)        ((uint8_t)((n) << 1))      /* CTRL = dcsel<<1 | addrsel */

#define VRAMCAP_VALUE   22          /* 352 KB in 16 KB units (VERA816.md 4.1) */
#define BLT_ID_VALUE    0xB6
#define BLT_START_FILL  0x02

/* ---- geometry ---------------------------------------------------------- */
#define FB_BASE         0x00000UL   /* VERA816.md 2: the framebuffer lives here */
#define SCR_W           640
#define SCR_H           480
#define BAND_LINES      60          /* 480 / 8 bands                          */

/* The PSG / palette / sprite-attribute windows, in the CPU data port's view
 * of VRAM. Inclusive. */
#define REGWIN_LO       0x1F9C0UL
#define REGWIN_HI       0x1FFFFUL

/* Byte address of the first pixel of a line, 8bpp 640-wide. */
#define FB_ADDR(line)   (FB_BASE + (uint32_t)(line) * (uint32_t)SCR_W)

/* Band n is painted colour 1 + n*BAND_STEP, i.e. white, red, cyan, purple,
 * green, blue, yellow, orange down the screen -- eight distinct entries of
 * VERA's default palette, none of them black, so "nothing rendered" is
 * distinguishable from "rendered wrongly".
 *
 * run-scanout.sh --negative rewrites BAND_STEP to 0 (and only that), which
 * makes every band the same colour. The program stays self-consistent -- its
 * own readback checks the same rule -- so it still paints and comes up, and
 * the runner's line probes are what must catch it: line 205 no longer differs
 * from line 0, which is precisely the assertion VERA816.md section 8 test 5
 * names. That proves the CHECK can fail, not merely that the program can. */
#define BAND_STEP       1

/* Set to 1 by run-scanout.sh --regwin (and by the SCANFULL.BIN the demo card
 * carries). Takes VERA816.md 4.4's escape hatch instead of 2.2's
 * choreography: CTRL816.REGWIN moves the register windows to $7F9C0-$7FFFF,
 * after which the CPU data port may paint every one of the 307,200 bytes and
 * the blitter is not involved at all.
 *
 * The two builds are the same test of the same renderer and must produce the
 * same 480 lines. What differs is visible while it paints: the default build
 * leaves a ragged 2.5-row black gap in the purple band until the blitter
 * closes it at the end, and this one never shows a gap at all -- which is the
 * entire practical point of section 4.4.
 *
 * #ifndef, because build.sh selects it with -DUSE_REGWIN=1 to produce
 * SCANFULL.BIN from this same source. An unguarded #define silently wins over
 * the command line -- the two binaries came out byte-identical until a cmp
 * caught it. run-scanout.sh --regwin seds the line instead, so it worked
 * either way and would not have shown the problem. */
#ifndef USE_REGWIN
#define USE_REGWIN      0
#endif

/* Pre-display failure colours. Deliberately outside the 1-8 band range so the
 * runner can never confuse a failure paint with a legitimate band. */
#define COL_NOCAP       11          /* dark grey    VRAMCAP is not 22          */
#define COL_NOMEM       9           /* brown        top of framebuffer is not
                                                    real, independent memory   */
#define COL_ALIAS       12          /* grey         a store above 128 KB
                                                    aliased onto a window      */
#define COL_NOBLT       13          /* light green  no blitter, so the window
                                                    band cannot be painted     */
#define COL_BADFB       10          /* light red    framebuffer readback       */
#define COL_NOCTRL      14          /* light blue   CTRL816 absent -- this
                                                    bitstream predates 4.4     */
#define COL_STUCK       15          /* light grey   REGWIN would not stick     */

/* Non-zero once CTRL816.REGWIN is confirmed set, which is what makes
 * $1F9C0-$1FFFF ordinary memory. A RUNTIME flag rather than #if, because
 * fail() paints through the same paint_line() and must be safe to call
 * BEFORE the bit is confirmed -- painting straight through with the windows
 * still in their stock position would rewrite the palette and repaint the
 * failure in a colour of VERA's choosing, which is the exact trap 2.2
 * describes. */
static uint8_t regwin_active;

static uint8_t
band_colour(uint16_t line)
{
    uint16_t band = line / BAND_LINES;
    return (uint8_t)(1U + band * BAND_STEP);
}

/* ---- 19-bit data-port access ------------------------------------------
 *
 * ADDRX MUST GO FIRST. The data port's fetch-ahead refreshes only on
 * ADDR_L/M/H writes, so writing ADDRX afterwards moves the address but not
 * the prefetch and the first read comes from {old bits 18:17, new low bits}
 * -- on both implementations. VERA816.md 4.1 states this; blittest.c has the
 * long version. */
static void
vset19(uint32_t a)
{
    VERA_CTRL   = DCSEL(32);
    VERA_ADDRX  = (uint8_t)((a >> 17) & 3);
    VERA_CTRL   = 0;
    VERA_ADDR_L = (uint8_t)a;
    VERA_ADDR_M = (uint8_t)(a >> 8);
    VERA_ADDR_H = (uint8_t)(0x10 | ((a >> 16) & 1));   /* increment 1 */
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

/* ---- blitter: the only way to put pixels in the window band ------------ */

static void
blt_fill(uint32_t dst, uint32_t len, uint8_t val)
{
    VERA_CTRL     = DCSEL(33);
    VERA_BLT_IDX  = 0;
    VERA_BLT_DATA = 0;                      /* 0-2: SRC, unused by FILL */
    VERA_BLT_DATA = 0;
    VERA_BLT_DATA = 0;
    VERA_BLT_DATA = (uint8_t)dst;           /* 3-5: DST */
    VERA_BLT_DATA = (uint8_t)(dst >> 8);
    VERA_BLT_DATA = (uint8_t)(dst >> 16);
    VERA_BLT_DATA = (uint8_t)len;           /* 6-8: LEN */
    VERA_BLT_DATA = (uint8_t)(len >> 8);
    VERA_BLT_DATA = (uint8_t)(len >> 16);
    VERA_BLT_IDX  = 9;
    VERA_BLT_DATA = val;
    VERA_BLT_CTRL = BLT_START_FILL;
    while ((VERA_BLT_CTRL & 0x01) != 0) {
    }
    VERA_CTRL     = 0;
}

/* ---- painting ---------------------------------------------------------- */

/* True for addresses the CPU data port must not be used to paint. With
 * CTRL816.REGWIN set there are none: the windows are at $7F9C0-$7FFFF and
 * this whole range is ordinary VRAM. */
static uint8_t
in_regwin(uint32_t a)
{
    if (regwin_active) {
        return 0;
    }
    return (uint8_t)(a >= REGWIN_LO && a <= REGWIN_HI);
}

/* Fill one line with one colour, skipping the register windows. Lines that do
 * not touch them (all but 202-204) stream through auto-increment; the three
 * that do are written a byte at a time, and their window part is left for the
 * blitter. */
static void
paint_line(uint16_t line, uint8_t colour)
{
    uint32_t base = FB_ADDR(line);
    uint16_t i;

    if (!in_regwin(base) && !in_regwin(base + (uint32_t)(SCR_W - 1))) {
        vset19(base);
        for (i = 0; i < SCR_W; i++) {
            VERA_DATA0 = colour;
        }
        return;
    }
    for (i = 0; i < SCR_W; i++) {
        if (!in_regwin(base + (uint32_t)i)) {
            vpoke(base + (uint32_t)i, colour);
        }
    }
}

/* Fill the screen with one colour, for the failure verdicts. The window band
 * is deliberately NOT filled: this runs when something is already wrong, and
 * blt_fill polls a busy bit that on a machine with no blitter is a byte of
 * VERA's version string at DCSEL 33 — it could never fall. Leaving 2.5 of 480
 * lines stale costs nothing, because the runner judges these paints by
 * dominant colour. */
static void
paint(uint8_t colour)
{
    uint16_t line;

    for (line = 0; line < SCR_H; line++) {
        paint_line(line, colour);
    }
}

static void
fail(uint8_t colour)
{
    paint(colour);
    for (;;) {
    }
}

/* ---- the test ---------------------------------------------------------- */

int
main(void)
{
    uint16_t line;
    uint32_t base, end, lo, hi;
    uint8_t c, was_pal, was_spr;

    /* Display first, so that a failure below is visible rather than silent:
     * VGA output, layer 0 only, 1:1 scale -- 128 is VERA's unity step, so the
     * composer emits 640x480 and screen line y IS layer line y.
     *
     * L0_TILEB bit 0 is the bitmap width select (0 = 320, 1 = 640) and bits
     * [7:2] are the base; 0x01 therefore means "640 wide, base $00000".
     * L0_CONFIG 0x07 = bitmap mode, 8bpp.
     *
     * The scroll registers are written even though they reset to zero,
     * because HSCROLL_H[3:0] is not a scroll in bitmap mode -- both
     * implementations use it as the palette offset, and a non-zero one would
     * silently remap every band colour. This test must not inherit that. */
    VERA_CTRL      = 0;
    VERA_DC_VIDEO  = 0x11;      /* VGA + layer 0, sprites and layer 1 off */
    VERA_DC_HSCALE = 0x80;
    VERA_DC_VSCALE = 0x80;
    VERA_L0_CONFIG = 0x07;
    VERA_L0_MAPB   = 0x00;
    VERA_L0_TILEB  = 0x01;
    VERA_L0_HSCR_L = 0x00;
    VERA_L0_HSCR_H = 0x00;      /* palette offset 0 in bitmap mode */
    VERA_L0_VSCR_L = 0x00;
    VERA_L0_VSCR_H = 0x00;

    /* The high two bits of L0's MAPBASE/TILEBASE (VERA816.md 4.1). Zero is
     * the reset value and what a $00000 base needs, but writing it makes the
     * base a fact this test established rather than one it inherited -- and a
     * stock VERA, which has no such register, would take this as a DC_HSCALE
     * write and come up at 2x. That is a distinction worth having. */
    VERA_CTRL      = DCSEL(32);
    VERA_L0_BASEX  = 0x00;

    /* --- preflight 1: the extension is present -------------------------
     * If VRAMCAP does not read 22 there is no VERA816 here and every later
     * assertion would be about something else. */
    if (VERA_VRAMCAP != VRAMCAP_VALUE) {
        VERA_CTRL = 0;
        fail(COL_NOCAP);
    }
    VERA_CTRL = 0;

#if USE_REGWIN
    /* --- preflight 2a: take VERA816.md 4.4's escape hatch -----------------
     * Detection is the recipe 4.4 states: CTRL816 reads $00 at reset, while
     * a bitstream without the DCSEL-34 bank falls through to the version
     * string and reads $56 ('V'). Assert it rather than assume it, then set
     * the bit and require it back. Only after that is regwin_active raised,
     * so every fail() above and below paints safely either way. */
    VERA_CTRL = DCSEL(34);
    c = VERA_CTRL816;
    if (c != 0x00) {
        VERA_CTRL = 0;
        fail(COL_NOCTRL);
    }
    VERA_CTRL816 = REGWIN_BIT;
    c = VERA_CTRL816;
    VERA_CTRL = 0;
    if (c != REGWIN_BIT) {
        fail(COL_STUCK);
    }
    regwin_active = 1;
#else
    /* --- preflight 2b: the blitter, which paints the window band ---------
     * Only the stock path needs it. With REGWIN set the data port covers the
     * whole framebuffer and the blitter is not part of this test at all. */
    VERA_CTRL = DCSEL(33);
    c = VERA_BLT_ID;
    VERA_CTRL = 0;
    if (c != BLT_ID_VALUE) {
        fail(COL_NOBLT);
    }
#endif

    /* --- preflight 3: the far end of the framebuffer is real memory ------
     * A 640x480 8bpp framebuffer at $00000 ends at $4AFFF, which is 307,199 --
     * well above the stock 128 KB and inside the 352 KB populated region. Two
     * distinct values at the two ends, checked both ways round, so this cannot
     * pass on an alias: if $4AFFF mirrored $00000 the second read would see
     * the first write. */
    vpoke(0x00000UL, 0x5A);
    vpoke(0x4AFFFUL, 0xA5);
    if (vpeek(0x4AFFFUL) != 0xA5 || vpeek(0x00000UL) != 0x5A) {
        fail(COL_NOMEM);
    }

    /* --- preflight 4: the register windows do not alias above 128 KB -----
     * The palette / sprite-attribute / PSG decodes must test the full 19-bit
     * address, not just bits [16:0]. If they ignore [18:17] then $3FA00 is a
     * second palette and $3FC00 a second sprite-attribute file -- and since
     * the framebuffer spans $00000-$4AFFF, an ordinary paint would rewrite
     * the palette from its own pixels partway down the screen. Nothing else
     * would report that: the picture simply comes up in the wrong colours,
     * which is indistinguishable at a glance from a renderer fault.
     *
     * $1FA02/$1FA03 is palette entry 1 and $1FC00 is sprite 0's attribute
     * byte 0; both are readable through the data port. Read, write the alias,
     * read again, and require no change. The stores go to plain VRAM inside
     * the framebuffer, which is painted afterwards, so nothing is left behind.
     *
     * The PSG shares the decode and cannot be probed this way -- its
     * registers are write-only -- so it rides on the same fix. */
    was_pal = vpeek(0x1FA02UL);
    was_spr = vpeek(0x1FC00UL);
    vpoke(0x3FA02UL, (uint8_t)(was_pal ^ 0xFF));
    vpoke(0x3FC00UL, (uint8_t)(was_spr ^ 0xFF));
    if (vpeek(0x1FA02UL) != was_pal || vpeek(0x1FC00UL) != was_spr) {
        fail(COL_ALIAS);
    }

    /* --- paint the ramp -------------------------------------------------
     * Eight 60-line bands over the whole 307,200 bytes. Lines 0-204 live
     * below 128 KB and lines 205-479 above it; the boundary falls inside
     * band 3, so bands 4-7 exist ONLY above 128 KB. */
    for (line = 0; line < SCR_H; line++) {
        paint_line(line, band_colour(line));
    }

    /* The window band, one blitter fill per line so each keeps its own
     * colour -- lines 202-204 happen to share band 3, but nothing here
     * depends on that. Skipped entirely when REGWIN moved the windows away:
     * the loop above already painted those bytes through the data port. */
    if (!regwin_active) {
        for (line = 202; line <= 204; line++) {
            base = FB_ADDR(line);
            end  = base + (uint32_t)(SCR_W - 1);
            lo   = base > REGWIN_LO ? base : REGWIN_LO;
            hi   = end  < REGWIN_HI ? end  : REGWIN_HI;
            blt_fill(lo, hi - lo + 1UL, band_colour(line));
        }
    }

    /* --- verify what is actually in VRAM --------------------------------
     * Both ends of every line, through the data port. If this passes and the
     * screen is still wrong, the fault is in the renderer and nowhere else --
     * which is the entire point of the test, so it is worth 960 reads.
     *
     * Ends that fall inside the register windows are skipped, because a read
     * there returns palette or sprite-attribute state rather than the VRAM
     * byte the renderer will fetch. That silences line 203 entirely (both its
     * ends are inside) and one end each of 202 and 204; those three lines are
     * checked on the screen only, and the runner says so when they are the
     * ones that fail. */
    for (line = 0; line < SCR_H; line++) {
        c    = band_colour(line);
        base = FB_ADDR(line);
        end  = base + (uint32_t)(SCR_W - 1);
        if (!in_regwin(base) && vpeek(base) != c) {
            fail(COL_BADFB);
        }
        if (!in_regwin(end) && vpeek(end) != c) {
            fail(COL_BADFB);
        }
    }

    /* The framebuffer holds the ramp and the display is configured. Whatever
     * is on the screen from here is the renderer's answer; run-scanout.sh
     * reads it off the last GIF frame. */
    for (;;) {
    }
}
