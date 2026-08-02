/* ==========================================================================
 * memtest.c -- MEM_ALLOC / MEM_FREE, through $00:FE00 by entry NUMBER.
 *
 * X816_Core doc/KERNEL.md section 8 test 7: "MEM_ALLOC/MEM_FREE returning
 * distinct non-overlapping ranges". That is the property the test is named
 * for and it is the weakest one worth checking -- an allocator that returned
 * the same address every time would fail it, and so would almost nothing
 * else. So the checks below go after the properties a caller actually leans
 * on and an allocator actually gets wrong:
 *
 *   GREEN    every test passed
 *   RED      1: a first allocation, page-aligned, inside the arena, and the
 *              memory is really there -- written and read back
 *   YELLOW   2: three live blocks are pairwise DISJOINT (section 8 test 7)
 *   BLUE     3: a freed block is reused, and the reuse is exact -- the
 *              address that came back is the one that was let go
 *   MAGENTA  4: the refusals: size 0, a size larger than the arena, an
 *              unaligned free, a free of something never allocated, and a
 *              DOUBLE free. Each must be carry-set with the right KERR_ code
 *   CYAN     5: a failed MEM_ALLOC leaves the heap EXACTLY as it was --
 *              nothing half-inserted, nothing leaked
 *   ORANGE   6: the table fills at X816_HEAP_BLOCKS and refuses cleanly with
 *              KERR_NOSPACE, then recovers completely when the blocks go back
 *   BROWN    7: writing a block does not disturb its neighbours -- the check
 *              that would catch metadata living inside the arena
 *
 * The number also lands at $00:0400 for a debugger.
 *
 * WHY IT WRITES TO EVERY BLOCK. An allocator can hand out plausible,
 * disjoint, well-aligned addresses that are not backed by anything, or that
 * overlap the bookkeeping page. Only a write-then-read-back through the
 * returned address distinguishes an allocator from an address generator, and
 * only writing the NEIGHBOURS too (test 7) shows that the bookkeeping is not
 * sitting between them.
 * ========================================================================== */

#include "kernel.h"
#include "kmem.h"
#include "console.h"
#include "goshell.h"

#define VERA_ADDR_L     (*(volatile unsigned char *)0x9F20)
#define VERA_ADDR_M     (*(volatile unsigned char *)0x9F21)
#define VERA_ADDR_H     (*(volatile unsigned char *)0x9F22)
#define VERA_DATA0      (*(volatile unsigned char *)0x9F23)
#define VERA_CTRL       (*(volatile unsigned char *)0x9F25)
#define VERA_DC_VIDEO   (*(volatile unsigned char *)0x9F29)
#define VERA_DC_HSCALE  (*(volatile unsigned char *)0x9F2A)
#define VERA_DC_VSCALE  (*(volatile unsigned char *)0x9F2B)
#define VERA_L0_CONFIG  (*(volatile unsigned char *)0x9F2D)
#define VERA_L0_TILEB   (*(volatile unsigned char *)0x9F2F)

#define RESULT (*(volatile unsigned char *)0x0400)

static void
paint(unsigned char colour)
{
    unsigned int x, y;
    VERA_CTRL      = 0;
    VERA_DC_VIDEO  = 0x11;
    VERA_DC_HSCALE = 0x40;
    VERA_DC_VSCALE = 0x40;
    VERA_L0_CONFIG = 0x07;
    VERA_L0_TILEB  = 0;
    VERA_CTRL   = 0;
    VERA_ADDR_L = 0;
    VERA_ADDR_M = 0;
    VERA_ADDR_H = 0x10;
    for (y = 0; y < 240; y++)
        for (x = 0; x < 320; x++)
            VERA_DATA0 = colour;
}

static unsigned char __far *
far_ptr(unsigned long a)
{
    return (unsigned char __far *)a;
}

/* MEM_ALLOC: the 32-bit size goes out as C:X, the 24-bit address comes back
   as C:X. Returns 0 on failure, which is never a valid block -- the arena
   starts at X816_HEAP_BASE and that is not zero. */
static unsigned long
mem_alloc(unsigned long size)
{
    kern_c = (unsigned int)size;
    kern_x = (unsigned int)(size >> 16);
    kern_y = 0;
    kern_c = kern_call(K_MEM_ALLOC);
    if (kern_carry)
        return 0;
    return ((unsigned long)kern_x << 16) | kern_c;
}

/* MEM_FREE: -> 0 on success, else the KERR_ code. */
static unsigned int
mem_free(unsigned long addr)
{
    unsigned int r;
    kern_c = (unsigned int)addr;
    kern_x = (unsigned int)(addr >> 16);
    kern_y = 0;
    r = kern_call(K_MEM_FREE);
    return kern_carry ? r : 0;
}

/* The error code a failed MEM_ALLOC reported. mem_alloc() collapses failure
   to 0, so the code is fetched separately where a test cares which one. */
static unsigned int
alloc_err(unsigned long size)
{
    unsigned int r;
    kern_c = (unsigned int)size;
    kern_x = (unsigned int)(size >> 16);
    kern_y = 0;
    r = kern_call(K_MEM_ALLOC);
    return kern_carry ? r : 0;
}

static int
in_arena(unsigned long a, unsigned long size)
{
    if (a < X816_HEAP_BASE)
        return 0;
    /* Subtraction, not a + size: the sum can carry off the top. */
    if (size > (unsigned long)X816_HEAP_END + 1UL - a)
        return 0;
    return 1;
}

/* Write a signature through the returned address and read it back, at the
   first byte, the last byte, and one in the middle. An allocator that returns
   an address it does not own passes every arithmetic check and fails this.

   THE MASKS BELOW ARE `& 0xFF`, NOT `(unsigned char)`, AND THAT MATTERS.
   Calypsi 5.18 compiles `(unsigned char)(expr)` compared against a byte
   loaded from memory as a SIXTEEN-bit compare with mismatched extensions:
   the loaded byte is zero-extended (`and ##255`) while the cast expression
   is SIGN-extended (`eor ##128 / and ##255 / sec / sbc ##128`). Any value
   with bit 7 set then compares unequal to itself -- here `sig ^ 0xFF` = $EE
   did, and `sig ^ 0x5A` = $4B did not, which is why the failure looked
   intermittent. `& 0xFF` compiles to an 8-bit compare and is correct.

   The full characterisation, and the scan that keeps the tree clear of it,
   are in X816_Core doc/AUDIT.md section 6.2 and tools/calypsi_scan.py. */
static int
poke_ok(unsigned long a, unsigned long size, unsigned char sig)
{
    unsigned long mid = a + (size >> 1);
    unsigned long last = a + size - 1;
    far_ptr(a)[0]    = sig;
    far_ptr(mid)[0]  = (sig ^ 0x5A) & 0xFF;
    far_ptr(last)[0] = (sig ^ 0xFF) & 0xFF;
    if (far_ptr(a)[0] != sig)
        return 0;
    if (far_ptr(mid)[0] != ((sig ^ 0x5A) & 0xFF))
        return 0;
    if (far_ptr(last)[0] != ((sig ^ 0xFF) & 0xFF))
        return 0;
    return 1;
}

static int
overlaps(unsigned long a, unsigned long na, unsigned long b, unsigned long nb)
{
    if (a < b)
        return (b - a) < na;
    return (a - b) < nb;
}

#define K 1024UL

int
main(void)
{
    unsigned char fail = 0;
    unsigned long a, b, c, again;
    unsigned long blocks[X816_HEAP_BLOCKS];
    unsigned int  i, err;
    unsigned long free0;

    con_init();

    /* Stamp the table. This image links its own private copy of the kernel,
       exactly as kerntest.c and kfstest.c do, so bank $00's $FE00 page is
       whatever was there until this runs -- and every kern_call below would
       otherwise jump into it. Under the RESIDENT kernel on hardware the table
       is already installed and this rewrites it with the same bytes. */
    kern_install();

    /* Whatever ran before this may have left blocks live -- on hardware this
       is launched from a resident kernel that has been up for a while. Record
       the starting free space so test 5 can compare against it rather than
       against an assumption that the heap is empty. */
    free0 = kmem_free_bytes();

    /* ---- 1: one allocation, and the memory is really there ---------------- */
    a = mem_alloc(4 * K);
    if (a == 0)
        fail = 1;
    if (!fail && (a & (X816_HEAP_GRAIN - 1)) != 0)
        fail = 1;                       /* not page-aligned */
    if (!fail && !in_arena(a, 4 * K))
        fail = 1;
    if (!fail && a < X816_HEAP_BASE)
        fail = 1;                       /* inside the bookkeeping page */
    if (!fail && !poke_ok(a, 4 * K, 0x11))
        fail = 1;

    /* ---- 2: three live blocks, pairwise disjoint (section 8 test 7) ------- */
    if (!fail) {
        b = mem_alloc(1);               /* rounds up to one page */
        c = mem_alloc(64 * K);
        if (b == 0 || c == 0)
            fail = 2;
        if (!fail && (!in_arena(b, X816_HEAP_GRAIN) || !in_arena(c, 64 * K)))
            fail = 2;
        if (!fail && (overlaps(a, 4 * K, b, X816_HEAP_GRAIN)
                      || overlaps(a, 4 * K, c, 64 * K)
                      || overlaps(b, X816_HEAP_GRAIN, c, 64 * K)))
            fail = 2;
        /* Distinct addresses is the stated property; distinct MEMORY is the
           one that matters. Three different signatures, all still readable
           afterwards, is the difference. */
        if (!fail && (!poke_ok(b, X816_HEAP_GRAIN, 0x22)
                      || !poke_ok(c, 64 * K, 0x33)))
            fail = 2;
        if (!fail && far_ptr(a)[0] != 0x11)
            fail = 2;                   /* the first block was disturbed */
    }

    /* ---- 3: a freed block comes back, exactly ---------------------------- */
    if (!fail) {
        if (mem_free(b) != 0)
            fail = 3;
        if (!fail) {
            again = mem_alloc(X816_HEAP_GRAIN);
            /* First fit over an address-ordered table: the hole left by `b`
               is the first one big enough, so the same address must come
               back. An allocator that appended instead would pass "distinct
               and non-overlapping" while leaking the hole on every cycle. */
            if (again != b)
                fail = 3;
            else
                b = again;
        }
        if (!fail && far_ptr(c)[0] != 0x33)
            fail = 3;                   /* the neighbour above moved or died */
    }

    /* ---- 4: the refusals ------------------------------------------------- */
    if (!fail) {
        if (alloc_err(0) != KERR_BADARG)
            fail = 4;                   /* zero bytes is not an empty success */
        if (!fail && alloc_err(0xFFFFFFFFUL) != KERR_NOSPACE)
            fail = 4;                   /* bigger than the machine */
        if (!fail && alloc_err((unsigned long)X816_HEAP_END) != KERR_NOSPACE)
            fail = 4;                   /* bigger than the arena */
        if (!fail && mem_free(a + 1) != KERR_BADARG)
            fail = 4;                   /* unaligned: never handed out */
        if (!fail && mem_free(a + X816_HEAP_GRAIN) != KERR_BADARG)
            fail = 4;                   /* aligned, but INTO the block */
        if (!fail && mem_free(X816_HEAP_BASE - X816_HEAP_GRAIN) != KERR_BADARG)
            fail = 4;                   /* the bookkeeping page itself */
        if (!fail && mem_free(0) != KERR_BADARG)
            fail = 4;
        /* DOUBLE FREE. The first must succeed and the second must be refused;
           an allocator that removed a second entry would corrupt the table
           and the damage would show up somewhere else entirely. */
        if (!fail && mem_free(b) != 0)
            fail = 4;
        if (!fail && mem_free(b) != KERR_BADARG)
            fail = 4;
    }

    /* ---- 5: a refused allocation changes nothing ------------------------- */
    if (!fail) {
        unsigned long before = kmem_free_bytes();
        unsigned int  n      = kmem_live();
        (void)alloc_err(0);
        (void)alloc_err(0xFFFFFFFFUL);
        (void)mem_free(0);
        if (kmem_free_bytes() != before || kmem_live() != n)
            fail = 5;
    }

    /* ---- 6: the table fills, refuses, and recovers ----------------------- */
    if (!fail) {
        unsigned int got = 0;
        unsigned long before = kmem_free_bytes();
        unsigned int  n0     = kmem_live();

        for (i = 0; i < X816_HEAP_BLOCKS; i++) {
            blocks[i] = mem_alloc(X816_HEAP_GRAIN);
            if (blocks[i] == 0)
                break;
            got++;
        }
        /* a and c are still live, so the table cannot take a full
           X816_HEAP_BLOCKS more -- it must stop, and stop for the right
           reason. */
        if (got >= X816_HEAP_BLOCKS)
            fail = 6;
        if (!fail) {
            err = alloc_err(X816_HEAP_GRAIN);
            if (err != KERR_NOSPACE)
                fail = 6;
        }
        if (!fail && kmem_live() != X816_HEAP_BLOCKS)
            fail = 6;
        for (i = 0; i < got; i++)
            if (mem_free(blocks[i]) != 0)
                fail = 6;
        /* Everything handed back must come back: a table that leaked one
           entry per fill cycle would still pass every test above. */
        if (!fail && (kmem_free_bytes() != before || kmem_live() != n0))
            fail = 6;
    }

    /* ---- 7: neighbours do not bleed into each other ---------------------- */
    if (!fail) {
        unsigned long p[8];
        int ok = 1;
        for (i = 0; i < 8; i++) {
            p[i] = mem_alloc(X816_HEAP_GRAIN);
            if (p[i] == 0) { ok = 0; break; }
        }
        if (!ok)
            fail = 7;
        if (!fail) {
            /* Fill each block completely, then check every block again. If the
               allocator's bookkeeping lived inside the arena -- a boundary tag
               between two blocks -- this is what would find it: the write that
               fills block i would land on the header of block i+1. */
            for (i = 0; i < 8; i++) {
                unsigned int j;
                unsigned char __far *q = far_ptr(p[i]);
                for (j = 0; j < X816_HEAP_GRAIN; j++)
                    q[j] = (unsigned char)(0x40 + i);
            }
            for (i = 0; i < 8; i++) {
                unsigned int j;
                unsigned char __far *q = far_ptr(p[i]);
                for (j = 0; j < X816_HEAP_GRAIN; j++)
                    if (q[j] != (unsigned char)(0x40 + i))
                        fail = 7;
            }
        }
        for (i = 0; i < 8 && ok; i++)
            if (mem_free(p[i]) != 0)
                fail = 7;
    }

    /* Give everything back, so a resident kernel is left as it was found. */
    (void)mem_free(a);
    (void)mem_free(c);
    if (!fail && kmem_free_bytes() != free0)
        fail = 5;                       /* this test leaked, not the kernel */

    RESULT = fail;
    switch (fail) {
    case 0:  paint(0x05); break;        /* green     */
    case 1:  paint(0x02); break;        /* red       */
    case 2:  paint(0x07); break;        /* yellow    */
    case 3:  paint(0x06); break;        /* blue      */
    case 4:  paint(0x04); break;        /* magenta   */
    case 5:  paint(0x03); break;        /* cyan      */
    case 6:  paint(0x08); break;        /* orange    */
    default: paint(0x09); break;        /* brown     */
    }
    goshell_on_esc();
    return 0;                           /* unreachable */
}
