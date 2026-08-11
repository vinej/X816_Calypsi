/* ==========================================================================
 * cell.c -- the sheet's storage. Why it is shaped this way is in cell.h.
 *
 * BUILD AT -O0, like the rest of this tree.
 * ========================================================================== */

#include "cell.h"
#include "kernel.h"
#include "x816_contract.h"

/* The layout is load-bearing -- the address arithmetic below is a shift by
   four and nothing else -- so it is checked rather than trusted. A struct
   that grew a byte would otherwise turn every cell address into a wrong one,
   silently, and the sheet would look like memory corruption. */
typedef char cell_is_16_bytes[(sizeof(cell) == CELL_BYTES) ? 1 : -1];

/* The grid, the arena, and the two numbers that keep sweeps honest. All in
   bank $00: this is the bookkeeping, not the data. */
static uint32_t grid;                   /* 24-bit base, 0 until cell_init   */
static uint32_t arena;
static uint32_t arena_next;
static uint32_t arena_size;

static uint16_t max_row, max_col;
static bool     any;

/* One bit per row. 128 bytes to make a fresh sheet open instantly and to let
   every sweep skip a row without reading it. */
static uint8_t  row_live[KALK_ROWS / 8];

/* The arena is sized for the sheet a person actually types, not for the grid:
   262,144 cells could in principle hold 128 characters each, which is 33 MiB
   and more than the machine has. Half a megabyte is about 4,000 full-length
   labels or 30,000 ordinary ones, and it is the number to raise first if a
   real sheet ever runs out. cell_text_put refuses rather than overruns. */
#define ARENA_BYTES  0x080000UL         /* 512 KB */
#define GRID_BYTES   ((uint32_t)KALK_COLS * KALK_ROWS * CELL_BYTES)

static uint8_t __far *
far_ptr(uint32_t a)
{
    return (uint8_t __far *)a;
}

/* MEM_ALLOC, spelled the way memtest.c spells it. Zero means refused. */
static uint32_t
kalloc(uint32_t size)
{
    kern_c = (unsigned int)size;
    kern_x = (unsigned int)(size >> 16);
    kern_y = 0;
    kern_c = kern_call(K_MEM_ALLOC);
    if (kern_carry)
        return 0;
    return ((uint32_t)kern_x << 16) | kern_c;
}

bool
cell_init(void)
{
    grid = kalloc(GRID_BYTES);
    if (grid == 0)
        return false;
    arena = kalloc(ARENA_BYTES);
    if (arena == 0)
        return false;
    arena_size = ARENA_BYTES;

    /* Offset 0 is reserved so that a zero `text` field means "no text" and
       cannot also mean "the string at the start of the arena". */
    arena_next = 1;

    cell_clear_all();
    return true;
}

/* The whole reason for 256 columns and a 16-byte cell. row is 10 bits and col
   is 8, so the index is a shift and an or, and the byte offset is one more
   shift -- no multiply anywhere, and no table. */
static uint32_t
addr_of(uint16_t row, uint16_t col)
{
    return grid + ((((uint32_t)row << 8) | col) << 4);
}

static bool
row_is_live(uint16_t row)
{
    return (row_live[row >> 3] & (uint8_t)(1u << (row & 7))) != 0;
}

/* Clearing 4 MiB at startup would be seconds of a machine doing nothing for a
   sheet that is empty. A row is cleared when it is first written instead --
   4,096 bytes, once -- and a row that was never written is never read. */
static void
row_make_live(uint16_t row)
{
    uint8_t __far *p;
    uint16_t       i;

    if (row_is_live(row))
        return;

    p = far_ptr(addr_of(row, 0));
    for (i = 0; i < KALK_COLS * CELL_BYTES; i++)
        p[i] = 0;

    row_live[row >> 3] |= (uint8_t)(1u << (row & 7));
}

void
cell_get(uint16_t row, uint16_t col, cell *out)
{
    uint8_t __far *p;
    uint8_t       *q = (uint8_t *)out;
    uint8_t        i;

    /* Out of range and never-written both answer EMPTY without a read. The
       first is a program error the caller should not have made; the second is
       the ordinary case for almost every cell in the sheet. */
    if (row >= KALK_ROWS || col >= KALK_COLS || !row_is_live(row)) {
        for (i = 0; i < CELL_BYTES; i++)
            q[i] = 0;
        return;
    }

    p = far_ptr(addr_of(row, col));
    for (i = 0; i < CELL_BYTES; i++)
        q[i] = p[i];
}

void
cell_put(uint16_t row, uint16_t col, const cell *in)
{
    uint8_t __far  *p;
    const uint8_t  *q = (const uint8_t *)in;
    uint8_t         i;

    if (row >= KALK_ROWS || col >= KALK_COLS)
        return;

    row_make_live(row);

    p = far_ptr(addr_of(row, col));
    for (i = 0; i < CELL_BYTES; i++)
        p[i] = q[i];

    /* The watermark only ever grows here. Shrinking it on a delete would mean
       scanning to find the new edge, which is the sweep this exists to avoid;
       a sheet that was once wide stays cheap enough, and cell_clear_all is
       what actually resets it. */
    if (!any) {
        any = true;
        max_row = row;
        max_col = col;
        return;
    }
    if (row > max_row) max_row = row;
    if (col > max_col) max_col = col;
}

bool
cell_row_empty(uint16_t row)
{
    return row >= KALK_ROWS || !row_is_live(row);
}

uint16_t cell_max_row(void) { return max_row; }
uint16_t cell_max_col(void) { return max_col; }
bool     cell_any(void)     { return any; }

void
cell_clear_all(void)
{
    uint16_t i;

    /* Forgetting the rows IS clearing them: an unmarked row is never read, so
       whatever those 4 MiB still contain cannot be observed. The alternative
       is writing four megabytes to prove a point. */
    for (i = 0; i < KALK_ROWS / 8; i++)
        row_live[i] = 0;

    max_row = 0;
    max_col = 0;
    any = false;
    arena_next = 1;
}

/* ---- the text arena ------------------------------------------------------ */

uint32_t
cell_text_put(const char *s)
{
    uint32_t       off = arena_next;
    uint8_t __far *p;
    uint16_t       n = 0;

    while (n < CELL_TEXT_MAX - 1 && s[n])
        n++;

    /* Refuse rather than truncate: a truncated label is a different label,
       and a caller that got a valid-looking offset back has no way to tell. */
    if (off + n + 1 > arena_size)
        return 0;

    p = far_ptr(arena + off);
    for (n = 0; n < CELL_TEXT_MAX - 1 && s[n]; n++)
        p[n] = (uint8_t)s[n];
    p[n] = 0;

    arena_next = off + n + 1;
    return off;
}

void
cell_text_get(uint32_t off, char *out)
{
    uint8_t __far *p;
    uint16_t       n;

    if (off == 0 || off >= arena_next) {
        out[0] = '\0';
        return;
    }

    p = far_ptr(arena + off);
    for (n = 0; n < CELL_TEXT_MAX - 1 && p[n]; n++)
        out[n] = (char)p[n];
    out[n] = '\0';
}

uint32_t
cell_text_used(void)
{
    return arena_next;
}
