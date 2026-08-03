/* ==========================================================================
 * ctest.c -- runtime conformance test for the C wrapper.
 *
 *   GREEN screen  = every test passed
 *   RED           = test 1, one char in / one char out
 *   YELLOW        = test 2, no-argument call and a 16-bit return
 *   BLUE          = test 3, two arguments (the second one via the stack)
 *   MAGENTA       = test 4, direct-page arguments
 *   CYAN          = test 5, register width preserved across a call
 *   (test 6, a VERA-side module through the glue, also paints CYAN on
 *    failure -- RESULT at $0400 tells 5 and 6 apart)
 *
 * The companion to examples/asm-lib/libtest.s. That one proves the converted
 * library runs; this one proves C can call it. Everything checked here is
 * something the glue could get wrong while still compiling and linking:
 * jsl/rtl against jsr/rts, accumulator and index width, which register a
 * parameter arrives in, and whether the second argument is read from the
 * right stack offset.
 *
 * Expected values come from the same formula that generated the library's
 * table, not from the library itself, so agreeing with it means something.
 * ========================================================================== */

#include "x816.h"

#define VERA_ADDR_L    (*(volatile unsigned char *)0x9F20)
#define VERA_ADDR_M    (*(volatile unsigned char *)0x9F21)
#define VERA_ADDR_H    (*(volatile unsigned char *)0x9F22)
#define VERA_DATA0     (*(volatile unsigned char *)0x9F23)
#define VERA_CTRL      (*(volatile unsigned char *)0x9F25)
#define VERA_DC_VIDEO  (*(volatile unsigned char *)0x9F29)
#define VERA_DC_HSCALE (*(volatile unsigned char *)0x9F2A)
#define VERA_DC_VSCALE (*(volatile unsigned char *)0x9F2B)
#define VERA_L0_CONFIG (*(volatile unsigned char *)0x9F2D)
#define VERA_L0_TILEB  (*(volatile unsigned char *)0x9F2F)

#define RESULT         (*(volatile unsigned char *)0x0400)

static void paint(unsigned char colour)
{
    unsigned int x, y;

    VERA_CTRL      = 0;
    VERA_DC_VIDEO  = 0x11;      /* VGA output + layer 0 enable */
    VERA_DC_HSCALE = 0x40;      /* half scale -> 320x240 active */
    VERA_DC_VSCALE = 0x40;
    VERA_L0_CONFIG = 0x07;      /* bitmap mode, 8bpp */
    VERA_L0_TILEB  = 0;         /* bitmap base 0, 320 wide */

    VERA_CTRL   = 0;
    VERA_ADDR_L = 0;
    VERA_ADDR_M = 0;
    VERA_ADDR_H = 0x10;         /* increment 1, addr[19:16] = 0 */

    for (y = 0; y < 240; y++)
        for (x = 0; x < 320; x++)
            VERA_DATA0 = colour;
}

int main(void)
{
    unsigned char fail = 0;
    unsigned int r1, r2;
    unsigned char i;

    /* ---- 1: one char in, one char out --------------------------------- */
    /* Four points around the circle, including both extremes. sin8 is signed,
     * sin8u is the same value biased by 128 -- checking both catches a stub
     * that returns the right byte with the wrong sign handling. */
    if (x816_sin8(0) != 0 || x816_sin8(64) != 127 ||
        x816_sin8(128) != 0 || x816_sin8(192) != -127 ||
        x816_cos8(0) != 127 || x816_sin8u(64) != 255 || x816_sin8u(192) != 1)
        fail = 1;

    /* ---- 2: no argument, and a 16-bit return --------------------------- */
    /* A xorshift generator never repeats immediately and never returns zero,
     * so two successive draws differing (and both non-zero) means the call
     * really ran. Seeding first makes it deterministic. rnd16 also exercises
     * the 16-bit return path, where the stub folds X into the high half. */
    if (!fail) {
        x816_rnd_seed(0x1234);
        r1 = x816_rnd16();
        r2 = x816_rnd16();
        if (r1 == 0 || r2 == 0 || r1 == r2)
            fail = 2;
    }

    /* ---- 3: two arguments, the second one via the stack ---------------- */
    /* atan2 takes dx in A and dy in X, so the second argument has to be read
     * back off the stack at the right offset. The four axes are exact, and
     * they are all DIFFERENT -- a stub that ignored dy entirely would still
     * get east right, so testing one direction would prove nothing. */
    if (!fail) {
        if (x816_atan2(127, 0) != 0 || x816_atan2(0, 127) != 64 ||
            x816_atan2(-127, 0) != 128 || x816_atan2(0, -127) != 192)
            fail = 3;
    }

    /* ---- 4: direct-page arguments -------------------------------------- */
    /* lerp8 reads its endpoints from X16_P0/X16_P1, written from C. Exact at
     * both ends, so a wrong direct-page address cannot pass by luck. */
    if (!fail) {
        if (x816_lerp8(10, 200, 0) != 10 || x816_lerp8(10, 200, 255) != 200)
            fail = 4;
    }

    /* ---- 5: register width survives the call --------------------------- */
    /* The stubs run the library with 8-bit A/X/Y and must restore 16 bits
     * before returning. If they did not, this loop's 16-bit counter would
     * wrap at 256 and never terminate -- or terminate early with a wrong
     * total. The sum is over a full period, which is 0 for a sine table. */
    if (!fail) {
        unsigned int sum = 0;
        for (i = 0;; i++) {
            sum += (unsigned char)x816_sin8(i);
            if (i == 255)
                break;
        }
        if ((sum & 0xFF) != 0)
            fail = 5;
    }

    /* ---- 6: a VERA-side module through the glue ------------------------ */
    /* pal_set writes entry 100 of the palette shadow at $1FA00; reading the
     * two bytes back through the data port checks index scaling (the *2 with
     * its carry into ADDR_M), the argument order (index in A, colour on the
     * stack), and that the second argument's two halves both arrived. $0ABC
     * splits into $BC (G<<4|B) then $0A (R) -- two different bytes, so a
     * swapped pair cannot pass. */
    if (!fail) {
        unsigned char lo, hi;
        x816_pal_set(100, 0x0ABC);
        VERA_CTRL   = 0;
        VERA_ADDR_L = (unsigned char)(100 * 2);
        VERA_ADDR_M = 0xFA;
        VERA_ADDR_H = 0x11;     /* increment 1, addr bit 16 set */
        lo = VERA_DATA0;
        hi = VERA_DATA0;
        if (lo != 0xBC || hi != 0x0A)
            fail = 6;
    }

    RESULT = fail;
    switch (fail) {
    case 0:  paint(0x05); break;   /* green   */
    case 1:  paint(0x02); break;   /* red     */
    case 2:  paint(0x07); break;   /* yellow  */
    case 3:  paint(0x06); break;   /* blue    */
    case 4:  paint(0x04); break;   /* magenta */
    default: paint(0x03); break;   /* cyan    */
    }

    for (;;)
        ;
}
