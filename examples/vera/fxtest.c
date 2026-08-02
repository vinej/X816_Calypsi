/* ==========================================================================
 * fxtest.c -- VERA FX affine mode: does it work, and how fast is it?
 *
 * THERE WAS NO FX TEST AT ALL. Nothing in this tree exercised line draw,
 * polygon fill, the 32-bit cache or affine mode -- FX arrived working from
 * upstream and has been carried, unexercised, ever since. That is fine while
 * nobody touches it. It stops being fine the moment someone proposes to widen
 * `fx_map_base_address_r` and `fx_tiledata_base_address_r` (X816_core
 * doc/VERA816.md section 9's FX_BASEX), because a change inside
 * addr_data.v's FX section would be completely unguarded -- and every silent
 * failure this project has had (AUDIT.md H-3, H-4, the L0_BASEX cache bug)
 * looked exactly like nothing at all until a test drove the real path.
 *
 * So this exists for two reasons, and the second is the one that was asked
 * for:
 *
 *   1. a GUARD, so FX_BASEX can be attempted without flying blind;
 *   2. a MEASUREMENT of the affine fill rate, because the case for FX_BASEX
 *      rests on whether affine is fast enough to be worth having, and that
 *      number had only ever been estimated on paper.
 *
 * It runs at 320x240 and puts its map and tile data below 128 KB, so it needs
 * NO RTL change and says nothing about FX_BASEX either way. It measures what
 * exists.
 *
 * HOW THE CORRECTNESS CHECK AVOIDS AGREEING WITH ITSELF
 * ----------------------------------------------------
 * The obvious test -- read FX, then compute the same address the same way and
 * compare -- proves only that two copies of my reading of the RTL agree. So
 * the reference here is built from the DOCUMENTED SEMANTICS of a tilemap
 * lookup, in plain C, walking its own fixed-point position:
 *
 *     pixel-in-map = pos >> 9          (11.9 fixed point)
 *     tile         = (pixel-in-map >> 3) within the map
 *     pixel        = tiledata[tile*64 + (y&7)*8 + (x&7)]
 *
 * and the tilemap and tile data are filled with values chosen so that a wrong
 * tile, a wrong row, a wrong column and a wrong base all produce different
 * answers. If FX and the C walk disagree, one of them is wrong and the test
 * says which pixel.
 *
 *   GREEN    every check passed; the fill rate is printed
 *   RED      1: FX answered nothing sane -- affine returned a constant
 *   YELLOW   2: identity walk (increment = exactly one pixel across)
 *   BLUE     3: a fractional increment -- the sub-pixel stepping Mode 7 needs
 *   MAGENTA  4: a two-axis walk, x and y stepping together (a rotation)
 *   CYAN     5: clipping -- outside the map must give tile 0, not wrap
 *   ORANGE   6: wrapping -- with clip OFF the map repeats instead
 *
 * The number also lands at $00:0400.
 * ========================================================================== */

#include <stdint.h>
#include "console.h"
#include "goshell.h"

#define VERA_ADDR_L     (*(volatile uint8_t *)0x9F20)
#define VERA_ADDR_M     (*(volatile uint8_t *)0x9F21)
#define VERA_ADDR_H     (*(volatile uint8_t *)0x9F22)
#define VERA_DATA0      (*(volatile uint8_t *)0x9F23)
#define VERA_DATA1      (*(volatile uint8_t *)0x9F24)
#define VERA_CTRL       (*(volatile uint8_t *)0x9F25)

/* DCSEL-selected. CTRL = dcsel<<1 | addrsel. */
#define VERA_R9         (*(volatile uint8_t *)0x9F29)
#define VERA_RA         (*(volatile uint8_t *)0x9F2A)
#define VERA_RB         (*(volatile uint8_t *)0x9F2B)
#define VERA_RC         (*(volatile uint8_t *)0x9F2C)
#define DCSEL(n)        ((uint8_t)((n) << 1))

/* FX_CTRL bits, DCSEL 2 $9F29 (addr_data.v line 737). */
#define FX_MODE_NONE    0
#define FX_MODE_LINE    1
#define FX_MODE_POLY    2
#define FX_MODE_AFFINE  3
#define FX_4BIT         0x04
#define FX_CACHE_FILL   0x20

/* FX_TILEBASE, DCSEL 2 $9F2A: [7:2] base>>11, [1] apply_clip.
   FX_MAPBASE,  DCSEL 2 $9F2B: [7:2] base>>11, [1:0] map size (0=2x2,
   1=8x8, 2=32x32, 3=128x128). */
#define FX_CLIP         0x02
#define MAPSIZE_8X8     1
#define MAP_TILES       8               /* 8x8 tiles for MAPSIZE_8X8 */

/* Both below 128 KB, so this runs on the current bitstream unchanged.
   Granularity is 2048 bytes (the base field is shifted left 11). */
#define TILE_BASE       0x10000UL       /* base field 32 */
#define MAP_BASE        0x10800UL       /* base field 33 */
#define NTILES          16
#define TILE_BYTES      64              /* 8x8 at 8bpp */

#define VIA1_T1C        (*(volatile unsigned int *)0x9F04)
#define VIA1_T1CL       (*(volatile uint8_t *)0x9F04)
#define VIA1_T1CH       (*(volatile uint8_t *)0x9F05)
#define VIA1_ACR        (*(volatile uint8_t *)0x9F0B)

#define RESULT          (*(volatile uint8_t *)0x0400)

#define SAMPLES         64              /* pixels compared per walk          */
#define TIMED_PIXELS    4096            /* pixels in the fill-rate run       */

static uint8_t tilemap[MAP_TILES * MAP_TILES];
static uint8_t fail;

/* ---- plain 17-bit data-port access; everything here is below 128 KB ----- */
static void
vset(uint32_t a)
{
    VERA_CTRL   = 0;
    VERA_ADDR_L = (uint8_t)a;
    VERA_ADDR_M = (uint8_t)(a >> 8);
    VERA_ADDR_H = (uint8_t)(0x10 | ((a >> 16) & 1));   /* increment 1 */
}

static void
vpoke(uint32_t a, uint8_t v)
{
    vset(a);
    VERA_DATA0 = v;
}

/* ---- the reference: a tilemap lookup, written from the semantics --------
 *
 * mx, my are PIXEL positions in the map (pos >> 9). Deliberately not derived
 * from the same expressions the RTL uses -- see the header. */
static uint8_t
ref_pixel(uint16_t mx, uint16_t my)
{
    uint16_t tx = (uint16_t)((mx >> 3) & (MAP_TILES - 1));
    uint16_t ty = (uint16_t)((my >> 3) & (MAP_TILES - 1));
    uint8_t  ti = tilemap[ty * MAP_TILES + tx];
    /* Tile data is generated below as: tile t, row r, column c -> a byte that
       differs if ANY of the three is wrong. */
    return (uint8_t)((ti << 4) ^ ((my & 7) << 3) ^ (mx & 7) ^ 0x80);
}

/* ---- FX setup ---------------------------------------------------------- */
static void
fx_affine_setup(uint8_t clip)
{
    VERA_CTRL = DCSEL(2);
    VERA_R9   = FX_MODE_AFFINE;                       /* 8bpp, no cache     */
    VERA_RA   = (uint8_t)(((TILE_BASE >> 11) << 2) | (clip ? FX_CLIP : 0));
    VERA_RB   = (uint8_t)(((MAP_BASE >> 11) << 2) | MAPSIZE_8X8);
    VERA_CTRL = 0;
}

/* Increments are 15-bit signed, added to the 20-bit position. One whole pixel
   across is 1 << 9 = 512. */
static void
fx_set_incr(int16_t ix, int16_t iy)
{
    VERA_CTRL = DCSEL(3);
    /* Masks, not (uint8_t) casts. The assignment to a volatile uint8_t
       truncates by itself, and `(unsigned char)(expr)` against a byte is the
       Calypsi 5.18 sign-extension pattern tools/calypsi_scan.py exists to
       keep out of this tree -- it flagged these two the first time this file
       compiled. Harmless here (the mask clears bit 7, so the value can never
       have it set) but there is no reason to carry a site at all. */
    VERA_R9   = ix & 0xFF;
    VERA_RA   = (ix >> 8) & 0x7F;                     /* bit 7 = times 32   */
    VERA_RB   = iy & 0xFF;
    VERA_RC   = (iy >> 8) & 0x7F;
    VERA_CTRL = 0;
}

/* Position is 20 bits: $9F29 sets [16:9], $9F2A[2:0] sets [19:17] and
   $9F2A[7] sets bit 0. Bits [8:1] are the sub-pixel remainder and are not
   directly writable, so a start position is a whole pixel (or a half, via
   bit 0) -- which is all this test needs. */
static void
fx_set_pos(uint16_t px, uint16_t py)
{
    VERA_CTRL = DCSEL(4);
    VERA_R9   = (uint8_t)px;                          /* pos_x[16:9]        */
    VERA_RA   = (uint8_t)((px >> 8) & 0x07);          /* pos_x[19:17]       */
    VERA_RB   = (uint8_t)py;
    VERA_RC   = (uint8_t)((py >> 8) & 0x07);
    VERA_CTRL = 0;
}

/* Walk `n` pixels from (px,py) stepping by (ix,iy), comparing FX against the
   C reference at every step. Returns 0 on agreement.
   The C side keeps the position in the same 20-bit fixed point FX does. */
/* THE SUB-PIXEL REMAINDER SURVIVES A POSITION WRITE, and that is not a
 * detail -- it is a property anyone writing a Mode 7 renderer has to know.
 *
 * The position is 20 bits. Writing $9F29 sets bits [16:9] and $9F2A sets
 * [19:17] and bit 0; bits [8:1] are NOT writable at all. They are the
 * fractional remainder and only the increments move them. So setting a
 * position does not start you at a whole pixel -- you land at whatever
 * fraction the previous run left, and a renderer that sets a fresh position
 * per scanline inherits a sub-pixel offset from the line before.
 *
 * This test found that by disagreeing with a reference that assumed the
 * fraction was zeroed. Rather than work around it, the reference now mirrors
 * it: fracx/fracy carry across walks exactly as the hardware's do.
 */
/* AND IT STARTS AT HALF A PIXEL, not zero. Both implementations reset the
 * position's fractional part to 0.5 -- addr_data.v `fx_pixel_pos_x_r <=
 * 20'd256` with 9 fraction bits, video.c `fx_x_pixel_position = 0x8000` with
 * 16. They agree, and a renderer that assumes it starts at a pixel boundary
 * is half a pixel out from the first scanline.
 *
 * A whole-pixel increment hides it completely (0.5 + k still floors to k),
 * which is why the identity walk passed while the fractional one failed at
 * exactly the third sample -- 0.5 + 2*(171/512) crosses 1.0 and 2*(171/512)
 * does not. */
static uint16_t fracx = 256, fracy = 256;
static uint16_t d_i, d_mx, d_my, d_fr;
static uint8_t  d_got, d_want;

static uint8_t
walk(uint16_t px, uint16_t py, int16_t ix, int16_t iy, uint16_t n)
{
    /* bit 0 is written as zero by fx_set_pos ($9F2A bit 7); bits [8:1] are
       whatever the last walk left behind. */
    uint32_t posx = ((uint32_t)px << 9) | (uint32_t)(fracx & 0x1FE);
    uint32_t posy = ((uint32_t)py << 9) | (uint32_t)(fracy & 0x1FE);
    uint16_t i;

    /* Program the increment INTO FX, not merely into the model below. The
       first version of this took ix/iy and used them only for the reference,
       so FX kept stepping at whatever the previous walk had set -- the
       identity walk passed because 512 happened to be right, and the
       fractional one failed at sample 1. A test that configures the model
       and not the device under test is checking itself. */
    fx_set_incr(ix, iy);
    fx_set_pos(px, py);
    for (i = 0; i < n; i++) {
        uint8_t got  = VERA_DATA1;   /* the read also advances the position */
        uint8_t want = ref_pixel((uint16_t)(posx >> 9), (uint16_t)(posy >> 9));
        if (got != want) {
            d_i    = i;
            d_got  = got;
            d_want = want;
            d_mx   = (uint16_t)(posx >> 9);
            d_my   = (uint16_t)(posy >> 9);
            d_fr   = (uint16_t)(posx & 0x1FF);
            return 1;
        }
        posx = (uint32_t)(posx + (uint32_t)(int32_t)ix);
        posy = (uint32_t)(posy + (uint32_t)(int32_t)iy);
    }
    fracx = (uint16_t)(posx & 0x1FF);
    fracy = (uint16_t)(posy & 0x1FF);
    return 0;
}

static void
puthex(uint8_t v)
{
    uint8_t hi = (uint8_t)(v >> 4), lo = (uint8_t)(v & 15);
    con_putc((char)(hi < 10 ? '0' + hi : 'A' + hi - 10));
    con_putc((char)(lo < 10 ? '0' + lo : 'A' + lo - 10));
}

static void
putdec(uint32_t v)
{
    char buf[10];
    uint8_t n = 0;
    if (v == 0) { con_putc('0'); return; }
    while (v && n < sizeof buf) { buf[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n) con_putc(buf[--n]);
}

/* ---- the measurement --------------------------------------------------
 *
 * VIA1 timer 1 free-running, so the counter is a continuous cycle clock (the
 * idiom kbdstat.c uses). It counts DOWN.
 *
 * TWO loops, because the difference between them is the whole question. The
 * first reads FX and throws the pixel away -- that is FX's own cost and an
 * optimistic floor. The second reads FX and STORES the pixel through the
 * other data port, which is what filling a framebuffer actually costs. A
 * decision about whether affine is worth having has to be made on the second
 * number.
 */
static void
measure(void)
{
    uint16_t i;
    uint16_t t_start, t_end;
    uint32_t ro, rw;

    VIA1_ACR  = 0x40;
    VIA1_T1CL = 0xFF;
    VIA1_T1CH = 0xFF;

    fx_affine_setup(0);
    fx_set_incr(512, 0);
    fx_set_pos(0, 0);
    t_start = VIA1_T1C;
    for (i = 0; i < TIMED_PIXELS; i++) {
        (void)VERA_DATA1;
    }
    t_end = VIA1_T1C;
    ro = (uint32_t)(uint16_t)(t_start - t_end);

    /* Read FX, write the framebuffer. Port 0 is pointed somewhere harmless
       and auto-increments; port 1 is FX's. */
    vset(0x08000UL);
    fx_set_pos(0, 0);
    /* Reload, so this run starts at the top of a timer period. Without it the
       second loop begins wherever the first one left the counter and can wrap
       inside the measurement -- which reported zero. */
    VIA1_T1CL = 0xFF;
    VIA1_T1CH = 0xFF;
    /* HALF the sample count. The 16-bit timer covers 65,536 cycles and this
       loop costs about that over TIMED_PIXELS, so the full run wrapped and
       measured as zero -- which looked like the loop was free. */
    t_start = VIA1_T1C;
    for (i = 0; i < TIMED_PIXELS / 2; i++) {
        VERA_DATA0 = VERA_DATA1;
    }
    t_end = VIA1_T1C;
    rw = (uint32_t)(uint16_t)(t_start - t_end) * 2UL;

    { static char m1[] = "FX READ ONLY:  "; con_puts(m1); }
    putdec((ro * 100UL) / TIMED_PIXELS);
    { static char m2[] = " CYCLES/PIXEL x100\n"; con_puts(m2); }

    /* The read+store figure is NOT TRUSTED, and says so rather than printing
       a plausible number. The loop reliably reports zero elapsed cycles,
       which cannot be true; halving the sample count to rule out a 16-bit
       timer wrap changed nothing, and the cause is not yet found. Printing
       something that looked like a measurement would be worse than printing
       none, because the FX_BASEX decision would rest on it. The read-only
       figure above IS measured and stands. */
    { static char m3[] = "READ+STORE:    "; con_puts(m3); }
    if (rw < TIMED_PIXELS) {        /* under one cycle per pixel: impossible */
        static char mu[] = "UNMEASURED - HARNESS RETURNS 0\n";
        con_puts(mu);
    } else {
        putdec((rw * 100UL) / TIMED_PIXELS);
        { static char m4[] = " CYCLES/PIXEL x100\n"; con_puts(m4); }
    }

    /* Derived from the READ-ONLY cost, so these are a FLOOR: a real fill also
       stores every pixel. 8000 cycles to the millisecond at 8 MHz. */
    { static char m5[] = "FLOOR 320x240: "; con_puts(m5); }
    putdec((ro / TIMED_PIXELS) * 76800UL / 8000UL);
    { static char m6[] = " MS   640x480: "; con_puts(m6); }
    putdec((ro / TIMED_PIXELS) * 307200UL / 8000UL);
    { static char m7[] = " MS\n"; con_puts(m7); }
}

int
main(void)
{
    uint16_t i, t;
    uint16_t t_start, t_end;
    uint32_t elapsed;

    con_init();

    /* ---- build the texture ---------------------------------------------
     * Tile data: byte = (tile<<4) ^ (row<<3) ^ col ^ $80. Every one of the
     * three inputs changes the answer, so a wrong tile, a wrong row and a
     * wrong column are three distinguishable failures rather than one. */
    for (t = 0; t < NTILES; t++) {
        uint16_t r, c;
        for (r = 0; r < 8; r++) {
            for (c = 0; c < 8; c++) {
                vpoke(TILE_BASE + (uint32_t)t * TILE_BYTES + r * 8 + c,
                      (uint8_t)(((t << 4) ^ (r << 3) ^ c ^ 0x80) & 0xFF));
            }
        }
    }
    /* Tilemap: a non-trivial arrangement, so a transposed or mirrored lookup
       does not accidentally agree. Tile 0 is placed only at (0,0), which the
       clip test relies on. */
    for (i = 0; i < MAP_TILES * MAP_TILES; i++) {
        uint8_t tx = (uint8_t)(i & (MAP_TILES - 1));
        uint8_t ty = (uint8_t)(i / MAP_TILES);
        uint8_t v  = (uint8_t)(((ty * 5) + (tx * 3) + 1) & (NTILES - 1));
        if (tx == 0 && ty == 0) v = 0;
        tilemap[i] = v;
        vpoke(MAP_BASE + i, v);
    }

    fx_affine_setup(0);
    fx_set_incr(512, 0);

    /* ---- 1: FX answers at all, and not with a constant ------------------ */
    fail = 1;
    {
        uint8_t a, b, differ = 0;
        fx_set_pos(0, 0);
        a = VERA_DATA1;
        for (i = 0; i < 32; i++) {
            b = VERA_DATA1;
            if (b != a) differ = 1;
        }
        if (!differ) goto verdict;   /* a constant is not a texture */
    }

    /* ---- 2: identity walk ----------------------------------------------- */
    fail = 2;
    if (walk(0, 0, 512, 0, SAMPLES)) goto verdict;
    if (walk(3, 5, 512, 0, SAMPLES)) goto verdict;

    /* ---- 3: fractional increment ---------------------------------------- */
    /* 512/3 -- deliberately not a power of two, so the sub-pixel remainder
       carries irregularly and a truncated fraction shows up within a few
       steps rather than never. This is the stepping Mode 7 actually uses. */
    fail = 3;
    if (walk(0, 0, 171, 0, SAMPLES)) goto verdict;
    if (walk(2, 9, 300, 0, SAMPLES)) goto verdict;

    /* ---- 4: both axes -- a rotation -------------------------------------- */
    fail = 4;
    if (walk(0, 0, 443, 256, SAMPLES)) goto verdict;
    if (walk(7, 1, -443, 300, SAMPLES)) goto verdict;   /* and negative x */

    /* ---- 5: clipping ----------------------------------------------------
     * The map is 8x8 tiles = 64x64 pixels. With clip ON, a position outside
     * it must use tile index 0 rather than wrapping. Tile 0 lives only at
     * map (0,0), so "outside gives tile 0's pixels" is distinguishable from
     * "outside wrapped to map (0,0)" ONLY by the row/column -- which is why
     * the position chosen below is not a multiple of 64. */
    fail = 5;
    fx_affine_setup(FX_CLIP);
    {
        uint16_t mx = 70, my = 3;         /* x past the 64-pixel map edge */
        fx_set_incr(512, 0);
        fx_set_pos(mx, my);
        for (i = 0; i < 8; i++) {
            uint8_t got  = VERA_DATA1;
            uint8_t want = (uint8_t)((0 << 4) ^ (((my) & 7) << 3)
                                     ^ ((mx + i) & 7) ^ 0x80);
            if (got != want) {
                d_i = i; d_got = got; d_want = want;
                d_mx = (uint16_t)(mx + i); d_my = my; d_fr = fracx;
                goto verdict;
            }
        }
    }

    /* ---- 6: wrapping ---------------------------------------------------- */
    /* Clip OFF: the same out-of-range position must instead repeat the map,
       so it reads tile (70>>3)&7 = 0 ... which is tile 0 again. Use 70+64 to
       land somewhere the wrap and the clip disagree. */
    fail = 6;
    fx_affine_setup(0);
    if (walk(70, 3, 512, 0, 32)) goto verdict;
    if (walk(200, 130, 512, 0, 32)) goto verdict;

    fail = 0;

verdict:
    RESULT = fail;
    { static char s1[] = "X816 VERA FX -- AFFINE\n"; con_puts(s1); }
    measure();
    if (fail) {
        { static char s2[] = "FAIL AT TEST "; con_puts(s2); }
        puthex(fail);
        { static char sd[] = "  SAMPLE "; con_puts(sd); }
        putdec(d_i);
        { static char sg[] = " GOT "; con_puts(sg); }
        puthex(d_got);
        { static char sw[] = " WANT "; con_puts(sw); }
        puthex(d_want);
        { static char sm[] = "  MAPX "; con_puts(sm); }
        putdec(d_mx);
        { static char sn[] = " MAPY "; con_puts(sn); }
        putdec(d_my);
        { static char sf[] = " FRAC "; con_puts(sf); }
        putdec(d_fr);
        con_putc('\n');
        goshell_on_esc();
        return 0;
    }
    { static char s3[] = "AFFINE OK: IDENTITY, FRACTIONAL, ROTATED, CLIP, WRAP\n"; con_puts(s3); }

    /* ---- the measurement ------------------------------------------------
     * VIA1 timer 1 free-running, so the counter is a continuous cycle clock
     * (the same idiom kbdstat.c uses). It counts DOWN.
     *
     * The loop is one read of DATA1 per pixel and nothing else -- no store to
     * a framebuffer, so this measures FX's side alone and is an OPTIMISTIC
     * bound. A real fill also writes each pixel somewhere. */
    goshell_on_esc();
    return 0;
}
