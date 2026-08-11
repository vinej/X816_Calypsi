/* ==========================================================================
 * expr.h -- formulas.
 *
 * kalk.c's grammar, which is NOT VisiCalc's. VisiCalc evaluated strictly left
 * to right and 1+2*3 was 9; kalk.c has ordinary precedence and it is 7:
 *
 *      expr    := term (('+' | '-') term)*
 *      term    := primary (('*' | '/') primary)*
 *      primary := number | ref | '(' expr ')' | ('-'|'+') primary | '@' fn
 *
 * References carry VisiCalc's dollars -- A1, $A$1, $A1, A$1 -- and a range is
 * written with THREE dots, A1...B5, which is the notation kalk.c parses and
 * the one a VisiCalc user types.
 *
 * WHY THE RESULT IS NOT A NUMBER
 * ------------------------------
 * kalk.c signals a bad formula by returning NaN and rendering NaN as ERROR.
 * MFLPT has no NaN -- it is five bytes of exponent and mantissa with no room
 * for one -- so a failure has to be carried beside the value rather than
 * inside it. expr_eval returns a status and leaves the number in FAC, and the
 * cell stores the status in its flags. The Prog8 port reached the same
 * arrangement from the same constraint.
 *
 * That turns out to be the better shape anyway, because VisiCalc distinguishes
 * two failures that NaN cannot: @ERROR propagates as "this is wrong" and @NA
 * as "this is not available yet", and a formula that meets both reports
 * ERROR. A single NaN collapses them.
 *
 * EMPTY CELLS ARE ZERO, which is kalk.c's rule and every spreadsheet's: a
 * @SUM over a range with gaps in it is the ordinary case, not an error.
 * Labels are zero too -- a text cell has no value to contribute.
 *
 * RECURSION IS REAL HERE, and it is worth saying because it is the thing the
 * Prog8 port could not have. Prog8's subroutine locals are static, so its
 * parser had to carry an explicit float stack and an operator stack; in C the
 * intermediate operand is simply a local, and locals live on the hardware
 * stack in bank $00 -- which is exactly where the float package requires its
 * operands to be. The two constraints happen to agree.
 * ========================================================================== */

#ifndef KALK_EXPR_H
#define KALK_EXPR_H

#include <stdint.h>
#include <stdbool.h>

#define EXPR_OK     0
#define EXPR_ERROR  1       /* bad syntax, bad domain, divide by zero      */
#define EXPR_NA     2       /* @NA, or a formula that read one             */

/* Evaluate `s` in the context of the cell at (row, col) -- which is what a
 * relative reference is relative to, and what a later /R replicate will
 * rewrite against.
 *
 * The value is left in FAC on EXPR_OK. On the other two it is undefined and
 * the caller should store the flag rather than the number.
 */
uint8_t expr_eval(const char *s, uint16_t row, uint16_t col);

/* True if `s` looks like a formula rather than a number or a label. kalk's
   rule, and it is a rule about the FIRST character only: + - ( @ start a
   formula, a digit or a dot starts a number, anything else is a label. A
   leading quote forces a label and is stripped by the caller. */
bool expr_is_formula(const char *s);

/* Parse a reference at the start of `s`: A1, $A$1, $A1, A$1. Returns the
   number of characters consumed, or 0. Exposed because cell entry and the
   replicate command both need it, and two parsers for one notation is how
   $A$1 comes to mean different things in different places. */
uint8_t expr_parse_ref(const char *s, uint16_t *row, uint16_t *col,
                       bool *abs_col, bool *abs_row);

#endif /* KALK_EXPR_H */
