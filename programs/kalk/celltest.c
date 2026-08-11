/* ==========================================================================
 * celltest.c -- does the sheet actually store what it is told, everywhere?
 *
 * The addressing is two shifts over four megabytes of SDRAM. That is fast and
 * it is also unforgiving: one bit wrong in the index and cell (5,3) aliases
 * cell (5,19) or a cell in the next row, and a spreadsheet with aliased cells
 * looks like a recalculation bug for a very long time before it looks like an
 * addressing one.
 *
 * So the cases below are chosen to fail if any bit of the index is wrong:
 * both far corners, the row and column that differ by exactly one in each
 * field, and a diagonal walk that touches every power of two.
 *
 * It also checks the two mechanisms that make the size affordable rather than
 * merely possible -- the watermark and per-row initialisation -- because both
 * are the kind of thing that appears to work while quietly doing nothing.
 *
 * BUILD AT -O0.
 * ========================================================================== */

#include "console.h"
#include "shell.h"
#include "cell.h"
#include "fp.h"
#include "goshell.h"

static uint8_t failed;
static uint8_t ncase;

static void
ok(const char *what)
{
    static char okmark[] = "  ok\n";
    ncase++;
    con_puts(what);
    con_puts(okmark);
}

static void
bad(const char *what)
{
    static char badmark[] = "  FAILED\n";
    ncase++;
    con_puts(what);
    con_puts(badmark);
    if (!failed)
        failed = ncase;
}

static void
expect(const char *what, bool cond)
{
    if (cond) ok(what); else bad(what);
}

/* A value that depends on BOTH coordinates, so a cell that aliases another
   answers with the other one's number rather than with a plausible zero. */
static uint16_t
stamp(uint16_t row, uint16_t col)
{
    return (uint16_t)(row * 7u + col * 1009u + 1u);
}

static void
put_stamp(uint16_t row, uint16_t col)
{
    cell c;
    cell_get(row, col, &c);
    c.type = CELL_NUMBER;
    c.fmt  = (uint8_t)(col & 7);
    fp_from_s16((int16_t)stamp(row, col));
    fp_store(&c.value);
    cell_put(row, col, &c);
}

static bool
check_stamp(uint16_t row, uint16_t col)
{
    cell c;
    cell_get(row, col, &c);
    if (c.type != CELL_NUMBER)
        return false;
    if (c.fmt != (uint8_t)(col & 7))
        return false;
    fp_load(&c.value);
    return fp_to_s16() == (int16_t)stamp(row, col);
}

int
main(void)
{
    static char banner[]  = "X816 CELL STORE\n\n";
    static char c_init[]  = "init: 4 MiB grid + 512 KB arena";
    static char c_empty[] = "a fresh sheet reads empty everywhere";
    static char c_corner[]= "both far corners, (0,0) and (1023,255)";
    static char c_adj[]   = "neighbours do not alias";
    static char c_pow2[]  = "every power-of-two index bit";
    static char c_mark[]  = "watermark tracks the live range";
    static char c_rowsk[] = "untouched rows report empty";
    static char c_text[]  = "text arena round trip";
    static char c_text0[] = "offset 0 means no text";
    static char c_clr[]   = "clear returns an empty sheet";
    static char noinit[]  = "MEM_ALLOC REFUSED -- is the kernel resident?\n";

    cell c;
    uint16_t i;
    bool good;

    con_init();
    con_puts(banner);

    if (!cell_init()) {
        con_puts(noinit);
        goshell_on_esc();
    }
    ok(c_init);

    /* ---- 1: nothing is in it, and reading proves it without allocating -- */
    cell_get(0, 0, &c);
    good = (c.type == CELL_EMPTY) && !cell_any();
    cell_get(1023, 255, &c);
    good = good && (c.type == CELL_EMPTY);
    expect(c_empty, good);

    /* ---- 2: the corners. (1023,255) is the last byte of the 4 MiB, so a
       base or a shift that is wrong by anything at all lands outside it. --- */
    put_stamp(0, 0);
    put_stamp(1023, 255);
    expect(c_corner, check_stamp(0, 0) && check_stamp(1023, 255));

    /* ---- 3: neighbours. (5,3) against (5,4) catches a column bit; (5,3)
       against (6,3) catches the row/column boundary, which is the one a
       wrong shift blurs. ------------------------------------------------- */
    put_stamp(5, 3);
    put_stamp(5, 4);
    put_stamp(6, 3);
    expect(c_adj, check_stamp(5, 3) && check_stamp(5, 4) && check_stamp(6, 3));

    /* ---- 4: one cell per power of two in each field, so a single stuck or
       swapped index bit cannot survive. --------------------------------- */
    good = true;
    for (i = 1; i < KALK_ROWS; i <<= 1)
        put_stamp(i, (uint16_t)(i & 0xFF));
    for (i = 1; i < KALK_COLS; i <<= 1)
        put_stamp((uint16_t)(i * 3u), i);
    for (i = 1; i < KALK_ROWS; i <<= 1)
        good = good && check_stamp(i, (uint16_t)(i & 0xFF));
    for (i = 1; i < KALK_COLS; i <<= 1)
        good = good && check_stamp((uint16_t)(i * 3u), i);
    /* and the earlier cells must still be there -- a later write must not
       have walked over them */
    good = good && check_stamp(0, 0) && check_stamp(1023, 255)
                && check_stamp(5, 3) && check_stamp(6, 3);
    expect(c_pow2, good);

    /* ---- 5: the watermark. It is what every sweep will be bounded by, so
       "it grew to the largest thing written" is the whole contract. ------ */
    expect(c_mark, cell_any() && cell_max_row() == 1023
                              && cell_max_col() == 255);

    /* ---- 6: rows nobody wrote. This is the mechanism that lets a fresh
       sheet open instantly, and the way it fails is by quietly reporting
       every row live -- which still passes every test above.
     *
     * 7 and 999 are picked, not arbitrary. The cases above between them have
     * written rows 0, 5, 6, 1023, every power of two up to 512, and every
     * 3 x power of two up to 384; 7 and 999 are in neither sequence. The
     * first draft asserted row 1 was untouched and this test failed, which is
     * the right outcome for a test that was wrong about its own fixture. */
    good = cell_row_empty(7) && cell_row_empty(999)
           && !cell_row_empty(0) && !cell_row_empty(5)
           && !cell_row_empty(1) && !cell_row_empty(1023);
    expect(c_rowsk, good);

    /* ---- 7: text ------------------------------------------------------- */
    {
        static char t1[] = "Widget A", t2[] = "Subtotal";
        char back[CELL_TEXT_MAX];
        uint32_t o1 = cell_text_put(t1);
        uint32_t o2 = cell_text_put(t2);
        good = (o1 != 0) && (o2 != 0) && (o1 != o2);
        cell_text_get(o1, back);
        good = good && back[0] == 'W' && back[7] == 'A' && back[8] == '\0';
        cell_text_get(o2, back);
        good = good && back[0] == 'S' && back[7] == 'l' && back[8] == '\0';
        expect(c_text, good);

        cell_text_get(0, back);
        expect(c_text0, back[0] == '\0');
    }

    /* ---- 8: and back to empty, without writing four megabytes ---------- */
    cell_clear_all();
    cell_get(0, 0, &c);
    good = (c.type == CELL_EMPTY) && !cell_any() && cell_row_empty(0)
           && cell_row_empty(1023);
    expect(c_clr, good);

    /* ---- verdict -------------------------------------------------------- */
    {
        static char okv[]  = "\nCELL STORE OK\n";
        static char badv[] = "\nFAILED AT CASE ";
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
