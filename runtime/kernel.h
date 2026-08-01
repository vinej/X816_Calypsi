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
 * RESIDENCY -- read this before relying on it
 * -------------------------------------------
 * The kernel today is the resident shell, which lives at $01:0000. `run`
 * loads a program to exactly that address, so a program started with `run`
 * has ALREADY OVERWRITTEN the code the table points at. The table is valid
 * for the resident program and for anything loaded above bank $01 -- not for
 * a program that replaced the shell.
 *
 * Making it survive `run` means moving the kernel to bank $02 or above, which
 * `run` cannot reach (it copies at most $FF00 bytes to $01:0000). That is the
 * next step and it does not change this interface -- which is the point of
 * fixing the interface first.
 * ========================================================================== */

#ifndef X816_KERNEL_H
#define X816_KERNEL_H

#include <stdint.h>
#include <stdbool.h>

/* One page, 64 entries of 4 bytes: `jmp long:handler`. */
#define KERN_TABLE   0x00FE00UL
#define KERN_ENTRIES 64
#define KERN_ENTRY(n) (KERN_TABLE + (n) * 4)

/* ---- console, 0-15 ---------------------------------------------------- */
#define K_CON_PUTC     0    /* C = character                               */
#define K_CON_PUTS     1    /* C:X = 24-bit pointer to a NUL-terminated str */
#define K_CON_GETC     2    /* -> C = character, blocking                   */
#define K_CON_GETKEY   3    /* -> C = character, or 0 if none              */
#define K_CON_CLS      4
#define K_CON_GOTOXY   5    /* C = column, X = row                         */
#define K_CON_GETXY    6    /* -> C = column, X = row                      */
#define K_CON_PUTRAW   7    /* C = column, X = row, Y = glyph code         */

/* ---- filesystem, 16-31 ------------------------------------------------ */
#define K_FS_OPEN     16
#define K_FS_CLOSE    17
#define K_FS_READ     18
#define K_FS_WRITE    19
#define K_FS_SEEK     20
#define K_FS_SIZE     21
#define K_FS_DELETE   22
#define K_FS_RENAME   23
#define K_DIR_OPEN    24
#define K_DIR_NEXT    25
#define K_DIR_CLOSE   26
#define K_FS_CHDIR    27
#define K_FS_GETCWD   28
#define K_FS_MKDIR    29
#define K_FS_RMDIR    30

/* ---- programs, 32-39 -------------------------------------------------- */
#define K_EXEC        32    /* C:X = path; does not return on success      */
#define K_EXIT        33    /* C = status; does not return                 */

/* ---- memory, 40-47 ---------------------------------------------------- */
#define K_MEM_ALLOC   40
#define K_MEM_FREE    41

/* ---- system, 48-63 ---------------------------------------------------- */
#define K_SYS_VERSION 48    /* -> C = (major << 8) | minor                 */

/* ---- error codes ------------------------------------------------------ */
/* Returned in C with carry SET. Zero is never an error, so a caller that
   only checks carry and a caller that checks the code agree. */
#define KERR_NOSYS     1    /* call number not implemented in this build   */
#define KERR_NOTFOUND  2
#define KERR_NOSPACE   3
#define KERR_BADARG    4
#define KERR_IO        5
#define KERR_EXISTS    6
#define KERR_NOTEMPTY  7

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

#endif /* X816_KERNEL_H */
