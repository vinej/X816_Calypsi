/* ==========================================================================
 * kalk.c -- the spreadsheet itself: keys in, sheet out.
 *
 * Everything under this is already tested on its own -- the cell store, the
 * formatter, the evaluator, the view -- so this file is the part that was
 * missing: a loop that reads the keyboard, decides what a typed line MEANS,
 * and keeps the screen agreeing with the sheet.
 *
 * WHAT A TYPED LINE MEANS, which is kalk's rule and VisiCalc's before it:
 *
 *      + - ( @    a FORMULA. The text is kept and the value computed.
 *      0-9 .      a NUMBER.
 *      "          a LABEL, forced. The quote is stripped.
 *      anything   a LABEL.
 *
 * So `+A1*2` calculates and `Total` does not, without a mode to be in. The
 * one ambiguity a person actually hits is a label that starts with a digit --
 * a part number, a year -- and the quote is the answer to it.
 *
 * RECALCULATION IS A FIXED POINT, NOT AN ORDER
 * --------------------------------------------
 * A formula may read a cell whose own formula has not run yet, so one pass
 * down the sheet gets dependencies wrong whenever a cell refers forwards.
 * Rather than build a dependency graph -- which needs memory proportional to
 * the formulas and a topological sort -- this evaluates every formula
 * repeatedly until nothing changes, which is what a sheet of a few hundred
 * formulas can afford and what VisiCalc itself did.
 *
 * The pass limit is what makes a CIRCULAR reference terminate. A1 = +A2 and
 * A2 = +A1 never settle; after the limit the sheet keeps whatever it last
 * computed rather than hanging, which is the behaviour a user can recover
 * from by editing one of the two cells.
 *
 * WHAT IS DELIBERATELY NOT HERE YET: the / command menu beyond blank and
 * quit, insert and delete of rows and columns, replicate, locked titles, and
 * CSV. Each is a separate command over this loop rather than a change to it.
 *
 * BUILD AT -O0.
 * ========================================================================== */

#include "console.h"
#include "view.h"
#include "cell.h"
#include "expr.h"
#include "fmt.h"
#include "fp.h"
#include "goshell.h"

/* One line of typing. kalk.c's own MAXIN, and the same limit the text arena
   stores, so anything that fits in the entry line fits in a cell. */
#define ENTRY_MAX CELL_TEXT_MAX

static char    entry[ENTRY_MAX];
static uint8_t entry_len;
static bool    editing;

/* Ten is generous for a sheet whose formulas mostly point upwards and cheap
   for one that does not: the loop stops early the moment a pass changes
   nothing, so the limit is only ever reached by a genuine cycle. */
#define RECALC_PASSES 10

/* ---- recalculation ------------------------------------------------------- */

/* One pass over every formula in the live range. Returns true if any value
   or flag moved, which is what tells the caller another pass is worth doing.
   Bounded by the watermark, not by the grid: that is the whole reason the
   sheet can be 256x1024 and still recalculate at typing speed. */
static bool
recalc_pass(void)
{
    char     src[ENTRY_MAX];
    cell     c;
    uint16_t r, col;
    bool     moved = false;

    if (!cell_any())
        return false;

    for (r = 0; r <= cell_max_row(); r++) {
        if (cell_row_empty(r))
            continue;                   /* a whole row skipped without a read */
        for (col = 0; col <= cell_max_col(); col++) {
            fp_t     was;
            uint8_t  was_flags, st;

            cell_get(r, col, &c);
            if (c.type != CELL_FORMULA)
                continue;

            was_flags = c.flags;
            was[0] = c.value[0]; was[1] = c.value[1]; was[2] = c.value[2];
            was[3] = c.value[3]; was[4] = c.value[4];

            cell_text_get(c.text, src);
            st = expr_eval(src, r, col);

            c.flags = (uint8_t)(c.flags & ~(CELL_ERROR | CELL_NA));
            if (st == EXPR_ERROR) {
                c.flags |= CELL_ERROR;
                fp_zero();
            } else if (st == EXPR_NA) {
                c.flags |= CELL_NA;
                fp_zero();
            }
            fp_store(&c.value);

            if (c.flags != was_flags
                || c.value[0] != was[0] || c.value[1] != was[1]
                || c.value[2] != was[2] || c.value[3] != was[3]
                || c.value[4] != was[4])
                moved = true;

            cell_put(r, col, &c);
        }
    }
    return moved;
}

/* Returns true if anything on the sheet moved, which is what decides between
   repainting two rows and repainting sixty. Typing a NUMBER into a sheet with
   no formulas in it moves nothing, and that is the common case. */
static bool
recalc(void)
{
    uint8_t i;
    bool    any = false;

    for (i = 0; i < RECALC_PASSES; i++) {
        if (!recalc_pass())
            break;                      /* settled */
        any = true;
    }
    return any;
}

/* ---- the entry line ------------------------------------------------------ */

/* Any fixed row, written with putraw so it cannot scroll however long the
   string is or wherever it is put. */
static void
show_at(uint8_t y, const char *s)
{
    uint8_t i;
    for (i = 0; i < CON_COLS; i++)
        con_putraw(i, y, (uint8_t)(s[i] ? s[i] : ' '));
}

/* The whole entry row. Only for starting and ending an edit -- see
   entry_echo for what happens on each keystroke. */
static void
entry_show(void)
{
    uint8_t i;

    for (i = 0; i < CON_COLS; i++)
        con_putraw(i, VIEW_ENTRY_ROW, ' ');
    if (!editing)
        return;

    for (i = 0; i < entry_len && i < CON_COLS - 2; i++)
        con_putraw(i, VIEW_ENTRY_ROW, (uint8_t)entry[i]);
    /* a block where the next character will land, since the console's own
       blinking cursor is off while a program owns the screen */
    if (entry_len < CON_COLS - 2)
        con_putraw(entry_len, VIEW_ENTRY_ROW, 0xDB);
}

/* ONE character, not eighty.
 *
 * Redrawing the whole row per keystroke is 80 cell writes for a line that
 * changed in one place, and on this machine a cell write is six VERA accesses
 * through volatile pointers at -O0. That is the difference between keeping up
 * with a fast typist and falling behind them -- and falling behind is not
 * merely slow here, because keys arrive into a SIXTEEN-entry FIFO that
 * discards what it cannot hold. A typed 11 became a 1 that way. */
static void
entry_echo(void)
{
    if (!editing || entry_len >= CON_COLS - 2)
        return;
    if (entry_len)
        con_putraw((uint8_t)(entry_len - 1), VIEW_ENTRY_ROW,
                   (uint8_t)entry[entry_len - 1]);
    con_putraw(entry_len, VIEW_ENTRY_ROW, 0xDB);
}

static void
entry_begin(char first)
{
    editing = true;
    entry_len = 0;
    if (first) {
        entry[0] = first;
        entry_len = 1;
    }
    entry[entry_len] = '\0';
    entry_show();
}

static void
entry_cancel(void)
{
    editing = false;
    entry_len = 0;
    entry_show();
}

/* Commit what was typed into the cursor cell, deciding what it is. Returns
   true if the recalculation changed anything ELSEWHERE, so the caller knows
   whether the whole sheet needs repainting or just the row it edited. */
static bool
entry_commit(void)
{
    cell     c;
    uint16_t r = view_cur_row(), col = view_cur_col();
    char    *s = entry;

    entry[entry_len] = '\0';
    editing = false;

    if (entry_len == 0) {
        entry_show();
        return false;
    }

    cell_get(r, col, &c);
    c.flags = 0;

    if (*s == '"') {
        s++;                            /* forced label: the quote is syntax */
        c.type = CELL_LABEL;
        c.text = cell_text_put(s);
    } else if (expr_is_formula(s)) {
        c.type = CELL_FORMULA;
        c.text = cell_text_put(s);
        /* Evaluated once here so the cell shows something immediately; the
           full recalculation below settles anything it depends on. */
        {
            uint8_t st = expr_eval(s, r, col);
            if (st == EXPR_ERROR) { c.flags |= CELL_ERROR; fp_zero(); }
            else if (st == EXPR_NA) { c.flags |= CELL_NA; fp_zero(); }
            fp_store(&c.value);
        }
    } else if (fp_from_str(s)) {
        c.type = CELL_NUMBER;
        c.text = 0;
        fp_store(&c.value);
    } else {
        /* Not a number and not a formula, so it is a label -- which is how
           `Total` and `12 units` both end up as text without a mode. */
        c.type = CELL_LABEL;
        c.text = cell_text_put(s);
    }

    cell_put(r, col, &c);
    entry_len = 0;
    return recalc();
}

/* ---- commands ------------------------------------------------------------ */

/* `>` in VisiCalc: jump to a cell by name. Typed on the entry line and
   parsed by the same reference parser formulas use, so >$B$4 works and means
   what it says. */
static void
do_goto(void)
{
    uint16_t r, c;
    bool     ac, ar;
    if (expr_parse_ref(entry, &r, &c, &ac, &ar))
        view_move_to(r, c);
    entry_cancel();
}

static bool
do_blank(void)
{
    cell c;
    cell_get(view_cur_row(), view_cur_col(), &c);
    c.type = CELL_EMPTY;
    c.flags = 0;
    c.text = 0;
    fp_zero();
    fp_store(&c.value);
    cell_put(view_cur_row(), view_cur_col(), &c);
    return recalc();
}

/* ---- the loop ------------------------------------------------------------ */

int
main(void)
{
    static char noinit[] = "MEM_ALLOC REFUSED -- is the kernel resident?\n";
    /* Under 80 characters, and drawn with putraw. con_puts at the LAST row
       wraps and therefore SCROLLS -- which moved the whole sheet up by one
       and put the header row where the status line belongs. It looked like a
       layout bug in the view and was a string one character too long. */
    static char help[]   = "arrows move  type  \" label  INS blank  "
                           "! recalc  > goto  ESC quit";
    uint16_t k;
    bool     goto_mode = false;

    con_init();
    ccur_off();                 /* the program owns the screen */

    if (!cell_init()) {
        con_puts(noinit);
        goshell_on_esc();
        return 0;
    }
    view_init();
    view_draw();
    show_at(VIEW_HELP_ROW, help);

    for (;;) {
        uint16_t r = view_cur_row(), c = view_cur_col();
        bool     scrolled = false;
        bool     moved = false;

        k = con_getc();

        /* ---- while typing ------------------------------------------------ */
        if (editing) {
            /* Return and Tab both commit AND ADVANCE -- down and right
               respectively -- which is what makes typing a column of figures
               one continuous action rather than type-enter-arrow-repeat. It
               is VisiCalc's behaviour and the Prog8 port's. */
            if (k == 0x0D || k == 0x09) {
                if (goto_mode) {
                    goto_mode = false;
                    do_goto();
                } else {
                    bool spread = entry_commit();
                    bool jumped = false;
                    if (k == 0x0D) {
                        if (r + 1 < KALK_ROWS)
                            jumped = view_move_to((uint16_t)(r + 1), c);
                    } else if (c + 1 < KALK_COLS) {
                        jumped = view_move_to(r, (uint16_t)(c + 1));
                    }
                    /* TWO ROWS, unless something moved that is not on them.
                     * A full repaint is around 4,500 character writes; typing
                     * a number into a sheet with no formulas changes exactly
                     * one cell, and repainting the other 4,400 for it is not
                     * merely wasteful. It is long enough that keys arriving
                     * meanwhile overrun the 16-entry keyboard FIFO -- which
                     * is how a typed 345 became a 3, and how two Returns went
                     * missing out of five.
                     *
                     * spread is the recalculation reporting that a formula
                     * somewhere else changed; jumped is the view reporting a
                     * scroll. Either genuinely needs the whole screen. */
                    if (spread || jumped) {
                        view_draw();
                    } else {
                        view_draw_row(r);
                        view_draw_row(view_cur_row());
                        view_draw_cursor();
                        view_draw_status();
                    }
                }
                show_at(VIEW_HELP_ROW, help);
                entry_show();
                continue;
            }
            if (k == 0x1B) {                        /* ESC abandons it */
                goto_mode = false;
                entry_cancel();
                continue;
            }
            if (k == 0x08) {                        /* backspace */
                if (entry_len)
                    entry_len--;
                entry[entry_len] = '\0';
                con_putraw((uint8_t)(entry_len + 1), VIEW_ENTRY_ROW, ' ');
                entry_echo();
                continue;
            }
            if (k > 0xFF)                           /* a key with no character */
                continue;
            if (entry_len + 1 < ENTRY_MAX) {
                entry[entry_len++] = (char)k;
                entry[entry_len] = '\0';
            }
            entry_echo();
            continue;
        }

        /* ---- navigating -------------------------------------------------- */
        switch (k) {
        case KEY_LEFT:  if (c) { scrolled = view_move_to(r, (uint16_t)(c - 1)); moved = true; } break;
        case KEY_RIGHT: if (c + 1 < KALK_COLS) { scrolled = view_move_to(r, (uint16_t)(c + 1)); moved = true; } break;
        case KEY_UP:    if (r) { scrolled = view_move_to((uint16_t)(r - 1), c); moved = true; } break;
        case KEY_DOWN:  if (r + 1 < KALK_ROWS) { scrolled = view_move_to((uint16_t)(r + 1), c); moved = true; } break;
        case KEY_HOME:  scrolled = view_move_to(0, 0); moved = true; break;
        case KEY_PGUP:
            scrolled = view_move_to((r > VIEW_ROWS) ? (uint16_t)(r - VIEW_ROWS) : 0, c);
            moved = true;
            break;
        case KEY_PGDN:
            scrolled = view_move_to(((uint16_t)(r + VIEW_ROWS) < KALK_ROWS)
                                    ? (uint16_t)(r + VIEW_ROWS) : (KALK_ROWS - 1), c);
            moved = true;
            break;
        case KEY_INS:                               /* blank the cell */
            if (do_blank())
                view_draw();
            else {
                view_draw_row(r);
                view_draw_cursor();
                view_draw_status();
            }
            show_at(VIEW_HELP_ROW, help);
            continue;
        case 0x09:                                  /* Tab advances right */
            if (c + 1 < KALK_COLS) { scrolled = view_move_to(r, (uint16_t)(c + 1)); moved = true; }
            break;
        case 0x0D:                                  /* Return advances down */
            if (r + 1 < KALK_ROWS) { scrolled = view_move_to((uint16_t)(r + 1), c); moved = true; }
            break;
        case 0x1B:                                  /* ESC leaves */
            goshell();                              /* does not return */
            continue;
        case '!':
            recalc();
            view_draw();
            show_at(VIEW_HELP_ROW, help);
            continue;
        case '>':
            goto_mode = true;
            entry_begin('\0');
            continue;
        default:
            /* Anything printable starts an entry, carrying the character
               that started it -- so typing simply works, with no mode to
               enter first. */
            if (k >= 0x20 && k <= 0xFF) {
                entry_begin((char)k);
                continue;
            }
            continue;
        }

        if (moved) {
            if (scrolled) {
                view_draw();
    show_at(VIEW_HELP_ROW, help);
            } else {
                /* Two rows and the status line, not the screen: the row the
                   cursor left has to lose its highlight and the row it
                   arrived on has to gain one. This is the whole reason
                   view_move_to reports whether it scrolled. */
                view_draw_row(r);
                view_draw_row(view_cur_row());
                view_draw_cursor();
                view_draw_status();
            }
        }
    }
    return 0;
}
