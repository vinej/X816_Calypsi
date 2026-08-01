/* ==========================================================================
 * goshell.c -- get back to the prompt from a program that replaced it.
 *
 * A conformance test paints a colour and then stops, because there is nowhere
 * to go: `run` loaded it over the shell at $01:0000, so the thing that would
 * normally take control back no longer exists. On hardware that means a power
 * cycle to read the next result, which turns a five-minute test round into a
 * five-minute test.
 *
 * So the way back is to load the shell again and hand over exactly as `run`
 * does -- same staging area, same relocator, same "does not return". The shell
 * is an X816 image like any other, and nothing about running one is special to
 * the shell having started it.
 *
 * WHY IT WAITS FOR ESC RATHER THAN JUST RETURNING
 * ----------------------------------------------
 * The result is on the screen. Reloading immediately would erase it before
 * anyone had read it, so the key is what says "I have seen it".
 *
 * WHY IT CAN FAIL, AND WHAT HAPPENS THEN
 * --------------------------------------
 * The card may be the very thing under test and may be unreadable, in which
 * case there is no shell to load. Then this returns and the caller goes back
 * to waiting, leaving the failure colour on screen -- which is the right
 * outcome: a test that cleared its own diagnosis because the recovery path
 * also failed would be worse than one that just stopped.
 * ========================================================================== */

#include "goshell.h"
#include "console.h"
#include "fat32.h"

/* Where `run` stages an image before the relocator moves it down. Bank $10 is
   out of the way of both the program at $01:0000 and anything in bank $00. */
#define EXEC_STAGE 0x100000UL

extern uint16_t x816_exec_len;
extern void     x816_exec(void);         /* does not return */

/* The card layout mksdcard.py builds. A test that has replaced the shell has
   no way to ask where it came from -- boot1.rom is handed over by the HPS and
   leaves no path behind -- so this is the one place the location is written
   down outside the card builder. */
static char shell_path[] = "/DEMO/SHELL.BIN";

bool
goshell(void)
{
    fat32_file f;
    uint32_t   got;

    if (!fat32_mount())
        return false;
    if (!fat32_open(shell_path, &f))
        return false;

    /* TWO passes, and the second is not optional.
     *
     * fat32_read_far moves WHOLE CLUSTERS by DMA and returns how many bytes it
     * actually moved -- its contract, since mixing a byte loop into it would
     * cost the DMA's whole advantage. So it stops at the last cluster boundary
     * and a short return is NORMAL, not a failure. Treating it as one is why
     * ESC did nothing: the shell is 22 KB, the last cluster is partial, and
     * every attempt refused. runtime/shell.c documents this exactly and it
     * still got rewritten here -- so the two-pass read now lives in both
     * places rather than the rule living in a comment. */
    got = fat32_read_far(&f, EXEC_STAGE, f.size);
    while (got < f.size) {
        uint8_t        buf[64];
        uint16_t       n = fat32_read(&f, buf, sizeof buf);
        uint8_t __far *q;
        uint16_t       i;

        if (n == 0)
            break;                       /* EOF before the stated size */
        q = (uint8_t __far *)(EXEC_STAGE + got);
        for (i = 0; i < n; i++)
            q[i] = buf[i];
        got += n;
    }
    if (got != f.size)
        return false;

    x816_exec_len = (uint16_t)f.size;
    x816_exec();
    return false;                        /* unreachable */
}

void
goshell_on_esc(void)
{
    for (;;) {
        uint16_t c = con_getkey();
        if (c == 0x1B)
            goshell();                   /* returns only if it could not */
    }
}
