/* ==========================================================================
 * fmt.c -- turning a float into a column. The rules, and why they are these
 * rules, are in fmt.h.
 *
 * BUILD AT -O0.
 * ========================================================================== */

#include "fmt.h"

#define GEN_SIG 6               /* %g's default precision                    */

/* ---- reading the library's answer back ---------------------------------- */

/* f_to_str_trim gives one of two shapes and the caller does not get to choose
   which: plain "-123.45" while 1 <= |v| < 1e9, and "1.23450000e-03" outside
   that. Both are parsed the same way -- collect the digits, remember where
   the point was, then fold the exponent in -- so the shape stops mattering
   after this function. */
void
fmt_normalise(fmt_num *n)
{
    const char *s = fp_to_str_trim();
    uint8_t     ndig = 0;
    int16_t     point = 0;      /* digits before the point                   */
    int16_t     e10 = 0;
    bool        seen_point = false;
    uint8_t     i, lead;

    n->neg = false;
    n->zero = false;
    n->exp = 0;
    n->d[0] = '\0';

    if (*s == '-') {
        n->neg = true;
        s++;
    }

    while (*s) {
        if (*s == '.') {
            seen_point = true;
            s++;
            continue;
        }
        if (*s == 'e' || *s == 'E') {
            bool eneg = false;
            s++;
            if (*s == '+') s++;
            else if (*s == '-') { eneg = true; s++; }
            while (*s >= '0' && *s <= '9') {
                e10 = (int16_t)(e10 * 10 + (*s - '0'));
                s++;
            }
            if (eneg)
                e10 = (int16_t)-e10;
            break;
        }
        if (*s < '0' || *s > '9')
            break;
        if (ndig < FMT_DIGITS)
            n->d[ndig++] = *s;
        if (!seen_point)
            point++;
        s++;
    }
    n->d[ndig] = '\0';

    /* 0.<digits> x 10^exp, so the exponent is where the point was plus
       whatever the e-suffix moved it. */
    point = (int16_t)(point + e10);

    /* Leading zeros carry no value and would break the digits[0] != '0'
       promise the rest of this file relies on. Each one dropped moves the
       point left with it. */
    lead = 0;
    while (n->d[lead] == '0')
        lead++;
    if (lead) {
        for (i = 0; n->d[lead + i]; i++)
            n->d[i] = n->d[lead + i];
        n->d[i] = '\0';
        ndig = i;
        point = (int16_t)(point - lead);
    }

    /* Trailing zeros do not change the value and would otherwise be rendered
       as significant -- 2.50 instead of 2.5. */
    while (ndig > 0 && n->d[ndig - 1] == '0')
        n->d[--ndig] = '\0';

    if (ndig == 0) {
        n->zero = true;
        n->neg = false;         /* there is no negative zero on a screen */
        n->exp = 0;
        return;
    }
    n->exp = (int8_t)point;
}

/* Round the digit string to `keep` significant digits, half-up. A carry out
   of the leading digit makes the number one decade larger -- 999.6 to four
   digits is 1000, which is "1" with the exponent bumped -- and forgetting
   that is how a formatter turns 999.6 into 0.0. */
static void
round_sig(fmt_num *n, uint8_t keep)
{
    uint8_t len = 0, i;

    while (n->d[len])
        len++;
    if (keep >= len)
        return;

    if (n->d[keep] < '5') {
        n->d[keep] = '\0';
    } else {
        i = keep;
        for (;;) {
            if (i == 0) {                       /* carried off the top */
                for (keep = FMT_DIGITS; keep > 0; keep--)
                    n->d[keep] = n->d[keep - 1];
                n->d[0] = '1';
                n->d[i + 1] = '\0';
                n->exp = (int8_t)(n->exp + 1);
                return;
            }
            i--;
            if (n->d[i] != '9') {
                n->d[i]++;
                n->d[i + 1] = '\0';
                break;
            }
        }
    }
    /* rounding can expose new trailing zeros: 2.999 -> 3.00 -> 3 */
    len = 0;
    while (n->d[len])
        len++;
    while (len > 0 && n->d[len - 1] == '0')
        n->d[--len] = '\0';
    if (len == 0) {
        n->zero = true;
        n->exp = 0;
    }
}

/* Chop to `keep` significant digits with no rounding at all -- what a C cast
   to an integer does. Separate from round_sig because the difference between
   them is a whole digit of every value shown in an /F I column. */
static void
trunc_sig(fmt_num *n, uint8_t keep)
{
    uint8_t len = 0;

    while (n->d[len])
        len++;
    if (keep >= len)
        return;
    n->d[keep] = '\0';
    len = keep;
    while (len > 0 && n->d[len - 1] == '0')
        n->d[--len] = '\0';
    if (len == 0) {
        n->zero = true;
        n->exp = 0;
    }
}

/* ---- writing --------------------------------------------------------- */

/* A tiny append-with-a-limit, so every writer below can be written as if it
   had room and stop cleanly when it does not. */
typedef struct { char *p; uint8_t n, max; } buf;

static void
bput(buf *b, char c)
{
    if (b->n < b->max)
        b->p[b->n++] = c;
}

static void
bdigits(buf *b, const char *d, uint8_t from, uint8_t count)
{
    uint8_t i;
    for (i = 0; i < count; i++)
        bput(b, d[from + i] ? d[from + i] : '0');
}

/* Fixed notation: the point goes after `exp` digits, with zeros invented on
   either side as needed. exp <= 0 means the value is below 1 and needs a
   leading "0." and -exp zeros after it. */
static void
write_fixed(buf *b, const fmt_num *n, uint8_t ndec_force, bool use_force)
{
    uint8_t len = 0, i;
    int8_t  e = n->exp;

    while (n->d[len])
        len++;

    if (n->neg)
        bput(b, '-');

    if (e <= 0) {
        bput(b, '0');
        if (len > 0 || use_force) {
            uint8_t want = use_force ? ndec_force : (uint8_t)(len - e);
            if (want) {
                bput(b, '.');
                for (i = 0; i < want; i++) {
                    int8_t k = (int8_t)(i + e);
                    bput(b, (k < 0 || k >= (int8_t)len) ? '0' : n->d[k]);
                }
            }
        }
        return;
    }

    /* integer part, padded with zeros when the exponent runs past the digits
       we actually have (4 with exp 3 is 400) */
    bdigits(b, n->d, 0, (uint8_t)e);

    {
        uint8_t have = (len > (uint8_t)e) ? (uint8_t)(len - e) : 0;
        uint8_t want = use_force ? ndec_force : have;
        if (want) {
            bput(b, '.');
            for (i = 0; i < want; i++)
                bput(b, (i < have) ? n->d[e + i] : '0');
        }
    }
}

/* Exponent notation, d.ddde+XX -- %g's form, and only reached when the value
   is outside the range %g renders plainly. */
static void
write_exp(buf *b, const fmt_num *n)
{
    int8_t  x = (int8_t)(n->exp - 1);   /* the exponent in d.ddd form */
    uint8_t len = 0, i;

    while (n->d[len])
        len++;

    if (n->neg)
        bput(b, '-');
    bput(b, n->d[0]);
    if (len > 1) {
        bput(b, '.');
        for (i = 1; i < len; i++)
            bput(b, n->d[i]);
    }
    bput(b, 'e');
    if (x < 0) { bput(b, '-'); x = (int8_t)-x; } else bput(b, '+');
    bput(b, (char)('0' + (x / 10)));
    bput(b, (char)('0' + (x % 10)));
}

/* ---- the formats -------------------------------------------------------- */

static void
pad_into(const char *src, uint8_t width, bool left, char *out)
{
    uint8_t len = 0, i, gap;

    while (src[len])
        len++;

    /* Longer than the column is TRUNCATED, which is what kalk.c's
       `snprintf(fb, cw + 1, ...)` does. Truncating from the left would hide
       the magnitude; from the right it hides precision, which is the lesser
       lie and the one the source tells. */
    if (len >= width) {
        for (i = 0; i < width; i++)
            out[i] = src[i];
        out[width] = '\0';
        return;
    }

    gap = (uint8_t)(width - len);
    if (left) {
        for (i = 0; i < len; i++) out[i] = src[i];
        for (i = 0; i < gap; i++) out[len + i] = ' ';
    } else {
        for (i = 0; i < gap; i++) out[i] = ' ';
        for (i = 0; i < len; i++) out[gap + i] = src[i];
    }
    out[width] = '\0';
}

void
fmt_number(uint8_t code, uint8_t width, char *out)
{
    char    t[40];
    buf     b;
    fmt_num n;
    bool    left = false;

    if (width > FMT_MAX_WIDTH)
        width = FMT_MAX_WIDTH;

    b.p = t; b.n = 0; b.max = (uint8_t)(sizeof t - 1);

    /* The bar is not a number at all -- it is a picture of one, and it is the
     * only numeric format kalk left-aligns.
     *
     * The count is kalk.c's loop verbatim: `for (i = 0; i < cw && i < val;
     * i++)`, an INTEGER compared against the value. That is not the same as
     * truncating the value first -- 2.5 draws three stars, because 0, 1 and 2
     * are all less than 2.5 -- and truncating first draws two. Cheap to get
     * subtly wrong, and a bar chart that is short by one everywhere is the
     * kind of thing nobody reports as a bug.
     */
    if (code == FMT_BAR) {
        fp_t    v;
        uint8_t i;
        fp_store(&v);                       /* the loop overwrites FAC */
        for (i = 0; i < width; i++) {
            fp_from_s16((int16_t)i);
            if (fp_cmp(&v) >= 0)            /* i >= val: the bar ends here */
                break;
            bput(&b, '*');
        }
        t[b.n] = '\0';
        pad_into(t, width, true, out);
        return;
    }

    fmt_normalise(&n);

    if (code == FMT_DOLLAR || code == FMT_PERCENT) {
        if (code == FMT_PERCENT)
            n.exp = (int8_t)(n.exp + 2);        /* x100, without arithmetic */
        /* %.2f: round at the second decimal, which is two digits past the
           point -- so exp + 2 significant digits, and never fewer than one. */
        {
            int16_t keep = (int16_t)n.exp + 2;
            if (keep < 0) keep = 0;
            round_sig(&n, (uint8_t)keep);
        }
        write_fixed(&b, &n, 2, true);
        if (code == FMT_PERCENT)
            bput(&b, '%');
        t[b.n] = '\0';
        pad_into(t, width, false, out);
        return;
    }

    if (n.zero) {
        t[0] = '0'; t[1] = '\0';
        pad_into(t, width, false, out);
        return;
    }

    /* `%ld` -- and note kalk.c reaches it for GENERAL too whenever the value
       is integral and under 1e9, which is what keeps a column of whole
       numbers free of decimal points. Integral means every significant digit
       sits before the point. */
    {
        uint8_t len = 0;
        while (n.d[len])
            len++;
        if (code == FMT_INTEGER || (n.exp >= (int8_t)len && n.exp <= 9)) {
            /* TRUNCATES, and does not round: kalk.c casts, `(long)val`, and a
               C cast truncates toward zero. 123456.789 shows as 123456. The
               integral case below needs neither -- there is nothing after the
               point to lose -- so this only ever runs for an explicit /F I. */
            if (code == FMT_INTEGER)
                trunc_sig(&n, (n.exp > 0) ? (uint8_t)n.exp : 0);
            if (n.zero) {
                t[0] = '0'; t[1] = '\0';
            } else {
                write_fixed(&b, &n, 0, true);
                t[b.n] = '\0';
            }
            pad_into(t, width, false, out);
            return;
        }
    }

    /* `%g`: six significant digits, then fixed or exponent form depending on
       where the point ended up. C's rule is on the d.ddd exponent X: fixed
       while -4 <= X < precision, exponent form otherwise. */
    round_sig(&n, GEN_SIG);
    {
        int8_t x = (int8_t)(n.exp - 1);
        if (x >= -4 && x < GEN_SIG)
            write_fixed(&b, &n, 0, false);
        else
            write_exp(&b, &n);
    }
    t[b.n] = '\0';
    pad_into(t, width, left, out);
}

void
fmt_label(const char *s, uint8_t width, char *out)
{
    if (width > FMT_MAX_WIDTH)
        width = FMT_MAX_WIDTH;
    pad_into(s, width, true, out);
}

void
fmt_error(uint8_t width, char *out)
{
    static char e[] = "ERROR";
    pad_into(e, width, false, out);
}

void
fmt_na(uint8_t width, char *out)
{
    static char e[] = "NA";
    pad_into(e, width, false, out);
}
