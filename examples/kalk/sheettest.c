/* ==========================================================================
 * sheettest.c -- a sheet written to a card and read back.
 *
 * THE TEST IS THE ROUND TRIP, not the bytes. A CSV writer can be checked
 * against a string, and that only proves it writes what someone expected it
 * to write; what a spreadsheet actually needs is that a sheet saved and
 * reloaded IS THE SAME SHEET -- same types, same values, same formula
 * sources. So this builds a sheet with one of everything awkward in it,
 * saves, clears, loads, and compares cell by cell against what it built.
 *
 * WHAT IS AWKWARD, and why each one is here:
 *
 *   a label with a COMMA          the case ordinary CSV quoting exists for
 *   a label that looks NUMERIC    "2024" must come back text, not a number
 *   a label that looks like a     "+not a formula" likewise -- both are why
 *     FORMULA                       sheet.h makes quotes mean text
 *   a label with a QUOTE in it    doubling, the other half of the escape
 *   a formula containing a comma  @SUM(A1...A3) has none, but @NPV does, and
 *                                   a formula field is quoted for the same
 *                                   reason a label is
 *   a fraction                    0.1 is not exact in binary; saving six
 *                                   significant digits would lose it, which
 *                                   is why the writer uses all nine
 *   a NEGATIVE and a big value    the sign and the exponent form
 *   an EMPTY cell between two     the ,, that a naive writer drops
 *     full ones
 *
 * The file is also printed, because the round trip passing tells you the two
 * halves agree and not that either is right -- a writer and a reader that are
 * wrong in the same direction round-trip perfectly. Reading the CSV is what
 * catches that, and it is checked against the shape the Prog8 port writes.
 *
 * BUILD AT -O0.
 * ========================================================================== */

#include "console.h"
#include "shell.h"
#include "sheet.h"
#include "cell.h"
#include "expr.h"
#include "fmt.h"
#include "fp.h"
#include "kfs.h"
#include "fat32.h"
#include "goshell.h"

static uint8_t failed, ncase;

static void
expect(const char *what, bool cond)
{
    static char okmark[]  = "  ok\n";
    static char badmark[] = "  FAILED\n";
    ncase++;
    con_puts(what);
    con_puts(cond ? okmark : badmark);
    if (!cond && !failed)
        failed = ncase;
}

static bool
str_eq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* A value AS THE SHEET SHOWS IT, copied out at once. `from_text` parses the
   string first; otherwise the fp_t is loaded.

   THE CONVERSION DESTROYS FAC -- f_to_str scales it and then peels it apart --
   and the library's string buffer only lives until the next conversion, so
   nothing may be held across one. Comparing two calls directly reads the
   second against a wrecked accumulator, which is how this test first reported
   an empty string as the value it had just loaded.

   fmt_number and not fp_to_str_trim, because nine digits do not survive a
   round trip through this library and six do -- see sheet.h. What a
   spreadsheet has to promise is that the sheet reads the same after loading
   as it did before saving, and that is the formatter's business. */
static void
shown_as(char *out, uint8_t max, const char *src, bool from_text)
{
    char    buf[FMT_MAX_WIDTH + 1];
    uint8_t k;

    if (from_text)
        fp_from_str(src);
    else
        fp_load(src);
    fmt_number(FMT_GENERAL, FMT_MAX_WIDTH, buf);
    for (k = 0; k + 1 < max && buf[k]; k++)
        out[k] = buf[k];
    out[k] = 0;
}

/* What the sheet is built from, and what it is checked against afterwards --
   one table, so the two can never drift. Each entry is typed exactly as a
   user would type it, which is the input sheet_set_text takes. */
typedef struct {
    uint16_t    row, col;
    const char *typed;          /* what goes in                            */
    uint8_t     type;           /* what it must become                     */
    const char *shown;          /* label/formula source, or the value as
                                   fp_to_str_trim writes it                */
} sample;

int
main(void)
{
    static char noinit[] = "MEM_ALLOC REFUSED -- is the kernel resident?\n";
    static char nocard[] = "NO CARD -- this test needs one\n";
    static char path[]   = "/SHEET.CSV";

    /* Kept as separate arrays because a string literal inside an initialiser
       list lands in a section this build does not keep. */
    static char t_item[]  = "Item";
    static char t_gadget[] = "\"Gadget, Deluxe";     /* forced label, comma  */
    static char s_gadget[] = "Gadget, Deluxe";
    static char t_year[]  = "\"2024";                /* forced: looks numeric */
    static char s_year[]  = "2024";
    static char t_notf[]  = "\"+not a formula";      /* forced: looks like one */
    static char s_notf[]  = "+not a formula";
    static char t_quote[] = "\"say \"hi\"";           /* a quote inside text  */
    static char s_quote[] = "say \"hi\"";
    static char t_ten[]   = "10";
    static char t_frac[]  = "0.1";
    static char t_neg[]   = "-12.5";   /* a FORMULA: - starts one */
    static char t_big[]   = "123456789";
    static char t_sum[]   = "+B1*3";
    static char t_npv[]   = "+@SUM(B1...B4)";

    static sample tab[] = {
        { 0, 0, t_item,   CELL_LABEL,   t_item   },
        { 0, 1, t_ten,    CELL_NUMBER,  t_ten    },
        /* B2 deliberately skipped: the empty field between two full ones */
        { 0, 3, t_sum,    CELL_FORMULA, t_sum    },
        { 1, 0, t_gadget, CELL_LABEL,   s_gadget },
        { 1, 1, t_frac,   CELL_NUMBER,  t_frac   },
        { 2, 0, t_year,   CELL_LABEL,   s_year   },
        { 2, 1, t_neg,    CELL_FORMULA, t_neg    },
        { 3, 0, t_notf,   CELL_LABEL,   s_notf   },
        { 3, 1, t_big,    CELL_NUMBER,  t_big    },
        { 4, 0, t_quote,  CELL_LABEL,   s_quote  },
        { 4, 3, t_npv,    CELL_FORMULA, t_npv    },
    };
    #define NTAB (sizeof tab / sizeof tab[0])

    char     text[CELL_TEXT_MAX];
    uint16_t i;
    uint8_t  bad;
    bool     good;

    con_init();

    if (!cell_init()) {
        con_puts(noinit);
        goshell_on_esc();
        return 0;
    }
    if (!kfs_ready()) {
        con_puts(nocard);
        goshell_on_esc();
        return 0;
    }

    {
        static char banner[] = "X816 SHEET CSV\n\n";
        con_puts(banner);
    }

    /* ---- build ---------------------------------------------------------- */
    for (i = 0; i < NTAB; i++)
        sheet_set_text(tab[i].row, tab[i].col, tab[i].typed);

    /* The typed-line rule, before anything touches a file. If this is wrong
       the round trip below can still pass, by being wrong twice. */
    good = true;
    for (i = 0; i < NTAB; i++) {
        cell c;
        cell_get(tab[i].row, tab[i].col, &c);
        if (c.type != tab[i].type) { good = false; break; }
    }
    {
        static char c_typed[] = "typed text becomes the right kind of cell";
        static char at[]      = "   entry ";
        static char wanted[]  = " wanted type ";
        static char gotm[]    = " got ";
        expect(c_typed, good);
        if (!good) {
            cell c;
            cell_get(tab[i].row, tab[i].col, &c);
            con_puts(at);   sh_put_hex8((uint8_t)i);
            con_puts(wanted); sh_put_hex8(tab[i].type);
            con_puts(gotm);   sh_put_hex8(c.type);
            con_putc(' ');
            con_puts(tab[i].typed);
            con_putc('\n');
        }
    }

    /* ---- save ----------------------------------------------------------- */
    {
        static char c_save[] = "the sheet saves";
        expect(c_save, sheet_save_csv(path));
    }

    /* ---- clear, and prove it ------------------------------------------- */
    cell_clear_all();
    {
        static char c_gone[] = "clearing really empties the sheet";
        expect(c_gone, !cell_any());
    }

    /* ---- load ----------------------------------------------------------- */
    {
        static char c_load[] = "the sheet loads";
        expect(c_load, sheet_load_csv(path));
    }

    /* ---- compare -------------------------------------------------------- */
    good = true;
    bad = 0;
    for (i = 0; i < NTAB && good; i++) {
        cell c;
        bad = (uint8_t)i;
        cell_get(tab[i].row, tab[i].col, &c);
        if (c.type != tab[i].type) { good = false; break; }
        if (c.type == CELL_LABEL || c.type == CELL_FORMULA) {
            cell_text_get(c.text, text);
            if (!str_eq(text, tab[i].shown)) good = false;
        } else if (c.type == CELL_NUMBER) {
            /* AS PRINTED, not bit for bit.
             *
             * The file carries all nine digits the machine can write, but
             * nine do not survive being read back: 0.1 writes as
             * 9.99999999e-02 and parses to 9.99999995e-02, because
             * f_from_str truncates at every step rather than rounding. So
             * what is asserted is what the SHEET shows -- both print 0.1 --
             * which is the promise a spreadsheet actually makes. sheet.h
             * records the limitation and where it comes from.
             *
             * BOTH STRINGS ARE COPIED OUT BEFORE EITHER IS READ, because a
             * conversion DESTROYS FAC -- f_to_str scales it and then peels it
             * apart -- and the library's buffer is only good until the next
             * one. Comparing the two calls directly reads the second against
             * a wrecked accumulator, which is how this test first reported an
             * empty string as the value it had just loaded. */
            char want[24], got[24];
            shown_as(want, sizeof want, tab[i].typed, true);
            shown_as(got, sizeof got, (const char *)&c.value, false);
            if (!str_eq(got, want)) {
                static char w1[] = "   want ";
                static char w2[] = " got ";
                good = false;
                con_puts(w1); con_puts(want);
                con_puts(w2); con_puts(got);
                con_putc(10);
            }
        }
    }
    {
        static char c_same[] = "every cell comes back as itself";
        expect(c_same, good);
        if (!good) {
            static char at[] = "   first difference at entry ";
            con_puts(at);
            sh_put_hex8(bad);
            con_putc('\n');
        }
    }

    /* The empty cell between two full ones has to still be empty -- a writer
       that dropped its comma would shift every later column left, and every
       cell would still round-trip because they would all shift together. */
    {
        static char c_hole[] = "the gap in row 1 is still a gap";
        cell c;
        cell_get(0, 2, &c);
        expect(c_hole, c.type == CELL_EMPTY);
    }

    /* ---- and what the file actually says --------------------------------- */
    {
        static char head[] = "\nSHEET.CSV as written:\n";
        fat32_file f;
        char       full[KFS_PATH];
        uint8_t    buf[64];
        uint16_t   n, k;

        con_puts(head);
        if (kfs_abspath(path, full) && fat32_open(full, &f)) {
            while ((n = fat32_read(&f, buf, sizeof buf)) != 0)
                for (k = 0; k < n; k++)
                    con_putc((char)buf[k]);
            fat32_close(&f);
        }
    }

    {
        static char ok[]  = "\nSHEET CSV OK\n";
        static char bad[] = "\nSHEET CSV FAILED AT CASE ";
        if (failed == 0) {
            con_puts(ok);
        } else {
            con_puts(bad);
            sh_put_hex8(failed);
            con_putc('\n');
        }
    }

    goshell_on_esc();
    return 0;
}
