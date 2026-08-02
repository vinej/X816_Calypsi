/* ==========================================================================
 * kmem.c -- MEM_ALLOC / MEM_FREE. See kmem.h for the design and why.
 *
 * BUILD AT -O0 like the rest of the runtime. Nothing here touches a device
 * register, so the volatile-elision hazard does not apply -- but this links
 * into the same images as fat32.c and console.c, which do, and a mixed-flag
 * link is a thing nobody should have to reason about.
 *
 * WHERE THE TABLE LIVES, AND WHY IT IS NOT IN BANK $00
 * ---------------------------------------------------
 * It was, for about ten minutes, and the kernel would not link: the claim at
 * $2100-$2FFF had 38 bytes free before this file existed. That is not a
 * budgeting accident to work around -- doc/KERNEL.md 3 says outright that
 * anything scaling with the number of live objects stays out of bank $00, and
 * x816-kernel.scm's own comment gives the rule for an overflow: move the
 * offender into an SDRAM buffer, do not grow the claim.
 *
 * So the block table is the FIRST PAGE OF THE ARENA, at X816_HEAP_TABLE, and
 * MEM_ALLOC never hands that page out. What stays in bank $00 is one word:
 * `live`. That split is deliberate and load-bearing.
 *
 *   SDRAM comes up as noise. If `live` lived in the arena too, the kernel
 *   would have to tell an initialised table from power-up garbage, and every
 *   scheme for that (a magic word, a checksum) has a false-positive rate and
 *   a "who calls the init" problem. `live` in bank $00 is zero-initialised by
 *   cstartup, exactly like kfs.c's handle table, and live == 0 IS the empty
 *   heap -- the array is only ever read below `live`, so its contents before
 *   first use are never looked at. There is no init call to forget.
 *
 * The honest note on protection: keeping the table out of the arena's
 * allocatable range stops an in-bounds write to a NEIGHBOURING block from
 * reaching it, which is the common overrun. It is not a hardware guarantee --
 * a wild far pointer reaches this page as easily as it reaches $2000-$2FFF,
 * and nothing on this machine says otherwise. Neither location was ever
 * protected; this one is merely the one bank $00 can afford.
 *
 * PAGES, NOT BYTES
 * ----------------
 * Everything here is granular to X816_HEAP_GRAIN = one page, so the table
 * stores page numbers and all the arithmetic is 16-bit. Base pages run
 * $2001-$EFFF and a size is at most $CFFF pages, so cursor + need reaches
 * exactly $F000 and cannot wrap -- the whole class of 32-bit overflow bugs
 * that a byte-addressed version has to reason about does not arise.
 * ========================================================================== */

#include "kmem.h"
#include "kernel.h"
#include "kfs.h"

/* Page numbers: the arena's first allocatable page, and one past its last. */
#define PAGE_FIRST ((uint16_t)(X816_HEAP_BASE >> 8))
#define PAGE_LIMIT ((uint16_t)(((uint32_t)X816_HEAP_END + 1u) >> 8))

/* The table, two uint16 per block: base page then size in pages, kept sorted
   strictly ascending by base with no two entries touching or overlapping.
   Every function below may assume that and must restore it. */
#define T_BASE(i) (2u * (i))
#define T_SIZE(i) (2u * (i) + 1u)

static uint16_t __far *
table(void)
{
    return (uint16_t __far *)X816_HEAP_TABLE;
}

/* The only thing in bank $00 -- see the header. uint16_t and not uint8_t on
   purpose: Calypsi 5.18 widens 8-bit read-modify-write and has been measured
   corrupting the neighbouring variable (X816_Calypsi README, runtime/smc.s),
   and `live++` is exactly that shape. */
static uint16_t live;

uint16_t
kmem_live(void)
{
    return live;
}

uint32_t
kmem_free_bytes(void)
{
    uint16_t __far *t = table();
    uint16_t used = 0;
    uint16_t i;
    for (i = 0; i < live; i++)
        used += t[T_SIZE(i)];
    return (uint32_t)(PAGE_LIMIT - PAGE_FIRST - used) << 8;
}

/* Insert at index `at`, shifting the tail up. The caller has established that
   live < X816_HEAP_BLOCKS. */
static void
insert(uint16_t at, uint16_t base, uint16_t size)
{
    uint16_t __far *t = table();
    uint16_t i;
    for (i = live; i > at; i--) {
        t[T_BASE(i)] = t[T_BASE(i - 1)];
        t[T_SIZE(i)] = t[T_SIZE(i - 1)];
    }
    t[T_BASE(at)] = base;
    t[T_SIZE(at)] = size;
    live++;
}

uint16_t
kmem_alloc(void)
{
    uint32_t want = ((uint32_t)kfs_x << 16) | kfs_c;
    uint16_t __far *t;
    uint16_t need, cursor, i;

    kfs_x = 0;                          /* the bank half of the result */

    /* Zero is a caller bug, not an empty success: some address would have to
       come back, MEM_FREE would then have to accept it, and two zero-byte
       allocations would have to be distinct from each other. */
    if (want == 0) {
        kfs_carry = 1;
        return KERR_BADARG;
    }
    /* Round up to whole pages, refusing anything the arena could not hold
       even when empty. Checked BEFORE the rounding: want + 0xFF wraps for a
       size above $FFFFFF00, and a huge request would become a zero-page one. */
    if (want > ((uint32_t)(PAGE_LIMIT - PAGE_FIRST) << 8)) {
        kfs_carry = 1;
        return KERR_NOSPACE;
    }
    if (live >= X816_HEAP_BLOCKS) {
        kfs_carry = 1;
        return KERR_NOSPACE;
    }
    need = (uint16_t)((want + (X816_HEAP_GRAIN - 1u)) >> 8);

    /* First fit over the gaps, in address order. `cursor` is always the first
       free page at or after the blocks already walked, so the gap before
       block i is [cursor, base[i]) and the last gap runs to PAGE_LIMIT. */
    t = table();
    cursor = PAGE_FIRST;
    for (i = 0; i < live; i++) {
        uint16_t b = t[T_BASE(i)];
        if (b - cursor >= need) {
            insert(i, cursor, need);
            kfs_carry = 0;
            kfs_x = cursor >> 8;                /* bank */
            return (uint16_t)(cursor << 8);     /* offset within it */
        }
        cursor = b + t[T_SIZE(i)];
    }
    if (PAGE_LIMIT - cursor >= need) {
        insert(live, cursor, need);
        kfs_carry = 0;
        kfs_x = cursor >> 8;
        return (uint16_t)(cursor << 8);
    }

    kfs_carry = 1;
    return KERR_NOSPACE;                /* fragmented, or genuinely full */
}

uint16_t
kmem_free(void)
{
    uint16_t __far *t = table();
    uint16_t page = (uint16_t)(((kfs_x & 0xFFu) << 8) | (kfs_c >> 8));
    uint16_t i, j;

    /* A returned address is always page-aligned, so an offset with anything in
       its low byte was never handed out by this allocator. Catching it here
       makes "you freed a pointer INTO the block" a refusal rather than a
       silent free of the block it points into. */
    if ((kfs_c & 0xFFu) != 0) {
        kfs_x = 0;
        kfs_carry = 1;
        return KERR_BADARG;
    }
    kfs_x = 0;

    /* Exact base only. Accepting an address inside a block would let a caller
       free the same block twice by passing two different addresses into it,
       and the second free would release a block somebody else had since been
       given. */
    for (i = 0; i < live; i++) {
        if (t[T_BASE(i)] == page) {
            for (j = i; j + 1 < live; j++) {
                t[T_BASE(j)] = t[T_BASE(j + 1)];
                t[T_SIZE(j)] = t[T_SIZE(j + 1)];
            }
            live--;
            kfs_carry = 0;
            return 0;
        }
    }

    kfs_carry = 1;
    return KERR_BADARG;
}
