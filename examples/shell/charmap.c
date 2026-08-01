/* ==========================================================================
 * charmap -- show every one of the 256 CP437 glyphs.
 *
 * A 16x16 grid with hex labels, so a code can be read straight off the screen:
 * find the glyph, read its row for the high nibble and its column for the low
 * one. That is the thing you actually want when writing a program that draws
 * with box characters -- otherwise you are guessing at $C9 versus $DA.
 *
 * Every cell goes through con_putraw rather than con_putc, and that is not an
 * optimisation. con_putc interprets $08, $0A and $0D as backspace, newline and
 * carriage return, so those three glyphs cannot be printed through it at all --
 * and CP437 has real pictures there. A character map that silently skipped
 * three characters would be worse than none.
 *
 * The panel underneath draws a real double-line box out of the glyphs above
 * it, because a grid proves the glyphs EXIST and a box proves they JOIN. The
 * corners are the part that goes wrong: the rails have to meet outer-to-outer
 * and inner-to-inner, and when they do not, the seams show here immediately.
 * ========================================================================== */

#include "console.h"

/* CP437 box drawing, by code. Named because $C9 means nothing on sight. */
#define B_DH   0xCD     /* double horizontal    */
#define B_DV   0xBA     /* double vertical      */
#define B_DTL  0xC9     /* double top-left      */
#define B_DTR  0xBB     /* double top-right     */
#define B_DBL  0xC8     /* double bottom-left   */
#define B_DBR  0xBC     /* double bottom-right  */
#define B_SH   0xC4     /* single horizontal    */
#define B_SV   0xB3     /* single vertical      */
#define B_STL  0xDA
#define B_STR  0xBF
#define B_SBL  0xC0
#define B_SBR  0xD9

#define GRID_X   6
#define GRID_Y   4

static char
hexdig(uint8_t v)
{
    return (char)(v < 10 ? '0' + v : 'A' + v - 10);
}

static void
box(uint8_t x, uint8_t y, uint8_t w, uint8_t h, bool dbl)
{
    uint8_t i;
    uint8_t hz = dbl ? B_DH  : B_SH,  vt = dbl ? B_DV  : B_SV;
    uint8_t tl = dbl ? B_DTL : B_STL, tr = dbl ? B_DTR : B_STR;
    uint8_t bl = dbl ? B_DBL : B_SBL, br = dbl ? B_DBR : B_SBR;

    for (i = 1; i < w - 1; i++) {
        con_putraw((uint8_t)(x + i), y, hz);
        con_putraw((uint8_t)(x + i), (uint8_t)(y + h - 1), hz);
    }
    for (i = 1; i < h - 1; i++) {
        con_putraw(x, (uint8_t)(y + i), vt);
        con_putraw((uint8_t)(x + w - 1), (uint8_t)(y + i), vt);
    }
    con_putraw(x, y, tl);
    con_putraw((uint8_t)(x + w - 1), y, tr);
    con_putraw(x, (uint8_t)(y + h - 1), bl);
    con_putraw((uint8_t)(x + w - 1), (uint8_t)(y + h - 1), br);
}

static void
at(uint8_t x, uint8_t y, char *s)
{
    con_gotoxy(x, y);
    con_puts(s);
}

int
main(void)
{
    static char title[] = "X816 -- CP437 CHARACTER SET";
    static char howto[] = "row = high nibble, column = low nibble";
    static char l1[]    = "double box";
    static char l2[]    = "single box";
    static char l3[]    = "shading  \xB0\xB1\xB2\xDB   halves \xDC\xDF\xDD\xDE";
    static char l4[]    = "accents  \x82\x84\x94\x81\xA4\x87   maths \xE3\xE4\xF1\xF7\xFB";
    uint8_t hi, lo;

    con_init();
    con_cls();

    at(0, 0, title);
    at(0, 1, howto);

    /* Column headings, then a row heading per sixteen. */
    for (lo = 0; lo < 16; lo++)
        con_putraw((uint8_t)(GRID_X + lo * 2), (uint8_t)(GRID_Y - 1),
                   (uint8_t)hexdig(lo));

    for (hi = 0; hi < 16; hi++) {
        uint8_t y = (uint8_t)(GRID_Y + hi);
        con_putraw(GRID_X - 4, y, (uint8_t)hexdig(hi));
        con_putraw(GRID_X - 3, y, '0');
        for (lo = 0; lo < 16; lo++)
            con_putraw((uint8_t)(GRID_X + lo * 2), y,
                       (uint8_t)((hi << 4) | lo));
    }

    /* Proof that the glyphs JOIN, not merely exist. */
    box(42, 3, 22, 5, true);
    at(45, 5, l1);
    box(42, 9, 22, 5, false);
    at(45, 11, l2);

    at(42, 16, l3);
    at(42, 17, l4);

    for (;;)
        ;
}
