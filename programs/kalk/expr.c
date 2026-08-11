/* ==========================================================================
 * expr.c -- the formula evaluator. The grammar is in expr.h.
 *
 * BUILD AT -O0.
 * ========================================================================== */

#include "expr.h"
#include "cell.h"
#include "fp.h"

/* The parser's whole state. `err` is checked after every descent rather than
   thrown, because there is no way to throw: a failed sub-expression has to
   leave FAC in some state, and the only safe answer is to stop looking at
   it. */
typedef struct {
    const char *p;
    uint16_t    row, col;       /* the cell being evaluated */
    uint8_t     status;         /* EXPR_OK / EXPR_ERROR / EXPR_NA */
    uint8_t     depth;
} ctx;

/* Deep enough for any formula a person types and shallow enough that the 4 KB
   hardware stack cannot be reached: each level costs one small frame with an
   fp_t in it. A formula that nests past this is refused rather than allowed
   to walk off the stack, which on this machine means into the direct page. */
#define MAX_DEPTH 24

static void p_expr(ctx *x);

static void
fail(ctx *x, uint8_t how)
{
    /* ERROR wins over NA when a formula meets both -- VisiCalc's rule, and
       the useful one: "this is wrong" is more urgent than "not yet". */
    if (x->status == EXPR_OK || how == EXPR_ERROR)
        x->status = how;
}

static void
skipws(ctx *x)
{
    while (*x->p == ' ' || *x->p == '\t')
        x->p++;
}

static bool is_digit(char c) { return c >= '0' && c <= '9'; }
static bool is_alpha(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}
static char up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

/* ---- references ---------------------------------------------------------- */

uint8_t
expr_parse_ref(const char *s, uint16_t *row, uint16_t *col,
               bool *abs_col, bool *abs_row)
{
    const char *p = s;
    uint16_t    c, n;

    *abs_col = *abs_row = false;

    if (*p == '$') { *abs_col = true; p++; }
    if (!is_alpha(*p))
        return 0;

    /* One letter or two: A..Z then AA..IV, which is 256 columns and exactly
       what kalk.c's `*col * 26 + ...` reaches. */
    c = (uint16_t)(up(*p++) - 'A' + 1);
    if (is_alpha(*p))
        c = (uint16_t)(c * 26 + (up(*p++) - 'A' + 1));

    if (*p == '$') { *abs_row = true; p++; }
    if (!is_digit(*p))
        return 0;

    n = 0;
    while (is_digit(*p)) {
        n = (uint16_t)(n * 10 + (uint16_t)(*p - '0'));
        if (n > KALK_ROWS)              /* stop before it wraps */
            return 0;
        p++;
    }
    if (n == 0 || c == 0 || c > KALK_COLS)
        return 0;

    *row = (uint16_t)(n - 1);           /* rows are 1-based when typed */
    *col = (uint16_t)(c - 1);
    return (uint8_t)(p - s);
}

/* FAC = the value at (row, col). Empty and label cells are ZERO, which is
   kalk.c's rule and what makes a @SUM over a ragged range work; a cell
   carrying ERROR or NA propagates it instead of its number. */
static void
load_cell(ctx *x, uint16_t row, uint16_t col)
{
    cell c;

    if (row >= KALK_ROWS || col >= KALK_COLS) {
        fail(x, EXPR_ERROR);
        return;
    }
    cell_get(row, col, &c);

    if (c.flags & CELL_ERROR) { fail(x, EXPR_ERROR); return; }
    if (c.flags & CELL_NA)    { fail(x, EXPR_NA);    return; }

    if (c.type == CELL_NUMBER || c.type == CELL_FORMULA)
        fp_load(&c.value);
    else
        fp_zero();
}

/* ---- functions ----------------------------------------------------------- */

/* A range walk, shared by every aggregate. `what` picks the reduction; the
   walk itself -- and the decision about which cells count -- is written once,
   because @COUNT and @AVERAGE disagreeing with @SUM about what an empty cell
   is would be a very quiet bug. */
#define AGG_SUM   0
#define AGG_MIN   1
#define AGG_MAX   2
#define AGG_COUNT 3
#define AGG_AVG   4

static void
aggregate(ctx *x, uint8_t what, uint16_t r1, uint16_t c1,
          uint16_t r2, uint16_t c2)
{
    fp_t     acc, v;
    uint16_t r, c, n = 0;
    bool     first = true;

    fp_zero();
    fp_store(&acc);

    for (r = r1; r <= r2 && r < KALK_ROWS; r++) {
        for (c = c1; c <= c2 && c < KALK_COLS; c++) {
            cell cl;
            cell_get(r, c, &cl);

            if (cl.flags & CELL_ERROR) { fail(x, EXPR_ERROR); return; }
            if (cl.flags & CELL_NA)    { fail(x, EXPR_NA);    return; }

            /* Empty and label cells are not values: they contribute nothing
               to a sum and, importantly, are not counted by @COUNT or
               averaged over. An @AVERAGE that divided by the size of the
               range rather than by the number of values in it is the classic
               spreadsheet lie. */
            if (cl.type != CELL_NUMBER && cl.type != CELL_FORMULA)
                continue;

            n++;
            fp_load(&cl.value);
            fp_store(&v);

            switch (what) {
            case AGG_SUM:
            case AGG_AVG:
                fp_load(&acc);
                fp_add(&v);
                fp_store(&acc);
                break;
            case AGG_MIN:
                if (first) { fp_load(&v); fp_store(&acc); }
                else {
                    fp_load(&v);
                    if (fp_cmp(&acc) < 0) fp_store(&acc);
                }
                break;
            case AGG_MAX:
                if (first) { fp_load(&v); fp_store(&acc); }
                else {
                    fp_load(&v);
                    if (fp_cmp(&acc) > 0) fp_store(&acc);
                }
                break;
            default:
                break;
            }
            first = false;
        }
    }

    if (what == AGG_COUNT) {
        fp_from_s16((int16_t)n);
        return;
    }
    if (what == AGG_AVG) {
        if (n == 0) {                   /* nothing to average: 0/0 */
            fail(x, EXPR_ERROR);
            return;
        }
        fp_from_s16((int16_t)n);
        fp_store(&v);
        fp_load(&acc);
        fp_div(&v);
        return;
    }
    /* MIN and MAX of an empty range are zero rather than an error: a column
       that has not been filled in yet is not a mistake. */
    fp_load(&acc);
}

/* @NPV(rate, range) -- each flow discounted one more period than the last,
   so the first cell is divided by (1+rate), the second by (1+rate) squared,
   and so on. VisiCalc's definition, and the one every finance textbook
   writes: the flows are assumed to arrive at the END of each period, which
   is why nothing is divided by 1.

   A rate of -1 makes the divisor zero. The float package answers zero for a
   division by zero rather than aborting, so without this the whole sum would
   quietly come out as nothing. */
static void
npv_range(ctx *x, uint16_t r1, uint16_t c1, uint16_t r2, uint16_t c2)
{
    static char one[] = "1";
    fp_t     step, div, acc, k1;
    uint16_t r, c;

    /* The rate arrives in FAC. step = 1 + rate, and that is also the first
       divisor -- the first flow is one period away, not zero. */
    fp_store(&step);
    fp_from_str(one);
    fp_store(&k1);
    fp_load(&step);
    fp_add(&k1);
    fp_store(&step);
    if (fp_sgn() == 0) {                /* a rate of -1: every divisor zero */
        fail(x, EXPR_ERROR);
        return;
    }
    fp_store(&div);

    fp_zero();
    fp_store(&acc);

    for (r = r1; r <= r2 && r < KALK_ROWS; r++) {
        for (c = c1; c <= c2 && c < KALK_COLS; c++) {
            cell cl;
            cell_get(r, c, &cl);
            if (cl.flags & CELL_ERROR) { fail(x, EXPR_ERROR); return; }
            if (cl.flags & CELL_NA)    { fail(x, EXPR_NA);    return; }
            if (cl.type != CELL_NUMBER && cl.type != CELL_FORMULA)
                continue;
            fp_load(&cl.value);
            fp_div(&div);
            fp_add(&acc);
            fp_store(&acc);
            fp_load(&div);
            fp_mul(&step);
            fp_store(&div);
        }
    }
    fp_load(&acc);
}

/* @LOOKUP(value, range) -- walk the range while its entries stay at or below
   the value, and answer from the cell just PAST the range in the same
   direction. A single-column range reads the column to its right, a
   single-row range the row below it.

   The range is the keys and the answer comes from beside them, which is what
   makes it a lookup table rather than a search: a column of thresholds with
   the rates next to it is the shape everyone writes.

   Nothing at or below the value is @NA rather than ERROR -- the table is
   fine, the question simply has no answer in it. */
static void
lookup_range(ctx *x, uint16_t r1, uint16_t c1, uint16_t r2, uint16_t c2)
{
    fp_t     want;
    uint16_t r, c, hr = 0, hc = 0;
    bool     vertical = (c1 == c2);
    bool     found = false;
    cell     cl;

    fp_store(&want);

    r = r1;
    c = c1;
    for (;;) {
        if (r > r2 || c > c2 || r >= KALK_ROWS || c >= KALK_COLS)
            break;
        cell_get(r, c, &cl);
        if (cl.type == CELL_NUMBER || cl.type == CELL_FORMULA) {
            fp_load(&cl.value);
            if (fp_cmp(&want) > 0)
                break;                  /* past the value: the last hit wins */
            hr = r;
            hc = c;
            found = true;
        }
        if (vertical) r++; else c++;
    }

    if (!found) { fail(x, EXPR_NA); return; }

    if (vertical) hc++; else hr++;
    if (hc >= KALK_COLS || hr >= KALK_ROWS) { fail(x, EXPR_NA); return; }

    cell_get(hr, hc, &cl);
    if (cl.flags & CELL_ERROR) { fail(x, EXPR_ERROR); return; }
    if (cl.flags & CELL_NA)    { fail(x, EXPR_NA);    return; }
    if (cl.type == CELL_NUMBER || cl.type == CELL_FORMULA)
        fp_load(&cl.value);
    else
        fp_zero();                      /* an empty answer cell is zero */
}

/* asin x = atan(x / sqrt(1 - x*x)), with both ends done by hand.
 *
 * The float package has atan and no asin, and this is the identity that gets
 * one from the other -- but it divides by zero at |x| = 1, exactly where the
 * answer is a right angle, so those two points are answered directly. Beyond
 * 1 there is no answer at all and the caller gets ERROR rather than the zero
 * a division by zero would otherwise produce. */
static void
fn_asin(ctx *x)
{
    fp_t v, root;
    static char one[] = "1";
    static char halfpi[] = "1.57079633";

    fp_store(&v);                       /* v = x */
    fp_mul(&v);                         /* x * x */
    fp_store(&root);
    fp_from_str(one);
    fp_sub(&root);                      /* 1 - x*x */

    if (fp_sgn() < 0) { fail(x, EXPR_ERROR); return; }   /* |x| > 1 */
    if (fp_sgn() == 0) {                                /* |x| = 1 */
        fp_load(&v);
        {
            int8_t s = fp_sgn();
            fp_from_str(halfpi);
            if (s < 0) fp_neg();
        }
        return;
    }
    fp_sqrt();
    fp_store(&root);
    fp_load(&v);
    fp_div(&root);
    fp_atan();
}

/* ---- the function table --------------------------------------------------
 *
 * A table and an index rather than a chain of string comparisons, and the
 * reason is the data model rather than taste: a string LITERAL lands in
 * `cdata` out in bank $01 and cannot be addressed from bank $00, so
 * `strcmp(fn, "SUM")` -- which is exactly what kalk.c writes -- does not
 * link here. shell.h explains it at length and shell.c's command table solves
 * it the same way: an inline array in a non-const table, which goes to `data`
 * and is copied down at startup.
 *
 * It costs 160 bytes of bank $00 and buys a switch instead of nineteen
 * comparisons, so it is the better shape regardless.
 */
enum {
    F_SUM, F_MIN, F_MAX, F_COUNT, F_AVERAGE, F_AVG,
    F_NPV, F_LOOKUP,
    F_ABS, F_INT, F_SQRT, F_EXP, F_LN, F_LOG10,
    F_SIN, F_COS, F_TAN, F_ASIN, F_ACOS, F_ATAN,
    F_PI, F_ERROR, F_NA,
    F_NONE
};

static char fn_names[F_NONE][8] = {
    "SUM", "MIN", "MAX", "COUNT", "AVERAGE", "AVG",
    "NPV", "LOOKUP",
    "ABS", "INT", "SQRT", "EXP", "LN", "LOG10",
    "SIN", "COS", "TAN", "ASIN", "ACOS", "ATAN",
    "PI", "ERROR", "NA"
};

/* The name after '@', upper-cased, resolved to one of the above. F_NONE for
   anything unrecognised, which the caller turns into ERROR -- a misspelled
   function must not quietly evaluate to its argument. */
static uint8_t
fn_lookup(ctx *x)
{
    char    name[9];
    uint8_t n = 0, i, j;

    /* Letters, then letters OR DIGITS. @LOG10 is the only name in the table
       with a digit in it and it is why this is not simply is_alpha: stopping
       at the '1' reads the name as "LOG", finds nothing, and reports a
       misspelled function for a formula that was spelled correctly. */
    while ((is_alpha(*x->p) || (n > 0 && is_digit(*x->p))) && n < 8)
        name[n++] = up(*x->p++);
    name[n] = '\0';
    if (n == 0)
        return F_NONE;

    for (i = 0; i < F_NONE; i++) {
        for (j = 0; j < 8; j++) {
            char a = fn_names[i][j];
            char b = (j < n) ? name[j] : '\0';
            if (a != b)
                break;
            if (a == '\0')
                return i;
        }
        if (j == 8 && n == 8)
            return i;
    }
    return F_NONE;
}

/* @LN scaled: log10(x) = ln(x) * (1 / ln 10). Held as a decimal because the
   float package parses one exactly as well as it would load a constant, and
   this way the number is readable. */
static void
mul_recip_ln10(void)
{
    static char k[] = "0.434294482";
    fp_t v;
    fp_store(&v);
    fp_from_str(k);
    fp_mul(&v);
}

static void
p_function(ctx *x)
{
    uint16_t r1, c1, r2, c2;
    bool     ac, ar;
    uint8_t  n, fn;

    x->p++;                             /* the '@' */
    fn = fn_lookup(x);
    if (fn == F_NONE) {
        fail(x, EXPR_ERROR);
        return;
    }

    /* The three that stand alone. VisiCalc writes them bare -- @PI, not
       @PI() -- but empty parentheses are accepted because people type them. */
    if (fn == F_PI || fn == F_ERROR || fn == F_NA) {
        static char pi[] = "3.14159265";
        skipws(x);
        if (*x->p == '(') { x->p++; skipws(x); if (*x->p == ')') x->p++; }
        if (fn == F_PI)         fp_from_str(pi);
        else if (fn == F_ERROR) fail(x, EXPR_ERROR);
        else                    fail(x, EXPR_NA);
        return;
    }

    skipws(x);
    if (*x->p != '(') {
        fail(x, EXPR_ERROR);
        return;
    }
    x->p++;
    skipws(x);

    /* THE TWO THAT TAKE A VALUE AND A RANGE, handled before the range/value
       decision below because they are the only ones that take both. */
    if (fn == F_NPV || fn == F_LOOKUP) {
        fp_t arg;

        p_expr(x);                      /* the rate, or the value sought */
        if (x->status != EXPR_OK)
            return;
        fp_store(&arg);
        skipws(x);
        if (*x->p != ',') { fail(x, EXPR_ERROR); return; }
        x->p++;
        skipws(x);

        n = expr_parse_ref(x->p, &r1, &c1, &ac, &ar);
        if (n == 0 || x->p[n] != '.' || x->p[n + 1] != '.'
                   || x->p[n + 2] != '.') {
            fail(x, EXPR_ERROR);        /* the second argument IS a range */
            return;
        }
        {
            const char *q = x->p + n + 3;
            uint8_t     n2 = expr_parse_ref(q, &r2, &c2, &ac, &ar);
            if (n2 == 0) { fail(x, EXPR_ERROR); return; }
            x->p = q + n2;
        }
        skipws(x);
        if (*x->p != ')') { fail(x, EXPR_ERROR); return; }
        x->p++;
        if (r1 > r2) { uint16_t t = r1; r1 = r2; r2 = t; }
        if (c1 > c2) { uint16_t t = c1; c1 = c2; c2 = t; }

        fp_load(&arg);
        if (fn == F_NPV)
            npv_range(x, r1, c1, r2, c2);
        else
            lookup_range(x, r1, c1, r2, c2);
        return;
    }

    /* A range argument, or an ordinary expression? Only a reference followed
       by "..." is a range, so a lone A1 stays an expression and @SUM(A1) is
       the single-value form kalk.c also accepts. */
    n = expr_parse_ref(x->p, &r1, &c1, &ac, &ar);
    if (n && x->p[n] == '.' && x->p[n + 1] == '.' && x->p[n + 2] == '.') {
        const char *q = x->p + n + 3;
        uint8_t     n2 = expr_parse_ref(q, &r2, &c2, &ac, &ar);
        if (n2 == 0) {
            fail(x, EXPR_ERROR);
            return;
        }
        x->p = q + n2;
        skipws(x);
        if (*x->p != ')') { fail(x, EXPR_ERROR); return; }
        x->p++;

        /* Typed either way round -- B5...A1 is the same rectangle as
           A1...B5, and refusing it would be a rule with no purpose. */
        if (r1 > r2) { uint16_t t = r1; r1 = r2; r2 = t; }
        if (c1 > c2) { uint16_t t = c1; c1 = c2; c2 = t; }

        switch (fn) {
        case F_SUM:     aggregate(x, AGG_SUM,   r1, c1, r2, c2); break;
        case F_MIN:     aggregate(x, AGG_MIN,   r1, c1, r2, c2); break;
        case F_MAX:     aggregate(x, AGG_MAX,   r1, c1, r2, c2); break;
        case F_COUNT:   aggregate(x, AGG_COUNT, r1, c1, r2, c2); break;
        case F_AVERAGE:
        case F_AVG:     aggregate(x, AGG_AVG,   r1, c1, r2, c2); break;
        default:        fail(x, EXPR_ERROR);                     break;
        }
        return;
    }

    /* A value argument. */
    p_expr(x);
    skipws(x);
    if (*x->p != ')') {
        fail(x, EXPR_ERROR);
        return;
    }
    x->p++;
    if (x->status != EXPR_OK)
        return;

    /* Domain errors are caught HERE, before the call. The float package
       answers rather than aborting -- sqrt of a negative is 0, ln of zero is
       0 -- which is what lets a program keep running, but it also means
       nothing downstream would ever know. A cell showing 0 where it should
       show ERROR is the worse failure. */
    switch (fn) {
    case F_ABS:  fp_abs();  return;
    case F_INT:  fp_int();  return;
    case F_SIN:  fp_sin();  return;
    case F_COS:  fp_cos();  return;
    case F_TAN:  fp_tan();  return;
    case F_ATAN: fp_atan(); return;
    /* The float package has atan and no inverse sine, so these are built
       from it -- fn_asin carries the identity and the two ends it breaks
       down at. acos is the complement, which is one subtraction away. */
    case F_ASIN:
        fn_asin(x);
        return;
    case F_ACOS: {
        static char halfpi[] = "1.57079633";
        fp_t s;
        fn_asin(x);
        if (x->status != EXPR_OK)
            return;
        fp_store(&s);
        fp_from_str(halfpi);
        fp_sub(&s);                     /* pi/2 - asin x */
        return;
    }
    case F_EXP:  fp_exp();  return;
    case F_SQRT:
        if (fp_sgn() < 0) { fail(x, EXPR_ERROR); return; }
        fp_sqrt();
        return;
    case F_LN:
        if (fp_sgn() <= 0) { fail(x, EXPR_ERROR); return; }
        fp_ln();
        return;
    case F_LOG10:
        if (fp_sgn() <= 0) { fail(x, EXPR_ERROR); return; }
        fp_ln();
        mul_recip_ln10();
        return;
    /* The single-value forms kalk.c allows for the aggregates: @SUM(x) is x,
       and so are @MIN, @MAX and @AVERAGE of one value. @COUNT of one is 1. */
    case F_SUM: case F_MIN: case F_MAX: case F_AVERAGE: case F_AVG:
        return;
    case F_COUNT:
        fp_from_s16(1);
        return;
    default:
        fail(x, EXPR_ERROR);
        return;
    }
}

/* ---- the grammar --------------------------------------------------------- */

static void
p_primary(ctx *x)
{
    skipws(x);

    if (x->depth >= MAX_DEPTH) {
        fail(x, EXPR_ERROR);
        return;
    }

    if (*x->p == '-') {
        x->p++;
        x->depth++;
        p_primary(x);
        x->depth--;
        if (x->status == EXPR_OK)
            fp_neg();
        return;
    }
    if (*x->p == '+') {
        x->p++;
        x->depth++;
        p_primary(x);
        x->depth--;
        return;
    }
    if (*x->p == '(') {
        x->p++;
        x->depth++;
        p_expr(x);
        x->depth--;
        skipws(x);
        if (*x->p != ')') {
            fail(x, EXPR_ERROR);
            return;
        }
        x->p++;
        return;
    }
    if (*x->p == '@') {
        x->depth++;
        p_function(x);
        x->depth--;
        return;
    }

    /* A reference, and only if it parses whole -- otherwise a stray letter
       would be silently swallowed as a zero-valued cell. */
    if (is_alpha(*x->p) || *x->p == '$') {
        uint16_t r, c;
        bool     ac, ar;
        uint8_t  n = expr_parse_ref(x->p, &r, &c, &ac, &ar);
        if (n == 0) {
            fail(x, EXPR_ERROR);
            return;
        }
        x->p += n;
        load_cell(x, r, c);
        return;
    }

    if (is_digit(*x->p) || *x->p == '.') {
        char    num[24];
        uint8_t n = 0;
        while ((is_digit(*x->p) || *x->p == '.') && n < sizeof num - 1)
            num[n++] = *x->p++;
        /* an exponent, if one follows: 1e6 and 1.5e-3 are numbers a user
           may reasonably type into a cell */
        if ((*x->p == 'e' || *x->p == 'E') && n < sizeof num - 4) {
            num[n++] = *x->p++;
            if (*x->p == '+' || *x->p == '-')
                num[n++] = *x->p++;
            while (is_digit(*x->p) && n < sizeof num - 1)
                num[n++] = *x->p++;
        }
        num[n] = '\0';
        if (!fp_from_str(num))
            fail(x, EXPR_ERROR);
        return;
    }

    fail(x, EXPR_ERROR);
}

static void
p_term(ctx *x)
{
    fp_t l, r;

    p_primary(x);
    if (x->status != EXPR_OK)
        return;
    fp_store(&l);

    for (;;) {
        char op;
        skipws(x);
        op = *x->p;
        if (op != '*' && op != '/')
            break;
        x->p++;
        p_primary(x);
        if (x->status != EXPR_OK)
            return;
        fp_store(&r);

        if (op == '*') {
            fp_load(&l);
            fp_mul(&r);
        } else {
            /* Division by zero is an ERROR, not the library's answer of
               zero. The package answers so that a program keeps running;
               a spreadsheet has somewhere to put the diagnosis. */
            fp_load(&r);
            if (fp_sgn() == 0) {
                fail(x, EXPR_ERROR);
                return;
            }
            fp_load(&l);
            fp_div(&r);
        }
        fp_store(&l);
    }
    fp_load(&l);
}

static void
p_expr(ctx *x)
{
    fp_t l, r;

    p_term(x);
    if (x->status != EXPR_OK)
        return;
    fp_store(&l);

    for (;;) {
        char op;
        skipws(x);
        op = *x->p;
        if (op != '+' && op != '-')
            break;
        x->p++;
        p_term(x);
        if (x->status != EXPR_OK)
            return;
        fp_store(&r);
        fp_load(&l);
        if (op == '+') fp_add(&r); else fp_sub(&r);
        fp_store(&l);
    }
    fp_load(&l);
}

/* ---- the door ------------------------------------------------------------ */

uint8_t
expr_eval(const char *s, uint16_t row, uint16_t col)
{
    ctx x;

    x.p = s;
    x.row = row;
    x.col = col;
    x.status = EXPR_OK;
    x.depth = 0;

    /* A leading '+' is how a VisiCalc user starts a formula -- +A1*2 -- and
       p_primary already treats it as the unary operator it looks like. */
    p_expr(&x);

    if (x.status != EXPR_OK)
        return x.status;

    /* Anything left over is a syntax error rather than something to ignore:
       "1 2" and "A1)" are mistakes, and accepting the first token of them
       would show a plausible number for a formula the user got wrong. */
    skipws(&x);
    if (*x.p != '\0')
        return EXPR_ERROR;

    return EXPR_OK;
}

bool
expr_is_formula(const char *s)
{
    while (*s == ' ')
        s++;
    return *s == '+' || *s == '-' || *s == '(' || *s == '@';
}
