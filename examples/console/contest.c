/* ==========================================================================
 * contest.c -- console conformance test.
 *
 * The console is the first thing here a person reads directly, so this test
 * does two things at once: it prints something a human can check on a screen,
 * and it verifies what it wrote by reading VRAM back, so it can also fail
 * automatically in the emulator.
 *
 * Reading VRAM back is the point. "It printed something" is not a test --
 * every one of these cases can produce plausible-looking output while being
 * wrong in a way that only shows up later, when the shell tries to line
 * something up in a column.
 *
 *   GREEN    every test passed
 *   RED      test 1: a character did not land where it was addressed
 *   YELLOW   test 2: cls did not clear, or did not home the cursor
 *   BLUE     test 3: the cursor did not wrap at the right margin
 *   MAGENTA  test 4: newline / carriage return / backspace
 *   CYAN     test 5: scrolling lost or misplaced a line
 *   WHITE    test 6: a code did not land as its own glyph (CP437 is
 *            unfiltered: only \n, \r and \b are intercepted)
 *
 * On failure the screen is painted flat, so the colour is unmistakable even
 * though the console itself is what is being tested.
 * ========================================================================== */

#include "console.h"

#define VERA_ADDR_L     (*(volatile unsigned char *)0x9F20)
#define VERA_ADDR_M     (*(volatile unsigned char *)0x9F21)
#define VERA_ADDR_H     (*(volatile unsigned char *)0x9F22)
#define VERA_DATA0      (*(volatile unsigned char *)0x9F23)
#define VERA_CTRL       (*(volatile unsigned char *)0x9F25)
#define VERA_DC_VIDEO   (*(volatile unsigned char *)0x9F29)
#define VERA_DC_HSCALE  (*(volatile unsigned char *)0x9F2A)
#define VERA_DC_VSCALE  (*(volatile unsigned char *)0x9F2B)
#define VERA_L0_CONFIG  (*(volatile unsigned char *)0x9F2D)
#define VERA_L0_TILEB   (*(volatile unsigned char *)0x9F2F)

#define RESULT (*(volatile unsigned char *)0x0400)

/* Read the character cell at (x, y) straight out of VRAM. */
static unsigned char
cell(unsigned char x, unsigned char y)
{
    VERA_CTRL   = 0;
    VERA_ADDR_L = (unsigned char)(x << 1);
    VERA_ADDR_M = y;
    VERA_ADDR_H = 0x10;
    return VERA_DATA0;
}

static void
paint(unsigned char colour)
{
    unsigned int x, y;
    VERA_CTRL      = 0;
    VERA_DC_VIDEO  = 0x11;
    VERA_DC_HSCALE = 0x40;      /* back to 320x240 for a flat fill */
    VERA_DC_VSCALE = 0x40;
    VERA_L0_CONFIG = 0x07;      /* bitmap, 8bpp */
    VERA_L0_TILEB  = 0;
    VERA_CTRL   = 0;
    VERA_ADDR_L = 0;
    VERA_ADDR_M = 0;
    VERA_ADDR_H = 0x10;
    for (y = 0; y < 240; y++)
        for (x = 0; x < 320; x++)
            VERA_DATA0 = colour;
}

int
main(void)
{
    unsigned char fail = 0;
    unsigned char i;

    con_init();

    /* ---- 1: a character lands where it was addressed ------------------- */
    con_gotoxy(5, 3);
    con_putc('A');
    if (cell(5, 3) != 'A' || con_getx() != 6 || con_gety() != 3)
        fail = 1;

    /* ---- 2: cls clears and homes -------------------------------------- */
    /* Check a cell far from the origin: a cls that only clears the first row
       would pass a check at (0,0). */
    if (!fail) {
        con_cls();
        if (cell(5, 3) != ' ' || cell(79, 59) != ' '
            || con_getx() != 0 || con_gety() != 0)
            fail = 2;
    }

    /* ---- 3: the cursor wraps at the right margin ----------------------- */
    if (!fail) {
        con_gotoxy(CON_COLS - 1, 10);
        con_putc('Z');
        if (cell(CON_COLS - 1, 10) != 'Z' || con_getx() != 0
            || con_gety() != 11)
            fail = 3;
    }

    /* ---- 4: newline, carriage return, backspace ------------------------ */
    if (!fail) {
        con_gotoxy(4, 20);
        con_putc(0x0A);                         /* \n */
        if (con_getx() != 0 || con_gety() != 21) fail = 4;
        con_gotoxy(4, 22);
        con_putc(0x0D);                         /* \r */
        if (!fail && (con_getx() != 0 || con_gety() != 22)) fail = 4;
        con_gotoxy(4, 23);
        con_putc(0x08);                         /* \b */
        if (!fail && con_getx() != 3) fail = 4;
        con_gotoxy(0, 23);
        con_putc(0x08);                         /* \b at column 0 must hold */
        if (!fail && con_getx() != 0) fail = 4;
    }

    /* ---- 5: scrolling -------------------------------------------------- */
    /* Mark two rows, scroll once, and check BOTH moved up. Checking one row
       would pass for a scroll that copied a single line. */
    if (!fail) {
        con_cls();
        con_gotoxy(0, CON_ROWS - 2); con_putc('P');
        con_gotoxy(0, CON_ROWS - 1); con_putc('Q');
        con_gotoxy(0, CON_ROWS - 1);
        con_putc(0x0A);                         /* forces a scroll */
        if (cell(0, CON_ROWS - 3) != 'P' || cell(0, CON_ROWS - 2) != 'Q'
            || cell(0, CON_ROWS - 1) != ' '
            || con_getx() != 0 || con_gety() != CON_ROWS - 1)
            fail = 5;
    }

    /* ---- 6: every code lands as its own glyph --------------------------- */
    /* CP437: all 256 codes have a glyph now and con_putc intercepts only \n,
       \r and \b, so $7F and $01 must land as tile indices $7F and $01. (The
       old 64-glyph console filtered both to spaces, and this test asserted
       that; it was updated when the filtering was removed on purpose.) */
    if (!fail) {
        con_cls();
        con_gotoxy(0, 0);
        con_putc((char)0x7F);
        con_putc((char)0x01);
        if (cell(0, 0) != 0x7F || cell(1, 0) != 0x01)
            fail = 6;
    }

    RESULT = fail;

    if (fail == 0) {
        /* Leave something readable on screen: this test is also the first
           demonstration that the console works at all. */
        /* static char[], not string literals. A literal lands in `cdata`,
           which the linker places with the code in bank $01, and a near read
           from bank $00 cannot reach it -- the link fails outright with
           "_StringLiteral_... out of range". Non-const arrays go to `data`,
           whose initialiser rides in the image and is copied into bank $00 at
           startup. See the README. */
        static char l1[] = "X816 CONSOLE OK\n";
        static char l2[] = "80X60 TEXT, VERA TILE MODE, SMC KEYBOARD\n\n";
        static char l3[] = "ALL SIX CONSOLE TESTS PASSED.\n";
        con_cls();
        con_puts(l1);
        con_puts(l2);
        con_puts(l3);
        /* And STOP here -- no flat green.
         *
         * Painting a success colour would erase the one thing a person can
         * actually check on hardware: that the glyphs render. Every other
         * test in this project signals pass with a flat colour because it has
         * nothing to show; this one does. So PASS is "text on screen" and
         * FAIL is a flat colour, which an automated check tells apart by
         * whether one colour covers the whole frame. */
        for (;;)
            ;
    }

    switch (fail) {
    case 1:  paint(0x02); break;   /* red     */
    case 2:  paint(0x07); break;   /* yellow  */
    case 3:  paint(0x06); break;   /* blue    */
    case 4:  paint(0x04); break;   /* magenta */
    case 5:  paint(0x03); break;   /* cyan    */
    default: paint(0x01); break;   /* white   */
    }
    for (;;)
        ;
    (void)i;
}
