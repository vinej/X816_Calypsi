/* ==========================================================================
 * sheet.c -- setting a cell from text, and CSV both ways. The rules, and why
 * they are these rules, are in sheet.h.
 *
 * BUILD AT -O0.
 * ========================================================================== */

#include "sheet.h"
#include "cell.h"
#include "expr.h"
#include "fp.h"
#include "fat32.h"
#include "kfs.h"

static uint8_t sheet_err;

uint8_t
sheet_error(void)
{
    return sheet_err;
}

/* ---- the typed-line rule -------------------------------------------------
 *
 * kalk.c's entry line and the CSV loader both come through here, which is the
 * only way the two can be guaranteed to agree. See sheet.h.
 */
bool
sheet_set_text(uint16_t row, uint16_t col, const char *s)
{
    cell c;

    if (row >= KALK_ROWS || col >= KALK_COLS)
        return false;

    cell_get(row, col, &c);
    c.flags = 0;

    if (s[0] == '\0') {
        c.type = CELL_EMPTY;
        c.text = 0;
        fp_zero();
        fp_store(&c.value);
        cell_put(row, col, &c);
        return true;
    }

    if (s[0] == '"') {
        s++;                            /* forced label: the quote is syntax */
        c.type = CELL_LABEL;
        c.text = cell_text_put(s);
    } else if (expr_is_formula(s)) {
        c.type = CELL_FORMULA;
        c.text = cell_text_put(s);
        /* Evaluated once so the cell shows something immediately; whatever it
           depends on is settled by the caller's recalculation. */
        {
            uint8_t st = expr_eval(s, row, col);
            if (st == EXPR_ERROR) { c.flags |= CELL_ERROR; fp_zero(); }
            else if (st == EXPR_NA) { c.flags |= CELL_NA; fp_zero(); }
            fp_store(&c.value);
        }
    } else if (fp_from_str(s)) {
        c.type = CELL_NUMBER;
        c.text = 0;
        fp_store(&c.value);
    } else {
        /* Not a number and not a formula, so it is a label -- which is how
           `Total` and `12 units` both end up as text without a mode. */
        c.type = CELL_LABEL;
        c.text = cell_text_put(s);
    }

    cell_put(row, col, &c);
    return true;
}

/* ---- writing -------------------------------------------------------------
 *
 * Buffered, because fat32_write is a read-modify-write of a whole sector
 * (fat32.h says so) and a cell at a time would re-read and re-write the same
 * sector for every comma on the row.
 */
#define OUTBUF 256

static fat32_file out_f;
static uint8_t    outbuf[OUTBUF];
static uint16_t   outn;

/* A SHORT write is a failure, and the distinction fat32.h draws between its
   two causes does not matter here: with the error flag set the device failed,
   without it the volume filled up, and either way the file on the card is not
   the sheet. Testing only fat32_ioerr() would call a full card a success. */
static bool
out_flush(void)
{
    uint16_t want = outn, wrote;

    if (want == 0)
        return true;
    wrote = fat32_write(&out_f, outbuf, want);
    outn = 0;
    return wrote == want;
}

static bool
out_ch(char ch)
{
    if (outn >= OUTBUF && !out_flush())
        return false;
    outbuf[outn++] = (uint8_t)ch;
    return true;
}

static bool
out_str(const char *s)
{
    while (*s)
        if (!out_ch(*s++))
            return false;
    return true;
}

/* True if this label, written bare, would come back as something other than a
   label -- which is the whole reason sheet.h makes quotes mean text. */
static bool
needs_quotes(const char *s)
{
    uint16_t i;

    if (s[0] == '\0')
        return false;
    for (i = 0; s[i]; i++)
        if (s[i] == ',' || s[i] == '"')
            return true;            /* escaping, in the ordinary CSV sense */
    if (expr_is_formula(s))
        return true;                /* would reload as a formula */
    if (fp_from_str(s))
        return true;                /* would reload as a number */
    return false;
}

static bool
out_field_quoted(const char *s)
{
    if (!out_ch('"'))
        return false;
    while (*s) {
        if (*s == '"' && !out_ch('"'))      /* doubled, as CSV wants */
            return false;
        if (!out_ch(*s++))
            return false;
    }
    return out_ch('"');
}

bool
sheet_save_csv(const char *path)
{
    char     full[KFS_PATH];
    char     text[CELL_TEXT_MAX];
    uint16_t r, col, maxr, maxc;

    sheet_err = SHEET_OK;
    outn = 0;

    if (!kfs_ready())            { sheet_err = SHEET_ENOCARD; return false; }
    if (!kfs_abspath(path, full)){ sheet_err = SHEET_ENOPATH; return false; }
    fat32_clearerr();
    if (!fat32_create(full, &out_f)) { sheet_err = SHEET_ENOFILE; return false; }

    /* An empty sheet is an empty file, not an error and not a grid of commas:
       the watermark is meaningless when nothing has been written, which
       cell.h is explicit about. */
    if (cell_any()) {
        maxr = cell_max_row();
        maxc = cell_max_col();

        for (r = 0; r <= maxr; r++) {
            for (col = 0; col <= maxc; col++) {
                cell c;

                if (col && !out_ch(','))
                    goto io_error;

                /* A row nothing was ever written to is all empty fields, and
                   answering that without reading 256 cells is the whole point
                   of the row map. */
                if (cell_row_empty(r))
                    continue;

                cell_get(r, col, &c);
                switch (c.type) {
                case CELL_LABEL:
                    cell_text_get(c.text, text);
                    if (needs_quotes(text)) {
                        if (!out_field_quoted(text)) goto io_error;
                    } else if (!out_str(text)) {
                        goto io_error;
                    }
                    break;

                case CELL_FORMULA:
                    /* The SOURCE, not the result. Never quoted: it starts
                       with +, - , ( or @, so it cannot be mistaken for text,
                       and a comma inside @SUM(D2...D5) would need escaping --
                       which is why a formula containing one is quoted too. */
                    cell_text_get(c.text, text);
                    {
                        uint16_t i;
                        bool comma = false;
                        for (i = 0; text[i]; i++)
                            if (text[i] == ',' || text[i] == '"')
                                comma = true;
                        if (comma) {
                            if (!out_field_quoted(text)) goto io_error;
                        } else if (!out_str(text)) {
                            goto io_error;
                        }
                    }
                    break;

                case CELL_NUMBER:
                    /* fp_to_str_trim, not the formatter: the formatter is six
                       significant digits and a column width, which is how a
                       sheet loses precision every time it is saved. This is
                       all nine digits the value actually has. */
                    fp_load(&c.value);
                    if (!out_str(fp_to_str_trim()))
                        goto io_error;
                    break;

                default:
                    break;                      /* empty: an empty field */
                }
            }
            if (!out_ch('\n'))
                goto io_error;
        }
    }

    if (!out_flush())
        goto io_error;
    /* Nothing is durable until close -- fat32.h is explicit that the
       directory entry is what carries the size, so a save that ignored this
       would leave a file the card believes is empty. */
    if (!fat32_close(&out_f) || fat32_ioerr()) {
        sheet_err = SHEET_EIO;
        return false;
    }
    return true;

io_error:
    fat32_close(&out_f);
    sheet_err = SHEET_EIO;
    return false;
}

/* ---- reading -------------------------------------------------------------
 *
 * One pass, a byte at a time out of a block buffer, with the field
 * accumulated into a cell-sized string. A whole file will not fit in bank $00
 * -- a full sheet is megabytes -- so there is nothing to be gained by reading
 * it in one go even if it did.
 */
bool
sheet_load_csv(const char *path)
{
    char       full[KFS_PATH];
    char       field[CELL_TEXT_MAX];
    fat32_file f;
    uint8_t    buf[128];
    uint16_t   n, i, len = 0;
    uint16_t   row = 0, col = 0;
    bool       quoted = false;      /* inside "..."                         */
    bool       was_quoted = false;  /* this field had quotes: it is TEXT    */
    bool       pending = false;     /* a quote seen inside quotes           */

    sheet_err = SHEET_OK;

    if (!kfs_ready())             { sheet_err = SHEET_ENOCARD; return false; }
    if (!kfs_abspath(path, full)) { sheet_err = SHEET_ENOPATH; return false; }
    fat32_clearerr();
    if (!fat32_open(full, &f))    { sheet_err = SHEET_ENOFILE; return false; }

    /* Cleared FIRST, so a load that fails part way leaves an obviously empty
       sheet rather than a plausible half of one. sheet.h promises this. */
    cell_clear_all();

    for (;;) {
        n = fat32_read(&f, buf, sizeof buf);
        if (n == 0)
            break;

        for (i = 0; i < n; i++) {
            char ch = (char)buf[i];

            if (quoted) {
                if (pending) {
                    pending = false;
                    if (ch == '"') {            /* "" is one literal quote */
                        if (len + 1 < CELL_TEXT_MAX) field[len++] = '"';
                        continue;
                    }
                    quoted = false;             /* the quote closed the field */
                    /* fall through and treat ch normally */
                } else if (ch == '"') {
                    pending = true;
                    continue;
                } else {
                    if (len + 1 < CELL_TEXT_MAX) field[len++] = ch;
                    continue;
                }
            }

            if (ch == '"' && len == 0) {
                quoted = true;
                was_quoted = true;
                continue;
            }
            if (ch == ',' || ch == '\n') {
                field[len] = '\0';
                if (len || was_quoted) {
                    /* A quoted field is TEXT, whatever it looks like -- the
                       decision sheet.h argues for. Prefixing the quote is how
                       that is said in the one vocabulary sheet_set_text has,
                       and it is the same character a user types to force a
                       label. */
                    if (was_quoted) {
                        char forced[CELL_TEXT_MAX];
                        uint16_t k;
                        forced[0] = '"';
                        for (k = 0; k + 2 < CELL_TEXT_MAX && field[k]; k++)
                            forced[k + 1] = field[k];
                        forced[k + 1] = '\0';
                        if (!sheet_set_text(row, col, forced))
                            goto too_big;
                    } else if (!sheet_set_text(row, col, field)) {
                        goto too_big;
                    }
                }
                len = 0;
                was_quoted = false;
                if (ch == ',') {
                    col++;
                    if (col >= KALK_COLS) goto too_big;
                } else {
                    col = 0;
                    row++;
                    if (row >= KALK_ROWS) goto too_big;
                }
                continue;
            }
            if (ch == '\r')
                continue;                       /* a file written on a PC */
            if (len + 1 < CELL_TEXT_MAX)
                field[len++] = ch;
        }
    }

    /* A last line with no newline after it is still a line. */
    if (len || was_quoted) {
        field[len] = '\0';
        if (!sheet_set_text(row, col, field))
            goto too_big;
    }

    fat32_close(&f);
    if (fat32_ioerr()) {
        cell_clear_all();
        sheet_err = SHEET_EIO;
        return false;
    }
    return true;

too_big:
    fat32_close(&f);
    cell_clear_all();
    sheet_err = SHEET_EBIG;
    return false;
}

/* ---- structural edits ----------------------------------------------------
 *
 * Moving the cells is the easy half. The half that makes it a spreadsheet
 * operation rather than a memmove is rewriting what the formulas SAY, and
 * sheet.h argues the two rules that are not obvious -- that the dollars are
 * ignored, and what happens to a reference into a deleted line.
 */

/* A1-style name into `out`, returning its length. The same A..Z then AA..IV
   scheme view.c draws in the headers, and the same one expr.c parses; a third
   opinion about what column 26 is called is how a rewritten formula starts
   pointing one column over. */
static uint8_t
put_ref(char *out, uint16_t row, uint16_t col, bool ac, bool ar)
{
    char    tmp[6];
    uint8_t n = 0, i = 0, d = 0;
    uint16_t v;

    if (ac)
        out[n++] = '$';
    if (col < 26) {
        out[n++] = (char)('A' + col);
    } else {
        uint16_t c2 = (uint16_t)(col - 26);
        out[n++] = (char)('A' + (c2 / 26));
        out[n++] = (char)('A' + (c2 % 26));
    }
    if (ar)
        out[n++] = '$';

    v = (uint16_t)(row + 1);            /* rows are 1-based in a reference */
    if (v == 0) { out[n++] = '0'; return n; }
    while (v) { tmp[d++] = (char)('0' + (v % 10)); v /= 10; }
    while (d) out[n + i++] = tmp[--d];
    return (uint8_t)(n + i);
}

/* HOW A REFERENCE MOVES, and there are two quite different reasons it does.
 *
 * RW_SHIFT is a structural edit: a line was inserted or removed and every
 * reference past it has to follow. The DOLLARS ARE IGNORED, because $B$4
 * names the cell B4 and if B4 itself moved then so did what the reference
 * means. sheet.h argues this at length.
 *
 * RW_OFFSET is a replicate: the formula is being copied somewhere else and
 * its relative references should point the same way RELATIVE TO THE COPY.
 * Here the dollars are the entire point -- an anchored component is what the
 * user wrote to say "not this one".
 *
 * One walker, two rules, because the walking is the fiddly part and having it
 * twice is how the two commands come to disagree about what $A1 means. */
#define RW_SHIFT  0
#define RW_OFFSET 1
#define RW_SWAP   2             /* /M: two lines trade places               */

typedef struct {
    uint8_t  mode;
    char     axis;              /* RW_SHIFT and RW_SWAP: 'R' or 'C'         */
    uint16_t at;                /* the line inserted, removed, or moved     */
    uint16_t to;                /* RW_SWAP: the line it trades with         */
    bool     ins;
    int16_t  drow, dcol;        /* RW_OFFSET: how far the copy moved        */
} rw_rule;

/* INSERT SHIFTS AT THE LINE, DELETE SHIFTS PAST IT: inserting at row 4 moves
   everything from row 4 down, so a reference TO row 4 becomes row 5; deleting
   row 4 moves everything after it up, and a reference to row 4 itself is left
   alone because there is nothing left to point at. Getting those two
   comparisons the same way round is the whole of the bug this can have. */
static bool
ref_apply(const rw_rule *w, uint16_t *row, uint16_t *col, bool ac, bool ar)
{
    if (w->mode == RW_SWAP) {
        /* Two lines trade places, so the references to them do too. Neither
           is "before" the other afterwards, which is why this is a swap and
           not a pair of shifts: doing it as a delete and an insert would
           drag every line between them along as well. */
        uint16_t *v = (w->axis == 'R') ? row : col;

        if (*v == w->at) { *v = w->to; return true; }
        if (*v == w->to) { *v = w->at; return true; }
        return false;
    }

    if (w->mode == RW_SHIFT) {
        uint16_t *v = (w->axis == 'R') ? row : col;
        uint16_t  lim = (w->axis == 'R') ? KALK_ROWS : KALK_COLS;

        if (w->ins) {
            if (*v >= w->at && *v + 1 < lim) { (*v)++; return true; }
        } else {
            if (*v > w->at) { (*v)--; return true; }
        }
        return false;
    }

    /* RW_OFFSET. An anchored component does not move -- that is what the
       dollar was typed for. A reference that would go off the top or the left
       CLAMPS to zero rather than wrapping to the far edge, which is what an
       unsigned subtraction would otherwise do and which would turn +A1
       replicated upwards into a reference to row 65535. */
    {
        bool moved = false;

        if (!ar && w->drow) {
            int32_t v = (int32_t)*row + w->drow;
            if (v < 0) v = 0;
            if (v >= KALK_ROWS) v = KALK_ROWS - 1;
            *row = (uint16_t)v;
            moved = true;
        }
        if (!ac && w->dcol) {
            int32_t v = (int32_t)*col + w->dcol;
            if (v < 0) v = 0;
            if (v >= KALK_COLS) v = KALK_COLS - 1;
            *col = (uint16_t)v;
            moved = true;
        }
        return moved;
    }
}

/* Rewrite every reference in `src` into `dst`. False means it would not fit,
   and then `dst` holds nothing worth having. */
static bool
rewrite_refs(char *dst, const char *src, const rw_rule *w, bool *changed)
{
    uint16_t si = 0, di = 0;

    *changed = false;
    while (src[si]) {
        uint16_t r, c;
        bool     ac, ar;
        uint8_t  n = expr_parse_ref(src + si, &r, &c, &ac, &ar);

        if (n == 0) {
            /* Not a reference here: copy one character and look again at the
               next. Walking character by character is what keeps SUM in
               @SUM(A1...A4) from being read as a column name -- it has no
               digits after it, so the parser declines it. */
            if (di + 1 >= CELL_TEXT_MAX)
                return false;
            dst[di++] = src[si++];
            continue;
        }

        if (ref_apply(w, &r, &c, ac, ar))
            *changed = true;

        /* A reference is at most $IV$1024, eight characters. */
        if (di + 9 >= CELL_TEXT_MAX)
            return false;
        di += put_ref(dst + di, r, c, ac, ar);
        si += n;
    }
    dst[di] = '\0';
    return true;
}

/* Every formula in the live range, after the cells have moved. */
static void
fix_formulas(const rw_rule *w)
{
    char     src[CELL_TEXT_MAX];
    char     dst[CELL_TEXT_MAX];
    uint16_t r, c, maxr, maxc;

    if (!cell_any())
        return;
    maxr = cell_max_row();
    maxc = cell_max_col();

    for (r = 0; r <= maxr; r++) {
        if (cell_row_empty(r))
            continue;
        for (c = 0; c <= maxc; c++) {
            cell cl;
            bool changed;

            cell_get(r, c, &cl);
            if (cl.type != CELL_FORMULA)
                continue;

            cell_text_get(cl.text, src);
            if (!rewrite_refs(dst, src, w, &changed)) {
                /* It no longer fits. Keep the text it had -- which is still
                   readable and still says what the user wrote -- and flag it,
                   so the cell shows ERROR rather than a stale answer. */
                cl.flags |= CELL_ERROR;
                cell_put(r, c, &cl);
                continue;
            }
            if (!changed)
                continue;
            cl.text = cell_text_put(dst);
            cell_put(r, c, &cl);
        }
    }
}

/* An empty cell, for blanking a line that has been moved out of. */
static void
put_empty(uint16_t row, uint16_t col)
{
    cell cl;
    cell_get(row, col, &cl);
    cl.type = CELL_EMPTY;
    cl.flags = 0;
    cl.text = 0;
    fp_zero();
    fp_store(&cl.value);
    cell_put(row, col, &cl);
}

static void
copy_cell(uint16_t dr, uint16_t dc, uint16_t sr, uint16_t sc)
{
    cell cl;
    cell_get(sr, sc, &cl);
    cell_put(dr, dc, &cl);
}

bool
sheet_insert_row(uint16_t at)
{
    uint16_t r, c, maxr, maxc;

    if (at >= KALK_ROWS)
        return false;
    if (!cell_any())
        return true;                    /* nothing to move */
    maxr = cell_max_row();
    maxc = cell_max_col();
    if (maxr >= KALK_ROWS - 1)
        maxr = KALK_ROWS - 2;           /* the last row falls off the bottom */

    for (r = maxr + 1; r-- > at; ) {
        /* Two empty rows need nothing done to them, and asking is one bitmap
           test against reading a row of cells. On a sparse sheet this is the
           difference between a command and a pause. */
        if (cell_row_empty(r) && cell_row_empty(r + 1))
            continue;
        for (c = 0; c <= maxc; c++)
            copy_cell(r + 1, c, r, c);
    }
    if (!cell_row_empty(at))
        for (c = 0; c <= maxc; c++)
            put_empty(at, c);

    {
        rw_rule w;
        w.mode = RW_SHIFT; w.axis = 'R'; w.at = at; w.ins = true;
        w.drow = 0; w.dcol = 0;
        fix_formulas(&w);
    }
    return true;
}

bool
sheet_delete_row(uint16_t at)
{
    uint16_t r, c, maxr, maxc;

    if (at >= KALK_ROWS)
        return false;
    if (!cell_any())
        return true;
    maxr = cell_max_row();
    maxc = cell_max_col();

    for (r = at; r < maxr; r++) {
        if (cell_row_empty(r) && cell_row_empty(r + 1))
            continue;
        for (c = 0; c <= maxc; c++)
            copy_cell(r, c, r + 1, c);
    }
    if (!cell_row_empty(maxr))
        for (c = 0; c <= maxc; c++)
            put_empty(maxr, c);

    {
        rw_rule w;
        w.mode = RW_SHIFT; w.axis = 'R'; w.at = at; w.ins = false;
        w.drow = 0; w.dcol = 0;
        fix_formulas(&w);
    }
    return true;
}

bool
sheet_insert_col(uint16_t at)
{
    uint16_t r, c, maxr, maxc;

    if (at >= KALK_COLS)
        return false;
    if (!cell_any())
        return true;
    maxr = cell_max_row();
    maxc = cell_max_col();
    if (maxc >= KALK_COLS - 1)
        maxc = KALK_COLS - 2;           /* the last column falls off the edge */

    for (r = 0; r <= maxr; r++) {
        if (cell_row_empty(r))
            continue;                   /* the row map earns its keep here */
        for (c = maxc + 1; c-- > at; )
            copy_cell(r, c + 1, r, c);
        put_empty(r, at);
    }

    {
        rw_rule w;
        w.mode = RW_SHIFT; w.axis = 'C'; w.at = at; w.ins = true;
        w.drow = 0; w.dcol = 0;
        fix_formulas(&w);
    }
    return true;
}

bool
sheet_delete_col(uint16_t at)
{
    uint16_t r, c, maxr, maxc;

    if (at >= KALK_COLS)
        return false;
    if (!cell_any())
        return true;
    maxr = cell_max_row();
    maxc = cell_max_col();

    for (r = 0; r <= maxr; r++) {
        if (cell_row_empty(r))
            continue;
        for (c = at; c < maxc; c++)
            copy_cell(r, c, r, c + 1);
        put_empty(r, maxc);
    }

    {
        rw_rule w;
        w.mode = RW_SHIFT; w.axis = 'C'; w.at = at; w.ins = false;
        w.drow = 0; w.dcol = 0;
        fix_formulas(&w);
    }
    return true;
}


/* ---- moving a line -------------------------------------------------------
 *
 * /M drags the row or column under the cursor with the arrow keys, one step
 * at a time. Each step is a SWAP with its neighbour rather than a delete and
 * an insert -- which matters for the references: swapping rows 4 and 5 means
 * a formula naming 4 now names 5 and vice versa, while everything else stays
 * where it was. A delete-then-insert would renumber every row between the
 * two, and there is no "between" when they are adjacent anyway.
 */
static void
swap_cells(uint16_t ar, uint16_t ac, uint16_t br, uint16_t bc)
{
    cell x, y;
    cell_get(ar, ac, &x);
    cell_get(br, bc, &y);
    cell_put(ar, ac, &y);
    cell_put(br, bc, &x);
}

bool
sheet_swap_rows(uint16_t a, uint16_t b)
{
    uint16_t c, maxc;
    rw_rule  w;

    if (a >= KALK_ROWS || b >= KALK_ROWS || a == b)
        return false;
    if (cell_any()) {
        maxc = cell_max_col();
        /* Two rows nobody has written to have nothing to trade, and the row
           map answers that without reading a cell. */
        if (!cell_row_empty(a) || !cell_row_empty(b))
            for (c = 0; c <= maxc; c++)
                swap_cells(a, c, b, c);
    }
    w.mode = RW_SWAP; w.axis = 'R'; w.at = a; w.to = b;
    w.ins = false; w.drow = 0; w.dcol = 0;
    fix_formulas(&w);
    return true;
}

bool
sheet_swap_cols(uint16_t a, uint16_t b)
{
    uint16_t r, maxr;
    rw_rule  w;

    if (a >= KALK_COLS || b >= KALK_COLS || a == b)
        return false;
    if (cell_any()) {
        maxr = cell_max_row();
        for (r = 0; r <= maxr; r++) {
            if (cell_row_empty(r))
                continue;
            swap_cells(r, a, r, b);
        }
    }
    w.mode = RW_SWAP; w.axis = 'C'; w.at = a; w.to = b;
    w.ins = false; w.drow = 0; w.dcol = 0;
    fix_formulas(&w);
    return true;
}

/* ---- replicate -----------------------------------------------------------
 *
 * The command the dollars exist for. sheet.h has the rules; this is the
 * mechanism.
 */

/* One cell of it. A formula is REWRITTEN, everything else is copied as it
   stands -- a label does not become a different label because it moved. The
   format travels with the cell either way, because a column of currency
   replicated across should still be currency. */
static void
replicate_cell(uint16_t sr, uint16_t sc, uint16_t dr, uint16_t dc)
{
    char src[CELL_TEXT_MAX];
    char dst[CELL_TEXT_MAX];
    cell c;

    if (sr == dr && sc == dc)
        return;
    cell_get(sr, sc, &c);

    if (c.type == CELL_FORMULA) {
        rw_rule w;
        bool    changed;

        w.mode = RW_OFFSET;
        w.axis = 0;
        w.at   = 0;
        w.ins  = false;
        w.drow = (int16_t)((int32_t)dr - (int32_t)sr);
        w.dcol = (int16_t)((int32_t)dc - (int32_t)sc);

        cell_text_get(c.text, src);
        if (rewrite_refs(dst, src, &w, &changed))
            c.text = cell_text_put(dst);
        else
            c.flags |= CELL_ERROR;      /* it no longer fits; say so */
    } else if (c.type == CELL_LABEL) {
        /* A FRESH copy of the text, not a shared offset. Two cells pointing
           at one arena record would come apart the moment either is edited --
           the arena is a bump allocator and an edit abandons the old string
           rather than updating it. */
        cell_text_get(c.text, src);
        c.text = cell_text_put(src);
    }

    cell_put(dr, dc, &c);
}

bool
sheet_replicate(uint16_t r1, uint16_t c1, uint16_t r2, uint16_t c2,
                uint16_t tr1, uint16_t tc1, uint16_t tr2, uint16_t tc2)
{
    uint16_t h, w, th, tw, i, j;

    if (r1 > r2 || c1 > c2 || tr1 > tr2 || tc1 > tc2)
        return false;
    if (r2 >= KALK_ROWS || c2 >= KALK_COLS)
        return false;
    if (tr2 >= KALK_ROWS || tc2 >= KALK_COLS)
        return false;

    h = (uint16_t)(r2 - r1 + 1);
    w = (uint16_t)(c2 - c1 + 1);

    /* A SINGLE CELL as the target means "put the block here"; a RANGE means
       "fill this with the block". Both are wanted and they are not the same
       command: A1...A3 onto B1 should land in B1..B3, while one formula onto
       B2...B4 should fill all three.

       The port this came from has only the first, so filling a column of
       totals there takes one command per cell. That is the commonest thing
       anyone does with a spreadsheet, so this adds the second -- and it is an
       ADDITION, because a single-cell target still behaves exactly as it
       does there. */
    if (tr1 == tr2 && tc1 == tc2) {
        th = h;
        tw = w;
    } else {
        th = (uint16_t)(tr2 - tr1 + 1);
        tw = (uint16_t)(tc2 - tc1 + 1);
    }

    /* THE DIRECTION IS CHOSEN, not fixed, and that is what makes an
       OVERLAPPING replicate work. Copying A1...A3 down onto A2 while walking
       forwards reads A2 after it has already been written -- the first cell
       smears down the column, which looks like a plausible result and is not
       one. Walking away from the overlap instead costs a comparison. */
    for (i = 0; i < th; i++) {
        uint16_t si = (tr1 > r1) ? (uint16_t)(th - 1 - i) : i;
        uint16_t dr = (uint16_t)(tr1 + si);

        if (dr >= KALK_ROWS)
            continue;
        for (j = 0; j < tw; j++) {
            uint16_t sj = (tc1 > c1) ? (uint16_t)(tw - 1 - j) : j;
            uint16_t dc = (uint16_t)(tc1 + sj);

            if (dc >= KALK_COLS)
                continue;
            /* The source repeats across a target bigger than itself, which is
               what lets one formula fill a whole column. */
            replicate_cell((uint16_t)(r1 + si % h), (uint16_t)(c1 + sj % w),
                           dr, dc);
        }
    }
    return true;
}

/* ---- ranges --------------------------------------------------------------
 *
 * "A1" or "A1...B5", which is kalk's notation -- THREE dots, because that is
 * what expr.h parses in a formula and a range typed at a prompt has to mean
 * the same thing as one typed inside @SUM.
 *
 * Normalised so the first corner is the top left, so a user who drags from
 * the bottom right gets the range they meant rather than an empty one.
 */
bool
sheet_parse_range(const char *s, uint16_t *r1, uint16_t *c1,
                  uint16_t *r2, uint16_t *c2)
{
    uint16_t ra, ca, rb, cb;
    bool     ac, ar;
    uint8_t  n;

    n = expr_parse_ref(s, &ra, &ca, &ac, &ar);
    if (n == 0)
        return false;
    s += n;

    if (s[0] == '.' && s[1] == '.' && s[2] == '.') {
        n = expr_parse_ref(s + 3, &rb, &cb, &ac, &ar);
        if (n == 0)
            return false;
        s += 3 + n;
    } else {
        rb = ra;
        cb = ca;
    }
    if (s[0] != '\0')
        return false;                   /* trailing rubbish is a typo */

    *r1 = (ra < rb) ? ra : rb;
    *r2 = (ra < rb) ? rb : ra;
    *c1 = (ca < cb) ? ca : cb;
    *c2 = (ca < cb) ? cb : ca;
    return true;
}
