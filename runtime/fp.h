/* ==========================================================================
 * fp.h -- the library's floating point, callable from C.
 *
 * x16lib's util/float.asm is 2,000 lines of software MFLPT arithmetic,
 * written for X816 because there is no ROM to bind to. It is the only float
 * package on this machine that a SMALL DATA MODEL program can reach: Calypsi's
 * own is unusable here (its const tables land in bank $01 and cannot be
 * addressed from bank $00 -- see shell.h), and where it does link, its 32-bit
 * transcendentals are wrong. Measured: sqrtf(2) came back 12.109, sin and tan
 * came back zero, and atan hung the machine. So this, not math.h.
 *
 * WHY A BRIDGE IS NEEDED AT ALL
 * -----------------------------
 * x16lib is 65C02 code. A/X/Y must be EIGHT BITS WIDE before any call into it,
 * and cstartup leaves them sixteen for C -- examples/kernel/libfs.s says this
 * in its header and every assembly caller in the tree opens by narrowing them.
 * C cannot. So each entry here is a thunk that narrows, calls, widens, and
 * hands the result back the way Calypsi expects it.
 *
 * ONE ARGUMENT, ALWAYS, and the reason is the one kcall.s gives: Calypsi's C
 * argument passing changes with arity AND with type -- first argument in A,
 * a second pushed, a third pushed ahead of it, a __far pointer in the
 * direct-page pseudo-registers regardless of position. One argument is
 * unambiguous under all of it: it is in A. Two thunks written against a
 * guessed convention were already wrong once in this tree before the rule was
 * measured, so nothing here takes two.
 *
 * FAC IS THE ACCUMULATOR, and this reads like the library rather than like C:
 *
 *     fp_from_s16(10);   fp_store(&a);       // a = 10
 *     fp_from_s16(4);    fp_store(&b);       // b = 4
 *     fp_load(&a);       fp_div(&b);         // FAC = 2.5
 *     puts(fp_to_str_trim());                // "2.5"
 *
 * An expression evaluator wants exactly this shape: one implicit accumulator
 * and a memory operand. It is also why f_rsub and f_rdiv exist -- they compute
 * mem - FAC and mem / FAC, which is what a left-to-right parser needs.
 *
 * OPERANDS MUST LIVE IN BANK $00
 * ------------------------------
 * The library follows a pointer with (dp),y, which reaches bank $00 and only
 * bank $00. An fp_t in ordinary C data satisfies that; one in the cell grid
 * does NOT, because the grid is far. Stage it through a near fp_t first. That
 * is not a wart to route around -- it is the same near/far staging kfs.c does
 * for its transfers, and it is why the grid can be four megabytes.
 * ========================================================================== */

#ifndef X816_FP_H
#define X816_FP_H

#include <stdint.h>
#include <stdbool.h>

/* The on-disk and in-memory float: five bytes of MFLPT, byte-for-byte what
   the X16 stored, so a sheet written by either machine means the same thing.
       byte 0    exponent, excess-128; 0 means the value IS zero
       byte 1    sign in bit 7, then the mantissa's top 7 bits
       bytes 2-4 the rest of the mantissa, big-endian
   Rounding is TRUNCATION -- values exact in binary stay exact, and the ninth
   digit of anything else may not. fp_to_str rounds the digit it prints. */
#define FP_SIZE 5
typedef uint8_t fp_t[FP_SIZE];

/* ---- moving values in and out ------------------------------------------ */
void fp_load(const void *p);        /* FAC = *p                             */
void fp_store(void *p);             /* *p  = FAC                            */
void fp_zero(void);                 /* FAC = 0                              */

void fp_from_s16(int16_t v);
void fp_from_u8(uint8_t v);
int16_t fp_to_s16(void);            /* truncates toward zero, SATURATES     */

/* Parses [+-]digits[.digits][eE[+-]digits]. False means nothing was parsed --
   which is how a cell tells a number from a label. */
bool fp_from_str(const char *s);

/* Both return a string in bank $00 owned by the library, valid until the next
   conversion. fp_to_str keeps the leading space the ROM put in front of a
   positive number; fp_to_str_trim does not, and is the one you want. */
char *fp_to_str(void);
char *fp_to_str_trim(void);

/* ---- arithmetic: FAC op= *p -------------------------------------------- */
void fp_add(const void *p);
void fp_sub(const void *p);         /* FAC - *p                             */
void fp_mul(const void *p);
void fp_div(const void *p);         /* FAC / *p; divisor 0 answers 0        */
void fp_rsub(const void *p);        /* *p - FAC                             */
void fp_rdiv(const void *p);        /* *p / FAC                             */
void fp_pow(const void *p);         /* FAC ^ *p                             */
void fp_rpow(const void *p);        /* *p ^ FAC                             */

/* ---- tests -------------------------------------------------------------- */
int8_t fp_sgn(void);                /* -1, 0, +1 for FAC                    */
int8_t fp_cmp(const void *p);       /* -1, 0, +1 for FAC against *p; FAC is
                                       PRESERVED -- comparing consumes
                                       nothing, which is what a sort wants  */

/* ---- unary, in place on FAC --------------------------------------------
 *
 * Domain errors ANSWER rather than abort, because there is no BASIC error
 * handler to land in: sqrt of a negative is 0, ln of zero is 0 and of a
 * negative uses |x|. A caller wanting a diagnostic tests with fp_sgn first --
 * which is what a spreadsheet showing ERROR has to do anyway.
 */
void fp_abs(void);
void fp_neg(void);
void fp_int(void);                  /* truncate toward zero                 */
void fp_sqrt(void);
void fp_sin(void);
void fp_cos(void);
void fp_tan(void);
void fp_atan(void);
void fp_ln(void);
void fp_exp(void);

#endif /* X816_FP_H */
