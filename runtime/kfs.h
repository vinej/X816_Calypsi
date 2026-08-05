/* ==========================================================================
 * kfs.h -- the kernel's filesystem policy layer.
 *
 * fat32.c is MECHANISM and stays a library: parsing FAT32 is something any
 * program may do for itself. This file is POLICY, which X816_Core
 * doc/KERNEL.md section 2.2 says is the part that has to be the kernel's --
 * who owns handle 3, where the working directory is, and the fact that there
 * can only be one FAT32 writer on the card.
 *
 * CALLING SHAPE -- read this before adding an entry
 * -------------------------------------------------
 * Every function here takes NO ARGUMENTS and returns uint16_t. The kernel ABI
 * arguments arrive in the globals kfs_c/kfs_x/kfs_y, which the assembly thunk
 * fills from C, X and Y, and failure is reported by setting kfs_carry before
 * returning the error code.
 *
 * That is deliberate and not laziness. Calypsi's C argument passing changes
 * with arity AND with type -- first argument in A, a second 16-bit argument
 * pushed, a third pushed ahead of it, a __far pointer in the direct-page
 * pseudo-registers regardless of position (measured with cc65816 -S, see
 * doc/KERNEL.md section 10.1). A thunk that marshals into a natural C
 * signature has to encode that table correctly for every entry, and two such
 * thunks were already written wrong before the rule was measured. Zero
 * arguments has no marshalling to get wrong: it is the same for all fifteen
 * entries, so it is right once or wrong once.
 * ========================================================================== */

#ifndef X816_KFS_H
#define X816_KFS_H

#include <stdint.h>
#include <stdbool.h>

/* ---- the ABI window ---------------------------------------------------- */
/* Written by the thunk before the call, read by the call. kfs_carry is the
   answer: 0 = success and the return value is the result, 1 = failure and it
   is a KERR_ code. */
extern uint16_t kfs_c, kfs_x, kfs_y;
extern uint16_t kfs_carry;

/* ---- handles ------------------------------------------------------------
 *
 * Files are 1..KFS_FILES and directories are 129..129+KFS_DIRS-1: two
 * DISJOINT ranges, so DIR_NEXT on a file handle is a clean KERR_BADARG rather
 * than a walk through a byte stream as if it were directory records. Zero is
 * never a handle, so a caller that forgot to check for failure gets a refusal
 * on first use instead of touching handle 0.
 */
/* Five, and five is the CEILING, not a preference. An interpreter holds
 * one handle per NESTED SOURCE: durexForth's boot chain is four deep
 * before a program runs - base.fs is still open when it includes AUTORUN,
 * which includes the test suite, which includes one test file - so at four
 * the pool was exhausted by nesting alone and the first OPEN-FILE inside
 * any include returned KERR_NOSPACE. That reads as "the card is full",
 * which is the wrong thing to go looking for.
 *
 * files[] lives in zdata, and this is a --data-model=small build, so the
 * array is competing for the 256-byte DIRECT PAGE. Six does not link:
 * "Failed to place ... zdata" from ln65816 against x816-lib.scm, the
 * LOADABLE-PROGRAM map, whose direct page is tighter than the resident
 * kernel's. Raising this further means first moving files[] out of the
 * direct page, which is a real change and not a constant edit.
 */
#define KFS_FILES  5
#define KFS_DIRS   2
#define KFS_DIRBIT 129

/* ---- open modes, K_FS_OPEN's Y --------------------------------------- */
#define KFS_READ   0    /* must exist                                       */
#define KFS_WRITE  1    /* created if absent, TRUNCATED if present          */

/* ---- FS_SEEK whence ---------------------------------------------------- */
#define KFS_SET    0
#define KFS_CUR    1
#define KFS_END    2

/* ---- parameter blocks, doc/KERNEL.md section 5.3 ------------------------
 *
 * Little-endian fields at fixed offsets, reached through a 24-bit pointer in
 * C:X. A 24-bit address occupies four bytes with the top byte reserved and
 * written as zero -- not three -- so that every field stays naturally aligned
 * for a compiler on either side of the interface.
 *
 * FS_READ / FS_WRITE                 FS_SEEK
 *   +0  u16 handle                     +0  u16 handle
 *   +2  u32 buffer, 24-bit             +2  u8  whence
 *   +6  u32 count                      +3  u8  reserved
 *   +10 u32 transferred  <- written    +4  i32 offset
 *                                      +8  u32 position  <- written
 * FS_RENAME
 *   +0  u32 path, 24-bit
 *   +4  u32 new name, 24-bit -- a BARE 8.3 name, not a path
 *
 * DIR_NEXT's entry buffer is not a parameter block; it is 18 bytes of result:
 *   +0  char name[13], NUL-terminated
 *   +13 u8   attributes, bit 0 = directory
 *   +14 u32  size in bytes, 0 for a directory
 */
#define KFS_DIRENT_SIZE 18

/* ---- the entries ------------------------------------------------------- */
uint16_t kfs_open(void);     /* C:X = path, Y = mode  -> handle             */
uint16_t kfs_close(void);    /* C = handle                                  */
uint16_t kfs_read(void);     /* C:X = block           -> bytes read         */
uint16_t kfs_write(void);    /* C:X = block           -> bytes written      */
uint16_t kfs_seek(void);     /* C:X = block           -> position, low 16   */
uint16_t kfs_size(void);     /* C = handle  -> low 16; high 16 in kfs_x     */
uint16_t kfs_delete(void);   /* C:X = path                                  */
uint16_t kfs_rename(void);   /* C:X = block                                 */
uint16_t kfs_diropen(void);  /* C:X = path            -> handle             */
uint16_t kfs_dirnext(void);  /* C = handle, X:Y = buffer; carry set at end  */
uint16_t kfs_dirclose(void); /* C = handle                                  */
uint16_t kfs_chdir(void);    /* C:X = path                                  */
uint16_t kfs_getcwd(void);   /* C:X = buffer, at least KFS_PATH bytes       */
uint16_t kfs_mkdir(void);    /* C:X = path                                  */
uint16_t kfs_rmdir(void);    /* C:X = path                                  */

/* The longest path the kernel will resolve, NUL included. FS_GETCWD's buffer
   must be at least this large. */
#define KFS_PATH 80

/* Resolve `arg` against the working directory: absolute paths, "." and "..",
   repeated and trailing slashes. Shared with the shell so that the prompt and
   a program calling FS_OPEN cannot disagree about what "../X" means. */
bool kfs_abspath(const char *arg, char *out);

/* Change directory by ordinary C string, which is what the prompt has. The ABI
   entry above is the same thing with the path arriving as 24 bits.

   The shell shares this rather than keeping its own copy, because there is
   only one working directory: if `cd` moved the prompt's idea of it and
   FS_CHDIR moved the kernel's, a program launched from the prompt would
   resolve relative paths against a different directory than the one the user
   is looking at. */
bool kfs_chdir_path(const char *arg);

/* The working directory, always absolute, always starting '/', and never
   ending in one except at the root. Normalised on the way in so every consumer
   can simply concatenate. */
const char *kfs_cwd(void);

/* Mount on first use rather than at boot: a machine with no card must still
   reach a prompt and still run dump/peek/poke, which are exactly the commands
   wanted when the card is the broken thing. */
bool kfs_ready(void);

#endif /* X816_KFS_H */
