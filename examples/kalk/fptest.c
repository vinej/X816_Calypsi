/* ==========================================================================
 * fptest.c -- does x16lib's floating point actually work when C drives it?
 *
 * runtime/fpcall.s is the first thing in this tree to call the converted
 * library from C, and the crossing it makes is the kind that fails quietly.
 * A/X/Y have to be EIGHT bits wide inside x16lib and sixteen outside; get that
 * wrong and an 8-bit routine reads one byte too many on its first immediate
 * load and wanders off. Nothing about that produces an error message -- it
 * produces a wrong number, or a machine that stops.
 *
 * So this checks ARITHMETIC, not linkage. Every case compares against a string
 * worked out by hand, because the whole point is to catch a bridge that
 * successfully calls the wrong thing.
 *
 * It also stands as the conformance test util/float.s never had. That module
 * is 2,000 lines of hand-written mantissa arithmetic and it is about to become
 * the single most load-bearing dependency of the spreadsheet; going in without
 * a test on it would mean debugging kalk and the float package at once.
 *
 * ON SCREEN: every case prints its expectation and what it got, so a failure
 * says which one and by how much rather than just stopping. The last line is
 * the verdict.
 *
 * BUILD AT -O0, like everything else that reaches hardware here.
 * ========================================================================== */

#include "console.h"
#include "fp.h"
#include "shell.h"
#include "goshell.h"

/* String literals cannot be addressed from bank $00 in this data model -- they
   land in `cdata` out in bank $01 and the link fails outright. `static char[]`
   goes to `data`, whose initialiser rides in the image and is copied down at
   startup. shell.h explains it at length; every string here obeys it. */

static fp_t a, b, t;
static uint8_t failed;
static uint8_t ncase;

static bool
str_eq(const char *x, const char *y)
{
    while (*x && *y) {
        if (*x != *y)
            return false;
        x++; y++;
    }
    return *x == '\0' && *y == '\0';
}

/* One case: a label, what the arithmetic should say, and what it did say.
   Printed either way -- a passing run is also the readable record of what this
   float package produces, which is worth having for a format whose rounding is
   truncation rather than round-to-nearest. */
static void
check(const char *what, const char *want)
{
    static char arrow[]  = " -> ";
    static char wanted[] = "   WANT ";
    static char okmark[] = "  ok\n";
    const char *got = fp_to_str_trim();

    ncase++;
    con_puts(what);
    con_puts(arrow);
    con_puts(got);
    if (str_eq(got, want)) {
        con_puts(okmark);
        return;
    }
    con_puts(wanted);
    con_puts(want);
    con_putc('\n');
    if (!failed)
        failed = ncase;                 /* the FIRST failure, not the last */
}

/* FAC = the decimal in `s`. Parsing is itself under test, so the setup path
   and the checked path are the same one -- which is deliberate: if from_str is
   broken, every case fails and says so, rather than the suite passing on
   values it built some other way. */
static void
set(fp_t *v, const char *s)
{
    fp_from_str(s);
    fp_store(v);
}

int
main(void)
{
    static char banner[] = "X816 FLOAT BRIDGE\n\n";

    static char s10[] = "10", s4[] = "4", s3[] = "3", s0[] = "0";
    static char sneg[] = "-2.5", spi[] = "3.14159265";

    static char c_add[] = "10+4", c_sub[] = "10-4", c_mul[] = "10*4";
    static char c_div[] = "10/4", c_rsub[] = "4-10", c_rdiv[] = "4/10";
    static char c_str[] = "str 3.14159265", c_neg[] = "abs -2.5";
    static char c_int[] = "int 3.14159265", c_s16[] = "s16 rt 12345";
    static char c_sqrt[] = "sqrt 10", c_ln[] = "ln 10", c_exp[] = "exp 1";
    static char c_sin[] = "sin 0", c_cos[] = "cos 0";
    static char c_dz[] = "10/0 (answers)", c_sqn[] = "sqrt -2.5 (answers)";
    static char c_cmp[] = "cmp 4 vs 10", c_sgn[] = "sgn -2.5";
    static char c_negop[] = "neg -2.5";

    static char r_add[] = "14", r_sub[] = "6", r_mul[] = "40";
    static char r_div[] = "2.5", r_sub2[] = "-6";
    static char r_neg[] = "2.5";
    static char r_int[] = "3", r_s16[] = "12345";
    static char r_sqrt[] = "3.16227766", r_ln[] = "2.30258509";
    static char r_sin[] = "0", r_cos[] = "1";
    static char r_zero[] = "0";

    /* ---- three expectations that are the LIBRARY's, not arithmetic's -----
     *
     * Each of these was written the obvious way first and the run corrected
     * it. They are pinned here rather than loosened, because all three are
     * things the spreadsheet has to design around.
     *
     * ANYTHING BELOW 1 PRINTS IN EXPONENT FORM. f_to_str computes the digits
     * before the point as exponent + 9 and goes scientific when that is <= 0
     * or >= 10, so 0.4 is "4.00000000e-01" and not ".4". VisiCalc shows ".4".
     * That is not a defect -- it is a general-purpose formatter meeting a
     * spreadsheet's expectations -- but it does mean kalk cannot simply print
     * what f_to_str_trim returns for its General format. It needs its own
     * point placement over the digits, which is what kalk.c's own formatter
     * does anyway.
     *
     * NINE DIGITS, AND THE NINTH MAY BE ONE OUT. The mantissa is 32 bits, so
     * about 9.6 decimal digits, and the arithmetic TRUNCATES rather than
     * rounding -- util/float.s says so in its header. A round trip through
     * f_from_str and f_to_str therefore costs an ulp at the bottom: pi typed
     * as 3.14159265 comes back 3.14159264, and e comes back 2.71828182. Both
     * are measured, not predicted. A spreadsheet showing eight significant
     * digits never sees it; one showing nine sometimes will.
     */
    static char r_rdiv[] = "4.00000000e-01";
    static char r_str[]  = "3.14159264";
    static char r_exp[]  = "2.71828182";

    con_init();
    con_puts(banner);

    set(&a, s10);
    set(&b, s4);

    /* ---- the four operations, FAC op= mem ------------------------------ */
    fp_load(&a); fp_add(&b); check(c_add, r_add);
    fp_load(&a); fp_sub(&b); check(c_sub, r_sub);
    fp_load(&a); fp_mul(&b); check(c_mul, r_mul);
    fp_load(&a); fp_div(&b); check(c_div, r_div);

    /* ---- and the reversed pair, which is why they exist -----------------
       A left-to-right expression parser holds the running value in FAC and
       meets the next operand in memory, so it needs mem - FAC and mem / FAC.
       Getting these backwards is the classic way a parser computes 4-10 as
       6 and nobody notices until a subtraction is nested. */
    fp_load(&a); fp_rsub(&b); check(c_rsub, r_sub2);
    fp_load(&a); fp_rdiv(&b); check(c_rdiv, r_rdiv);

    /* ---- strings both ways --------------------------------------------- */
    fp_from_str(spi); check(c_str, r_str);

    /* ---- unary --------------------------------------------------------- */
    set(&t, sneg);
    fp_load(&t); fp_abs(); check(c_neg, r_neg);
    fp_from_str(spi); fp_int(); check(c_int, r_int);

    /* ---- the 16-bit round trip, which crosses the width twice ---------- */
    fp_from_s16(12345);
    if (fp_to_s16() != 12345) {
        static char bad[] = "s16 round trip FAILED\n";
        con_puts(bad);
        if (!failed) failed = (uint8_t)(ncase + 1);
    }
    fp_from_s16(12345); check(c_s16, r_s16);

    /* ---- the transcendentals, which is where Calypsi's own library went
       wrong -- sqrtf(2) came back 12.109 and sin came back zero, so these
       are not a formality ------------------------------------------------ */
    fp_load(&a); fp_sqrt(); check(c_sqrt, r_sqrt);
    fp_load(&a); fp_ln();   check(c_ln,   r_ln);
    fp_from_s16(1); fp_exp(); check(c_exp, r_exp);
    fp_from_s16(0); fp_sin(); check(c_sin, r_sin);
    fp_from_s16(0); fp_cos(); check(c_cos, r_cos);

    /* ---- domain errors ANSWER, they do not abort ------------------------
       There is no BASIC error handler to land in, so the library returns a
       value and leaves the diagnosis to the caller. A spreadsheet showing
       ERROR depends on exactly this: it has to get control back. */
    set(&t, s0);
    fp_load(&a); fp_div(&t); check(c_dz, r_zero);
    set(&t, sneg);
    fp_load(&t); fp_sqrt(); check(c_sqn, r_zero);

    /* ---- comparison, which must not consume FAC ------------------------- */
    {
        static char cmpbad[] = "cmp did not preserve FAC\n";
        int8_t r;
        fp_load(&b);                    /* FAC = 4 */
        r = fp_cmp(&a);                 /* against 10 -> -1 */
        if (r != -1) {
            con_puts(c_cmp);
            con_putc('\n');
            if (!failed) failed = (uint8_t)(ncase + 1);
        }
        /* FAC must still be 4 afterwards -- a sort calls this in a loop */
        if (!str_eq(fp_to_str_trim(), s4)) {
            con_puts(cmpbad);
            if (!failed) failed = (uint8_t)(ncase + 1);
        }
    }
    set(&t, sneg);
    fp_load(&t);
    if (fp_sgn() != -1) {
        con_puts(c_sgn);
        con_putc('\n');
        if (!failed) failed = (uint8_t)(ncase + 1);
    }
    fp_load(&t); fp_neg(); check(c_negop, r_neg);
    (void)s3;

    /* ---- the nine digits, at every position ------------------------------
     *
     * f_to_str scales into [1e8, 1e9) and then peels nine digits out of what
     * is by then a whole number. That peeling is integer arithmetic --
     * subtract a power of ten until it borrows, add it back, and the count is
     * the digit -- and these are the values that catch it getting the count,
     * the borrow or the table index wrong. They are chosen rather than
     * pinned from a run: each one's decimal form is not in question.
     *
     *   the two ends of the range, so the first digit is 1 and then 9
     *   a nine in every position, which is the most add-backs possible
     *   INTERIOR ZEROS, which is what an off-by-one in the table shows up as
     *     -- 102030405 stays readable while 1023456789 would not
     *   a negative, because the sign is stripped before this runs and a
     *     routine that looked at it would be wrong only here
     */
    {
        static char s_lo[]  = "100000000", s_hi[] = "999999999";
        static char s_mid[] = "102030405", s_sgn[] = "-987654321";
        static char c_lo[]  = "digits 100000000", c_hi[] = "digits 999999999";
        static char c_mid[] = "digits 102030405", c_sgn2[] = "digits -987654321";

        fp_from_str(s_lo);  check(c_lo,   s_lo);
        fp_from_str(s_hi);  check(c_hi,   s_hi);
        fp_from_str(s_mid); check(c_mid,  s_mid);
        fp_from_str(s_sgn); check(c_sgn2, s_sgn);
    }

    /* ---- verdict -------------------------------------------------------- */
    {
        static char ok[]   = "\nFLOAT BRIDGE OK\n";
        static char bad2[] = "\nFAILED AT CASE ";
        if (failed == 0) {
            con_puts(ok);
        } else {
            con_puts(bad2);
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
