/* kbdprobe -- what is the SMC actually returning?
 *
 * shell.bin blocks forever in con_getc() on hardware: output works, input
 * never arrives, and one blind fix (zeroing the VIA output register) did not
 * change it. So stop guessing and look, exactly as boot/kbd.s's own
 * diagnostic did.
 *
 * Four hex pairs:
 *     RAW    the byte from the SMC this instant
 *     LAST   the last NON-ZERO byte seen -- a keypress is over in about a
 *            millisecond, so the instantaneous value would never be caught
 *     KEY    the last byte with bit 7 clear, i.e. a press rather than a release
 *     BEAT   a poll counter, so a stalled loop looks different from a quiet one
 *
 * Reading it:
 *     BEAT frozen        the loop is stuck -- not an I2C problem at all
 *     RAW = FE always    the transaction is not reaching the SMC: the bus, or
 *                        ORA/DDRA, or the command never gets armed
 *     RAW = 00 always    the bus works and the FIFO is empty -- keys are not
 *                        arriving from the host at all
 *     RAW changes        bus and SMC are both fine, and the fault is ABOVE
 *                        this layer, in the release-flag test or the keymap
 *
 * Each of those points somewhere completely different, which is why this shows
 * the raw byte instead of another pass/fail colour.
 */

#include "console.h"

static void
puthex(unsigned char v)
{
    unsigned char hi = (unsigned char)(v >> 4), lo = (unsigned char)(v & 15);
    con_putc((char)(hi < 10 ? '0' + hi : 'A' + hi - 10));
    con_putc((char)(lo < 10 ? '0' + lo : 'A' + lo - 10));
}

int
main(void)
{
    static char hdr[]  = "SMC KEYBOARD PROBE\n";
    static char cols[] = "RAW LAST KEY BEAT\n";
    unsigned char last = 0, key = 0, beat = 0;

    con_init();
    con_cls();
    con_puts(hdr);
    con_puts(cols);

    for (;;) {
        unsigned char raw = con_smc_raw();
        unsigned int  i;

        if (raw)
            last = raw;
        if (raw && !(raw & 0x80))
            key = raw;
        beat++;

        con_gotoxy(0, 3);
        puthex(raw);  con_putc(' ');
        puthex(last); con_putc(' ');
        puthex(key);  con_putc(' ');
        puthex(beat);

        /* Slow the poll so BEAT is readable rather than a blur. */
        for (i = 0; i < 3000; i++)
            (void)con_getx();
    }
}
