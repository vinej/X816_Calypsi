/* ==========================================================================
 * keyscan.c -- find out what every key on the keyboard actually sends.
 *
 * WHY THIS EXISTS
 * ---------------
 * con_getkey throws away everything it cannot name:
 *
 *     if (release)      return 0;      every key-up is noise
 *     if (key >= 64)    return 0;      off the end of the keymap
 *
 * The keymap has 64 entries and they cover a US typewriter layout. So F1, the
 * arrows, Home, the numeric keypad, the right-hand modifiers and the Windows
 * key are not merely unmapped -- they are DISCARDED before any program sees
 * them, and there is no way to find out what they send by writing code that
 * uses the console. The information has to be collected from below.
 *
 * WHAT IT DOES
 * ------------
 * Asks for one key at a time by name, reads the RAW byte from the SMC, and
 * records it. At the end it writes the whole table to /KEYMAP.TXT on the card,
 * because a table of fifty codes read off a screen is a table transcribed
 * wrongly. The file is the deliverable; the screen is just progress.
 *
 * Each entry is recorded from the key-DOWN edge -- bit 7 clear. The release is
 * then waited for, so that letting go of the key does not answer the next
 * prompt, and auto-repeat does not answer three of them.
 *
 * SOME KEYS SEND MORE THAN ONE BYTE, and Pause is the usual offender. The
 * extra bytes are recorded too, up to three per key, rather than being hidden:
 * a key that reports as two codes is a fact the future keymap has to know, not
 * a measurement error.
 *
 *     ENTER  skip the key being asked for -- not every keyboard has all of
 *            these, and a missing key must not stall the scan
 *     ESC    stop early and write what has been collected
 * ========================================================================== */

#include "console.h"
#include "fat32.h"
#include "goshell.h"

#include <stdint.h>

#define KEY_ENTER  43           /* known good, from the existing keymap */
#define KEY_ESCAPE  1

#define MAXCODES 3              /* bytes recorded per key */

/* The keys worth asking about: everything the current keymap cannot express.
   Order follows the keyboard rather than the code, so that scanning is a
   sweep across the hardware and a skipped key is obvious. */
/* A fixed-width ARRAY, not an array of pointers. String literals land in the
   far constant section, whose address C cannot form on this compiler, so a
   table of `const char *` does not link. Twelve bytes each and the padding is
   free next to not building at all. */
static char names[][12] = {
    "ESC(check)",
    "F1",  "F2",  "F3",  "F4",  "F5",  "F6",
    "F7",  "F8",  "F9",  "F10", "F11", "F12",
    "PRTSCR", "SCRLK", "PAUSE",
    "INS", "HOME", "PGUP", "DEL", "END", "PGDN",
    "UP", "DOWN", "LEFT", "RIGHT",
    "TAB", "CAPS", "LSHIFT", "LCTRL", "LWIN", "LALT",
    "RALT", "RWIN", "MENU", "RCTRL", "RSHIFT",
    "NUMLOCK", "KP/", "KP*", "KP-", "KP+", "KPENTER", "KP.",
    "KP0", "KP1", "KP2", "KP3", "KP4",
    "KP5", "KP6", "KP7", "KP8", "KP9",
    /* The top-row digits are already known, but two of them are asked for
       anyway: the whole point of the keypad entries above is whether the SMC
       distinguishes them, and that question has no answer without both
       halves measured by the same run. */
    "TOPROW 1", "TOPROW 0",
};

#define NKEYS (sizeof names / sizeof names[0])
#define NAMEW 12

static uint8_t codes[NKEYS][MAXCODES];
static uint8_t ncodes[NKEYS];
static uint8_t skipped[NKEYS];

static void
put_hex(uint8_t v)
{
    static char digits[] = "0123456789ABCDEF";
    con_putc('$');
    con_putc(digits[v >> 4]);
    con_putc(digits[v & 15]);
}

/* Blocking read of one raw SMC byte. con_smc_raw returns 0 for an empty FIFO
   and $FE when the bus itself is not answering -- the second is worth showing
   rather than spinning on, because a dead bus and a patient user look
   identical from here. */
static uint8_t
raw_wait(void)
{
    for (;;) {
        uint8_t r = con_smc_raw();
        if (r != 0 && r != 0xFE)
            return r;
    }
}

/* ---- the report ---------------------------------------------------------
 *
 * Written with fat32_* directly rather than through the kernel table. This is
 * a diagnostic for a machine whose keyboard is in question; giving it the
 * shortest path to the card keeps the number of things that can be wrong at
 * the same time down to one.
 */
static char line[40];
static uint8_t linelen;

static void
emit(char c)
{
    if (linelen < sizeof line)
        line[linelen++] = c;
}

static void
emit_hex(uint8_t v)
{
    static char digits[] = "0123456789ABCDEF";
    emit('$');
    emit(digits[v >> 4]);
    emit(digits[v & 15]);
}

static bool
write_report(void)
{
    static char path[] = "/KEYMAP.TXT";
    fat32_file  f;
    uint16_t    i, j;

    if (!fat32_mount())
        return false;
    if (!fat32_create(path, &f))
        return false;

    for (i = 0; i < NKEYS; i++) {
        const char *n = names[i];
        linelen = 0;
        for (j = 0; n[j]; j++)
            emit(n[j]);
        while (linelen < 12)
            emit(' ');
        if (skipped[i]) {
            emit('-');
        } else {
            for (j = 0; j < ncodes[i]; j++) {
                if (j)
                    emit(' ');
                emit_hex(codes[i][j]);
            }
        }
        emit('\r');
        emit('\n');
        if (fat32_write(&f, (const uint8_t *)line, linelen) != linelen) {
            fat32_close(&f);
            return false;
        }
    }
    return fat32_close(&f);
}

int
main(void)
{
    static char title[]  = "X816 KEY SCANNER\n";
    static char help1[]  = "press the key named below.  ENTER skips it, "
                           "ESC stops and writes the file.\n";
    static char help2[]  = "codes above $3F are the ones no program can see "
                           "today -- that is what this is for.\n\n";
    static char askmsg[] = "press: ";
    static char gotmsg[] = "  -> ";
    static char skipmsg[] = "  -> skipped\n";
    static char donemsg[] = "\nwriting /KEYMAP.TXT ... ";
    static char okmsg[]   = "ok\n";
    static char badmsg[]  = "FAILED (no card?)\n";
    static char endmsg[]  = "\nESC returns to the shell.\n";

    uint16_t i, j;
    uint8_t  stop = 0;

    con_init();
    con_cls();
    con_puts(title);
    con_puts(help1);
    con_puts(help2);

    for (i = 0; i < NKEYS && !stop; i++) {
        uint8_t code, key;

        con_puts(askmsg);
        con_puts(names[i]);
        con_puts(gotmsg);

        code = raw_wait();
        key  = (uint8_t)(code & 0x7F);

        /* A key-UP arriving first is the tail of the PREVIOUS key, not an
           answer to this prompt. Ignoring it is what keeps the scan in step
           with the person doing it. */
        while (code & 0x80)
            code = raw_wait(), key = (uint8_t)(code & 0x7F);

        if (key == KEY_ENTER && i != 0) {
            skipped[i] = 1;
            con_puts(skipmsg);
        } else if (key == KEY_ESCAPE && i != 0) {
            stop = 1;
            skipped[i] = 1;
            con_putc('\n');
            break;
        } else {
            codes[i][0] = code;
            ncodes[i]   = 1;
            put_hex(code);

            /* Drain the rest of this key's burst. A single key gives one more
               byte, its release; Pause gives several. Anything still arriving
               that is not the release of what was just pressed is part of the
               same keystroke and belongs in the record. */
            for (;;) {
                uint8_t r = con_smc_raw();
                if (r == 0 || r == 0xFE)
                    continue;
                if ((uint8_t)(r & 0x7F) == key && (r & 0x80))
                    break;                      /* the key came back up */
                if (ncodes[i] < MAXCODES) {
                    codes[i][ncodes[i]++] = r;
                    con_putc(' ');
                    put_hex(r);
                }
                if (r & 0x80)
                    break;      /* some other release: the burst is over */
            }
            con_putc('\n');
        }
    }

    con_puts(donemsg);
    con_puts(write_report() ? okmsg : badmsg);
    con_puts(endmsg);

    goshell_on_esc();
    return 0;                   /* unreachable */
}
