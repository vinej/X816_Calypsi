/* ==========================================================================
 * kernel.h -- the X816 native kernel ABI.
 *
 * Specified in X816_Core doc/KERNEL.md sections 3-5. This header is the
 * client side: the call numbers, the table address, and what each call
 * expects. The implementation side is kerntab.s.
 *
 * WHY A TABLE AND NOT DIRECT CALLS
 * --------------------------------
 * A program is linked and loaded independently of the kernel, so it cannot
 * know where any kernel routine sits. The table is at a FIXED address --
 * $00:FE00 -- and every entry is a `jmp long:` to wherever the implementation
 * happens to be. Recompiling the kernel moves the code and not the interface.
 *
 * 64 entries of 4 bytes fill exactly one page. The numbering leaves gaps
 * between groups on purpose: adding FS_TRUNCATE later must not renumber
 * MEM_ALLOC, because a renumber breaks every program already built.
 *
 * WHERE THE NUMBERS COME FROM
 * ---------------------------
 * Not from here. The call numbers, the table address and the error codes are
 * generated into x816_contract.h from one table in X816_core/tools/
 * contract.py, which also generates the BODY of kerntab.s's prototype table.
 * They used to be two hand-kept lists -- numbers here, positions there -- with
 * nothing checking that entry 21 in one was entry 21 in the other. A
 * mismatched pair produces no diagnostic anywhere: the program jumps to a
 * real, working, wrong routine.
 *
 * CALLING CONVENTION -- normative, doc/KERNEL.md section 4
 * -------------------------------------------------------
 *   entry     native mode, M=0 X=0 (16-bit A and index), reached by jsl
 *   return    rtl
 *   args      up to three 16-bit values in C, X, Y. A 24-bit pointer is C
 *             (low 16) plus the low byte of X (bank). More than that goes in
 *             a parameter block.
 *   result    CARRY CLEAR on success, result in C.
 *             CARRY SET on failure, error code in C.
 *   preserved D and DBR. A, X, Y and the flags may be clobbered.
 *
 * Every call reports through carry, including the ones that cannot currently
 * fail. Adding a failure mode later must not be an ABI break.
 *
 * RESIDENCY
 * ---------
 * The kernel is a firmware image at $F0:0000 (doc/KERNEL.md section 3),
 * HPS-loaded as boot2.rom and write-protected by the core, with its state and
 * direct page in the bank-0 claim at $2000-$2FFF. `run` erases $01:0000 and
 * cannot reach the kernel, so the table stays valid across a launch -- which
 * is the whole reason the interface was fixed before the implementation
 * moved. A program linked by x816-plain.scm or x816-lib.scm has the claim and
 * the table page carved out of its own map, so it cannot land on either.
 * ========================================================================== */

#ifndef X816_KERNEL_H
#define X816_KERNEL_H

#include <stdint.h>
#include <stdbool.h>

/* KERN_TABLE, KERN_ENTRIES, every K_* call number and every KERR_* code.
   Generated -- see the note above. */
#include "x816_contract.h"

#define KERN_ENTRY(n) (KERN_TABLE + (n) * KERN_ENTRY_SIZE)

/* Install the table. Call once, before anything calls through it. Writes 64
   entries into $00:FE00 -- the loader never touches bank $00, so this cannot
   be done at link time. */
void kern_install(void);

/* ---- calling the kernel from C ------------------------------------------
 *
 * Assembly just does `jsl $00FExx`. C cannot: the entry number is a variable
 * and the 65816 has no jsl through a pointer, so kern_call synthesises it.
 *
 * The register arguments travel in globals rather than parameters, because
 * Calypsi's argument passing changes with arity -- first in A, next two in
 * direct-page pseudo-registers, the rest pushed -- and a shim that depends on
 * that is a shim that breaks. One parameter is unambiguous: it is in A.
 *
 *     kern_c = 'X';
 *     kern_call(K_CON_PUTC);
 *     if (kern_carry) { ... kern_c is an error code ... }
 *
 * kern_call also returns the result, so the common case reads normally.
 */
extern uint16_t kern_c, kern_x, kern_y;
extern uint16_t kern_carry;         /* 1 = the call reported failure */
uint16_t kern_call(uint16_t n);

/* kern_c AND kern_x are written back after the call, because two entries
   return sixteen more bits in X: FS_SIZE's high half and MEM_ALLOC's bank.
   kern_y is input only -- no entry returns anything in Y. */

#endif /* X816_KERNEL_H */
