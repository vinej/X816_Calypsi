/* ==========================================================================
 * shtest.c -- shell conformance test.
 *
 * Drives the tokeniser and the dispatcher with canned lines, so everything
 * except the keyboard is tested without one. That is the whole reason
 * sh_exec() takes a line rather than reading it itself: an interactive
 * function is an untestable function.
 *
 *   GREEN    every test passed
 *   RED      test 1: tokeniser
 *   YELLOW   test 2: argument-count checking
 *   BLUE     test 3: hex parsing
 *   MAGENTA  test 4: peek / poke / fill reaching flat memory
 *   CYAN     test 5: move, including overlap
 *   WHITE    test 6: unknown command, or a blank line treated as an error
 * ========================================================================== */

#include "shell.h"
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

/* Scratch well clear of this program, which loads at $01:0000. */
#define SCRATCH 0x030000UL

static unsigned char __far *far_ptr(unsigned long a)
{
    return (unsigned char __far *)a;
}

static void
paint(unsigned char colour)
{
    unsigned int x, y;
    VERA_CTRL      = 0;
    VERA_DC_VIDEO  = 0x11;
    VERA_DC_HSCALE = 0x40;
    VERA_DC_VSCALE = 0x40;
    VERA_L0_CONFIG = 0x07;
    VERA_L0_TILEB  = 0;
    VERA_CTRL   = 0;
    VERA_ADDR_L = 0;
    VERA_ADDR_M = 0;
    VERA_ADDR_H = 0x10;
    for (y = 0; y < 240; y++)
        for (x = 0; x < 320; x++)
            VERA_DATA0 = colour;
}

/* Cell contents straight out of VRAM, to check what the shell printed. */
static unsigned char
cell(unsigned char x, unsigned char y)
{
    VERA_CTRL   = 0;
    VERA_ADDR_L = (unsigned char)(x << 1);
    VERA_ADDR_M = y;
    VERA_ADDR_H = 0x10;
    return VERA_DATA0;
}

static int
same(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == '\0' && *b == '\0';
}

int
main(void)
{
    unsigned char fail = 0;
    char  *argv[SH_MAX_ARGS];
    unsigned char n;
    unsigned long v;

    con_init();

    /* ---- 1: the tokeniser ---------------------------------------------- */
    /* Leading, repeated and trailing spaces all collapse; a line of nothing
       but spaces is zero words, not one empty one. Those are exactly the
       cases a naive split-on-space gets wrong. */
    if (!fail) {
        static char l1[] = "  dump   01:0000    40  ";
        static char w0[] = "dump", w1[] = "01:0000", w2[] = "40";
        n = sh_tokenise(l1, argv);
        if (n != 3 || !same(argv[0], w0) || !same(argv[1], w1)
            || !same(argv[2], w2))
            fail = 1;
    }
    if (!fail) {
        static char l2[] = "     ";
        if (sh_tokenise(l2, argv) != 0)
            fail = 1;
    }
    if (!fail) {
        static char l3[] = "a b c d e f g h i j";      /* > SH_MAX_ARGS */
        if (sh_tokenise(l3, argv) != SH_TOO_MANY_ARGS)
            fail = 1;
    }

    /* ---- 2: argument counts -------------------------------------------- */
    /* A command called with the wrong number of arguments must be REFUSED,
       not run with whatever happens to be in argv. */
    if (!fail) {
        static char bad[]  = "poke 030000";            /* needs 2 */
        static char good[] = "poke 030000 5A";
        *far_ptr(SCRATCH) = 0x11;
        sh_exec(bad);
        if (*far_ptr(SCRATCH) != 0x11)                 /* must not have run */
            fail = 2;
        if (!fail) {
            sh_exec(good);
            if (*far_ptr(SCRATCH) != 0x5A)
                fail = 2;
        }
    }

    /* ---- 3: hex parsing ------------------------------------------------ */
    if (!fail) {
        static char h1[] = "01:0000", h2[] = "ff", h3[] = "12G4";
        static char h4[] = "", h5[] = "1234567";
        if (!sh_parse_hex(h1, &v) || v != 0x010000UL) fail = 3;
        if (!fail && (!sh_parse_hex(h2, &v) || v != 0xFF)) fail = 3;
        if (!fail && sh_parse_hex(h3, &v))  fail = 3;   /* not hex */
        if (!fail && sh_parse_hex(h4, &v))  fail = 3;   /* empty */
        if (!fail && sh_parse_hex(h5, &v))  fail = 3;   /* > 24 bits */
    }

    /* ---- 4: peek/poke/fill reach flat memory --------------------------- */
    /* Bank $03 is well outside bank $00, so this only passes if the far
       pointers really are 24-bit. A near pointer would wrap into bank $00 and
       quietly corrupt the machine instead. */
    if (!fail) {
        static char f[] = "fill 030100 10 A5";
        sh_exec(f);
        if (*far_ptr(SCRATCH + 0x100) != 0xA5
            || *far_ptr(SCRATCH + 0x10F) != 0xA5
            || *far_ptr(SCRATCH + 0x110) == 0xA5)      /* must stop at len */
            fail = 4;
    }

    /* ---- 5: move, including overlap ------------------------------------ */
    /* Overlapping forwards is where a naive byte loop smears the first byte
       across the whole range. */
    if (!fail) {
        static char m[] = "move 030204 030200 8";
        unsigned char i;
        for (i = 0; i < 8; i++)
            *far_ptr(SCRATCH + 0x200 + i) = (unsigned char)(i + 1);
        sh_exec(m);
        for (i = 0; i < 8; i++) {
            if (*far_ptr(SCRATCH + 0x204 + i) != (unsigned char)(i + 1)) {
                fail = 5;
                break;
            }
        }
    }

    /* ---- 6: unknown command, and a blank line -------------------------- */
    /* A blank line must be silently ignored. Reporting "?" for pressing enter
       makes a prompt unusable. */
    if (!fail) {
        static char unknown[] = "frobnicate";
        static char blank[]   = "   ";
        con_cls();
        sh_exec(unknown);
        if (cell(0, 0) != 'F')                 /* it echoes the name, then ? */
            fail = 6;
        if (!fail) {
            con_cls();
            sh_exec(blank);
            if (cell(0, 0) != ' ' || con_gety() != 0)
                fail = 6;
        }
    }

    RESULT = fail;
    if (fail == 0) {
        static char l1[] = "X816 SHELL OK\n";
        static char l2[] = "TOKENISER, DISPATCH, HEX, FAR MEMORY, MOVE\n\n";
        static char l3[] = "ALL SIX SHELL TESTS PASSED.\n";
        con_cls();
        con_puts(l1);
        con_puts(l2);
        con_puts(l3);
        for (;;)
            ;
    }

    switch (fail) {
    case 1:  paint(0x02); break;
    case 2:  paint(0x07); break;
    case 3:  paint(0x06); break;
    case 4:  paint(0x04); break;
    case 5:  paint(0x03); break;
    default: paint(0x01); break;
    }
    for (;;)
        ;
}
