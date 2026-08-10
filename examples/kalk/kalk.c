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
 * WHAT IS DELIBERATELY NOT HERE YET: insert and delete of rows and columns,
 * replicate, and locked titles. Each is a separate command over this loop
 * rather than a change to it -- which is the reason the / menu was worth
 * having before any of them existed, since each one is now a letter rather
 * than a redesign. CSV arrived that way: /SL, /SS and /SQ are three cases in
 * the menu over sheet.c, and nothing else in this file changed for them.
 *
 * BUILD AT -O0.
 * ========================================================================== */

#include "console.h"
#include "view.h"
#include "cell.h"
#include "expr.h"
#include "fmt.h"
#include "sheet.h"
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
                || c.value[4] != was[4]) {
                moved = true;
                /* Per ROW, not a blanket clear. A recalculation usually moves
                   a handful of cells out of a screenful, and dirtying only
                   their rows is what keeps the repaint that follows to those
                   rows' worth of formatting. */
                view_dirty_row(r);
            }

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
   string is or wherever it is put. Padded to the full width, so a shorter
   string blanks whatever the row held before.
 *
 * THE NUL IS A STOP, NOT A SUBSTITUTION. Written as `s[i] ? s[i] : ' '` this
 * keeps INDEXING past the terminator for the rest of the row and prints
 * whatever happens to be next in memory -- which put "MEM_ALLOC REFU" on the
 * end of the help line, that being the next string the linker had placed. It
 * read like a memory-corruption bug and was a loop that did not know where
 * its string ended. */
static void
show_at(uint8_t y, const char *s)
{
    uint8_t i;
    bool    done = false;

    for (i = 0; i < CON_COLS; i++) {
        if (!done && s[i] == '\0')
            done = true;
        con_putraw(i, y, done ? (uint8_t)' ' : (uint8_t)s[i]);
    }
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
    uint16_t r = view_cur_row(), col = view_cur_col();
    char    *s = entry;

    entry[entry_len] = '\0';
    editing = false;

    if (entry_len == 0) {
        entry_show();
        return false;
    }

    /* The rule itself lives in sheet.c, because the CSV loader has to apply
       exactly the same one -- a file that said 2024 must become the cell the
       user would have got by typing it. Two copies of this is how a sheet
       starts reloading differently from how it was entered. */
    sheet_set_text(r, col, s);
    view_dirty_row(r);              /* the cell just changed; the cache cannot see that */
    entry_len = 0;
    return recalc();
}

/* ---- commands ------------------------------------------------------------ */

/* `>` in VisiCalc: jump to a cell by name. Typed on the entry line and
   parsed by the same reference parser formulas use, so >$B$4 works and means
   what it says.

   Returns whether the view scrolled, for the same reason view_move_to does:
   the caller has to repaint EITHER WAY. It did not, and a goto therefore
   moved the cursor while leaving the old highlight and the old status line on
   screen -- so the jump looked like it had been ignored, and the next thing
   typed went into a cell nobody could see was selected. A jump is exactly the
   move most likely to scroll, which is why this is the one place the return
   value cannot be dropped. */
static bool
do_goto(void)
{
    uint16_t r, c;
    bool     ac, ar;
    bool     scrolled = false;

    if (expr_parse_ref(entry, &r, &c, &ac, &ar))
        scrolled = view_move_to(r, c);
    entry_cancel();
    return scrolled;
}

/* ---- the / command menu --------------------------------------------------
 *
 * VisiCalc's menu, and kalk's: `/` then a letter, with `/G` the only one that
 * takes a second. The keys are not invented here -- they are the set the
 * Prog8 port's README documents, so a sheet built on one machine is driven
 * the same way on the other.
 *
 *      /B          blank the cell            /F <code>   this cell's format
 *      /C          clear the whole sheet     /GF <code>  every cell's format
 *      /Q          quit                      /GC <n>     column width, 4-20
 *
 * A menu is a MODE, and a mode that cannot be left is a trap: ESC backs out
 * of any of these, and an unrecognised key backs out rather than being
 * swallowed. The prompt says what is accepted while it is waiting, because
 * the one thing worse than a mode is a mode with no indication of what it
 * wants.
 *
 * WHAT IS STILL NOT HERE: /IR /IC /DR /DC (insert and delete, which have to
 * rewrite every reference), /M (move), /R (replicate), /TV /TH (locked
 * titles) and /SL /SS /SQ (CSV). Each is a command over this loop rather
 * than a change to it, which is why the menu is worth having before they
 * exist.
 */
#define CMD_OFF   0
#define CMD_MENU  1             /* / pressed, waiting for the letter        */
#define CMD_FMT   2             /* /F, waiting for a format code            */
#define CMD_G     3             /* /G, waiting for C or F                   */
#define CMD_GFMT  4             /* /GF, waiting for a format code           */
#define CMD_GCOL  5             /* /GC, typing a width                      */
#define CMD_S     6             /* /S, waiting for L, S or Q                */
#define CMD_NAME  7             /* typing a filename for one of those       */
#define CMD_I     8             /* /I, waiting for R or C                   */
#define CMD_D     9             /* /D, waiting for R or C                   */
#define CMD_RFROM 10            /* /R, typing the range to copy FROM        */
#define CMD_RTO   11            /* /R, typing the cell to copy TO           */

static uint8_t cmd;
static uint8_t cmd_num;         /* the width being typed for /GC            */
static uint8_t cmd_file;        /* which of /SL /SS /SQ wants the name      */
static char    cmd_name[40];    /* the filename or range being typed        */
static uint8_t cmd_namelen;
static uint16_t rep_r1, rep_c1, rep_r2, rep_c2;   /* /R's source block      */

/* kalk's format codes. Rejecting anything else is what stops /F X leaving a
   cell formatted with a letter the formatter will not recognise -- fmt.c
   falls back to general for an unknown code, so the cell would look right
   and be wrong. */
static bool
fmt_code_ok(char c)
{
    return c == FMT_LEFT || c == FMT_RIGHT || c == FMT_INTEGER
        || c == FMT_GENERAL || c == FMT_DEFAULT || c == FMT_DOLLAR
        || c == FMT_PERCENT || c == FMT_BAR;
}

static char
up(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
}

static void
cmd_prompt(void)
{
    static char p_menu[] = "/  B blank  C clear  F format  G global  "
                           "I ins  D del  R repl  S files  Q quit  ESC";
    static char p_fmt[]  = "/F  format code:  L left  R right  I integer  "
                           "G general  D default  $  %  *";
    static char p_g[]    = "/G  C column width   F format";
    static char p_gfmt[] = "/GF global format:  L left  R right  I integer  "
                           "G general  $  %  *";
    static char p_gcol[] = "/GC global column width (4-20), then Return: ";
    static char p_s[]    = "/S  L load   S save   Q save and quit";
    static char p_i[]    = "/I  R insert a row here   C insert a column here";
    static char p_d[]    = "/D  R delete this row     C delete this column";
    static char p_load[] = "/SL load which file, then Return: ";
    static char p_save[] = "/SS save as, then Return: ";
    static char p_from[] = "/R replicate FROM (a cell, or A1...B5), then Return: ";
    static char p_to[]   = "/R  ...TO which cell, then Return: ";
    static char p_off[]  = "";
    const char *s;

    switch (cmd) {
    case CMD_MENU: s = p_menu; break;
    case CMD_FMT:  s = p_fmt;  break;
    case CMD_G:    s = p_g;    break;
    case CMD_GFMT: s = p_gfmt; break;
    case CMD_GCOL: s = p_gcol; break;
    case CMD_S:    s = p_s;    break;
    case CMD_I:    s = p_i;    break;
    case CMD_D:    s = p_d;    break;
    case CMD_NAME: s = (cmd_file == 'L') ? p_load : p_save; break;
    case CMD_RFROM: s = p_from; break;
    case CMD_RTO:   s = p_to;   break;
    default:       s = p_off;  break;
    }
    show_at(VIEW_ENTRY_ROW, s);
    /* Every state that TYPES echoes it back after the prompt, with a block
       where the next character will land. One place, because a prompt that
       silently swallows keystrokes is what makes a mode feel broken. */
    if (cmd == CMD_NAME || cmd == CMD_RFROM || cmd == CMD_RTO) {
        const char *pr = s;
        uint8_t i = 0, j;
        while (pr[i])
            i++;
        for (j = 0; j < cmd_namelen && (uint16_t)(i + j) < CON_COLS - 1; j++)
            con_putraw((uint8_t)(i + j), VIEW_ENTRY_ROW, (uint8_t)cmd_name[j]);
        if ((uint16_t)(i + j) < CON_COLS)
            con_putraw((uint8_t)(i + j), VIEW_ENTRY_ROW, 0xDB);
        return;
    }
    if (cmd == CMD_GCOL && cmd_num) {
        /* echo the digits after the prompt, so a mistyped width is visible
           before Return rather than after it */
        uint8_t i = 0;
        while (p_gcol[i])
            i++;
        if (cmd_num >= 10)
            con_putraw(i++, VIEW_ENTRY_ROW, (uint8_t)('0' + cmd_num / 10));
        con_putraw(i, VIEW_ENTRY_ROW, (uint8_t)('0' + cmd_num % 10));
    }
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
    view_dirty_row(view_cur_row());
    return recalc();
}

/* /F -- this cell only. A format is presentation, so nothing is recalculated
   and only the row it is on has to be redrawn. */
static void
do_format(char code)
{
    cell c;
    cell_get(view_cur_row(), view_cur_col(), &c);
    c.fmt = (uint8_t)code;
    cell_put(view_cur_row(), view_cur_col(), &c);
    view_dirty_row(view_cur_row());
}

/* /C -- back to an empty sheet. cell_clear_all forgets the rows rather than
   writing over four megabytes, and view.h is explicit that whoever calls it
   owes a view_dirty_all: every cached line describes a sheet that no longer
   exists, and there is no row to pin that to. This is that caller. */
static void
do_clear_all(void)
{
    cell_clear_all();
    view_dirty_all();
    view_move_to(0, 0);
}

/* /SL, /SS, /SQ. The sheet is saved as its SOURCE -- formula text, not
   results -- and loaded by the same rule the entry line uses, so a file
   written here reads back as the sheet that wrote it. sheet.h has the format
   and the two places it is not exactly reversible.

   The message goes on the ENTRY line rather than the status line, because
   that is where the question was asked and it is the row already being
   redrawn. Naming the fault matters: "cannot save" sends somebody looking at
   the sheet, and "no card" sends them to the slot. */
static void
file_report(bool ok, bool saving)
{
    static char m_saved[] = "saved";
    static char m_loaded[] = "loaded";
    static char m_card[]  = "NO CARD -- is one in the slot?";
    static char m_path[]  = "that name will not resolve";
    static char m_file[]  = "no such file, or it cannot be created";
    static char m_io[]    = "the transfer failed part way";
    static char m_big[]   = "the file has more rows or columns than the sheet";
    static char m_huh[]   = "it did not work";
    const char *m;

    if (ok) {
        m = saving ? m_saved : m_loaded;
    } else {
        switch (sheet_error()) {
        case SHEET_ENOCARD: m = m_card; break;
        case SHEET_ENOPATH: m = m_path; break;
        case SHEET_ENOFILE: m = m_file; break;
        case SHEET_EIO:     m = m_io;   break;
        case SHEET_EBIG:    m = m_big;  break;
        default:            m = m_huh;  break;
        }
    }
    show_at(VIEW_ENTRY_ROW, m);
}

/* Returns whether the screen needs a full repaint. A load replaces every
   cell, so the whole render cache goes with it -- view.h is explicit that
   nobody else can know that. */
static bool
do_file(uint8_t which, const char *name)
{
    bool ok;

    if (which == 'L') {
        ok = sheet_load_csv(name);
        view_dirty_all();
        view_move_to(0, 0);
        if (ok)
            recalc();               /* a file holds sources, not results */
        file_report(ok, false);
        return true;                /* either way the sheet changed */
    }

    ok = sheet_save_csv(name);
    file_report(ok, true);
    if (ok && which == 'Q')
        goshell();                  /* does not return */
    return false;                   /* saving changes nothing on screen */
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
    static char help[]   = "arrows move  type  \" label  / commands  "
                           "INS blank  ! recalc  > goto  ESC quit";
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

        /* ---- inside a / command ------------------------------------------ */
        /* Before the editing branch, because a menu is not an entry: the keys
           that mean something here mean something else while typing, and a
           mode that leaks into the one below it is how /F 5 ends up putting a
           5 in a cell. */
        if (cmd != CMD_OFF) {
            static char badref[] = "that is not a cell or a range -- try A1 or A1...B5";
            bool whole = false;         /* does the sheet need repainting?   */
            bool done  = true;          /* is the command finished?          */

            if (k == 0x1B) {            /* ESC backs out of any of them */
                cmd = CMD_OFF;
                entry_show();
                continue;
            }
            if (k > 0xFF) continue;     /* a key with no character */

            switch (cmd) {
            case CMD_MENU:
                switch (up((char)k)) {
                /* do_blank's answer says whether a formula somewhere else
                   moved. The menu repaints the screen either way -- a command
                   is a rare, deliberate act and a cached repaint is 74 ms,
                   which is not worth a special case to avoid. */
                case 'B': (void)do_blank(); whole = true; break;
                case 'C': do_clear_all(); whole = true; break;
                case 'F': cmd = CMD_FMT;  done = false; break;
                case 'G': cmd = CMD_G;    done = false; break;
                case 'S': cmd = CMD_S;    done = false; break;
                case 'I': cmd = CMD_I;    done = false; break;
                case 'D': cmd = CMD_D;    done = false; break;
                case 'R': cmd = CMD_RFROM; cmd_namelen = 0;
                          cmd_name[0] = 0; done = false; break;
                case 'Q': goshell();      break;    /* does not return */
                default:  break;                    /* anything else cancels */
                }
                break;

            case CMD_FMT:
                if (fmt_code_ok(up((char)k))) {
                    do_format(up((char)k));
                    whole = true;
                }
                break;

            case CMD_G:
                switch (up((char)k)) {
                case 'C': cmd = CMD_GCOL; cmd_num = 0; done = false; break;
                case 'F': cmd = CMD_GFMT;              done = false; break;
                default:  break;
                }
                break;

            case CMD_GFMT:
                if (fmt_code_ok(up((char)k))) {
                    view_set_global_fmt((uint8_t)up((char)k));
                    whole = true;       /* view_set_global_fmt dirtied it all */
                }
                break;

            /* /IR /IC /DR /DC. Everything after the line moves and every
               formula's references are rewritten, so the whole cache goes and
               the sheet is recalculated -- the values did not change but
               which cells they came from did. */
            case CMD_I:
            case CMD_D:
                {
                    bool ins = (cmd == CMD_I);
                    char what = up((char)k);
                    if (what == 'R')
                        ins ? sheet_insert_row(view_cur_row())
                            : sheet_delete_row(view_cur_row());
                    else if (what == 'C')
                        ins ? sheet_insert_col(view_cur_col())
                            : sheet_delete_col(view_cur_col());
                    else
                        break;                  /* anything else cancels */
                    view_dirty_all();
                    recalc();
                    whole = true;
                }
                break;

            case CMD_S:
                switch (up((char)k)) {
                case 'L': case 'S': case 'Q':
                    cmd_file = (uint8_t)up((char)k);
                    cmd = CMD_NAME;
                    cmd_namelen = 0;
                    cmd_name[0] = 0;
                    done = false;
                    break;
                default:
                    break;
                }
                break;

            /* /R takes two answers, so it is two states with the same
               editing behaviour. The FROM is a range and the TO is the corner
               it lands on -- the block is copied once, not tiled, which is
               the reference port's rule and the one that makes replicating a
               column of totals do what is expected. */
            case CMD_RFROM:
            case CMD_RTO:
                if (k == 0x08) {
                    if (cmd_namelen)
                        cmd_namelen--;
                    cmd_name[cmd_namelen] = 0;
                    done = false;
                } else if (k == 0x0D) {
                    uint16_t a1, b1, a2, b2;
                    if (!cmd_namelen) {
                        break;                      /* empty cancels */
                    }
                    if (!sheet_parse_range(cmd_name, &a1, &b1, &a2, &b2)) {
                        show_at(VIEW_ENTRY_ROW, badref);
                        break;                      /* a typo, and it says so */
                    }
                    if (cmd == CMD_RFROM) {
                        rep_r1 = a1; rep_c1 = b1; rep_r2 = a2; rep_c2 = b2;
                        cmd = CMD_RTO;
                        cmd_namelen = 0;
                        cmd_name[0] = 0;
                        done = false;
                    } else {
                        sheet_replicate(rep_r1, rep_c1, rep_r2, rep_c2,
                                       a1, b1, a2, b2);
                        view_dirty_all();
                        recalc();
                        whole = true;
                    }
                } else if (k >= 0x20 && k < 0x7F) {
                    if (cmd_namelen + 1 < sizeof cmd_name) {
                        cmd_name[cmd_namelen++] = (char)k;
                        cmd_name[cmd_namelen] = 0;
                    }
                    done = false;
                } else {
                    done = false;
                }
                break;

            case CMD_NAME:
                if (k == 0x08) {                    /* backspace */
                    if (cmd_namelen)
                        cmd_namelen--;
                    cmd_name[cmd_namelen] = 0;
                    done = false;
                } else if (k == 0x0D) {
                    if (cmd_namelen)
                        whole = do_file(cmd_file, cmd_name);
                    /* An empty name cancels, rather than trying to open "" */
                } else if (k >= 0x20 && k < 0x7F) {
                    if (cmd_namelen + 1 < sizeof cmd_name) {
                        cmd_name[cmd_namelen++] = (char)k;
                        cmd_name[cmd_namelen] = 0;
                    }
                    done = false;
                } else {
                    done = false;                   /* ignore, keep typing */
                }
                break;

            case CMD_GCOL:
                if (k >= '0' && k <= '9') {
                    uint8_t n = (uint8_t)(cmd_num * 10 + (k - '0'));
                    if (n <= VIEW_WIDTH_MAX)
                        cmd_num = n;
                    done = false;
                } else if (k == 0x08) {             /* backspace */
                    cmd_num = (uint8_t)(cmd_num / 10);
                    done = false;
                } else if (k == 0x0D) {
                    /* Out of range is CLAMPED, not refused: view_set_global_width
                       already bounds it, and a width silently doing nothing is
                       worse than a width doing the nearest legal thing. */
                    if (cmd_num)
                        view_set_global_width(cmd_num);
                    whole = true;
                }
                break;

            default:
                break;
            }

            if (!done) {
                cmd_prompt();
                continue;
            }
            cmd = CMD_OFF;
            if (whole) {
                view_draw();
                show_at(VIEW_HELP_ROW, help);
            }
            entry_show();
            continue;
        }

        /* ---- while typing ------------------------------------------------ */
        if (editing) {
            /* Return and Tab both commit AND ADVANCE -- down and right
               respectively -- which is what makes typing a column of figures
               one continuous action rather than type-enter-arrow-repeat. It
               is VisiCalc's behaviour and the Prog8 port's. */
            if (k == 0x0D || k == 0x09) {
                if (goto_mode) {
                    goto_mode = false;
                    if (do_goto()) {
                        view_draw();
                    } else {
                        /* The row jumped FROM has to lose its highlight, and
                           the row jumped TO has to gain one -- the same two
                           rows an arrow key repaints. */
                        view_draw_row(r);
                        view_draw_row(view_cur_row());
                        view_draw_cursor();
                        view_draw_status();
                    }
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
                     * spread is the recalculation reporting that a formula
                     * somewhere else changed; jumped is the view reporting a
                     * scroll. Either genuinely needs the whole screen.
                     *
                     * BOTH ARMS ARE STILL WORTH HAVING with the render cache
                     * in. The cache made view_draw cheap -- 74 ms for 56 rows
                     * that are all hits -- but it did not make it free, and
                     * the arm below is the one that runs when somebody types a
                     * column of figures into a sheet with no formulas in it,
                     * which is the common case and touches two rows.
                     *
                     * The cache is what fixed the dropped keystrokes on the
                     * OTHER arm: a commit whose sum changes repaints all 56,
                     * and composing those cost more than the typing that was
                     * arriving meanwhile, so the 16-entry FIFO overran and a
                     * typed 345 became a 3. run-kalk.sh types this exact case
                     * and its negative control reproduces the loss. */
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
        case '/':
            cmd = CMD_MENU;
            cmd_prompt();
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
