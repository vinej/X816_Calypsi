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

uint8_t keymap_shift[64] = {
    0,    0,    '!',  '@',  '#',  '$',  '%',  '^',
    '&',  '*',  '(',  ')',  '_',  '+',  0,    0x08,
    0x09, 'Q',  'W',  'E',  'R',  'T',  'Y',  'U',
    'I',  'O',  'P',  '{',  '}',  '|',  0,    'A',
    'S',  'D',  'F',  'G',  'H',  'J',  'K',  'L',
    ':',  '"',  0,    0x0D, 0,    0,    'Z',  'X',
    'C',  'V',  'B',  'N',  'M',  '<',  '>',  '?',
    0,    0,    0,    0,    0,    ' ',  0,    0
};

/* Unshifted is LOWER case, which is what a keyboard does. It was upper here
 * until the console could draw lower case at all -- with a 64-glyph font there
 * was nothing else to do, and it hid the fact that you could not type lower
 * case even if you wanted to.
 *
 * Everything downstream is case-insensitive, which is not a style choice: FAT32
 * stores 8.3 names upper-cased and cannot hold "readme.txt" and "README.TXT" as
 * two files, so to_83 folds and str_eq folds. Unix-style case sensitivity would
 * let you type a distinction the disk cannot keep. */
uint8_t keymap[64] = {
    0,    0,    '1',  '2',  '3',  '4',  '5',  '6',
    '7',  '8',  '9',  '0',  '-',  '=',  0,    0x08,
    0x09, 'q',  'w',  'e',  'r',  't',  'y',  'u',
    'i',  'o',  'p',  '[',  ']',  92,   0,    'a',
    's',  'd',  'f',  'g',  'h',  'j',  'k',  'l',
    ';',  39,   0,    0x0D, 0,    0,    'z',  'x',
    'c',  'v',  'b',  'n',  'm',  ',',  '.',  '/',
    0,    0,    0,    0,    0,    ' ',  0,    0
};
