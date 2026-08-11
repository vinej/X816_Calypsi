/* ==========================================================================
 * cell.h -- the sheet: 256 columns by 1024 rows, in flat memory.
 *
 * kalk.c's own dimensions, and they are affordable here for the reason the
 * X16 port could not have them: X816 is flat. The X16 version fitted 26x256
 * into eight 8 KB banks and paid for it in every access --
 *
 *     bank   = 1 + (row >> 5)
 *     offset = $A000 + (row & 31) * 234 + col * 9
 *
 * plus two lookup tables to avoid the multiplies, plus "reserves RAM banks
 * 1-8, nothing else may use those banks". With a 16-byte cell and 256
 * columns, the whole of that becomes two shifts and an or:
 *
 *     addr = base + ((row << 8) | col) << 4
 *
 * SIZE IS FREE. SWEEPING IS NOT.
 * ------------------------------
 * 262,144 cells at 16 bytes is 4 MiB out of the allocator's 12 MiB, which
 * costs nothing. What would cost something is any operation that walks the
 * whole grid: the X16 port measured 0.83 s to insert a row across 6,656
 * cells -- rows moved, references rewritten, everything recalculated -- which
 * is about 125 us of real work per cell. At 262,144 cells that is tens of
 * seconds.
 *
 * So nothing here ever walks the allocation. Two mechanisms keep the cost
 * proportional to what is actually in the sheet rather than to what was
 * declared:
 *
 *   THE WATERMARK. cell_max_row() and cell_max_col() bound every sweep. A
 *   sheet with 200 live cells costs 200 cells of work no matter how large the
 *   grid is. This is not an optimisation to add later -- it is what makes the
 *   dimensions affordable, and retrofitting it means auditing every operation
 *   that walks the sheet.
 *
 *   PER-ROW INITIALISATION. MEM_ALLOC does not zero, and zeroing 4 MiB up
 *   front is seconds of startup for a sheet that is about to be empty. A row
 *   is cleared the first time anything is written to it and remembered in a
 *   1,024-bit map; a read from a row that was never written answers empty
 *   without touching memory at all.
 *
 * THE NEAR COPY IS THE INTERFACE
 * ------------------------------
 * cell_get and cell_put move a whole cell between the grid and an ordinary C
 * struct. That is not a convenience wrapper over a pointer -- it is required.
 * The grid is far and the float package's operands must be in bank $00
 * (fp.h), so a value cannot be computed where it lives. The struct's `value`
 * field is a normal fp_t and fp_load/fp_store work on it directly.
 *
 * It is the same staging kfs.c does for its transfers, and it is the reason
 * the grid gets to be four megabytes.
 * ========================================================================== */

#ifndef KALK_CELL_H
#define KALK_CELL_H

#include <stdint.h>
#include <stdbool.h>
#include "fp.h"

#define KALK_COLS   256         /* A..IV, kalk.c's own width               */
#define KALK_ROWS   1024
#define CELL_BYTES  16          /* a power of two, so addressing is shifts */

/* ---- what a cell is ------------------------------------------------------ */
#define CELL_EMPTY   0
#define CELL_NUMBER  1
#define CELL_LABEL   2          /* text only; `text` names it               */
#define CELL_FORMULA 3          /* text is the source, value is the result  */

/* flags */
#define CELL_ERROR   0x01       /* @ERROR, or a domain error caught here    */
#define CELL_NA      0x02       /* @NA -- propagates differently to ERROR   */

/* Exactly CELL_BYTES wide, and the layout is CHECKED at compile time in
 * cell.c rather than trusted -- the address arithmetic is a shift by four and
 * nothing else, so a struct that quietly grew a byte would turn every cell
 * address into a wrong one and the sheet would look like memory corruption.
 * The check caught this struct on its first build.
 *
 * `text` comes FIRST because it is the only field wider than a byte, and at
 * offset 0 it is aligned whatever the compiler's rule turns out to be. Laid
 * out in the obvious reading order -- type, then value, then the offset -- it
 * landed at offset 10 and the compiler padded the struct to 20 bytes, which
 * is not a power of two and would have cost a multiply on every access.
 *
 * A 24-bit offset written into four bytes with the top one zero is the
 * convention the kernel ABI already uses (kfs.h section 5.3).
 */
typedef struct {
    uint32_t text;              /* arena offset, low 24 bits; 0 = no text   */
    fp_t     value;             /* 5 bytes of MFLPT, usable by fp_* as-is   */
    uint8_t  type;
    uint8_t  fmt;               /* kalk's format code: L R I G D $ % *      */
    uint8_t  flags;
    uint8_t  rsv[4];            /* spare, and what makes the size a power
                                   of two rather than an accident           */
} cell;

/* ---- the store ----------------------------------------------------------- */

/* Claims the grid and the text arena from MEM_ALLOC. False means the kernel
   refused -- no resident kernel, or an arena too small -- and the caller has
   nothing to fall back on, so it should say so and stop. */
bool cell_init(void);

/* Neither bounds-checks into a refusal: an out-of-range reference is a
   PROGRAM error, not a user error, and the sheet's own reference parser is
   what turns a typed `ZZ9999` into a diagnostic. cell_get answers an empty
   cell for anything out of range so that a read can never invent memory;
   cell_put ignores it. */
void cell_get(uint16_t row, uint16_t col, cell *out);
void cell_put(uint16_t row, uint16_t col, const cell *in);

/* True if the row was never written -- a whole-row skip for any sweep, and
   the reason a fresh sheet costs nothing to open. */
bool cell_row_empty(uint16_t row);

/* The watermark. Both are inclusive, and both are meaningless when the sheet
   is empty -- ask cell_any() first. */
uint16_t cell_max_row(void);
uint16_t cell_max_col(void);
bool     cell_any(void);

/* Back to an empty sheet without touching the 4 MiB: forgetting the rows is
   the same thing as clearing them, because a row that is not marked is never
   read. */
void cell_clear_all(void);

/* ---- the text arena ------------------------------------------------------
 *
 * A bump allocator, and deliberately one: cell text is written far more often
 * than it is freed, kalk.c's own arena never frees, and a free list here
 * would cost more bank $00 bookkeeping than the memory it recovered. Editing
 * a cell abandons its old string. Reclaiming the space is a compaction pass
 * over the used range, which is a later problem and a cheap one -- the
 * watermark already says exactly which cells to walk.
 */
#define CELL_TEXT_MAX 128       /* kalk.c's own per-cell limit               */

/* Copies `s` into the arena. Returns the offset to store in cell.text, or 0
   if it does not fit -- and 0 is not a valid offset for that reason: the
   arena's first byte is reserved so that a zero `text` field unambiguously
   means "no text". */
uint32_t cell_text_put(const char *s);

/* Copies text back into `out`, which must hold CELL_TEXT_MAX bytes. An offset
   of 0 gives the empty string. */
void cell_text_get(uint32_t off, char *out);

/* How much of the arena is gone, for the status line and for a test. */
uint32_t cell_text_used(void);

#endif /* KALK_CELL_H */
