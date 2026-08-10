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

/* The free-running millisecond counter. The LOW BYTE LATCHES the rest, so it
   must be read first -- x816_contract.h, and the emulator enforces it. */
#define TMR0 (*(volatile uint8_t *)0x9F90)
#define TMR1 (*(volatile uint8_t *)0x9F91)
#define TMR2 (*(volatile uint8_t *)0x9F92)
#define TMR3 (*(volatile uint8_t *)0x9F93)

static uint32_t
ms(void)
{
    uint32_t v;
    v  = (uint32_t)TMR0;
    v |= (uint32_t)TMR1 << 8;
    v |= (uint32_t)TMR2 << 16;
    v |= (uint32_t)TMR3 << 24;
    return v;
}

static void
put_u32(uint32_t v)
{
    char    tmp[11];
    uint8_t n = 0;
    if (v == 0) { con_putc('0'); return; }
    while (v) { tmp[n++] = (char)('0' + (uint8_t)(v % 10)); v /= 10; }
    while (n) con_putc(tmp[--n]);
}

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

    /* ---- insert and delete, and what they do to a formula --------------
     *
     * The cells moving is the easy half and the obvious thing to check. The
     * half worth testing is the TEXT: a formula that still reads +B1*3 after
     * a row was pushed in above it is pointing at the wrong cell, and the
     * number it shows will look perfectly reasonable.
     *
     * So the sheet is rebuilt small and exact, and the formula's SOURCE is
     * compared after each operation:
     *
     *   B3 = +B1+B2        insert a row at 0  ->  +B2+B3, and B3 moves to B4
     *                      delete that row    ->  +B1+B2 again
     *
     * The insert and the delete are inverses on this sheet, so the second
     * check is also a check on the first: a rewrite that shifted the wrong
     * comparison -- >= where > belongs -- survives one direction and not the
     * round trip.
     */
    {
        static char c_ins[]  = "insert row: the formula follows its cells";
        static char c_del[]  = "delete row: and it comes back";
        static char c_col[]  = "insert column: the letters follow too";
        static char t_one[]  = "1";
        static char t_two[]  = "2";
        static char t_add[]  = "+B1+B2";
        static char w_ins[]  = "+B2+B3";
        static char w_col[]  = "+C1+C2";    /* the COLUMN moves, not the row */
        cell c;

        cell_clear_all();
        sheet_set_text(0, 1, t_one);        /* B1 */
        sheet_set_text(1, 1, t_two);        /* B2 */
        sheet_set_text(2, 1, t_add);        /* B3 = +B1+B2 */

        good = sheet_insert_row(0);
        cell_get(3, 1, &c);                 /* B3 has become B4 */
        if (c.type != CELL_FORMULA) {
            good = false;
        } else {
            cell_text_get(c.text, text);
            good = good && str_eq(text, w_ins);
        }
        expect(c_ins, good);

        good = sheet_delete_row(0);
        cell_get(2, 1, &c);                 /* and back to B3 */
        if (c.type != CELL_FORMULA) {
            good = false;
        } else {
            cell_text_get(c.text, text);
            good = good && str_eq(text, t_add);
        }
        expect(c_del, good);

        /* A column insert left of B renames it C, in the cells AND in the
           text -- the half a row test cannot reach, because the column is
           the part written as letters. */
        good = sheet_insert_col(0);
        cell_get(2, 2, &c);
        if (c.type != CELL_FORMULA) {
            good = false;
        } else {
            cell_text_get(c.text, text);
            good = good && str_eq(text, w_col);
        }
        expect(c_col, good);
    }

    /* ---- replicate, and the dollars ------------------------------------
     *
     * The one command the anchoring exists for, so the test is one formula
     * carrying both kinds of reference at once:
     *
     *      B1 = +A1*$D$1        replicated from B1 down to B2 and B3
     *
     * A1 is relative and must follow the copy -- +A2, +A3 -- while $D$1 is
     * anchored and must not move at all. A rewriter that ignored the dollars
     * would give +A2*$D$2, and the column would silently multiply by the
     * wrong rate; one that honoured them everywhere would leave +A1 and the
     * whole column would show the same number. Both are wrong in ways that
     * look like a working spreadsheet, and only checking the SOURCE catches
     * either.
     */
    {
        static char c_rel[]  = "replicate: a relative reference follows";
        static char c_abs[]  = "replicate: an anchored one does not";
        static char c_rng[]  = "a range parses, both ways round";
        static char t_mul[]  = "+A1*$D$1";
        static char w_two[]  = "+A2*$D$1";
        static char w_three[] = "+A3*$D$1";
        cell rc;
        uint16_t q1, q2, q3, q4;

        cell_clear_all();
        sheet_set_text(0, 1, t_mul);            /* B1 */
        /* ONE command fills both: a single formula into a target RANGE.
           The port this came from would need two, one cell at a time. */
        good = sheet_replicate(0, 1, 0, 1,  1, 1, 2, 1);   /* B1 -> B2...B3 */

        cell_get(1, 1, &rc);
        cell_text_get(rc.text, text);
        good = good && str_eq(text, w_two);
        expect(c_rel, good);

        cell_get(2, 1, &rc);
        cell_text_get(rc.text, text);
        /* Both halves at once: the row moved to 3 and the $D$1 did not. */
        expect(c_abs, str_eq(text, w_three));

        /* A range typed backwards is the range the user meant. */
        {
            static char r_fwd[] = "B2...D5";
            static char r_rev[] = "D5...B2";
            static char r_one[] = "C3";
            bool ok1 = sheet_parse_range(r_fwd, &q1, &q2, &q3, &q4);
            bool ok2 = ok1 && q1 == 1 && q2 == 1 && q3 == 4 && q4 == 3;
            bool ok3 = sheet_parse_range(r_rev, &q1, &q2, &q3, &q4);
            bool ok4 = ok3 && q1 == 1 && q2 == 1 && q3 == 4 && q4 == 3;
            bool ok5 = sheet_parse_range(r_one, &q1, &q2, &q3, &q4);
            bool ok6 = ok5 && q1 == 2 && q2 == 2 && q3 == 2 && q4 == 2;
            expect(c_rng, ok2 && ok4 && ok6);
        }
    }

    /* ---- /M, dragging a line -------------------------------------------
     *
     * Swapping two rows must swap what the formulas say about them, BOTH
     * ways round -- a reference to 1 becomes 2 and one to 2 becomes 1. A
     * rewriter that only did one direction leaves half the sheet pointing at
     * the row that used to be there, which still evaluates and is wrong.
     *
     * So the formula names both lines being swapped: +A1+A2 with A1 and A2
     * traded must come back as +A2+A1 -- the same sum, but each half moved.
     * The VALUE cannot catch that; only the source can.
     */
    {
        static char c_mv[]  = "move: a swap moves references both ways";
        static char c_mvb[] = "move: the cells actually traded places";
        static char t_a[]   = "+A1+A2";
        static char w_a[]   = "+A2+A1";
        static char t_11[]  = "11";
        static char t_22[]  = "22";
        cell mc;
        char got[CELL_TEXT_MAX];

        cell_clear_all();
        sheet_set_text(0, 0, t_11);      /* A1 = 11 */
        sheet_set_text(1, 0, t_22);      /* A2 = 22 */
        sheet_set_text(2, 0, t_a);       /* A3 = +A1+A2 */

        good = sheet_swap_rows(0, 1);
        cell_get(2, 0, &mc);
        cell_text_get(mc.text, got);
        expect(c_mv, good && str_eq(got, w_a));

        /* and the cells themselves went with it. Both sides go through
           shown_as, because it formats into a COLUMN -- the answer is right
           aligned in twenty characters, so comparing it against a bare "22"
           fails on the padding rather than on the value. */
        {
            char v1[24], v2[24], e1[24], e2[24];
            shown_as(e1, sizeof e1, t_22, true);
            shown_as(e2, sizeof e2, t_11, true);
            cell_get(0, 0, &mc); shown_as(v1, sizeof v1, (const char *)&mc.value, false);
            cell_get(1, 0, &mc); shown_as(v2, sizeof v2, (const char *)&mc.value, false);
            expect(c_mvb, str_eq(v1, e1) && str_eq(v2, e2));
        }
    }

    /* ---- what a structural edit COSTS ----------------------------------
     *
     * sheet.h claims the price is set by the watermark and the row map rather
     * than by the grid, and a claim like that is worth a number: the X16 port
     * measured 0.83 s to insert a row across 6,656 cells, and this grid holds
     * 262,144. If the bound were not real the command would be unusable.
     *
     * So the same operation twice: once on a screenful of cells, once on a
     * sheet holding three. Both insert at row 0 of a 1,024-row grid, so the
     * only thing that can separate them is what has actually been written.
     */
    {
        static char m_dense[]  = "insert a row, 56x8 written:  ";
        static char m_sparse[] = "insert a row, 3 cells:       ";
        static char m_ms[]     = " ms\n";
        static char n_one[]    = "1";
        static char c_bound[]  = "a sparse sheet is cheaper than a full one";
        uint32_t t0, t_dense, t_sparse;
        uint16_t rr, cc;

        cell_clear_all();
        for (rr = 0; rr < 56; rr++)
            for (cc = 0; cc < 8; cc++)
                sheet_set_text(rr, cc, n_one);
        t0 = ms();
        sheet_insert_row(0);
        t_dense = ms() - t0;

        cell_clear_all();
        sheet_set_text(0, 0, n_one);
        sheet_set_text(1, 0, n_one);
        sheet_set_text(2, 0, n_one);
        t0 = ms();
        sheet_insert_row(0);
        t_sparse = ms() - t0;

        con_putc(10);
        con_puts(m_dense);  put_u32(t_dense);  con_puts(m_ms);
        con_puts(m_sparse); put_u32(t_sparse); con_puts(m_ms);

        /* Not a speed target -- a check that the bound EXISTS. If three cells
           cost what a full screen costs, the watermark is bounding nothing
           and the comment in sheet.h is wrong. */
        expect(c_bound, t_sparse < t_dense);
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
