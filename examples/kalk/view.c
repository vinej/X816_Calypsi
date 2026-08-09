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

static uint16_t cur_row, cur_col;
static uint16_t top_row, left_col;

/* One screen row, built whole and then written. Building the line first is
   what makes spilling possible at all -- a label has to be allowed to write
   past its own column before the next cell decides whether to cut it. */
static char line[CON_COLS + 1];

void
view_init(void)
{
    uint16_t i;
    for (i = 0; i < KALK_COLS; i++)
        width_of[i] = 0;
    width_global = VIEW_WIDTH_DEF;
    cur_row = cur_col = 0;
    top_row = left_col = 0;
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

void
view_set_width(uint16_t col, uint8_t w)
{
    if (col >= KALK_COLS)
        return;
    if (w < VIEW_WIDTH_MIN) w = VIEW_WIDTH_MIN;
    if (w > VIEW_WIDTH_MAX) w = VIEW_WIDTH_MAX;
    width_of[col] = w;
}

void
view_set_global_width(uint8_t w)
{
    if (w < VIEW_WIDTH_MIN) w = VIEW_WIDTH_MIN;
    if (w > VIEW_WIDTH_MAX) w = VIEW_WIDTH_MAX;
    width_global = w;
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
    uint8_t x;
    for (x = from; x < to && x < CON_COLS; x++)
        con_putraw(x, y, (uint8_t)line[x]);
}

static void
put_at(uint8_t x, const char *s, uint8_t max)
{
    uint8_t i;
    for (i = 0; i < max && s[i] && (uint16_t)(x + i) < CON_COLS; i++)
        line[x + i] = s[i];
}

void
view_draw_row(uint16_t row)
{
    char     buf[VIEW_WIDTH_MAX + 1];
    char     text[CELL_TEXT_MAX];
    cell     c;
    uint16_t col;
    uint8_t  y, x;
    uint8_t  spill_to = 0;      /* first screen column a spill has claimed  */

    if (row < top_row || row >= (uint16_t)(top_row + VIEW_ROWS))
        return;
    y = (uint8_t)(VIEW_TOP_ROW + (row - top_row));

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
            fp_load(&c.value);
            fmt_number(c.fmt ? c.fmt : FMT_GENERAL, w, buf);
        }
        put_at(x, buf, w);
    }

    show_line(y, 0, CON_COLS);
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

    con_cls();
    draw_headers();
    for (r = top_row; r < (uint16_t)(top_row + VIEW_ROWS) && r < KALK_ROWS; r++)
        view_draw_row(r);
    /* the cursor's row again, so `line` holds it when the highlight goes on */
    view_draw_row(cur_row);
    view_draw_cursor();
    view_draw_status();
}
