/* ==========================================================================
 * curtest.c -- the console cursor: does it blink, follow, and stay out of the
 *              way?
 *
 * The cursor is the first thing built on IRQ_SET, and it is the first thing
 * in the tree that touches VERA from inside an interrupt. That second part is
 * what this test is really about. doc/KERNEL.md §5.6 says the dispatcher never
 * goes near VERA's address registers because an interrupt can land between a
 * console write setting the address and the store that uses it -- and a cursor
 * has to break that rule to draw at all. runtime/ccursor.s makes itself
 * transparent instead (port 1, with CTRL and the port-1 address saved and
 * restored); check 5 is what says it worked.
 *
 *   GREEN    every check passed
 *   RED      1: the cursor cell's attribute takes BOTH values over time --
 *              it actually blinks, rather than being set once
 *   YELLOW   2: the GLYPH under the cursor never changes while it blinks
 *   BLUE     3: it FOLLOWS -- printing a character settles the old cell back
 *              to normal and starts blinking the new one
 *   MAGENTA  4: ccur_off() leaves the cell normal and it stops changing
 *   CYAN     5: text printed WHILE the cursor is blinking is not corrupted --
 *              the property that pays for all the save/restore in ccur_put
 *
 * The number also lands at $00:0400.
 *
 * Everything is read back out of VRAM through the CPU's own port 0. That is
 * deliberate: the cursor uses port 1, so if these reads ever came back wrong
 * it would mean the handler had disturbed a port it does not own -- which is
 * the same failure check 5 looks for from the other direction.
 * ========================================================================== */

#include "kernel.h"
#include "console.h"
#include "goshell.h"

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

#define ATTR_NORMAL 0x01
#define ATTR_CURSOR 0x10

void kirq_install(void);

/* A cell is at y*256 + x*2 -- the map is 128 wide so there is no multiply.
   The glyph is the first byte, the attribute the second. */
static unsigned int
cell_addr(unsigned char x, unsigned char y)
{
    return ((unsigned int)y << 8) | ((unsigned int)x << 1);
}

static unsigned char
vram(unsigned int a, unsigned char off)
{
    unsigned int addr = a + off;
    VERA_CTRL   = 0;                 /* port 0 -- the cursor owns port 1 */
    VERA_ADDR_L = (unsigned char)(addr & 0xFF);
    VERA_ADDR_M = (unsigned char)(addr >> 8);
    VERA_ADDR_H = 0x10;
    return VERA_DATA0;
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

static void
fail(unsigned char n, unsigned char colour)
{
    RESULT = n;
    paint(colour);
    for (;;) { }
}

static unsigned int
frames(void)
{
    return kern_call(K_IRQ_FRAMES);
}

static int
wait_frames(unsigned int n)
{
    unsigned int  start = frames();
    unsigned long guard = 0;
    while ((unsigned int)(frames() - start) < n)
        if (++guard > 1000000UL)
            return 0;
    return 1;
}

/* Sample one cell's attribute for `n` frames and report which values it took.
   Returns bit 0 for "seen normal" and bit 1 for "seen reversed". Sampling in
   a tight loop rather than once per frame is what makes a half-second blink
   observable without waiting for a specific phase. */
static unsigned char
sample(unsigned int addr, unsigned int n)
{
    unsigned int  start = frames();
    unsigned char seen  = 0;
    unsigned long guard = 0;
    while ((unsigned int)(frames() - start) < n) {
        unsigned char a = vram(addr, 1);
        if (a == ATTR_NORMAL) seen |= 1;
        if (a == ATTR_CURSOR) seen |= 2;
        if (++guard > 2000000UL) break;
    }
    return seen;
}

int
main(void)
{
    unsigned int  home, moved;
    unsigned char glyph, i;
    static char   probe[] = "CURSOR";

    con_init();
    kern_install();
    kirq_install();

    /* Put something under the cursor: a cell that has never been written has
       no defined glyph, and check 2 would then be asserting about noise. */
    con_puts(probe);
    home  = cell_addr(con_curx, con_cury);
    glyph = vram(home, 0);

    ccur_on();

    /* ---- 1: it blinks --------------------------------------------------- */
    /* Both values, not just one. A cursor drawn once and never undrawn passes
       "the attribute is reversed" and is not a cursor. */
    if (!wait_frames(2))
        fail(1, 0x02);
    if (sample(home, 90) != 3)
        fail(1, 0x02);

    /* ---- 2: the glyph underneath survives -------------------------------- */
    if (vram(home, 0) != glyph)
        fail(2, 0x07);

    /* ---- 3: it follows the console's cursor ------------------------------ */
    con_putc('X');
    moved = cell_addr(con_curx, con_cury);
    if (moved == home)
        fail(3, 0x06);            /* the test itself must have moved it */
    if (!wait_frames(4))
        fail(3, 0x06);
    /* The cell it left must settle to normal and STAY there... */
    if (sample(home, 70) != 1)
        fail(3, 0x06);
    /* ...and the new one must be blinking. */
    if (sample(moved, 90) != 3)
        fail(3, 0x06);

    /* ---- 4: ccur_off leaves no trace ------------------------------------- */
    ccur_off();
    if (!wait_frames(2))
        fail(4, 0x04);
    if (sample(moved, 70) != 1)
        fail(4, 0x04);

    /* ---- 5: the console is not corrupted while the cursor blinks ---------- */
    /* The one that pays for ccur_put's save/restore. Printing drives VERA's
       port 0 a byte at a time; the handler fires ~60 times a second in the
       middle of that and drives port 1. If it disturbed CTRL or port 0's
       address, characters would land in the wrong cells -- rarely, and only
       while the cursor was on, which is the worst way for this to fail. */
    ccur_on();
    con_putc('\n');
    home = cell_addr(con_curx, con_cury);
    for (i = 0; i < 6; i++)
        con_putc(probe[i]);
    if (!wait_frames(30))         /* let the handler run over the top of it */
        fail(5, 0x03);
    ccur_off();
    if (!wait_frames(2))
        fail(5, 0x03);
    for (i = 0; i < 6; i++) {
        if (vram(home + ((unsigned int)i << 1), 0) != (unsigned char)probe[i])
            fail(5, 0x03);
        if (vram(home + ((unsigned int)i << 1), 1) != ATTR_NORMAL)
            fail(5, 0x03);
    }

    RESULT = 0;
    paint(0x05);
    for (;;) { }
    return 0;
}
