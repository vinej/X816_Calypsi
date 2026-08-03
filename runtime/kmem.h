/* ==========================================================================
 * kmem.h -- MEM_ALLOC / MEM_FREE, the kernel's flat-memory allocator.
 *
 * X816_Core doc/KERNEL.md 5.5. This is a kernel call and not a library
 * because of 2.1's first test: two programs each with their own allocator
 * would hand out the same bytes twice, and nothing would say so.
 *
 * WHAT IT HANDS OUT
 * -----------------
 * Flat 24-bit SDRAM from X816_HEAP_BASE to X816_HEAP_END -- banks $20-$DF,
 * 13.6 MB. There is no banking API because there is no banking; the whole
 * point of the X816 ABI over the X16 KERNAL's is that an address can say
 * "bank $47" (doc/KERNEL.md 5.2). Everything below the arena already has an
 * owner: bank $00 is BRAM, $01-$0F is the program image, $10-$1F is FarRAM
 * and the EXEC staging area. `contract.py --check` verifies that the linker
 * scripts and this arena still meet exactly, with no gap and no overlap.
 *
 * A FIXED TABLE, NOT A FREE LIST -- and this is the design decision
 * ----------------------------------------------------------------
 * The obvious allocator puts its metadata in the arena as boundary tags, one
 * header immediately before each block. On a machine with no MMU that means
 * the bookkeeping sits in memory the caller is writing to, one byte past the
 * end of what it was given -- so the commonest overrun there is corrupts the
 * allocator, and the damage surfaces in a LATER, UNRELATED call. This project
 * has paid for silent corruption more than once (doc/AUDIT.md H-3, H-4).
 *
 * So the table is one page at X816_HEAP_TABLE, outside the range MEM_ALLOC
 * ever hands out, with X816_HEAP_BLOCKS entries -- the same shape as the
 * handle table (KFS_FILES = 4): a small fixed number, exceeded cleanly with
 * KERR_NOSPACE rather than degrading. A program that needs a thousand small
 * objects should take one block and sub-allocate it -- that is a library's
 * job, and it can pick a policy the kernel has no business freezing (2.1's
 * second test).
 *
 * Not in bank $00, though it started there: the kernel's $2000-$2FFF claim
 * had 38 bytes free, and doc/KERNEL.md 3 is explicit that anything scaling
 * with the number of live objects stays out of it. See kmem.c for what one
 * word does stay in bank $00 and why that one has to.
 *
 * FIRST FIT, ADDRESS-ORDERED. The table is kept sorted by base address, so
 * `free` merges by construction: removing an entry makes the gap on each
 * side one gap, with no coalescing pass and no way to leave two adjacent
 * free regions that cannot serve a request spanning both.
 *
 * GRANULARITY. Sizes are rounded up to X816_HEAP_GRAIN (one page) and every
 * returned address is page-aligned. That is not tidiness: a 65816 direct
 * page must be page-aligned to avoid a cycle penalty on every dp access, and
 * a caller taking a block to use as one should not have to align it again.
 *
 * WHAT IT DOES NOT DO. It does not zero the memory -- a caller that needs
 * that can do it and pay for it, and a caller that does not should not.
 * There is no realloc: free and allocate.
 * ========================================================================== */

#ifndef X816_KMEM_H
#define X816_KMEM_H

#include <stdint.h>
#include "x816_contract.h"

/* ---- the entries --------------------------------------------------------
 *
 * Same calling shape as kfs.h: no C arguments, the caller's registers parked
 * in kfs_c/kfs_x by the thunk, failure reported through kfs_carry. See
 * kfs.h's header for why that is the shape and not laziness. The ABI window
 * is shared with the filesystem and with kexec -- it is the kernel's
 * register window, not the filesystem's, and one thunk macro marshals it for
 * everything so it is right once or wrong once.
 *
 * MEM_ALLOC   in   C = size low 16, X = size high 16
 *             out  C = address low 16, X = bank      (carry clear)
 *                  C = KERR_NOSPACE / KERR_BADARG    (carry set)
 * MEM_FREE    in   C = address low 16, X = bank
 *             out  carry clear, or KERR_BADARG if that is not a live block
 */
uint16_t kmem_alloc(void);
uint16_t kmem_free(void);

/* How many blocks are live, and how many bytes are unallocated. Not ABI --
 * no jump-table slot, no promise. The conformance test uses them to check
 * that a failed MEM_ALLOC left nothing behind, which is the property a
 * caller most needs and the one an allocator most easily gets wrong.
 */
uint16_t kmem_live(void);
uint32_t kmem_free_bytes(void);

#endif /* X816_KMEM_H */
