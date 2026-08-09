/* ==========================================================================
 * viewtest.c -- the sheet, drawn, with everything the renderer has to get
 * right visible at once.
 *
 * This one is judged by LOOKING at it as well as by assertion, so it builds a
 * sheet worth looking at: the sample order from the Prog8 port's ORDER.CSV,
 * with a computed subtotal, then exercises the two things a renderer gets
 * wrong quietly.
 *
 *   SPILL. A label wider than its column runs into empty neighbours and is
 *   cut by the first occupied one. "Widget A description here" in a 9-wide
 *   column next to an empty cell must show more than nine characters; the
 *   same label next to a full one must show exactly nine. Both are on screen
 *   together so the difference is the thing you see.
 *
 *   ALIGNMENT. Numbers right, labels left, column letters centred. A column
 *   whose numbers have drifted one place is obvious in a picture and nearly
 *   invisible in a list of strings, which is why the earlier tests drew a
 *   sheet at the end and this one is nothing but.
 *
 * The screen decoder checks the rows it can state exactly. What it cannot
 * check is whether the result looks like a spreadsheet, and that is the part
 * a person reads off the GIF.
 *
 * BUILD AT -O0.
 * ========================================================================== */

#include "console.h"
#include "shell.h"
#include "view.h"
#include "cell.h"
#include "fmt.h"
#include "fp.h"
#include "goshell.h"

static uint8_t failed, ncase;

static void
expect(const char *what, bool cond)
{
    static char okmark[] = "  ok\n";
    static char badmark[] = "  FAILED\n";
    ncase++;
    con_puts(what);
    con_puts(cond ? okmark : badmark);
    if (!cond && !failed)
        failed = ncase;
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

static void
put_num(uint16_t row, uint16_t col, const char *s, uint8_t fmt)
{
    cell c;
    cell_get(row, col, &c);
    c.type = CELL_NUMBER;
    c.fmt  = fmt;
    fp_from_str(s);
    fp_store(&c.value);
    cell_put(row, col, &c);
}

/* The sample sheet: item, quantity, price, total -- and the totals are
   MULTIPLIED here rather than typed, so what appears on screen is the float
   package's answer laid out by the formatter. */
static void
build_sheet(void)
{
    static char h1[] = "Item", h2[] = "Qty", h3[] = "Price", h4[] = "Total";
    static char n1[] = "Widget A", n2[] = "Widget B", n3[] = "Gizmo";
    static char sub[] = "Subtotal";
    static char q[3][4]  = { "10", "25", "3" };
    static char p[3][8]  = { "4.99", "2.5", "12.75" };
    uint16_t r;
    fp_t acc, tmp;

    put_label(0, 0, h1); put_label(0, 1, h2);
    put_label(0, 2, h3); put_label(0, 3, h4);

    put_label(1, 0, n1); put_label(2, 0, n2); put_label(3, 0, n3);

    fp_from_s16(0);
    fp_store(&acc);

    for (r = 0; r < 3; r++) {
        put_num((uint16_t)(r + 1), 1, q[r], FMT_GENERAL);
        put_num((uint16_t)(r + 1), 2, p[r], FMT_DOLLAR);
        /* total = qty * price */
        fp_from_str(p[r]);
        fp_store(&tmp);
        fp_from_str(q[r]);
        fp_mul(&tmp);
        {
            cell c;
            cell_get((uint16_t)(r + 1), 3, &c);
            c.type = CELL_NUMBER;
            c.fmt  = FMT_DOLLAR;
            fp_store(&c.value);
            cell_put((uint16_t)(r + 1), 3, &c);
            fp_load(&c.value);
        }
        fp_add(&acc);
        fp_store(&acc);
    }

    put_label(5, 0, sub);
    {
        cell c;
        cell_get(5, 3, &c);
        c.type = CELL_NUMBER;
        c.fmt  = FMT_DOLLAR;
        fp_load(&acc);
        fp_store(&c.value);
        cell_put(5, 3, &c);
    }
}

int
main(void)
{
    static char banner[] = "X816 SHEET VIEW";
    static char noinit[] = "MEM_ALLOC REFUSED -- is the kernel resident?\n";
    static char longlab[] = "This label runs on";
    static char blocker[] = "STOP";
    static char c_name[]  = "cell names: A1, D6, IV1024";
    static char c_map[]   = "col_at/row_at invert col_x";
    static char c_gut[]   = "the gutter is not a column";
    static char c_scr[]   = "moving off screen scrolls";

    char nm[8];
    uint16_t col, row;
    uint8_t  x;
    bool     good;

    con_init();
    ccur_off();                 /* the program owns the screen now */

    if (!cell_init()) {
        con_puts(noinit);
        goshell_on_esc();
    }
    view_init();
    build_sheet();

    /* Spill, both ways, on one row: row 8 column A has a long label with an
       empty B beside it, and row 9 has the same label with B occupied. */
    put_label(8, 0, longlab);
    put_label(9, 0, longlab);
    put_label(9, 1, blocker);

    view_draw();

    /* ---- assertions, printed below the sheet ---------------------------- */
    con_gotoxy(0, 40);
    con_puts(banner);
    con_putc('\n');

    view_cell_name(0, 0, nm);
    good = nm[0] == 'A' && nm[1] == '1' && nm[2] == '\0';
    view_cell_name(5, 3, nm);
    good = good && nm[0] == 'D' && nm[1] == '6' && nm[2] == '\0';
    /* the last column is IV and the last row is 1024 -- the pair that says
       256 columns really are reachable with two letters */
    view_cell_name(1023, 255, nm);
    good = good && nm[0] == 'I' && nm[1] == 'V' && nm[2] == '1'
                && nm[3] == '0' && nm[4] == '2' && nm[5] == '4';
    expect(c_name, good);

    /* Every visible column must map back to itself from its own first
       character AND from its last, or a click lands one column out. */
    good = true;
    for (col = 0; col < 6; col++) {
        uint16_t back;
        uint8_t w = view_width(col);
        if (!view_col_x(col, &x)) break;
        if (!view_col_at(x, &back) || back != col) { good = false; break; }
        if (!view_col_at((uint8_t)(x + w - 1), &back) || back != col) {
            good = false; break;
        }
    }
    expect(c_map, good);

    good = !view_col_at(0, &col) && !view_col_at(VIEW_GUTTER - 1, &col)
           && !view_row_at(VIEW_HEAD_ROW, &row)
           && !view_row_at(VIEW_STATUS_ROW, &row)
           && view_row_at(VIEW_TOP_ROW, &row) && row == view_top_row();
    expect(c_gut, good);

    /* A move to a cell below the last visible row must scroll, and say it
       did -- that return value is what tells a caller to repaint. */
    good = !view_move_to(3, 3);                 /* on screen already */
    good = good && view_move_to(200, 0) && view_top_row() > 0;
    good = good && view_move_to(0, 0) && view_top_row() == 0;
    expect(c_scr, good);

    {
        static char okv[]  = "\nSHEET VIEW OK\n";
        static char badv[] = "\nFAILED AT CASE ";
        if (failed == 0) {
            con_puts(okv);
        } else {
            con_puts(badv);
            sh_put_hex8(failed);
            con_putc('\n');
        }
    }

    /* Leave the sheet on screen, not the assertions: the picture is half the
       verdict and the GIF keeps only the last frame. */
    view_draw();
    con_gotoxy(0, VIEW_HELP_ROW);
    if (failed == 0) {
        static char okl[] = "SHEET VIEW OK";
        con_puts(okl);
    } else {
        static char badl[] = "SHEET VIEW FAILED";
        con_puts(badl);
    }


/* ESC goes back to the prompt instead of parking here forever. goshell.h is
   explicit that this is where a spin belongs -- with the kernel resident it
   restarts the kernel, so a card full of these can be run one after another
   without resetting the machine between them. The headless runs press
   nothing, so the verdict stays on the last frame either way. */
    goshell_on_esc();
    return 0;
}
