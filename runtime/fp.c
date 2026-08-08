/* ==========================================================================
 * fp.c -- the one float entry that cannot be a bare thunk.
 *
 * Everything else in fp.h is a register-width crossing and nothing more, so it
 * lives in fpcall.s. Parsing is different: f_from_str reads its string from
 * the PROGRAM BANK, not from bank $00, because in x16lib's world the thing you
 * parse is a literal and a literal is part of the image. fpcall.s's header
 * quotes util/float.s on the point.
 *
 * A spreadsheet never parses a literal. Every number it reads was typed a
 * moment ago and sits in a bank $00 buffer, so it has to be copied into the
 * image first -- which is exactly what float.s tells a caller to do. That copy
 * is the whole of this file.
 *
 * Done in C rather than in the thunk because the copy needs a 24-bit
 * destination and a 16-bit source at once, and C spells that with a __far
 * pointer without any of the direct-page juggling assembly would need. It is
 * the same move shell.c's far_ptr() makes, for the same reason.
 *
 * BUILD AT -O0, like the rest of the runtime.
 * ========================================================================== */

#include "fp.h"

extern uint16_t fp_parse(void);         /* fpcall.s: parses the staged copy */
extern uint32_t fp_strin_addr;          /* fpcall.s: where to stage it      */

/* Must match the `.space` in fpcall.s. Kept as a length rather than trusting
   the caller, because overrunning it would write into the code that follows --
   this buffer is in the image, and the image is executable. */
#define FP_STRIN_MAX 48

bool
fp_from_str(const char *s)
{
    uint8_t __far *dst = (uint8_t __far *)fp_strin_addr;
    uint8_t        i   = 0;

    while (i < FP_STRIN_MAX - 1 && s[i]) {
        dst[i] = (uint8_t)s[i];
        i++;
    }
    dst[i] = 0;

    /* A string too long to stage is REFUSED rather than truncated. Truncation
       would turn "123456789012345678901234567890123456789012345678.5" into a
       different number and report success -- and the caller's whole question
       is "is this a number at all", so a wrong yes is worse than a no. */
    if (s[i] != '\0')
        return false;

    return fp_parse() != 0;
}
