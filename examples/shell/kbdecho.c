/* ==========================================================================
 * kbdecho -- the same tight poll loop as kbdstat, but it also DECODES and
 * ECHOES, exactly as the shell does. Everything else is stripped away.
 *
 * kbdstat established that all twenty keystrokes reach the CPU (PRESS = 0014),
 * so nothing is lost in the SMC, either FIFO, the bridge or the bit-banging.
 * The remaining suspects sit above that, and there are only three of them.
 * This counts each one separately so a single run names the guilty stage:
 *
 *   PRESS    raw key-down events, straight off the bus. kbdstat already
 *            showed this is correct, and it is repeated here as the baseline
 *            everything else is compared against.
 *   DECODE   how many of those survived keymap[] -- the console's translation
 *            from IBM System/2 keycode to ASCII. Anything the table maps to
 *            zero, such as Shift, is not counted.
 *   ECHO     how many characters were actually handed to con_putc.
 *
 * Then type twenty characters and compare the three numbers with the text
 * that appears on the ECHO line:
 *
 *   PRESS 20, DECODE 20, ECHO 20, and twenty glyphs on screen
 *       Nothing is lost anywhere in the console. The shell must then differ
 *       for some other reason and the next place to look is sh_readline.
 *
 *   PRESS 20 but DECODE < 20
 *       The keymap is dropping real keys -- either the table is wrong for
 *       this keyboard, or con_getkey's filtering is too aggressive.
 *
 *   DECODE 20, ECHO 20, but FEWER than twenty glyphs on screen
 *       The characters were accepted and written, and the screen still does
 *       not show them. That is con_putc or VERA, i.e. an OUTPUT fault wearing
 *       the costume of a keyboard fault -- which would explain why every
 *       measurement of the input path has come back clean.
 *
 * The hardware counters ARRIVE / HWPUSH / HWDROP come from the core itself and
 * extend the same chain upstream, so the guilty stage is whichever number
 * first falls short of what was typed:
 *
 *   ARRIVE < typed    the keystroke never crossed into the core -- MiSTer/HPS
 *                     or the PS/2 clock-domain sync
 *   HWPUSH < ARRIVE   translation dropped it, or the key FIFO was full
 *   HWDROP > 0        the key FIFO overflowed, so the CPU polls too slowly
 *   PRESS  < HWPUSH   the CPU never read it back off the bus
 *
 * ONE EXCEPTION, and it looks alarming until you know it. Hold a key until
 * auto-repeat starts: ARRIVE FREEZES while HWPUSH, PRESS, DECODE and ECHO keep
 * climbing and the repeated characters appear normally. That is correct.
 * MiSTer Main discards the host's key-repeat events for non-ps2ctl cores, so
 * X816 synthesizes typematic ITSELF, inside ps2_to_smc_bridge -- downstream of
 * the clock-domain sync that ARRIVE counts, upstream of the FIFO that HWPUSH
 * counts. A repeat is therefore a real keystroke that never crossed from the
 * host, and a frozen ARRIVE beside a climbing HWPUSH is positive proof the
 * bridge's typematic is doing its job.
 *
 * So the "first counter to fall short is the culprit" rule holds only for
 * DISTINCT keypresses. Let go of the key before reading the numbers.
 *
 * Counters are in hex: twenty reads as 0014. The hardware ones are 8-bit and
 * wrap at FF.
 * ========================================================================== */

#include "console.h"

/* Same table the console decodes with. Shared deliberately: the question here
   is whether the console's OWN decode loses keys, so using a second copy would
   answer a different question. */
extern unsigned char keymap[64];

/* Hardware-side counters, added to the core alongside the PS/2 sync fix.
 * They count the SAME keystrokes at earlier stages, so the six numbers on
 * screen trace one keypress from the wire to the glyph:
 *
 *   ARRIVE  makes that crossed into the core at all (X816.sv PS/2 sync)
 *   HWPUSH  makes that reached the SMC key FIFO     (translation + space)
 *   HWDROP  keys discarded because that FIFO was full
 *
 * If ARRIVE reads 00 while keys clearly work, the bitstream predates these
 * registers -- rebuild before believing anything below.
 */
#define KBD_ARRIVE (*(volatile unsigned char *)0x9F8D)
#define KBD_PUSH   (*(volatile unsigned char *)0x9F8E)
#define KBD_DROP   (*(volatile unsigned char *)0x9F8F)

#define ECHO_ROW 9

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

int
main(void)
{
    static char t0[] = "SMC KEYBOARD DECODE + ECHO";
    static char t1[] = "TYPE 20 CHARACTERS, THEN COMPARE THE THREE COUNTS";
    static char t2[] = "AGAINST THE TEXT ON THE ECHO LINE";
    static char lp[] = "PRESS   ";
    static char ld[] = "DECODE  ";
    static char le[] = "ECHO    ";
    static char la[] = "ARRIVE  ";
    static char lh[] = "HWPUSH  ";
    static char lw[] = "HWDROP  ";
    static char lx[] = "ECHO> ";

    unsigned int press = 0, decode = 0, echoed = 0;
    unsigned char col = 6;

    con_init();
    con_cls();
    con_gotoxy(0, 0); con_puts(t0);
    con_gotoxy(0, 1); con_puts(t1);
    con_gotoxy(0, 2); con_puts(t2);
    con_gotoxy(0, 4); con_puts(lp);
    con_gotoxy(0, 5); con_puts(ld);
    con_gotoxy(0, 6); con_puts(le);
    con_gotoxy(20, 4); con_puts(la);
    con_gotoxy(20, 5); con_puts(lh);
    con_gotoxy(20, 6); con_puts(lw);
    con_gotoxy(0, ECHO_ROW); con_puts(lx);

    for (;;) {
        unsigned char raw = con_smc_raw();

        if (raw != 0 && !(raw & 0x80)) {        /* a key-down */
            press++;

            if (raw < 64) {
                unsigned char ch = keymap[raw];
                if (ch != 0) {
                    decode++;
                    /* Echo it where it can be counted by eye. Wrapping is
                       manual so con_putc's own newline handling cannot be
                       blamed for a character going missing. */
                    if (col < 79) {
                        con_gotoxy(col, ECHO_ROW);
                        con_putc((char)ch);
                        col++;
                        echoed++;
                    }
                }
            }

            /* Repaint the counters only when something changed, so the poll
               loop stays tight while idle. */
            con_gotoxy(8, 4); puthex16(press);
            con_gotoxy(8, 5); puthex16(decode);
            con_gotoxy(8, 6); puthex16(echoed);
            /* The hardware side, counting the same keystrokes further up the
               chain. Read AFTER the software counters so a key still in
               flight cannot make hardware look behind software. */
            con_gotoxy(28, 4); puthex(KBD_ARRIVE);
            con_gotoxy(28, 5); puthex(KBD_PUSH);
            con_gotoxy(28, 6); puthex(KBD_DROP);
        }
    }
}
