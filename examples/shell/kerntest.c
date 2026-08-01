/* ==========================================================================
 * kerntest.c -- kernel jump table conformance.
 *
 * Everything here goes through $00:FE00 by entry NUMBER. Nothing calls a
 * console function directly, because a test that called con_putc would prove
 * con_putc works -- which was never in doubt. What is in doubt is the table:
 * whether the entries were stamped out correctly, whether the thunks marshal
 * the ABI, and whether carry reports the way every caller will rely on.
 *
 *   GREEN    every test passed
 *   RED      test 1: the table was not installed, or SYS_VERSION is wrong
 *   YELLOW   test 2: CON_PUTC / CON_PUTS did not reach the screen
 *   BLUE     test 3: CON_GOTOXY / CON_GETXY disagree
 *   MAGENTA  test 4: an unimplemented entry did not refuse cleanly
 *   CYAN     test 5: CON_PUTRAW placed the wrong glyph
 *
 * The result also lands at $00:0400.
 * ========================================================================== */

#include "kernel.h"
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

/* Read a screen cell straight out of VRAM, so the check does not depend on
   the same console code being tested. */
static unsigned char
cell(unsigned char x, unsigned char y)
{
    VERA_CTRL   = 0;
    VERA_ADDR_L = (unsigned char)(x << 1);
    VERA_ADDR_M = y;
    VERA_ADDR_H = 0x10;
    return VERA_DATA0;
}

int
main(void)
{
    unsigned char fail = 0;
    unsigned int  r;

    con_init();
    kern_install();

    /* ---- 1: the table exists and answers -------------------------------- */
    /* SYS_VERSION is the cheapest possible probe: pure constant, no side
       effect, and it fails if the table was never stamped -- an uninstalled
       page is whatever bank $00 held, which will not return 0x0001 with carry
       clear. */
    if (!fail) {
        r = kern_call(K_SYS_VERSION);
        if (kern_carry != 0 || r != 0x0001)
            fail = 1;
    }

    /* ---- 2: characters and strings reach the screen --------------------- */
    if (!fail) {
        static char msg[] = "KERNEL";
        kern_call(K_CON_CLS);
        kern_c = 'X';
        kern_call(K_CON_PUTC);
        if (cell(0, 0) != 'X')
            fail = 2;
        /* CON_PUTS takes C:X as a 24-bit pointer. The string is in bank $00
           (it is `static char[]`, so `data`), which makes the bank byte zero
           -- and passing it explicitly is the point: the thunk has to accept
           a bank at all. */
        if (!fail) {
            kern_c = (unsigned int)(unsigned long)msg;
            kern_x = 0;   /* bank */
            kern_call(K_CON_PUTS);
            if (cell(1, 0) != 'K' || cell(6, 0) != 'L')
                fail = 2;
        }
    }

    /* ---- 3: the cursor round-trips -------------------------------------- */
    /* Two arguments out and two back, which is the case where a marshalling
       mistake shows up: swap C and X in either thunk and this fails. */
    if (!fail) {
        kern_c = 17; kern_x = 5;
        kern_call(K_CON_GOTOXY);
        r = kern_call(K_CON_GETXY);
        if (kern_carry != 0 || r != 17)
            fail = 3;
    }

    /* ---- 4: an unimplemented entry refuses cleanly ----------------------- */
    /* Entry 63 is the last reserved slot and nothing is planned for it. It must
       come back with carry set and KERR_NOSYS -- not jump into whatever was in
       bank $00. This is the check that makes the reserved numbering safe: every
       slot is filled, so filling one later is not an ABI break.

       It used to probe K_FS_OPEN, which was unimplemented at the time. Then
       FS_OPEN landed and this test went red for the best possible reason, which
       is a bad reason for a test to go red. A slot that is reserved on purpose
       cannot be implemented out from under it. */
    if (!fail) {
        r = kern_call(63);
        if (kern_carry == 0 || r != KERR_NOSYS)
            fail = 4;
    }

    /* ---- 5: three arguments ---------------------------------------------- */
    /* CON_PUTRAW takes C, X and Y. Three is where the stack marshalling gets
       interesting, since only the first argument travels in a register. */
    if (!fail) {
        kern_c = 3; kern_x = 7; kern_y = 0xC9;  /* double top-left corner */
        kern_call(K_CON_PUTRAW);
        if (cell(3, 7) != 0xC9)
            fail = 5;
    }

    RESULT = fail;
    if (fail == 0) {
        paint(0x05);            /* green */
    } else {
        switch (fail) {
        case 1:  paint(0x02); break;
        case 2:  paint(0x07); break;
        case 3:  paint(0x06); break;
        case 4:  paint(0x04); break;
        default: paint(0x03); break;
        }
    }
    for (;;)
        ;
}
