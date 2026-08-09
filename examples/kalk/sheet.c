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

/* Which way a reference moves. `axis` is 'R' or 'C', `at` the line being
   inserted or removed, `ins` true for an insert.

   INSERT SHIFTS AT THE LINE, DELETE SHIFTS PAST IT: inserting at row 4 moves
   everything from row 4 down, so a reference TO row 4 becomes row 5; deleting
   row 4 moves everything after it up, and a reference to row 4 itself is left
   alone because there is nothing left to point at. Getting those two
   comparisons the same way round is the whole of the bug this can have. */
static bool
ref_shift(uint16_t *row, uint16_t *col, char axis, uint16_t at, bool ins)
{
    uint16_t *v = (axis == 'R') ? row : col;
    uint16_t  lim = (axis == 'R') ? KALK_ROWS : KALK_COLS;

    if (ins) {
        if (*v >= at && *v + 1 < lim) { (*v)++; return true; }
    } else {
        if (*v > at) { (*v)--; return true; }
    }
    return false;
}

/* Rewrite every reference in `src` into `dst`. False means it would not fit,
   and then `dst` holds nothing worth having. */
static bool
rewrite_refs(char *dst, const char *src, char axis, uint16_t at, bool ins,
             bool *changed)
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

        if (ref_shift(&r, &c, axis, at, ins))
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
fix_formulas(char axis, uint16_t at, bool ins)
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
            if (!rewrite_refs(dst, src, axis, at, ins, &changed)) {
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

    fix_formulas('R', at, true);
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

    fix_formulas('R', at, false);
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

    fix_formulas('C', at, true);
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

    fix_formulas('C', at, false);
    return true;
}
