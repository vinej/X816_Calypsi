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

/* The same cap cmd_run enforces (shell.c's EXEC_MAX): the exec blob copies
   with one 16-bit index pass, so nothing larger than this can be moved -- and
   a size of ZERO must never reach the blob at all, because its post-increment
   loop would copy 64 KB of stage garbage and jump into it. cmd_run refuses
   both; this path must refuse them too. */
#define EXEC_MAX   0xFF00UL

extern uint16_t x816_exec_len;
extern void     x816_exec(void);         /* does not return */
extern void     x816_fw_enter(void);     /* does not return */

/* The four bytes boot/boot.s checks at $F0:0000 -- present means the
   RESIDENT KERNEL owns the firmware region. */
static bool
fw_present(void)
{
    uint8_t __far *p = (uint8_t __far *)0xF00000UL;
    return p[0] == 'X' && p[1] == '8' && p[2] == '1' && p[3] == '6';
}

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

    /* The resident kernel, when present, IS the prompt -- return by
       restarting it. No card involved, so this works even when the card is
       the very thing the test broke; the reload path below stays as the
       fallback for kernel-less setups. */
    if (fw_present())
        x816_fw_enter();                 /* does not return */

    if (!fat32_mount())
        return false;
    if (!fat32_open(shell_path, &f))
        return false;

    /* Refuse a size the relocator cannot survive, exactly as cmd_run does. A
       zero-length SHELL.BIN -- a truncated copy, an interrupted write -- would
       otherwise be "loaded" and jumped into. The message stays on screen with
       the test result, which is the point of returning instead of executing. */
    if (f.size == 0 || f.size > EXEC_MAX) {
        static char badsz[] = "SHELL.BIN BAD SIZE\n";
        con_puts(badsz);
        return false;
    }

    /* TWO passes, and the second is not optional.
     *
     * fat32_read_far moves WHOLE CLUSTERS by DMA and returns how many bytes it
     * actually moved -- its contract, since mixing a byte loop into it would
     * cost the DMA's whole advantage. So it stops at the last cluster boundary
     * and a short return is NORMAL, not a failure. Treating it as one is why
     * ESC did nothing: the shell is ~23 KB (23,143 bytes as measured from a
     * clean build.sh run), the last cluster is partial, and
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
