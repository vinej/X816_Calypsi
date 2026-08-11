/* ==========================================================================
 * benchtest.c -- where a repaint actually goes, split four ways.
 *
 * WHY THIS EXISTS BEFORE THE CACHE
 * --------------------------------
 * The previous round measured a 56-row repaint at 364 ms and concluded the
 * cost was formatting rather than writing. The two numbers it recorded do not
 * agree with each other: con_putraw at 90 us times 80 columns is 7.2 ms, and
 * a whole row was measured at 6.44 ms -- which leaves nothing for the
 * formatter. One of the readings is measuring something other than what its
 * name says, and building a cache on top of that is guessing again.
 *
 * The machine's own history says not to. smc.s carries three rounds of
 * optimising a poll loop whose fault was in the slave; con_putrun carries the
 * second. So this splits the repaint into parts that sum, with each part
 * timed on its own against the millisecond counter at $9F90:
 *
 *      GET    448 cell_get out of the 4 MiB grid
 *      FMT    the same 448, plus fp_load and fmt_number (FMT - GET = formatting)
 *      EMIT   56 con_putrun of a ready-made 80-column line
 *      ROW    56 view_draw_row with NOTHING cached -- composition and all
 *      HOT    the same 56 rows, every one of them a cache hit
 *      DRAW   one whole view_draw, cold, headers and status included
 *      SCROLL one view_draw with a single row dirty, which is what a vertical
 *             scroll and a commit that touched one cell actually cost
 *
 * ROW minus FMT minus EMIT is what the spill lookahead costs, which is the
 * one part nobody had priced: every cell drawn examines its right-hand
 * neighbours to decide how far a label may run, and that is more cell_gets
 * than there are cells.
 *
 * WHAT THE ANSWER DECIDED. If FMT dominated, a cache of rendered strings was
 * the fix and could be per cell. If EMIT dominated, no cache of what to draw
 * would help at all -- the fix would have to avoid the VERA writes, by
 * knowing what is already on screen. If ROW came out much larger than
 * FMT + EMIT, the lookahead was the cost and the cache had to be per ROW.
 *
 * It was 92% formatting, 5% lookahead, 2% grid reads and 0.8% writing. So the
 * cache is per row -- the unit that owns the lookahead as well as the
 * formatting -- and HOT against ROW is the figure that says whether it works.
 *
 * THEN IT KEPT GOING, because "formatting" was not an answer either, and each
 * level moved the target: the cost was not in fmt.c at all but in
 * fp_to_str_trim (93% of it), and inside that in fp_nine_digits peeling
 * decimal digits off with a float DIVIDE apiece (80%). Replacing that with
 * integer subtraction took a conversion from 14 ms to 5 and a cold repaint
 * from 6.7 s to 2.8 s. The figures below are live, so this file's own
 * conclusions move when the code does -- and they now say the
 * scale-by-a-decade loop is what is left.
 *
 * 448 is 56 rows of eight nine-wide columns, which is what an 80-column screen
 * shows at the default width -- so every figure below is per repaint.
 *
 * THE EMULATOR'S TIMER IS SOUND FOR THIS. It advances on executed cycles
 * divided by the emulated clock, not on wall time (X816_Emulator memory.c),
 * so a warp run and a real-time run give the same milliseconds and two runs
 * of this give the same answer twice.
 *
 * BUILD AT -O0.
 * ========================================================================== */

#include "console.h"
#include "view.h"
#include "cell.h"
#include "fmt.h"
#include "fp.h"
#include "goshell.h"

/* The free-running millisecond counter. The LOW BYTE MUST BE READ FIRST: it
   latches bits 31:8, and without that a read straddling a carry returns a
   value that was never true and can go backwards. x816_contract.h says so and
   the emulator implements the latch on purpose, so that getting the order
   wrong breaks here rather than only on the board. */
#define TMR0 (*(volatile uint8_t *)0x9F90)
#define TMR1 (*(volatile uint8_t *)0x9F91)
#define TMR2 (*(volatile uint8_t *)0x9F92)
#define TMR3 (*(volatile uint8_t *)0x9F93)

static uint32_t
ms(void)
{
    uint32_t v;
    v  = (uint32_t)TMR0;                /* first, and it latches the rest */
    v |= (uint32_t)TMR1 << 8;
    v |= (uint32_t)TMR2 << 16;
    v |= (uint32_t)TMR3 << 24;
    return v;
}

/* Rows and columns of the sheet a repaint actually touches. VIEW_ROWS is 56;
   eight nine-wide columns is what fits beside a five-column gutter. */
#define BENCH_COLS 8
#define REPS       4            /* so a part costing under a millisecond still
                                   reads as something other than zero */

static uint8_t failed, ncase;

/* Somewhere for a timed loop's result to go. At -O0 nothing is eliminated, so
   this is belt and braces -- but a benchmark whose work the compiler is free
   to delete is the one kind that fails silently and reads as a triumph. */
static volatile uint8_t sink;

static void
put_u32(uint32_t v)
{
    char    tmp[11];
    uint8_t n = 0;

    if (v == 0) { con_putc('0'); return; }
    while (v) { tmp[n++] = (char)('0' + (uint8_t)(v % 10)); v /= 10; }
    while (n) con_putc(tmp[--n]);
}

/* name, then the per-repaint cost. Padded so the five figures line up in the
   GIF, which is where they are read from. */
static void
report(const char *name, uint32_t total)
{
    static char msmark[] = " ms\n";
    uint8_t i;

    con_puts(name);
    for (i = 0; name[i]; i++)
        ;
    while (i < 8) { con_putc(' '); i++; }     /* SCROLL is six, and needs a gap */
    put_u32(total / REPS);
    con_puts(msmark);
}

static void
expect(const char *what, bool cond)
{
    static char okmark[]  = "  ok\n";
    static char badmark[] = "  FAILED\n";
    ncase++;
    con_puts(what);
    con_puts(cond ? okmark : badmark);
    if (!cond && !failed)
        failed = ncase;
}

/* A screenful of numbers, which is the case that was slow. Values are built
   by dividing rather than typed, so they carry the awkward fractions a real
   sheet has -- a formatter given 1.0 four hundred times is not being measured
   on anything. */
static void
build_sheet(void)
{
    static char eighths[] = "8";
    uint16_t r, c;
    fp_t     div, v;

    fp_from_str(eighths);
    fp_store(&div);

    for (r = 0; r < VIEW_ROWS; r++) {
        for (c = 0; c < BENCH_COLS; c++) {
            cell cl;
            cell_get(r, c, &cl);
            cl.type = CELL_NUMBER;
            cl.fmt  = (c & 1) ? FMT_DOLLAR : FMT_GENERAL;
            fp_from_s16((int16_t)(r * 100 + c * 7 + 1));
            fp_div(&div);
            fp_store(&v);
            fp_load(&v);
            fp_store(&cl.value);
            cell_put(r, c, &cl);
        }
    }
}

int
main(void)
{
    static char noinit[]  = "MEM_ALLOC REFUSED -- is the kernel resident?\n";
    static char banner[]  = "X816 RENDER BENCH  448 cells, 56 rows, per repaint";
    static char n_get[]   = "GET";
    static char n_fmt[]   = "FMT";
    static char n_emit[]  = "EMIT";
    static char n_row[]   = "ROW";
    static char n_hot[]   = "HOT";
    static char n_draw[]  = "DRAW";
    static char n_scroll[]= "SCROLL";
    static char c_sane[]  = "the parts are not larger than the whole";
    static char c_moves[] = "a repaint costs measurable time at all";
    static char n_tostr[] = "TOSTR";
    static char n_norm[]  = "NORM";
    static char n_fmtg[]  = "FMTG";
    static char n_fmtd[]  = "FMTD";
    static char banner2[] = "WHERE ONE fmt_number GOES  the same 448, per repaint";
    static char c_cache[] = "a cached row is cheaper than a composed one";
    static char n_tsbig[] = "TSBIG";
    static char n_tsmid[] = "TSMID";
    static char n_tssml[] = "TSSML";
    static char banner3[] = "WHERE ONE CONVERSION GOES  by how far the value must scale";
    static char c_nest[]  = "each format step contains the one before it";
    static char c_scale[] = "scaling further costs more than not scaling";

    char     buf[VIEW_WIDTH_MAX + 1];
    char     wide[CON_COLS + 1];
    uint32_t t0, t_get, t_fmt, t_emit, t_row, t_hot, t_draw, t_scroll;
    uint32_t t_tostr, t_norm, t_fmtg, t_fmtd;
    uint32_t t_tsbig, t_tsmid, t_tssml;
    uint16_t r, c;
    uint8_t  i, rep;

    con_init();
    ccur_off();                 /* the program owns the screen */

    if (!cell_init()) {
        con_puts(noinit);
        goshell_on_esc();
        return 0;
    }
    view_init();
    build_sheet();
    view_draw();                /* warm: nothing below is measuring a cold start */

    for (i = 0; i < CON_COLS; i++)
        wide[i] = 'X';
    wide[CON_COLS] = '\0';

    /* ---- GET: the grid reads a repaint makes, and nothing else ----------- */
    t0 = ms();
    for (rep = 0; rep < REPS; rep++)
        for (r = 0; r < VIEW_ROWS; r++)
            for (c = 0; c < BENCH_COLS; c++) {
                cell cl;
                cell_get(r, c, &cl);
            }
    t_get = ms() - t0;

    /* ---- FMT: the same reads, plus turning each value into characters ---- */
    t0 = ms();
    for (rep = 0; rep < REPS; rep++)
        for (r = 0; r < VIEW_ROWS; r++)
            for (c = 0; c < BENCH_COLS; c++) {
                cell cl;
                cell_get(r, c, &cl);
                fp_load(&cl.value);
                fmt_number(cl.fmt, view_width(c), buf);
            }
    t_fmt = ms() - t0;

    /* ---- EMIT: 56 ready-made lines out to VERA --------------------------- */
    t0 = ms();
    for (rep = 0; rep < REPS; rep++)
        for (r = 0; r < VIEW_ROWS; r++)
            con_putrun(0, (uint8_t)(VIEW_TOP_ROW + r), wide, CON_COLS);
    t_emit = ms() - t0;

    /* ---- ROW: the renderer itself, composition, lookahead and all -------- */
    /* Dirtied first, or reps two to four would be measuring the cache. */
    t0 = ms();
    for (rep = 0; rep < REPS; rep++) {
        view_dirty_all();
        for (r = 0; r < VIEW_ROWS; r++)
            view_draw_row(r);
    }
    t_row = ms() - t0;

    /* ---- HOT: the same 56 rows, every one a hit -------------------------- */
    /* The loop above left them all cached, so this measures exactly what a
       repaint costs when nothing has changed: 56 copies out of BRAM and 56
       runs to VERA, and no formatting at all. */
    t0 = ms();
    for (rep = 0; rep < REPS; rep++)
        for (r = 0; r < VIEW_ROWS; r++)
            view_draw_row(r);
    t_hot = ms() - t0;

    /* ---- DRAW: a cold whole-screen repaint -------------------------------- */
    t0 = ms();
    for (rep = 0; rep < REPS; rep++) {
        view_dirty_all();
        view_draw();
    }
    t_draw = ms() - t0;

    /* ---- SCROLL: the case that was dropping keystrokes -------------------- */
    /* Arrowing down past the last visible row scrolls by one, which brings ONE
       row into view that was not there before and leaves 55 that were. A
       commit that changed one cell is the same shape. This is the number the
       16-entry key FIFO has to be compared against, not DRAW. */
    t0 = ms();
    for (rep = 0; rep < REPS; rep++) {
        view_dirty_row((uint16_t)rep);
        view_draw();
    }
    t_scroll = ms() - t0;

    /* ---- and inside the 92%: where ONE fmt_number goes -------------------
     *
     * The repaint split says formatting is the whole cost. fmt.c is not an
     * obvious suspect for it -- everything in that file is a loop over at most
     * twelve digits and a pad into at most twenty columns, which cannot be the
     * ~14 ms a call the figures above imply. The one thing it does that is not
     * its own code is the very first line of fmt_normalise: fp_to_str_trim,
     * the float package's decimal conversion.
     *
     * So the same treatment, one level down, and in the same order of
     * containment so the parts subtract:
     *
     *      TOSTR  fp_to_str_trim alone
     *      NORM   fmt_normalise    (NORM - TOSTR = parsing the digits back)
     *      FMTG   fmt_number, %g   (FMTG - NORM  = rounding and writing)
     *      FMTD   fmt_number, currency -- the other path through the file
     */
    t0 = ms();
    for (rep = 0; rep < REPS; rep++)
        for (r = 0; r < VIEW_ROWS; r++)
            for (c = 0; c < BENCH_COLS; c++) {
                cell cl;
                cell_get(r, c, &cl);
                fp_load(&cl.value);
                sink = (uint8_t)fp_to_str_trim()[0];
            }
    t_tostr = ms() - t0;

    t0 = ms();
    for (rep = 0; rep < REPS; rep++)
        for (r = 0; r < VIEW_ROWS; r++)
            for (c = 0; c < BENCH_COLS; c++) {
                cell    cl;
                fmt_num nm;
                cell_get(r, c, &cl);
                fp_load(&cl.value);
                fmt_normalise(&nm);
                sink = (uint8_t)nm.d[0];
            }
    t_norm = ms() - t0;

    t0 = ms();
    for (rep = 0; rep < REPS; rep++)
        for (r = 0; r < VIEW_ROWS; r++)
            for (c = 0; c < BENCH_COLS; c++) {
                cell cl;
                cell_get(r, c, &cl);
                fp_load(&cl.value);
                fmt_number(FMT_GENERAL, view_width(c), buf);
                sink = (uint8_t)buf[0];
            }
    t_fmtg = ms() - t0;

    t0 = ms();
    for (rep = 0; rep < REPS; rep++)
        for (r = 0; r < VIEW_ROWS; r++)
            for (c = 0; c < BENCH_COLS; c++) {
                cell cl;
                cell_get(r, c, &cl);
                fp_load(&cl.value);
                fmt_number(FMT_DOLLAR, view_width(c), buf);
                sink = (uint8_t)buf[0];
            }
    t_fmtd = ms() - t0;

    /* ---- and inside THAT: scaling, or peeling the digits? ----------------
     *
     * float.s converts in two stages, and they cost differently for different
     * values. First it scales the value into [1e8, 1e9) by multiplying or
     * dividing by ten ONE DECADE AT A TIME, each step a float compare and a
     * float multiply. Then fp_nine_digits peels off nine digits, each one a
     * float DIVIDE by ten, a float multiply back, a subtract and a truncate.
     *
     * Which of the two to attack is decided by timing values that need
     * different amounts of the first and identical amounts of the second:
     *
     *      TSBIG  123456789   already in range -- zero scaling steps, so this
     *                         is the peeling plus the fixed overhead
     *      TSMID  200.125     about six steps up
     *      TSSML  0.000123    about twelve
     *
     * so (TSSML - TSBIG) over twelve steps prices a scaling step, and TSBIG
     * prices the peeling. If TSBIG is most of TSMID, the divides are the
     * target and the scaling loop is a distraction.
     */
    {
        static char s_big[] = "123456789";
        static char s_mid[] = "200.125";
        static char s_sml[] = "0.000123";
        fp_t v_big, v_mid, v_sml;

        fp_from_str(s_big); fp_store(&v_big);
        fp_from_str(s_mid); fp_store(&v_mid);
        fp_from_str(s_sml); fp_store(&v_sml);

        t0 = ms();
        for (rep = 0; rep < REPS; rep++)
            for (r = 0; r < VIEW_ROWS; r++)
                for (c = 0; c < BENCH_COLS; c++) {
                    fp_load(&v_big);
                    sink = (uint8_t)fp_to_str_trim()[0];
                }
        t_tsbig = ms() - t0;

        t0 = ms();
        for (rep = 0; rep < REPS; rep++)
            for (r = 0; r < VIEW_ROWS; r++)
                for (c = 0; c < BENCH_COLS; c++) {
                    fp_load(&v_mid);
                    sink = (uint8_t)fp_to_str_trim()[0];
                }
        t_tsmid = ms() - t0;

        t0 = ms();
        for (rep = 0; rep < REPS; rep++)
            for (r = 0; r < VIEW_ROWS; r++)
                for (c = 0; c < BENCH_COLS; c++) {
                    fp_load(&v_sml);
                    sink = (uint8_t)fp_to_str_trim()[0];
                }
        t_tssml = ms() - t0;
    }

    /* ---- the numbers ----------------------------------------------------- */
    /* Over a drawn sheet, so the rows have to be cleared first: con_puts stops
       where the string does and leaves the columns to its right showing
       whatever the last repaint put there, which lands a figure and a price in
       the same line of the GIF. */
    for (i = 0; i < CON_COLS; i++)
        wide[i] = ' ';
    for (i = 36; i < CON_ROWS; i++)
        con_putrun(0, i, wide, CON_COLS);

    con_gotoxy(0, 36);
    con_puts(banner);
    con_putc('\n');
    report(n_get,    t_get);
    report(n_fmt,    t_fmt);
    report(n_emit,   t_emit);
    report(n_row,    t_row);
    report(n_hot,    t_hot);
    report(n_draw,   t_draw);
    report(n_scroll, t_scroll);
    con_puts(banner2);
    con_putc('\n');
    report(n_tostr, t_tostr);
    report(n_norm,  t_norm);
    report(n_fmtg,  t_fmtg);
    report(n_fmtd,  t_fmtd);
    con_puts(banner3);
    con_putc('\n');
    report(n_tsbig, t_tsbig);
    report(n_tsmid, t_tsmid);
    report(n_tssml, t_tssml);

    /* A part that came out larger than the whole means the timer was misread
       -- almost always the latch, by sampling $9F91 before $9F90. */
    expect(c_sane, t_get <= t_fmt && t_fmt <= t_draw && t_emit <= t_draw);
    expect(c_moves, t_draw >= REPS);
    /* Not a performance target -- a wiring check. A hit that costs what a
       miss costs means the cached branch is not being taken, and every figure
       claiming a speed-up would be measuring the same code twice. */
    expect(c_cache, t_hot < t_row);
    /* Containment, not speed: fmt_normalise calls fp_to_str_trim and
       fmt_number calls fmt_normalise, so if the figures do not nest, the
       subtractions below them are meaningless. */
    expect(c_nest, t_tostr <= t_norm && t_norm <= t_fmtg);
    /* If a value needing twelve scaling steps did not cost more than one
       needing none, the scaling loop is not what this thinks it is. */
    expect(c_scale, t_tssml > t_tsbig);

    {
        static char okv[]  = "\nRENDER BENCH OK\n";
        static char badv[] = "\nRENDER BENCH FAILED\n";
        con_puts(failed == 0 ? okv : badv);
    }

    goshell_on_esc();
    return 0;
}
