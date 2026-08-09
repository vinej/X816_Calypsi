/* ==========================================================================
 * sheet.h -- the document: what a line of text MEANS as a cell, and how the
 * whole sheet is written to and read back from a file.
 *
 * WHY THESE TWO THINGS ARE IN ONE PLACE. Loading a CSV is typing it in
 * quickly: a field becomes a cell by exactly the rule a keystroke does, so
 * `+B2*C2` in a file is a formula for the same reason it is a formula on the
 * entry line. Two copies of that rule is how a sheet starts reloading
 * differently from how it was entered -- the file says 2024 and gets a
 * number where the user had typed a label. So the rule lives here once, and
 * kalk.c's entry line calls the same function the loader does.
 *
 * THE FORMAT is the one the Prog8 port writes, which is what makes a sheet
 * portable between the two:
 *
 *      Item,Qty,Price,Total
 *      Widget A,10,4.99,+B2*C2
 *      "Gadget, Deluxe",3,29.99,+B5*C5
 *      ,,,
 *      Subtotal,,,+@SUM(D2...D5)
 *
 * A cell is saved as its SOURCE, not its result -- a formula's text, not the
 * number it worked out. That is the whole point of saving a spreadsheet
 * rather than a table, and it is why loading recalculates rather than
 * trusting what it read.
 *
 * QUOTES MEAN TEXT, AND THAT IS A DECISION
 * ----------------------------------------
 * Ordinary CSV uses quotes only to escape a comma, and a reader is free to
 * strip them and re-guess the type. Doing that here loses data: a label
 * "2024" -- a year, a part number -- saves as 2024 and comes back as a
 * NUMBER, right-aligned where it used to be left, and no longer text. So a
 * quoted field is loaded as a LABEL, and saving quotes any label that would
 * otherwise read back as a number or a formula.
 *
 * The cost is that a foreign file whose numeric column happens to be quoted
 * loads as text. The gain is that a sheet written here always reads back as
 * itself, which is the property a spreadsheet's own files have to have.
 *
 * TWO THINGS THAT DO NOT ROUND-TRIP EXACTLY, both properties of the machine
 * rather than of this file, and neither visible to a user:
 *
 *   A NEGATIVE NUMBER comes back as a FORMULA. kalk's rule is that `+ - ( @`
 *   all begin one, so -12.5 in a file is the formula -12.5 for exactly the
 *   reason it is a formula when typed. It shows the same, sorts the same and
 *   calculates the same; only the status line, which shows a cell's source,
 *   can tell. Writing it any other way would mean a file kalk itself would
 *   read differently from how a person would type it.
 *
 *   THE LAST FEW DIGITS of an inexact fraction. The writer emits all nine
 *   significant digits f_to_str produces, which is everything the machine can
 *   say about a value -- but nine do not survive being read back. MEASURED:
 *   0.1 writes as 9.99999999e-02 and parses to 9.99999995e-02, a difference
 *   in the EIGHTH digit and some hundred and seventy units in the last place,
 *   far more than rounding would explain. The cause is in the library rather
 *   than here: float.s says its arithmetic truncates instead of rounding, and
 *   f_from_str applies the exponent with those operations, so each one drops
 *   what it cannot keep.
 *
 *   What survives is every digit the sheet DISPLAYS -- the formatter shows
 *   six significant digits, and both of those values format as 0.1 -- so a
 *   saved and reloaded sheet reads identically. Making the ninth digit
 *   survive as well means giving f_from_str a rounding step, which is a
 *   change to the float package and not to this file.
 *
 * BUILD AT -O0.
 * ========================================================================== */

#ifndef KALK_SHEET_H
#define KALK_SHEET_H

#include <stdint.h>
#include <stdbool.h>

/* Set a cell from a line of text, by kalk's rule:
 *
 *      "          a LABEL, forced. The quote is stripped.
 *      + - ( @    a FORMULA. The text is kept and the value computed.
 *      0-9 .      a NUMBER, if it parses as one.
 *      anything   a LABEL.
 *
 * An EMPTY string empties the cell. The cell's format is left alone, so
 * retyping a value does not undo a /F.
 *
 * The formula is evaluated once here so the cell has a value immediately;
 * anything it depends on is settled by the caller's recalculation. Returns
 * false only for a row or column outside the grid.
 */
bool sheet_set_text(uint16_t row, uint16_t col, const char *s);

/* ---- files ---------------------------------------------------------------
 *
 * Both take a path the way the shell does -- absolute, or relative to the
 * current directory. Both answer false rather than half-doing it, and
 * sheet_error() says which of the handful of things went wrong so the caller
 * can put it on a status line instead of inventing its own wording.
 */
bool sheet_save_csv(const char *path);

/* REPLACES the sheet. On failure the sheet is left EMPTY rather than half
   loaded -- a partly-loaded sheet that looks plausible is worse than an
   obviously empty one. The caller must recalculate, and must throw away any
   render cache: every row changed. */
bool sheet_load_csv(const char *path);

#define SHEET_OK        0
#define SHEET_ENOCARD   1       /* no filesystem                            */
#define SHEET_ENOPATH   2       /* the name would not resolve               */
#define SHEET_ENOFILE   3       /* not there, or not creatable              */
#define SHEET_EIO       4       /* the transfer failed part way             */
#define SHEET_EBIG      5       /* more rows or columns than the grid has   */

uint8_t sheet_error(void);

#endif /* KALK_SHEET_H */
