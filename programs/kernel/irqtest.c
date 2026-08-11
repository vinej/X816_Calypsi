/* ==========================================================================
 * irqtest.c -- IRQ_SET, the dispatcher, and the two clocks.
 *
 * X816_Core doc/KERNEL.md section 8 test 8. Nine checks, one failure colour
 * each, and the number also lands at $00:0400 for a debugger:
 *
 *   GREEN    every check passed
 *   RED      1: kirq_install really installed -- the four CPU vectors point
 *              at trampolines, and the trampolines are real `jmp long:`
 *   YELLOW   2: IRQ_FRAMES advances on its own, so VSYNC is being dispatched
 *              AND acknowledged (an unacknowledged level IRQ would livelock
 *              instead, and this test would time out rather than fail)
 *   BLUE     3: a handler installed through IRQ_SET actually runs
 *   MAGENTA  4: IRQ_SET reports the PREVIOUS handler, and clearing a slot
 *              stops the handler while the frame count keeps advancing
 *   CYAN     5: BRK dispatches to KIRQ_BRK and execution RESUMES afterwards
 *   ORANGE   6: TIME_GET advances, never goes backwards, and agrees with the
 *              frame counter
 *   BROWN    7: TIME_SET moves the epoch and the clock keeps running from it
 *   GREY     8: IRQ_SET refuses a slot that does not exist, with KERR_BADARG
 *   PINK     9: the stuck-source defence: AFLOW enabled with nothing to
 *              service it gets DISABLED rather than locking the machine
 *
 * WHY CHECK 4 IS THE ONE THAT MATTERS MOST. Check 3 passes for a dispatcher
 * that calls every slot unconditionally, or that ignores the table and calls
 * a hard-wired address. Only "the counter STOPS when the slot is cleared,
 * while frames keep advancing" separates a dispatcher that reads the table
 * from one that merely happens to reach the right code -- and the frame count
 * advancing at the same time is what stops that check being satisfied by
 * interrupts having quietly died.
 *
 * WHY CHECK 6 CROSS-CHECKS TWO CLOCKS. The millisecond counter is hardware
 * this test cannot see inside. Comparing it against the frame count -- an
 * independent timebase, driven by VERA rather than by the divider -- is what
 * makes "the timer works" mean something. A timer stuck at zero, running at
 * the wrong rate, or wired to the wrong register all fail it; a timer checked
 * only against itself passes all three.
 * ========================================================================== */

#include "kernel.h"
#include "console.h"
#include "goshell.h"

#define VERA_ADDR_L     (*(volatile unsigned char *)0x9F20)
#define VERA_ADDR_M     (*(volatile unsigned char *)0x9F21)
#define VERA_ADDR_H     (*(volatile unsigned char *)0x9F22)
#define VERA_DATA0      (*(volatile unsigned char *)0x9F23)
#define VERA_CTRL       (*(volatile unsigned char *)0x9F25)
#define VERA_IEN        (*(volatile unsigned char *)0x9F26)
#define VERA_ISR        (*(volatile unsigned char *)0x9F27)
#define VERA_DC_VIDEO   (*(volatile unsigned char *)0x9F29)
#define VERA_DC_HSCALE  (*(volatile unsigned char *)0x9F2A)
#define VERA_DC_VSCALE  (*(volatile unsigned char *)0x9F2B)
#define VERA_L0_CONFIG  (*(volatile unsigned char *)0x9F2D)
#define VERA_L0_TILEB   (*(volatile unsigned char *)0x9F2F)

#define RESULT (*(volatile unsigned char *)0x0400)

/* runtime/kirq.s */
void kirq_install(void);
extern unsigned int  kirq_frames;
extern unsigned int  kirq_disabled;
extern unsigned char kirq_tramp[];

/* irqhelp.s -- handlers and the address arithmetic C must not guess at */
void irqh_install_vsync(void);
void irqh_install_vsync2(void);
void irqh_clear_vsync(void);
void irqh_install_brk(void);
void irqh_do_brk(void);
void irqh_set_bad_slot(void);
extern unsigned int irqh_vsync_count, irqh_brk_count;
extern unsigned int irqh_prev_lo, irqh_prev_bank;
extern unsigned int irqh_vsync_lo, irqh_vsync_bank;
extern unsigned int irqh_vsync2_lo, irqh_vsync2_bank;
extern unsigned int irqh_bad_carry, irqh_bad_code;

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

/* ---- the two clocks, through the jump table by entry NUMBER -------------- */

static unsigned int
frames(void)
{
    return kern_call(K_IRQ_FRAMES);
}

static unsigned long
ms(void)
{
    unsigned long lo = kern_call(K_TIME_GET);
    return lo | ((unsigned long)kern_x << 16);
}

/* Wait for n frames to pass, bounded. Returns 0 if the frame counter stopped
   advancing -- which is what a dispatcher that never acknowledges VSYNC looks
   like from here, and is worth reporting as a failed CHECK rather than as the
   90-second timeout it would otherwise become. */
static int
wait_frames(unsigned int n)
{
    unsigned int  start = frames();
    unsigned long guard = 0;
    while ((unsigned int)(frames() - start) < n) {
        if (++guard > 1000000UL)
            return 0;
    }
    return 1;
}

int
main(void)
{
    unsigned int  f0, f1, c0, c1;
    unsigned long t0, t1, t2, elapsed, expect;

    con_init();
    kern_install();
    kirq_install();     /* vectors, a cleared table, VSYNC on, interrupts on */

    /* ---- 1: the vectors really were installed --------------------------- */
    /* Read back through absolute addresses rather than trusting the call to
       have done anything. The trampoline check is the half that matters: a
       vector pointing at bank $00 RAM that happens to hold zeroes would look
       just as installed from the vector alone. */
    if (*(volatile unsigned int *)X816_VEC_COP != (unsigned int)&kirq_tramp[0]  ||
        *(volatile unsigned int *)X816_VEC_BRK != (unsigned int)&kirq_tramp[4]  ||
        *(volatile unsigned int *)X816_VEC_NMI != (unsigned int)&kirq_tramp[8]  ||
        *(volatile unsigned int *)X816_VEC_IRQ != (unsigned int)&kirq_tramp[12])
        fail(1, 0x02);
    if (kirq_tramp[0] != 0x5C || kirq_tramp[4] != 0x5C ||
        kirq_tramp[8] != 0x5C || kirq_tramp[12] != 0x5C)
        fail(1, 0x02);
    /* ABORT must NOT have been touched: x816.sv ties abort_n high, so there is
       no source, and boot.s's trap is the right owner. */
    if (*(volatile unsigned int *)X816_VEC_ABORT == (unsigned int)&kirq_tramp[0])
        fail(1, 0x02);

    /* ---- 2: frames advance, so VSYNC dispatches AND is acknowledged ------ */
    if (!wait_frames(3))
        fail(2, 0x07);

    /* ---- 3: an installed handler runs ------------------------------------ */
    irqh_install_vsync();
    if (irqh_prev_lo != 0 || irqh_prev_bank != 0)
        fail(3, 0x06);                  /* the slot was empty; so must the report be */
    c0 = irqh_vsync_count;
    if (!wait_frames(4))
        fail(3, 0x06);
    if (irqh_vsync_count == c0)
        fail(3, 0x06);

    /* ---- 4: the previous handler is reported, and clearing really stops it */
    irqh_install_vsync2();
    if (irqh_prev_lo != irqh_vsync_lo || irqh_prev_bank != irqh_vsync_bank)
        fail(4, 0x04);                  /* must report handler ONE */
    irqh_clear_vsync();
    if (irqh_prev_lo != irqh_vsync2_lo || irqh_prev_bank != irqh_vsync2_bank)
        fail(4, 0x04);                  /* ...and now handler TWO */

    c0 = irqh_vsync_count;
    f0 = frames();
    if (!wait_frames(5))
        fail(4, 0x04);
    c1 = irqh_vsync_count;
    f1 = frames();
    if (c1 != c0)
        fail(4, 0x04);                  /* cleared, so it must NOT have run */
    if ((unsigned int)(f1 - f0) < 5)
        fail(4, 0x04);                  /* ...and interrupts must still be live,
                                           or the line above proved nothing */

    /* ---- 5: BRK dispatches, and execution resumes ------------------------ */
    irqh_install_brk();
    c0 = irqh_brk_count;
    irqh_do_brk();                      /* returning from this AT ALL is half
                                           the check: the stack has to come
                                           back exactly as it went in */
    if (irqh_brk_count != (unsigned int)(c0 + 1))
        fail(5, 0x03);

    /* ---- 6: the millisecond clock, cross-checked against frames ---------- */
    t0 = ms();
    f0 = frames();
    if (!wait_frames(60))
        fail(6, 0x08);
    t1 = ms();
    f1 = frames();
    if (t1 <= t0)
        fail(6, 0x08);                  /* it must advance, and never backwards */

    /* VERA's frame is 800x525 at 25 MHz = 59.52 Hz, so n frames should be
       about n * 1000 / 59.52 ms. The tolerance is deliberately loose -- this
       is a "the timer is wired to the right thing and divides by roughly the
       right number" check, not a measurement. A stuck timer, a timer off by
       the 60x that picking jiffies instead of milliseconds would cause, or one
       reading a neighbouring register all land far outside it. */
    elapsed = t1 - t0;
    expect  = (unsigned long)(unsigned int)(f1 - f0) * 1000UL / 60UL;
    if (elapsed < expect / 2UL || elapsed > expect * 2UL)
        fail(6, 0x08);

    /* ---- 7: TIME_SET moves the epoch, and time still runs ---------------- */
    kern_c = 0x2000;
    kern_x = 0x0001;                    /* 0x00012000 = 73,728 ms */
    kern_call(K_TIME_SET);
    t0 = ms();
    if (t0 < 0x00012000UL || t0 > 0x00012000UL + 500UL)
        fail(7, 0x09);                  /* landed on the epoch we asked for */
    if (!wait_frames(30))
        fail(7, 0x09);
    t2 = ms();
    if (t2 <= t0)
        fail(7, 0x09);                  /* ...and kept running from there */

    /* ---- 8: a slot that does not exist is refused ------------------------ */
    irqh_set_bad_slot();
    if (irqh_bad_carry != 1 || irqh_bad_code != KERR_BADARG)
        fail(8, 0x0C);

    /* ---- 9: the stuck-source defence ------------------------------------- */
    /* AFLOW cannot be acknowledged -- it clears only when the audio FIFO is
       refilled -- so enabling it with no handler installed is the one case
       that would otherwise lock the machine solid.

       Establish the precondition FIRST. If the FIFO is not reporting low, the
       source cannot assert, and a "pass" here would mean nothing at all; that
       is a failed check, not a skipped one. */
    if ((VERA_ISR & 0x08) == 0)
        fail(9, 0x0A);
    kirq_disabled = 0;
    VERA_IEN |= 0x08;
    if (!wait_frames(3))                /* still alive: the defence, not a hang */
        fail(9, 0x0A);
    if ((kirq_disabled & 0x08) == 0)
        fail(9, 0x0A);                  /* the dispatcher must SAY it did it */
    if ((VERA_IEN & 0x08) != 0)
        fail(9, 0x0A);                  /* ...and must actually have done it */

    RESULT = 0;
    paint(0x05);
    for (;;) { }
    return 0;
}
