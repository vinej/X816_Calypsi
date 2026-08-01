/* Keymaps for the console.
 *
 * The FONT no longer lives here: 256 CP437 glyphs are 2 KB, and as a C array
 * that is 2 KB of bank $00 -- the machine's only fast memory -- for data read
 * once at boot. It moved to font_cp437.s, in a code section in bank $01.
 * These tables are small and are read on every keystroke, so they stay.
 *
 * NEITHER OF THESE IS const, AND THAT IS DELIBERATE.
 *
 * A const array lands in Calypsi's `cdata`, which the linker places with the
 * code in bank $01 and which "provides bits" -- content at an address, not
 * something cstartup initialises. A near read from bank $00 would fetch the
 * wrong memory entirely. Left non-const they land in `data`, whose initialiser
 * rides in the image as `idata` and is copied into bank $00 at startup.
 *
 * Costs 576 bytes of bank $00, once. When the kernel moves into the firmware
 * region the problem disappears, because there `cdata` is simply part of the
 * image and directly reachable.
 */

#include <stdint.h>


/* Unshifted is LOWER case, which is what a keyboard does. It was upper here
 * until the console could draw lower case at all -- with a 64-glyph font there
 * was nothing else to do, and it hid the fact that you could not type lower
 * case even if you wanted to.
 *
 * Everything downstream is case-insensitive, which is not a style choice: FAT32
 * stores 8.3 names upper-cased and cannot hold "readme.txt" and "README.TXT" as
 * two files, so to_83 folds and str_eq folds. Unix-style case sensitivity would
 * let you type a distinction the disk cannot keep. */

/* ==========================================================================
 * The SMC does not send ASCII, and it does not send PS/2 scancodes either. It
 * sends IBM KEY POSITION NUMBERS -- position 1 is the grave key, 2..11 are the
 * digit row, 43 is Enter, and everything that is not on a typewriter lives
 * from 64 upwards: Delete 76, the arrows 79/83/84/89, the keypad 90..108,
 * ESCAPE 110, F1..F12 112..123.
 *
 * This table used to stop at 64, so every one of those was DISCARDED by
 * con_getkey before any program could see it. ESC was not unbound -- it was
 * unreachable. Then position 1 was ASSUMED to be ESC and mapped to $1B, which
 * made the grave key type an escape while the real ESC still did nothing, and
 * the emulator agreed only because the same wrong assumption had been put in
 * its autokey table too. The numbering here is not a guess: it is the one
 * X816_Emulator's keynum_from_SDL_Scancode already implements, which is what
 * the SMC and the emulator both follow.
 *
 * WHAT IS STILL 0, AND WHY. The keypad and Delete have unambiguous ASCII, so
 * they are mapped. The arrows, F1..F12, Home/End/PgUp/PgDn, Insert and the
 * lock keys have none, and inventing a representation for them -- an escape
 * sequence, a high-bit code, a separate queue -- is a decision that belongs
 * with the measurements KEYSCAN.BIN produces rather than ahead of them.
 *
 * The keypad digits are mapped unconditionally, so NumLock does not turn them
 * into navigation keys the way a PC does: one behaviour, and the one printed
 * on the keys.
 * ========================================================================== */
uint8_t keymap[128] = {
    0,     '`',   '1',   '2',   '3',   '4',   '5',   '6',
    '7',   '8',   '9',   '0',   '-',   '=',   0,     0x08,
    0x09,  'q',   'w',   'e',   'r',   't',   'y',   'u',
    'i',   'o',   'p',   '[',   ']',   92,    0,     'a',
    's',   'd',   'f',   'g',   'h',   'j',   'k',   'l',
    ';',   39,    0,     0x0D,  0,     0,     'z',   'x',
    'c',   'v',   'b',   'n',   'm',   ',',   '.',   '/',
    0,     0,     0,     0,     0,     ' ',   0,     0,
    /* ---- 64..127 ---------------------------------------------------- */
    0,     0,     0,     0,     0,     0,     0,     0,
    0,     0,     0,     0,     0x7F,  0,     0,     0,
    0,     0,     0,     0,     0,     0,     0,     0,
    0,     0,     0,     '7',   '4',   '1',   0,     '/',
    '8',   '5',   '2',   '0',   '*',   '9',   '6',   '3',
    '.',   '-',   '+',   0,     0x0D,  0,     0x1B,  0,
    0,     0,     0,     0,     0,     0,     0,     0,
    0,     0,     0,     0,     0,     0,     0,     0,
};

uint8_t keymap_shift[128] = {
    0,     '~',   '!',   '@',   '#',   '$',   '%',   '^',
    '&',   '*',   '(',   ')',   '_',   '+',   0,     0x08,
    0x09,  'Q',   'W',   'E',   'R',   'T',   'Y',   'U',
    'I',   'O',   'P',   '{',   '}',   '|',   0,     'A',
    'S',   'D',   'F',   'G',   'H',   'J',   'K',   'L',
    ':',   '\"',  0,     0x0D,  0,     0,     'Z',   'X',
    'C',   'V',   'B',   'N',   'M',   '<',   '>',   '?',
    0,     0,     0,     0,     0,     ' ',   0,     0,
    /* ---- 64..127 ---------------------------------------------------- */
    0,     0,     0,     0,     0,     0,     0,     0,
    0,     0,     0,     0,     0x7F,  0,     0,     0,
    0,     0,     0,     0,     0,     0,     0,     0,
    0,     0,     0,     '7',   '4',   '1',   0,     '/',
    '8',   '5',   '2',   '0',   '*',   '9',   '6',   '3',
    '.',   '-',   '+',   0,     0x0D,  0,     0x1B,  0,
    0,     0,     0,     0,     0,     0,     0,     0,
    0,     0,     0,     0,     0,     0,     0,     0,
};
