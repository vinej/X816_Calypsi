/* ==========================================================================
 * fmttest.c -- does a column read the way a spreadsheet's column should?
 *
 * Every expectation here is what C's printf would produce for the same value
 * and format, because that is literally kalk.c's implementation -- snprintf
 * with "%ld", "%g", "%.2f" and "%*s". Reproducing those rules without a
 * printf is the whole job of fmt.c, so the test is a table of what printf
 * says.
 *
 * The cases are chosen around the boundaries where a hand-written %g goes
 * wrong: the switch to exponent form at either end, the integral-value
 * shortcut that keeps whole numbers free of decimal points, rounding that
 * carries off the top of the digit string, and values below 1 -- which the
 * float package's own formatter renders as 4.00000000e-01 and a column must
 * not.
 *
 * It ends by drawing a small sheet, because a table of strings can be right
 * while the columns still do not line up.
 *
 * BUILD AT -O0.
 * ========================================================================== */

#include "console.h"
#include "shell.h"
#include "fmt.h"
#include "fp.h"
#include "goshell.h"

static uint8_t failed, ncase;

static bool
str_eq(const char *x, const char *y)
{
    while (*x && *y) {
        if (*x != *y) return false;
        x++; y++;
    }
    return *x == '\0' && *y == '\0';
}

/* `want` includes the padding, so alignment is under test too -- a right
   aligned number that lost its spaces would otherwise pass. */
static void
check(const char *in, uint8_t code, uint8_t width, const char *want)
{
    static char bar[]  = "|";
    static char arrow[] = " -> |";
    static char wtag[]  = "  WANT |";
    static char okmark[] = "  ok\n";
    char got[FMT_MAX_WIDTH + 1];

    ncase++;
    fp_from_str(in);
    fmt_number(code, width, got);

    con_puts(in);
    con_puts(arrow);
    con_puts(got);
    con_puts(bar);
    if (str_eq(got, want)) {
        con_puts(okmark);
        return;
    }
    con_puts(wtag);
    con_puts(want);
    con_puts(bar);
    con_putc('\n');
    if (!failed)
        failed = ncase;
}

/* A few rows of a real sheet, drawn where they can be looked at. Numbers
   right-aligned under centred column letters is the arrangement every
   spreadsheet has, and it either lines up or it visibly does not. */
static void
draw_sheet(uint8_t top)
{
    static char hdr[]  = "     A          B          C";
    static char r1[]   = "  1 Widget A";
    static char r2[]   = "  2 Widget B";
    static char r3[]   = "  3 Subtotal";
    static char vals[3][3] = { { '1','0','\0' }, { '2','5','\0' }, { '\0' } };
    static char price[3][8] = { "4.99", "2.5", "" };
    char cell[11];
    uint8_t r, c;

    con_gotoxy(0, top);
    con_puts(hdr);

    for (r = 0; r < 3; r++) {
        con_gotoxy(0, (uint8_t)(top + 1 + r));
        con_puts(r == 0 ? r1 : (r == 1 ? r2 : r3));

        if (r < 2) {
            fp_from_str(vals[r]);
            fmt_number(FMT_GENERAL, 10, cell);
            con_gotoxy(15, (uint8_t)(top + 1 + r));
            con_puts(cell);

            fp_from_str(price[r]);
            fmt_number(FMT_DOLLAR, 10, cell);
            con_gotoxy(26, (uint8_t)(top + 1 + r));
            con_puts(cell);
        }
    }

    /* C3 = 10*4.99 + 25*2.5, computed rather than typed, so the sheet on
       screen is the arithmetic's answer and not a caption. */
    {
        fp_t a, b;
        static char s10[] = "10", s499[] = "4.99", s25[] = "25", s2p5[] = "2.5";
        fp_from_str(s499); fp_store(&a);
        fp_from_str(s10);  fp_mul(&a);  fp_store(&b);   /* 49.9  */
        fp_from_str(s2p5); fp_store(&a);
        fp_from_str(s25);  fp_mul(&a);                  /* 62.5  */
        fp_add(&b);
        fmt_number(FMT_DOLLAR, 10, cell);
        con_gotoxy(26, (uint8_t)(top + 3));
        con_puts(cell);
    }
}

int
main(void)
{
    static char banner[] = "X816 CELL FORMAT\n\n";

    /* --- inputs --- */
    static char i40[]   = "40",        i2p5[]  = "2.5";
    static char i0p4[]  = "0.4",       ipi[]   = "3.14159265";
    static char ineg[]  = "-6",        ibig[]  = "123456789";
    static char ihuge[] = "1234567890", itiny[] = "0.00001";
    static char izero[] = "0",         iround[] = "999999.6";
    static char ihalf[] = "2.675",     ipct[]  = "0.125";
    static char ilong[] = "123456.789";

    /* --- what printf would say --- */
    static char w40[]    = "        40";
    static char w2p5[]   = "       2.5";
    static char w0p4[]   = "       0.4";     /* NOT 4.00000000e-01        */
    static char wpi[]    = "   3.14159";     /* %g: six significant       */
    static char wneg[]   = "        -6";
    static char wbig[]   = " 123456789";     /* integral, under 1e9       */
    /* NOT the %ld path: kalk.c guards it with fabs(val) < 1e9, and this is
       over. So %g, "1.23457e+09", truncated by the column to ten. */
    static char whuge[]  = "1.23457e+0";
    static char wtiny[]  = "     1e-05";     /* X = -5, so exponent form  */
    static char wzero[]  = "         0";
    static char wround[] = "     1e+06";     /* 999999.6 -> 1e+06         */
    static char wdol[]   = "      2.68";     /* %.2f rounds half up       */
    static char wpct[]   = "    12.50%";
    static char wint[]   = "    123456";     /* (long)val TRUNCATES       */
    /* 0, 1 and 2 are all < 2.5, so three stars -- not two. */
    static char wbar[]   = "***       ";
    static char wnarrow[] = "3.14";          /* truncated to the column   */

    con_init();
    con_puts(banner);

    check(i40,   FMT_GENERAL, 10, w40);
    check(i2p5,  FMT_GENERAL, 10, w2p5);
    check(i0p4,  FMT_GENERAL, 10, w0p4);
    check(ipi,   FMT_GENERAL, 10, wpi);
    check(ineg,  FMT_GENERAL, 10, wneg);
    check(ibig,  FMT_GENERAL, 10, wbig);
    check(ihuge, FMT_GENERAL, 10, whuge);
    check(itiny, FMT_GENERAL, 10, wtiny);
    check(izero, FMT_GENERAL, 10, wzero);
    check(iround,FMT_GENERAL, 10, wround);
    check(ihalf, FMT_DOLLAR,  10, wdol);
    check(ipct,  FMT_PERCENT, 10, wpct);
    check(ilong, FMT_INTEGER, 10, wint);
    check(i2p5,  FMT_BAR,     10, wbar);
    check(ipi,   FMT_GENERAL,  4, wnarrow);

    /* ---- and a sheet, because a table of strings can be right while the
       columns still do not line up ------------------------------------- */
    draw_sheet((uint8_t)(3 + ncase + 2));

    {
        static char okv[]  = "\nCELL FORMAT OK\n";
        static char badv[] = "\nFAILED AT CASE ";
        con_gotoxy(0, (uint8_t)(3 + ncase + 7));
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
