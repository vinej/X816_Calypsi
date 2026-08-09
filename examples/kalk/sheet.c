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
