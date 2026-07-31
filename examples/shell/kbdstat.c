/* ==========================================================================
 * kbdstat -- measure the keyboard path on real hardware instead of modelling
 * it.
 *
 * Three guesses have now been wrong, each costing a trip to the board, so this
 * measures rather than argues. It polls the SMC in the tightest loop possible,
 * does NO echo and NO command dispatch, and reports:
 *
 *   CYCLES   CPU cycles for 32 polls, from VIA1 Timer 1 (a 16-bit down
 *            counter clocked at cpu_clk). Divide by 32 for cycles per poll.
 *            This is the number every previous estimate was guessing at.
 *   PRESS    key-down events seen (bit 7 clear, non-zero)
 *   RELEASE  key-up events seen (bit 7 set)
 *   POLLS    polls completed, in units of 4096, so the loop is visibly alive
 *
 * HOW TO READ IT
 * --------------
 * Type exactly twenty characters at a normal speed, then stop and look.
 *
 *   PRESS = 20   Every keystroke reached the CPU. Nothing is lost in the SMC,
 *                the FIFO, the bridge or the bit-banging, and the shell's
 *                missing letters are caused by what the SHELL does between
 *                polls -- not by this layer.
 *   PRESS < 20   Keystrokes are being lost even by a loop that does nothing
 *                else. Then the fault is upstream of all the software: the
 *                8-entry bridge FIFO or the 16-entry key FIFO overflowing, or
 *                events never arriving from the HPS at all.
 *
 * That single number decides where to look next, and no amount of reasoning
 * off the board can substitute for it.
 *
 * CYCLES also settles whether the poll rate could EVER have been the problem.
 * At 8 MHz, 1300 cycles per poll is 162 us, which drains ~3000 keystrokes a
 * second against maybe ten typed -- 300x of headroom. If the measured figure
 * is anywhere near that, poll cost is not the story and something is blocking
 * the loop for whole milliseconds.
 * ========================================================================== */

#include "console.h"

/* VIA1 Timer 1. Read as ONE 16-bit access: $9F04 is the low byte and $9F05
   the high byte, so a 16-bit read lands exactly on the counter and no shifting
   is needed to reassemble it. Avoiding that shift is deliberate -- an 8-bit
   shift is what Calypsi miscompiled into this whole saga. */
#define VIA1_T1C   (*(volatile unsigned int *)0x9F04)
#define VIA1_T1CL  (*(volatile unsigned char *)0x9F04)
#define VIA1_T1CH  (*(volatile unsigned char *)0x9F05)
#define VIA1_ACR   (*(volatile unsigned char *)0x9F0B)

#define BATCH 32

static void
puthex(unsigned char v)
{
    unsigned char hi = (unsigned char)(v >> 4), lo = (unsigned char)(v & 15);
    con_putc((char)(hi < 10 ? '0' + hi : 'A' + hi - 10));
    con_putc((char)(lo < 10 ? '0' + lo : 'A' + lo - 10));
}

static void
puthex16(unsigned int v)
{
    puthex((unsigned char)(v >> 8));
    puthex((unsigned char)(v & 0xFF));
}

static void
at(unsigned char y, char *label)
{
    con_gotoxy(0, y);
    con_puts(label);
}

int
main(void)
{
    static char t0[] = "SMC KEYBOARD MEASUREMENT";
    static char t1[] = "TYPE 20 CHARACTERS, THEN STOP AND READ -PRESS-";
    static char lc[] = "CYCLES/32 POLLS ";
    static char lp[] = "PRESS           ";
    static char lr[] = "RELEASE         ";
    static char lo[] = "POLLS/4096      ";

    unsigned int  press = 0, release = 0, polls = 0, cycles = 0;
    unsigned int  batch = 0;
    unsigned int  t_start;

    con_init();
    con_cls();
    at(0, t0);
    at(1, t1);
    at(3, lc);
    at(4, lp);
    at(5, lr);
    at(6, lo);

    /* Timer 1 free-running from $FFFF, so it never needs re-arming and the
       counter is a continuous cycle clock. */
    VIA1_ACR  = 0x40;
    VIA1_T1CL = 0xFF;
    VIA1_T1CH = 0xFF;

    t_start = VIA1_T1C;

    for (;;) {
        unsigned char raw = con_smc_raw();

        if (raw) {
            if (raw & 0x80)
                release++;
            else
                press++;
        }

        polls++;
        batch++;
        if (batch >= BATCH) {
            /* Counts DOWN, so elapsed = start - now, and unsigned wraparound
               makes that correct across a reload without any special case. */
            unsigned int now = VIA1_T1C;
            cycles  = (unsigned int)(t_start - now);
            t_start = now;
            batch   = 0;
        }

        /* Repaint rarely: the display itself costs VERA writes, and the point
           is to measure the POLL, not the painting. The key FIFO holds events
           across a repaint, so nothing is lost by not looking. */
        if ((polls & 0x0FFF) == 0) {
            con_gotoxy(16, 3); puthex16(cycles);
            con_gotoxy(16, 4); puthex16(press);
            con_gotoxy(16, 5); puthex16(release);
            con_gotoxy(16, 6); puthex16((unsigned int)(polls >> 12));
        }
    }
}
