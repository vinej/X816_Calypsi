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

/* ---- structural edits ----------------------------------------------------
 *
 * Insert or delete a whole row or column: everything after it moves, and the
 * REFERENCES INSIDE EVERY FORMULA are rewritten so they still name the cells
 * they used to. A sheet where +B4 went on meaning "wherever B4 is now" after
 * a row was pushed under it would be worse than one that refused the command.
 *
 * THE $ IS IGNORED HERE, and that surprises people who know it from copying.
 * $B$4 means "B4 and do not adjust when this formula is COPIED" -- but if the
 * cell B4 itself moves down a row, then the thing $B$4 names has moved, and
 * following it is what keeps the reference true. Anchoring is a property of
 * replication, not of the cell's identity. The Prog8 port draws the same line
 * and only consults the dollars for /R.
 *
 * A REFERENCE TO A DELETED ROW is left pointing at that position, which now
 * holds whatever moved up into it. Bigger spreadsheets answer #REF! instead.
 * This follows the port it came from, and the choice is at least visible: the
 * formula still reads +B4 and B4 is on screen.
 *
 * WHAT THEY COST is bounded by the watermark and by the row map, not by the
 * grid -- a column insert skips a row nobody has written to without reading a
 * cell of it. That is not an optimisation, it is what makes the operation
 * affordable at all: this grid holds 262,144 cells, and the X16 port measured
 * 0.83 s to insert a row across 6,656 of them.
 *
 * MEASURED, by run-sheet.sh, inserting at row 0 of the same 1,024-row grid:
 *
 *      56 x 8 written      474 ms
 *      three cells          42 ms
 *
 * -- so the price follows what has been written and not what could be. The
 * dense figure is about a millisecond a cell, which is what a cell_get and a
 * cell_put out of SDRAM cost at -O0; it is a deliberate command rather than a
 * keystroke, so that is left alone. If it ever needs to be faster the answer
 * is a row-at-a-time move inside cell.c, not anything here.
 *
 * Each answers false only for an out-of-range index. A formula whose rewrite
 * would not fit in a cell keeps its old text and is flagged CELL_ERROR, so it
 * shows as ERROR rather than quietly meaning something else.
 */
bool sheet_insert_row(uint16_t at);
bool sheet_delete_row(uint16_t at);
bool sheet_insert_col(uint16_t at);
bool sheet_delete_col(uint16_t at);

/* ---- replicate -----------------------------------------------------------
 *
 * Copy the block (r1,c1)..(r2,c2) into the target (tr1,tc1)..(tr2,tc2).
 *
 * A SINGLE CELL as the target means "put the block here" -- A1...A3 onto B1
 * fills B1, B2 and B3. A RANGE means "fill this with the block", so one
 * formula onto B2...B4 fills all three, which is how a column of totals gets
 * written once. The port this came from has only the first, and filling a
 * column there takes one command per cell; the second is an addition rather
 * than a change, since a single-cell target still behaves identically.
 *
 * THIS IS WHERE THE DOLLARS FINALLY MEAN SOMETHING, and it is the only place
 * they do. A formula's relative references move by the same offset the cell
 * did, so +A1 copied one column right becomes +B1 -- which is what makes a
 * column of totals worth writing once. An ANCHORED component does not move,
 * because $ is precisely the user saying "not this one": +A1*$D$1 replicated
 * down a column keeps multiplying by the rate in D1.
 *
 * Structural edits ignore the dollars for the opposite reason, and the two
 * rules live side by side in sheet.c so the difference is visible rather than
 * inferred.
 *
 * A reference pushed off the top or the left CLAMPS to row 1 or column A.
 * Real spreadsheets answer #REF!; the port this came from clamps, and an
 * unsigned subtraction left alone would silently produce row 65535.
 *
 * A label or a number is copied unchanged -- text does not mean something
 * different because it moved -- and the cell's FORMAT travels with it either
 * way, so replicating a currency column stays currency.
 *
 * OVERLAP IS HANDLED, by choosing which end to start from. Copying A1...A3
 * onto A2 while walking forwards would read A2 after writing it and smear the
 * first cell down the column.
 */
bool sheet_replicate(uint16_t r1, uint16_t c1, uint16_t r2, uint16_t c2,
                     uint16_t tr1, uint16_t tc1, uint16_t tr2, uint16_t tc2);

/* "A1" or "A1...B5" -- kalk's notation, THREE dots, the same one expr.h
   parses inside @SUM. Normalised so the first corner is the top left, and
   false for anything with rubbish after it. */
bool sheet_parse_range(const char *s, uint16_t *r1, uint16_t *c1,
                       uint16_t *r2, uint16_t *c2);

#define SHEET_OK        0
#define SHEET_ENOCARD   1       /* no filesystem                            */
#define SHEET_ENOPATH   2       /* the name would not resolve               */
#define SHEET_ENOFILE   3       /* not there, or not creatable              */
#define SHEET_EIO       4       /* the transfer failed part way             */
#define SHEET_EBIG      5       /* more rows or columns than the grid has   */

uint8_t sheet_error(void);

#endif /* KALK_SHEET_H */
