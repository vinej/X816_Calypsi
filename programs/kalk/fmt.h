/* ==========================================================================
 * fmt.h -- a cell value as it appears in a column.
 *
 * kalk.c does this with snprintf and gets C's own rules for free:
 *
 *     if (fmt == '$')       snprintf(t, "%.2f",    val);
 *     else if (fmt == '%')  snprintf(t, "%.2f%%",  val * 100);
 *     else if (fmt == '*')  bar of '*', then left-align
 *     else if (fmt == 'I' || (val == (long)val && fabs(val) < 1e9))
 *                           snprintf(t, "%ld",     (long)val);
 *     else                  snprintf(t, "%g",      val);
 *     snprintf(fb, cw + 1, fmt == 'L' ? "%-*s" : "%*s", cw, t);
 *
 * There is no printf here. The C library's is unreachable in this data model
 * (fp.h says why), and the float package's own f_to_str is a general-purpose
 * formatter rather than a spreadsheet's: it goes to EXPONENT FORM for
 * anything below 1, so a column holding 0.4 would read "4.00000000e-01". That
 * is measured, not assumed -- run-fp.sh pins it.
 *
 * So this reimplements the two rules that matter, %ld and %g, on top of the
 * digits f_to_str already produces correctly. The library did the hard part;
 * what is left is placing the point.
 *
 * WHY %g AND NOT SOMETHING SIMPLER. %g is what makes a spreadsheet column
 * read naturally: six significant digits, trailing zeros dropped, the point
 * omitted when nothing follows it, and exponent form only when the value is
 * genuinely too large or too small to write out. Approximating it would show
 * up immediately as columns of 2.50000 and 0.400000.
 *
 * OVERFLOW IS TRUNCATION, and that is kalk.c's behaviour, not a shortcut: its
 * `snprintf(fb, cw + 1, ...)` simply stops at the column width. VisiCalc
 * filled the field with '>' instead, which is arguably the more honest
 * answer, but this port follows its source. The Prog8 port made the same call
 * -- "Numbers never spill; they are truncated to the column."
 * ========================================================================== */

#ifndef KALK_FMT_H
#define KALK_FMT_H

#include <stdint.h>
#include <stdbool.h>
#include "fp.h"

/* Format codes, kalk's own letters. D means "use the global default", which
   is resolved before anything here is called. */
#define FMT_DEFAULT  'D'
#define FMT_GENERAL  'G'
#define FMT_INTEGER  'I'
#define FMT_LEFT     'L'
#define FMT_RIGHT    'R'
#define FMT_DOLLAR   '$'
#define FMT_PERCENT  '%'
#define FMT_BAR      '*'

/* The widest column kalk allows, so a caller can size a buffer without
   thinking. /GC accepts 4..20. */
#define FMT_MAX_WIDTH 20

/* Render FAC's value -- the caller loads it first, exactly as the arithmetic
 * does -- into `out`, which must hold `width` + 1 bytes.
 *
 * Padded to `width` and NUL-terminated: right-aligned for every numeric
 * format, left-aligned for the bar, which is what kalk.c's final snprintf
 * does. Too long for the column means truncated, per the header.
 *
 * Taking the value from FAC rather than by pointer keeps the one-argument
 * shape the rest of the float interface has, and costs the caller nothing --
 * a value being displayed has just been computed or just been loaded.
 */
void fmt_number(uint8_t code, uint8_t width, char *out);

/* A label in a column: left-aligned, truncated, padded. Spilling into empty
   neighbours is the VIEW's decision, not this one -- it needs to know what is
   in the next cell, and this does not. */
void fmt_label(const char *s, uint8_t width, char *out);

/* ERROR and NA, right-aligned like a number so a column of results stays in
   line when one of them fails. */
void fmt_error(uint8_t width, char *out);
void fmt_na(uint8_t width, char *out);

/* ---- exposed for the test ------------------------------------------------
 *
 * The value as sign, significant digits and a decimal exponent:
 *
 *      value = 0.<digits> x 10^exp        digits[0] != '0'
 *
 * so 3.14159 is {"314159", 1}, 40 is {"4", 2}, and 0.4 is {"4", 0}. Zero
 * gives an empty digit string and an exponent of 0.
 *
 * This is the whole trick: f_to_str_trim already produces correct digits, in
 * plain form or exponent form depending on magnitude, and everything above is
 * a matter of reading them back and putting the point somewhere else.
 */
#define FMT_DIGITS 12
typedef struct {
    char    d[FMT_DIGITS + 1];  /* significant digits, no point, NUL-ended  */
    int8_t  exp;                /* the power of ten the point sits before   */
    bool    neg;
    bool    zero;
} fmt_num;

void fmt_normalise(fmt_num *n);         /* reads FAC */

#endif /* KALK_FMT_H */
