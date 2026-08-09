/* ==========================================================================
 * view.h -- the sheet on an 80x60 screen.
 *
 * The arrangement is VisiCalc's, which is kalk's, which is what anyone who
 * has used a spreadsheet expects to find:
 *
 *      row 0    A1  Item                                          READY
 *      row 1    (the entry line, while typing)
 *      row 2               A          B          C          D
 *      row 3     1 Item      Qty        Price      Total
 *      row 4     2 Widget A         10       4.99       49.9
 *       ...
 *      row 59   (help)
 *
 * so 56 rows of sheet and, at the default width of 9, eight columns of it.
 *
 * WHAT THIS OWNS AND WHAT IT DOES NOT
 * -----------------------------------
 * Everything about WHERE things are: column widths, the scroll offsets, the
 * cursor, and the mapping between a screen position and a cell. It owns no
 * values -- cell.h has those -- and it decides no formats, which is fmt.h's
 * job. It is the layer that knows the screen is 80 columns wide.
 *
 * ONE MAPPING, USED BY BOTH DRAWING AND HIT-TESTING. view_col_at and
 * view_row_at are not a convenience for a future mouse; they are the same
 * arithmetic the renderer uses, exposed. The Prog8 port makes a point of
 * this -- its hit test goes through the same col_at/row_at as drawing "so it
 * stays right when the sheet is scrolled" -- and two copies of that mapping
 * is exactly how a click starts landing one column off after a scroll.
 *
 * LABELS SPILL, NUMBERS DO NOT
 * ----------------------------
 * A label wider than its column runs on into the columns to its right for as
 * far as they are empty, and the first occupied neighbour cuts it off exactly
 * where the text would have collided. That is VisiCalc's behaviour and kalk's,
 * and it is the reason a row cannot be drawn one cell at a time: a cell's
 * appearance depends on its neighbours. Numbers are truncated to their column
 * instead, because a number that ran on would be read as a different number.
 *
 * THE RENDER CACHE, AND WHOSE JOB IT IS TO INVALIDATE IT
 * ------------------------------------------------------
 * Every sheet row's finished 80-character line is kept in the BRAM that
 * x816-kalk.scm reserves -- 1,024 rows of 80 bytes -- so a repaint that would
 * have reformatted a screenful of floats writes the characters straight out
 * instead. A cold 56-row repaint of dense numbers is 6.7 s; the same repaint
 * cached is 74 ms. run-bench.sh is where both figures and the formatting
 * split that decided the design come from, and view.c's header carries them.
 *
 * The cache invalidates ITSELF for everything it owns: a column width, a
 * global width, a sideways scroll, view_init. It cannot see a cell change,
 * so THE CALLER MUST SAY. Any code that writes through cell_put and expects
 * the screen to follow owes a view_dirty_row for that row -- kalk.c does it
 * on commit, on blank, and per changed row inside recalc -- and anything that
 * calls cell_clear_all owes a view_dirty_all.
 *
 * Forgetting shows up as a stale row that redraws correctly the moment
 * something else forces it, which is the kind of bug that survives a demo. If
 * you are unsure, view_dirty_all is always correct and merely slow.
 *
 * Vertical scrolling is deliberately NOT an invalidation: a cached line is
 * keyed by sheet row and holds no opinion about where on screen it lands.
 * ========================================================================== */

#ifndef KALK_VIEW_H
#define KALK_VIEW_H

#include <stdint.h>
#include <stdbool.h>

/* The screen's share-out. The gutter holds a right-aligned row number and a
   separating space; 1024 rows means four digits, so five. */
#define VIEW_STATUS_ROW  0
#define VIEW_ENTRY_ROW   1
#define VIEW_HEAD_ROW    2
#define VIEW_TOP_ROW     3      /* first row of sheet                       */
#define VIEW_BOT_ROW     58     /* last row of sheet                        */
#define VIEW_HELP_ROW    59
#define VIEW_GUTTER      5
#define VIEW_ROWS        (VIEW_BOT_ROW - VIEW_TOP_ROW + 1)

#define VIEW_WIDTH_MIN   4      /* what /GC accepts                         */
#define VIEW_WIDTH_MAX   20
#define VIEW_WIDTH_DEF   9

void view_init(void);

/* ---- geometry ------------------------------------------------------------ */
uint8_t  view_width(uint16_t col);              /* this column's width       */
void     view_set_width(uint16_t col, uint8_t w);   /* /F for one column     */
void     view_set_global_width(uint8_t w);      /* /GC                       */

/* The format a cell with none of its own is drawn in -- /GF. A cell.fmt of 0
   (never formatted) and of FMT_DEFAULT (/F D, asking for the global back)
   both resolve to this at draw time rather than being copied into the cell,
   which is what lets /GF reach a sheet that is already full. */
uint8_t  view_global_fmt(void);
void     view_set_global_fmt(uint8_t code);

/* Screen x of a column's first character, and whether it is on screen at all.
   The one place the layout arithmetic lives. */
bool     view_col_x(uint16_t col, uint8_t *x);

/* The reverse, for a click or a hit test. False for the gutter, the headers
   and the status lines -- which is what stops a click on a column letter
   selecting something. */
bool     view_col_at(uint8_t x, uint16_t *col);
bool     view_row_at(uint8_t y, uint16_t *row);

/* ---- where we are -------------------------------------------------------- */
uint16_t view_cur_row(void);
uint16_t view_cur_col(void);

/* Move the cursor, scrolling only as far as it takes to bring the new cell
   into view. Returns true if the scroll offsets moved, which is what tells
   the caller a whole repaint is needed rather than two rows. */
bool     view_move_to(uint16_t row, uint16_t col);

uint16_t view_top_row(void);
uint16_t view_left_col(void);

/* ---- the render cache ---------------------------------------------------- */

/* Throw away the cached line for one row. Owed by anything that changes what
   that row's cells contain -- see the header. Out-of-range rows are ignored,
   so a caller need not bound-check what cell_put would have refused anyway. */
void     view_dirty_row(uint16_t row);

/* Throw away all of them. Owed after cell_clear_all, or any change that
   cannot be pinned to particular rows. Always correct, never wrong, and it
   costs a full repaint. */
void     view_dirty_all(void);

/* ---- drawing ------------------------------------------------------------- */
void     view_draw(void);                       /* the lot                   */
void     view_draw_row(uint16_t row);           /* one sheet row             */
void     view_draw_cursor(void);                /* highlight, after a move   */
void     view_draw_status(void);

/* A1-style name of a cell, into `out` (at least 8 bytes): columns are A..Z
   then AA..IV, which is 256 of them, and the row is 1-based on screen. */
void     view_cell_name(uint16_t row, uint16_t col, char *out);

#endif /* KALK_VIEW_H */
