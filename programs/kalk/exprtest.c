/* ==========================================================================
 * exprtest.c -- formulas, against a sheet with known values in it.
 *
 * The cases are kalk.c's own semantics, and several of them exist because a
 * spreadsheet parser can be wrong in ways that still produce a number:
 *
 *   PRECEDENCE. 1+2*3 is 7 here and 9 in VisiCalc, which evaluated strictly
 *   left to right. kalk.c has ordinary precedence, so a parser that walked
 *   left to right would agree with it on every single-operator formula and
 *   disagree on real ones.
 *
 *   OPERAND ORDER. 10-4 and 4-10 differ only in the order the parser feeds
 *   the accumulator, and the float package has both f_sub and f_rsub for
 *   exactly that reason. Getting it backwards is invisible until a
 *   subtraction is nested.
 *
 *   WHAT COUNTS AS A VALUE. @COUNT and @AVERAGE over a range with gaps must
 *   agree with @SUM about which cells are values. An @AVERAGE that divided by
 *   the size of the range rather than the number of values in it is the
 *   classic spreadsheet lie, and it reads perfectly plausibly.
 *
 *   ERROR BEATS NA. A formula that reads both must report ERROR. Since MFLPT
 *   has no NaN to carry either one, that precedence lives in the evaluator
 *   rather than in the arithmetic, so it is worth pinning.
 *
 * BUILD AT -O0.
 * ========================================================================== */

#include "console.h"
#include "shell.h"
#include "expr.h"
#include "cell.h"
#include "fmt.h"
#include "fp.h"
#include "goshell.h"

static uint8_t failed, ncase;

static bool
str_eq(const char *x, const char *y)
{
    while (*x && *y) { if (*x != *y) return false; x++; y++; }
    return *x == '\0' && *y == '\0';
}

/* Every case states the formula and the answer as a STRING, because that is
   what a cell shows and because comparing floats for equality would hide the
   truncation the format actually has. */
static void
check(const char *f, const char *want)
{
    static char arrow[] = " = ";
    static char wtag[]  = "   WANT ";
    static char okmark[] = "  ok\n";
    static char e_err[] = "ERROR", e_na[] = "NA";
    char got[FMT_MAX_WIDTH + 1];
    uint8_t st, i;

    ncase++;
    st = expr_eval(f, 0, 0);

    if (st == EXPR_ERROR) {
        for (i = 0; i < 5; i++) got[i] = e_err[i];
        got[5] = '\0';
    } else if (st == EXPR_NA) {
        got[0] = e_na[0]; got[1] = e_na[1]; got[2] = '\0';
    } else {
        char pad[FMT_MAX_WIDTH + 1];
        fmt_number(FMT_GENERAL, FMT_MAX_WIDTH, pad);
        for (i = 0; pad[i] == ' '; i++)
            ;
        for (st = 0; pad[i]; i++, st++)
            got[st] = pad[i];
        got[st] = '\0';
    }

    con_puts(f);
    con_puts(arrow);
    con_puts(got);
    if (str_eq(got, want)) {
        con_puts(okmark);
        return;
    }
    con_puts(wtag);
    con_puts(want);
    con_putc('\n');
    if (!failed)
        failed = ncase;
}

static void
put_num(uint16_t row, uint16_t col, const char *s)
{
    cell c;
    cell_get(row, col, &c);
    c.type = CELL_NUMBER;
    fp_from_str(s);
    fp_store(&c.value);
    cell_put(row, col, &c);
}

static void
put_flag(uint16_t row, uint16_t col, uint8_t flag)
{
    cell c;
    cell_get(row, col, &c);
    c.type = CELL_NUMBER;
    c.flags = flag;
    fp_zero();
    fp_store(&c.value);
    cell_put(row, col, &c);
}

static void
put_label(uint16_t row, uint16_t col, const char *s)
{
    cell c;
    cell_get(row, col, &c);
    c.type = CELL_LABEL;
    c.text = cell_text_put(s);
    cell_put(row, col, &c);
}

int
main(void)
{
    static char banner[] = "X816 FORMULAS\n\n";
    static char noinit[] = "MEM_ALLOC REFUSED -- is the kernel resident?\n";

    /* The fixture: A1..A5 = 10, 20, (a label), 40, 50; B1 = 4; C1 = ERROR,
       C2 = NA. The gap at A3 is what makes @COUNT and @AVERAGE meaningful. */
    static char v10[] = "10", v20[] = "20", v40[] = "40", v50[] = "50";
    static char v4[]  = "4";
    static char lab[] = "text";

    con_init();
    con_puts(banner);

    if (!cell_init()) {
        con_puts(noinit);
        goshell_on_esc();
    }

    put_num(0, 0, v10);          /* A1 */
    put_num(1, 0, v20);          /* A2 */
    put_label(2, 0, lab);        /* A3 -- a label, so not a value */
    put_num(3, 0, v40);          /* A4 */
    put_num(4, 0, v50);          /* A5 */
    put_num(0, 1, v4);           /* B1 */
    put_flag(0, 2, CELL_ERROR);  /* C1 */
    put_flag(1, 2, CELL_NA);     /* C2 */

    {
        /* --- arithmetic and precedence --- */
        static char f1[] = "1+2*3",        r1[] = "7";
        static char f2[] = "(1+2)*3",      r2[] = "9";
        static char f3[] = "10-4",         r3[] = "6";
        static char f4[] = "4-10",         r4[] = "-6";
        static char f5[] = "10/4",         r5[] = "2.5";
        static char f6[] = "-3+10",        r6[] = "7";
        static char f7[] = "2*3*4",        r7[] = "24";
        static char f8[] = "100/10/2",     r8[] = "5";
        /* --- references --- */
        static char f9[] = "+A1",          r9[] = "10";
        static char f10[] = "A1+A2",       r10[] = "30";
        static char f11[] = "$A$1*B1",     r11[] = "40";
        static char f12[] = "A1*$B1",      r12[] = "40";
        static char f13[] = "A3+A1",       r13[] = "10";   /* a label is 0 */
        static char f14[] = "Z9+A1",       r14[] = "10";   /* empty is 0   */
        /* --- ranges --- */
        static char f15[] = "@SUM(A1...A5)",     r15[] = "120";
        static char f16[] = "@MIN(A1...A5)",     r16[] = "10";
        static char f17[] = "@MAX(A1...A5)",     r17[] = "50";
        static char f18[] = "@COUNT(A1...A5)",   r18[] = "4";
        static char f19[] = "@AVERAGE(A1...A5)", r19[] = "30";
        static char f20[] = "@SUM(A5...A1)",     r20[] = "120"; /* reversed */
        static char f21[] = "@SUM(A1)",          r21[] = "10";  /* one value */
        /* --- value functions --- */
        static char f22[] = "@SQRT(16)",   r22[] = "4";
        static char f23[] = "@ABS(0-7)",   r23[] = "7";
        static char f24[] = "@INT(3.9)",   r24[] = "3";
        static char f25[] = "@LOG10(100)", r25[] = "2";
        static char f26[] = "@PI",         r26[] = "3.14159";
        /* --- errors --- */
        static char f27[] = "1/0",         r27[] = "ERROR";
        static char f28[] = "@SQRT(0-1)",  r28[] = "ERROR";
        static char f29[] = "@LN(0)",      r29[] = "ERROR";
        static char f30[] = "C1+1",        r30[] = "ERROR";
        static char f31[] = "C2+1",        r31[] = "NA";
        static char f32[] = "C1+C2",       r32[] = "ERROR"; /* ERROR wins */
        static char f33[] = "1+",          r33[] = "ERROR";
        static char f34[] = "1 2",         r34[] = "ERROR";
        static char f35[] = "@SUM(A1...A5",r35[] = "ERROR";
        static char f36[] = "@NOPE(1)",    r36[] = "ERROR";

        check(f1, r1);   check(f2, r2);   check(f3, r3);   check(f4, r4);
        check(f5, r5);   check(f6, r6);   check(f7, r7);   check(f8, r8);
        check(f9, r9);   check(f10, r10); check(f11, r11); check(f12, r12);
        check(f13, r13); check(f14, r14);
        check(f15, r15); check(f16, r16); check(f17, r17); check(f18, r18);
        check(f19, r19); check(f20, r20); check(f21, r21);
        check(f22, r22); check(f23, r23); check(f24, r24); check(f25, r25);
        check(f26, r26);
        check(f27, r27); check(f28, r28); check(f29, r29); check(f30, r30);
        check(f31, r31); check(f32, r32); check(f33, r33); check(f34, r34);
        check(f35, r35); check(f36, r36);
    }

    /* ---- the rest of VisiCalc's function set ---------------------------
     *
     * zserge's C original carries a handful of functions and VisiCalc had
     * rather more. These four are the gap, and the expected values are the
     * Prog8 port's own assertions rather than ones invented here -- the two
     * ports have to agree about what @LOOKUP means or a sheet moved between
     * them says something different.
     */
    {
        static char n0[]  = "0",   n10[] = "10",  n20[] = "20";
        static char n100[] = "100", n200[] = "200", n300[] = "300";
        static char n30[] = "30";

        static char f_npv[]  = "@NPV(0,A1...A3)",  r_npv[]  = "60";
        static char f_as0[]  = "@ASIN(0)",         r_as0[]  = "0";
        static char f_as1[]  = "@ASIN(1)",         r_as1[]  = "1.5708";
        static char f_ac1[]  = "@ACOS(1)",         r_ac1[]  = "0";
        static char f_as2[]  = "@ASIN(2)",         r_err2[] = "ERROR";
        static char f_lk1[]  = "@LOOKUP(0,A1...A3)",  r_lk1[] = "100";
        static char f_lk2[]  = "@LOOKUP(15,A1...A3)", r_lk2[] = "200";
        static char f_lk3[]  = "@LOOKUP(99,A1...A3)", r_lk3[] = "300";
        static char f_lk4[]  = "@LOOKUP(0-1,A1...A3)", r_na2[] = "NA";

        /* A fresh fixture: A1..A3 = 10, 20, 30 for @NPV. A rate of zero
           divides every flow by one, so the answer is the plain sum -- which
           is the one case where NPV can be checked without trusting the
           discounting to check itself. */
        cell_clear_all();
        put_num(0, 0, n10);
        put_num(1, 0, n20);
        put_num(2, 0, n30);
        check(f_npv, r_npv);

        check(f_as0, r_as0);
        check(f_as1, r_as1);        /* |x| = 1 is done by hand: a right angle */
        check(f_ac1, r_ac1);
        check(f_as2, r_err2);       /* outside -1..1 there is no answer */

        /* A lookup TABLE: the keys in A, the answers in the column beside
           them. That is the shape the function exists for. */
        cell_clear_all();
        put_num(0, 0, n0);   put_num(0, 1, n100);
        put_num(1, 0, n10);  put_num(1, 1, n200);
        put_num(2, 0, n20);  put_num(2, 1, n300);
        check(f_lk1, r_lk1);        /* an exact hit on the first key */
        check(f_lk2, r_lk2);        /* between keys: the one below */
        check(f_lk3, r_lk3);        /* past the end: the last key */
        check(f_lk4, r_na2);        /* below every key: NA, not ERROR */
    }

    {
        static char okv[]  = "\nFORMULAS OK\n";
        static char badv[] = "\nFAILED AT CASE ";
        if (failed == 0) {
            con_puts(okv);
        } else {
            con_puts(badv);
            sh_put_hex8(failed);
            con_putc('\n');
        }
    }


/* ESC goes back to the prompt instead of parking here forever. goshell.h is
   explicit that this is where a spin belongs -- with the kernel resident it
   restarts the kernel, so a card full of these can be run one after another
   without resetting the machine between them. The headless runs press
   nothing, so the verdict stays on the last frame either way. */
    goshell_on_esc();
    return 0;
}
