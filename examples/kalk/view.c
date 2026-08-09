/* ==========================================================================
 * view.c -- drawing the sheet. The layout, and why it is this layout, is in
 * view.h.
 *
 * BUILD AT -O0.
 * ========================================================================== */

#include "view.h"
#include "cell.h"
#include "fmt.h"
#include "fp.h"
#include "console.h"

/* Colours. Two attributes and nothing else: the console carries ONE at a
   time (console.h calls it a pen), so anything with its own colour has to be
   drawn in its own pass. Headers get the bright one, the sheet the plain one,
   and the cursor cell is drawn last in reverse. */
#define FG_TEXT   7
#define BG_TEXT   0
#define FG_HEAD   15
#define BG_HEAD   0
#define FG_CUR    0
#define BG_CUR    7

/* Per-column overrides, 0 meaning "use the global". 256 bytes of bank $00 out
   of the 28 KB a program gets, which buys /F on any column without a
   side table to search. */
static uint8_t width_of[KALK_COLS];
static uint8_t width_global = VIEW_WIDTH_DEF;

/* The format a cell gets when it has none of its own. cell.fmt of 0 means a
   cell nobody has formatted, and FMT_DEFAULT ('D') is /F D -- the user
   ASKING for the global one back. Both land here, which is what makes /GF
   change a whole sheet and /F D undo a /F on one cell. */
static uint8_t fmt_global = FMT_GENERAL;

static uint16_t cur_row, cur_col;
static uint16_t top_row, left_col;

/* One screen row, built whole and then written. Building the line first is
   what makes spilling possible at all -- a label has to be allowed to write
   past its own column before the next cell decides whether to cut it. */
static char line[CON_COLS + 1];

/* ---- the render cache ----------------------------------------------------
 *
 * WHAT IT HOLDS: the finished 80-character screen line for a sheet row, one
 * per row of the whole sheet, in the BRAM that x816-kalk.scm reserves.
 *
 * WHY PER ROW AND NOT PER CELL. The commit that deferred this said "keyed per
 * cell", and the measurement says otherwise. run-bench.sh splits a repaint of
 * 448 cells four ways:
 *
 *      grid reads          135 ms      5%
 *      formatting        2,247 ms     81%
 *      spill lookahead     328 ms     12%
 *      VERA writes          54 ms      2%
 *
 * Formatting was 92% of that when the decision was taken, and the other three
 * figures have not moved a millisecond -- what changed is that float.s stopped
 * peeling decimal digits with a float divide each. The argument for a per-row
 * cache only got stronger: the lookahead is now an eighth of a repaint rather
 * than a twentieth.
 *
 * A per-cell cache removes the 81% but not the 12%, because the lookahead is
 * not a property of a cell: view.h's own rule is that a cell's appearance
 * depends on its NEIGHBOURS, so composing a line means walking right from
 * every label until an occupied cell stops it, whatever is cached about the
 * cells themselves. The line is the smallest unit that owns its whole cost.
 * It is also the unit that fits: 262,144 cells of rendered text is megabytes
 * and does not, while 1,024 rows of 80 characters is 80 KB of the 192 KB
 * sitting empty, and needs no tags, no hashing and no eviction.
 *
 * VERTICAL SCROLLING IS THE POINT. The cached line is keyed by SHEET row and
 * does not depend on top_row -- the gutter number is the row's own and the
 * columns are decided by left_col -- so scrolling down one line repaints 56
 * rows of which 55 are hits. That is the case that was overrunning the
 * keyboard FIFO.
 *
 * WHAT INVALIDATES IT, which is the only interesting part of a cache:
 *
 *      that row's cells changed     view_dirty_row  -- kalk.c, on commit,
 *                                   on blank, and per row inside recalc
 *      left_col moved               view_dirty_all  -- here, in view_move_to
 *      any column width changed     view_dirty_all  -- here
 *      the sheet was replaced       view_dirty_all  -- callers of
 *                                   cell_clear_all, and view_init
 *
 * top_row is deliberately NOT on that list. Anything else that can change
 * what a row looks like belongs on it.
 *
 * The contents are never read before they are written, because view_init
 * clears every valid bit -- which is what lets this live in bss and cost the
 * image file nothing.
 */
/* IN TWO HALVES, and not for any reason to do with the machine: Calypsi
   refuses a single object larger than 65,535 bytes whatever memory it lives
   in, and 1,024 rows of 80 characters is 81,920. Two objects of 40,960 are
   accepted, the linker places both in FastRAM, and row_slot is the only code
   that ever has to know. Anything that wants a row's bytes asks it. */
#define RC_HALF (KALK_ROWS / 2)

static char __far rendered_lo[RC_HALF][CON_COLS];
static char __far rendered_hi[RC_HALF][CON_COLS];

static char __far *
row_slot(uint16_t row)
{
    return (row < RC_HALF) ? rendered_lo[row] : rendered_hi[row - RC_HALF];
}

/* The valid bits, in bank $00 rather than beside the data they describe:
   128 bytes, tested once per row drawn, and a far read to decide whether to
   do a far read is a poor trade. */
static uint8_t fresh[KALK_ROWS / 8];

static bool
row_fresh(uint16_t row)
{
    return (fresh[row >> 3] & (uint8_t)(1u << (row & 7))) != 0;
}

void
view_dirty_row(uint16_t row)
{
    if (row < KALK_ROWS)
        fresh[row >> 3] &= (uint8_t)~(1u << (row & 7));
}

void
view_dirty_all(void)
{
    uint16_t i;
    for (i = 0; i < KALK_ROWS / 8; i++)
        fresh[i] = 0;
}

void
view_init(void)
{
    uint16_t i;
    for (i = 0; i < KALK_COLS; i++)
        width_of[i] = 0;
    width_global = VIEW_WIDTH_DEF;
    fmt_global = FMT_GENERAL;
    cur_row = cur_col = 0;
    top_row = left_col = 0;
    view_dirty_all();
}

uint8_t
view_width(uint16_t col)
{
    uint8_t w;
    if (col >= KALK_COLS)
        return width_global;
    w = width_of[col] ? width_of[col] : width_global;
    if (w < VIEW_WIDTH_MIN) w = VIEW_WIDTH_MIN;
    if (w > VIEW_WIDTH_MAX) w = VIEW_WIDTH_MAX;
    return w;
}

/* Both width setters throw the whole cache away, and not just the column's
   own: a width moves every column to its right, so every cached line is now
   describing a layout that no longer exists. */
void
view_set_width(uint16_t col, uint8_t w)
{
    if (col >= KALK_COLS)
        return;
    if (w < VIEW_WIDTH_MIN) w = VIEW_WIDTH_MIN;
    if (w > VIEW_WIDTH_MAX) w = VIEW_WIDTH_MAX;
    width_of[col] = w;
    view_dirty_all();
}

void
view_set_global_width(uint8_t w)
{
    if (w < VIEW_WIDTH_MIN) w = VIEW_WIDTH_MIN;
    if (w > VIEW_WIDTH_MAX) w = VIEW_WIDTH_MAX;
    width_global = w;
    view_dirty_all();
}

uint8_t
view_global_fmt(void)
{
    return fmt_global;
}

/* Every unformatted cell on the sheet changes appearance, so the whole cache
   goes -- the same reasoning as a width, and cheaper to think about than
   working out which rows happened to contain one. */
void
view_set_global_fmt(uint8_t code)
{
    if (code == 0 || code == FMT_DEFAULT)
        code = FMT_GENERAL;         /* the global cannot itself be "global" */
    fmt_global = code;
    view_dirty_all();
}

bool
view_col_x(uint16_t col, uint8_t *x)
{
    uint16_t c;
    uint16_t at = VIEW_GUTTER;

    if (col < left_col)
        return false;
    for (c = left_col; c < col; c++) {
        at = (uint16_t)(at + view_width(c));
        if (at >= CON_COLS)
            return false;
    }
    /* A column that starts on screen but has no room for even one character
       is not on screen. Drawing it would put a stub of a number under the
       previous column's header. */
    if (at >= CON_COLS)
        return false;
    *x = (uint8_t)at;
    return true;
}

bool
view_col_at(uint8_t x, uint16_t *col)
{
    uint16_t c = left_col;
    uint16_t at = VIEW_GUTTER;

    if (x < VIEW_GUTTER)
        return false;                   /* the gutter is not a column */
    while (c < KALK_COLS) {
        uint16_t next = (uint16_t)(at + view_width(c));
        if (x < next) {
            *col = c;
            return true;
        }
        if (next >= CON_COLS)
            return false;
        at = next;
        c++;
    }
    return false;
}

bool
view_row_at(uint8_t y, uint16_t *row)
{
    uint16_t r;
    if (y < VIEW_TOP_ROW || y > VIEW_BOT_ROW)
        return false;                   /* status, headers and help */
    r = (uint16_t)(top_row + (y - VIEW_TOP_ROW));
    if (r >= KALK_ROWS)
        return false;
    *row = r;
    return true;
}

uint16_t view_cur_row(void)  { return cur_row; }
uint16_t view_cur_col(void)  { return cur_col; }
uint16_t view_top_row(void)  { return top_row; }
uint16_t view_left_col(void) { return left_col; }

/* Scrolls by the least that brings the cell into view, which is what makes
   arrow-key movement feel like a spreadsheet rather than like paging. */
bool
view_move_to(uint16_t row, uint16_t col)
{
    uint16_t old_top = top_row, old_left = left_col;
    uint8_t  x;

    if (row >= KALK_ROWS || col >= KALK_COLS)
        return false;

    cur_row = row;
    cur_col = col;

    if (row < top_row)
        top_row = row;
    else if (row >= (uint16_t)(top_row + VIEW_ROWS))
        top_row = (uint16_t)(row - VIEW_ROWS + 1);

    if (col < left_col)
        left_col = col;
    else {
        /* Widths vary, so "how many columns fit" is not a constant: pull the
           left edge right one column at a time until the cursor's column
           fits whole. A cursor half off the edge is worse than a scroll. */
        while (!view_col_x(col, &x)
               || (uint16_t)(x + view_width(col)) > CON_COLS) {
            if (left_col >= col)
                break;                  /* wider than the screen: show it */
            left_col++;
        }
    }

    /* A sideways scroll is the expensive one: every cached line was composed
       against the old left_col and describes the wrong columns now. A
       vertical scroll costs nothing, because a cached line is keyed by sheet
       row and says nothing about where on screen it goes -- which is the
       whole reason arrowing down a long sheet is now free. */
    if (left_col != old_left)
        view_dirty_all();

    return (top_row != old_top) || (left_col != old_left);
}

/* ---- names --------------------------------------------------------------- */

/* A..Z then AA..IV. 256 columns is exactly what two letters reach with the
   VisiCalc scheme, which is why kalk.c chose it. */
static uint8_t
col_name(uint16_t col, char *out)
{
    if (col < 26) {
        out[0] = (char)('A' + col);
        out[1] = '\0';
        return 1;
    }
    col -= 26;
    out[0] = (char)('A' + (col / 26));
    out[1] = (char)('A' + (col % 26));
    out[2] = '\0';
    return 2;
}

static uint8_t
put_u16(uint16_t v, char *out)
{
    char    tmp[6];
    uint8_t n = 0, i = 0;

    if (v == 0) { out[0] = '0'; out[1] = '\0'; return 1; }
    while (v) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n) out[i++] = tmp[--n];
    out[i] = '\0';
    return i;
}

void
view_cell_name(uint16_t row, uint16_t col, char *out)
{
    uint8_t n = col_name(col, out);
    put_u16((uint16_t)(row + 1), out + n);      /* rows are 1-based on screen */
}

/* ---- drawing -------------------------------------------------------------- */

static void
blank_line(void)
{
    uint8_t i;
    for (i = 0; i < CON_COLS; i++)
        line[i] = ' ';
    line[CON_COLS] = '\0';
}

/* con_putraw and not con_puts: putraw interprets nothing and moves no cursor,
   so a line that happens to contain a control code cannot scroll the screen
   out from under the renderer. console.h says as much. */
static void
show_line(uint8_t y, uint8_t from, uint8_t to)
{
    if (to > CON_COLS)
        to = CON_COLS;
    if (from >= to)
        return;
    con_putrun(from, y, line + from, (uint8_t)(to - from));
}

static void
put_at(uint8_t x, const char *s, uint8_t max)
{
    uint8_t i;
    for (i = 0; i < max && s[i] && (uint16_t)(x + i) < CON_COLS; i++)
        line[x + i] = s[i];
}

/* Builds `line` for a sheet row and nothing else -- no screen, no cache. This
   is the 93%: every number in the row goes through fmt_number, and every
   label walks its right-hand neighbours to find out how far it may run. */
static void
compose_row(uint16_t row)
{
    char     buf[VIEW_WIDTH_MAX + 1];
    char     text[CELL_TEXT_MAX];
    cell     c;
    uint16_t col;
    uint8_t  x;
    uint8_t  spill_to = 0;      /* first screen column a spill has claimed  */

    /* No test against top_row anywhere below, and that is a property worth
       stating: what a row looks like depends on its cells, on left_col and on
       the widths, and NOT on where the sheet is scrolled to vertically. It is
       what makes a cached line survive a scroll. */
    blank_line();

    /* the gutter: the row number, right-aligned, then a space */
    {
        char n[6];
        uint8_t len = put_u16((uint16_t)(row + 1), n);
        if (len < VIEW_GUTTER)
            put_at((uint8_t)(VIEW_GUTTER - 1 - len), n, len);
    }

    for (col = left_col; col < KALK_COLS; col++) {
        uint8_t w;

        if (!view_col_x(col, &x))
            break;
        w = view_width(col);

        cell_get(row, col, &c);

        /* A cell the previous label ran across draws nothing of its own --
           but it still had to be LOOKED AT, because whether it is empty is
           what decided how far that label was allowed to run. */
        if (x < spill_to) {
            if (c.type == CELL_EMPTY)
                continue;
        }

        if (c.type == CELL_EMPTY)
            continue;

        if (c.type == CELL_LABEL) {
            uint16_t avail = w;
            uint16_t look;

            cell_text_get(c.text, text);

            /* Run on through empty neighbours, and stop at the first one that
               is not: that is exactly where the text would have collided. */
            for (look = col + 1; look < KALK_COLS; look++) {
                cell nb;
                uint8_t nx;
                if (!view_col_x(look, &nx))
                    break;
                cell_get(row, look, &nb);
                if (nb.type != CELL_EMPTY)
                    break;
                avail = (uint16_t)(avail + view_width(look));
            }
            if ((uint16_t)(x + avail) > CON_COLS)
                avail = (uint16_t)(CON_COLS - x);

            put_at(x, text, (uint8_t)avail);
            spill_to = (uint8_t)(x + avail);
            continue;
        }

        if (c.flags & CELL_ERROR)
            fmt_error(w, buf);
        else if (c.flags & CELL_NA)
            fmt_na(w, buf);
        else {
            /* The cell's own format, or the sheet's. FMT_DEFAULT is a cell
               that has been told explicitly to use the global one, so it
               resolves here rather than being stored as a copy -- otherwise
               /GF would not reach it. */
            uint8_t code = c.fmt;
            if (code == 0 || code == FMT_DEFAULT)
                code = fmt_global;
            fp_load(&c.value);
            fmt_number(code, w, buf);
        }
        put_at(x, buf, w);
    }
}

/* A miss composes the row and files it; a hit goes from BRAM to the screen.
 *
 * WHAT `line` HOLDS AFTERWARDS, which is a contract and not an accident:
 * THE CURSOR'S ROW, and only that. view_draw_cursor re-emits the cursor
 * cell's characters out of `line` to change their attribute, so the row it
 * highlights has to be the one sitting there -- and every caller already
 * draws cur_row immediately before asking for the highlight, which is what
 * makes the narrow guarantee enough.
 *
 * The other fifty-five rows go straight out of the cache without being staged
 * anywhere, because staging them cost more than writing them did. */
void
view_draw_row(uint16_t row)
{
    uint8_t y, i;

    if (row < top_row || row >= (uint16_t)(top_row + VIEW_ROWS))
        return;
    y = (uint8_t)(VIEW_TOP_ROW + (row - top_row));

    {
        /* The row's slot, addressed ONCE. Indexing through row_slot inside a
           loop would recompute a 24-bit base from a 32-bit product eighty
           times over, at -O0, for eighty bytes. */
        char __far *slot = row_slot(row);

        if (!row_fresh(row)) {
            compose_row(row);
            for (i = 0; i < CON_COLS; i++)
                slot[i] = line[i];
            fresh[row >> 3] |= (uint8_t)(1u << (row & 7));
            show_line(y, 0, CON_COLS);
        } else if (row == cur_row) {
            /* Only the cursor's row is staged through `line`, and only
               because view_draw_cursor re-emits its characters from there to
               change their attribute. */
            for (i = 0; i < CON_COLS; i++)
                line[i] = slot[i];
            show_line(y, 0, CON_COLS);
        } else {
            /* BRAM straight to VERA. Staging this through bank $00 first cost
               more than the writing did -- run-bench.sh put a fully cached
               repaint at 124 ms against 54 ms of actual VERA traffic, and the
               difference was fifty-five rows copied twice for no reader. */
            con_putrun_far(0, y, slot, CON_COLS);
        }
    }
}

/* The column letters, centred over their columns -- which is what makes a
   header look like a header rather than a label. */
static void
draw_headers(void)
{
    uint16_t col;
    uint8_t  x;
    char     nm[4];

    blank_line();
    for (col = left_col; col < KALK_COLS; col++) {
        uint8_t w, n, pad;
        if (!view_col_x(col, &x))
            break;
        w = view_width(col);
        n = col_name(col, nm);
        pad = (uint8_t)((w > n) ? (w - n) / 2 : 0);
        put_at((uint8_t)(x + pad), nm, n);
    }
    con_color(FG_HEAD, BG_HEAD);
    show_line(VIEW_HEAD_ROW, 0, CON_COLS);
    con_color(FG_TEXT, BG_TEXT);
}

void
view_draw_status(void)
{
    static char ready[] = "READY";
    char     nm[8];
    char     text[CELL_TEXT_MAX];
    cell     c;
    uint8_t  i;

    blank_line();

    view_cell_name(cur_row, cur_col, nm);
    put_at(1, nm, 7);

    /* What the cell IS, not what it shows: a formula's source rather than its
       result, which is the whole reason a status line exists. */
    cell_get(cur_row, cur_col, &c);
    if (c.type == CELL_LABEL || c.type == CELL_FORMULA) {
        cell_text_get(c.text, text);
        put_at(6, text, (uint8_t)(CON_COLS - 6 - 8));
    } else if (c.type == CELL_NUMBER) {
        char buf[VIEW_WIDTH_MAX + 1];
        fp_load(&c.value);
        fmt_number(FMT_GENERAL, VIEW_WIDTH_MAX, buf);
        for (i = 0; buf[i] == ' '; i++)
            ;
        put_at(6, buf + i, VIEW_WIDTH_MAX);
    }

    put_at((uint8_t)(CON_COLS - 6), ready, 5);

    con_color(FG_HEAD, BG_HEAD);
    show_line(VIEW_STATUS_ROW, 0, CON_COLS);
    con_color(FG_TEXT, BG_TEXT);
}

/* Drawn LAST and on top, because the console carries one attribute at a time.
   Redrawing the cell's span in reverse is cheaper than a second full pass and
   is what lets a cursor move repaint two rows instead of the screen. */
void
view_draw_cursor(void)
{
    uint8_t x, w, i;

    if (!view_col_x(cur_col, &x))
        return;
    if (cur_row < top_row || cur_row >= (uint16_t)(top_row + VIEW_ROWS))
        return;

    w = view_width(cur_col);
    if ((uint16_t)(x + w) > CON_COLS)
        w = (uint8_t)(CON_COLS - x);

    /* view_draw_row has already put the characters down; this only changes
       the attribute they were written with, so it has to re-emit them. The
       line buffer still holds the row that was drawn last, which is why the
       caller draws the cursor's row immediately before calling this. */
    con_color(FG_CUR, BG_CUR);
    for (i = 0; i < w; i++)
        con_putraw((uint8_t)(x + i), (uint8_t)(VIEW_TOP_ROW + (cur_row - top_row)),
                   (uint8_t)line[x + i]);
    con_color(FG_TEXT, BG_TEXT);
}

void
view_draw(void)
{
    uint16_t r;

    /* No con_cls. Every row on screen is painted by somebody -- rows 0, 2
       and 3..58 here, the entry line and the help line by the caller -- so a
       clear is 16,384 VERA writes that are immediately overwritten. It also
       made a full repaint cost about 110 ms, which is long enough to overrun
       the keyboard FIFO while somebody is typing. */
    draw_headers();
    for (r = top_row; r < (uint16_t)(top_row + VIEW_ROWS) && r < KALK_ROWS; r++)
        view_draw_row(r);
    /* the cursor's row again, so `line` holds it when the highlight goes on */
    view_draw_row(cur_row);
    view_draw_cursor();
    view_draw_status();
}
