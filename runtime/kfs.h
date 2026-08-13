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
/* EIGHT, and eight is the CEILING in the current bank-$00 budget -- measured,
 * not chosen: 9 does not link. It was 5, and 5 was too few.
 *
 * WHY A POOL THIS SMALL RUNS OUT. An interpreter holds one handle per NESTED
 * SOURCE, and the depth grows every time a module gains an `include` or opens
 * a data file. durexForth's worst case is now SIX, reached inside its own test
 * suite:
 *
 *     1 BASE       still open -- base.fs's last line is the autorun catch
 *     2 AUTORUN
 *     3 TEST       the suite
 *     4 TESTFLOA   one test file
 *     5 FLOAT      testfloat.fs line 15, `include float`
 *     6 FPENGINE.BIN   float.fs (engload) -- the sixth OPEN
 *
 * At 5 that sixth open returned KERR_NOSPACE and the suite died. The previous
 * bump to 5 was sized against a chain measured at FOUR; nobody re-measured
 * when float.fs became a module that itself opens a data file. Assume the next
 * module does the same and leave headroom -- which is what 8 is for.
 *
 * WHY NOT 16. It is a SPACE limit, not a speed one. files[] is `zdata`, and in
 * the RESIDENT kernel `zdata` goes to KernRAM -- the $2000-$2FFF claim of
 * doc/KERNEL.md 3.1, ordinary single-cycle bank-$00 BRAM. So growing the array
 * costs nothing per access: `files[h-1]` is indexed the same way whatever its
 * length, and there is no direct-page pressure here at all. (The comment this
 * replaces blamed "the 256-byte DIRECT PAGE". That is x816-lib.scm's
 * constraint, not the kernel's -- DirectPage in x816-kernel.scm is $2000-$209F
 * and holds only `registers ztiny`.)
 *
 * What stops 16 is that the claim is FULL: at 16 the linker cannot place
 * cstack and reports 552 bytes free against the 768 it needs, so 16 is 192
 * bytes beyond the budget. Raising it further means first freeing bank $00,
 * and the documented candidate -- the shell command table's inline name/help
 * arrays, ~780 bytes of scaffolding (see shell.h) -- needs a second shell.o
 * built with -DKERNEL_RESIDENT and const strings reached through __far
 * pointers, which shell.h records as a toolchain fight rather than an edit.
 * Do that first, then this constant can move.
 *
 * A caller must check the return regardless. FS_OPEN reports exhaustion as
 * KERR_NOSPACE with carry set, and NOSPACE is not NOTFOUND -- reporting one as
 * the other sends the reader hunting for a file that is right there, which is
 * exactly how the failure above cost an afternoon.
 */
#define KFS_FILES  8
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

/* ---- the carry-over block ----------------------------------------------
 *
 * The working directory has to survive something that erases the kernel's
 * variables: K_EXIT restarts the resident kernel at X816_FW_ENTRY, which is
 * Calypsi's cstartup, which re-runs the data initialiser table -- so cwdbuf
 * comes back as "/" and a program launched from /GAMES returns you to the
 * root. Same for the loadable prompt, which goshell RELOADS from the card.
 *
 * So the launch directory is parked at a fixed address that no C runtime
 * touches, and the prompt picks it up on the way in.
 *
 * WHERE IT LIVES, AND WHY IT IS NOT A VARIABLE
 * --------------------------------------------
 * $00:20A0-$00:20FF, the top of the kernel's direct page region. It is
 * deliberately NOT a section the linker can fill -- the same arrangement as
 * the jump table at $00:FE00, and for the same kind of reason: anything the
 * linker places is something cstartup initialises. runtime/x816-kernel.scm
 * stops DirectPage at $209F so nothing can be placed on top of it, and every
 * program map already carves out the whole kernel claim ($2000-$2FFF, see
 * doc/KERNEL.md 3.1), so the loadable prompt reaches the same bytes.
 *
 *   +0  four magic bytes, "XCWD"
 *   +4  the path, NUL-terminated, at most KFS_PATH bytes
 *
 * The magic is what makes a COLD boot land at the root: bank $00 comes up as
 * whatever it was, so the block has to say for itself that it means something.
 */
#define KFS_CARRY_BASE 0x0020A0UL       /* must match x816-kernel.scm */
#define KFS_CARRY_SIZE 0x60             /* 96 bytes: 4 + KFS_PATH, rounded */
#define KFS_CARRY_PATH 4                /* offset of the string */

/* Park the working directory for the next prompt to find. Called on every
   path that hands the machine to a program -- `run`, `go`, K_EXEC -- so what
   is remembered is the LAUNCH directory, not wherever the program chdir'd to
   before it exited. */
void kfs_carry_save(void);

/* Adopt it, once. Does no card I/O: the prompt must come up on a machine with
   no card, and mounting here to validate the path would give that up for a
   check the first command makes anyway. A path that no longer exists (a card
   swapped between the save and the restore) costs one NOT FOUND and a `cd /`.

   For the PROMPT. It consumes the block, which is the prompt's right and no
   program's -- see kfs_carry_adopt. */
void kfs_carry_restore(void);

/* The same adoption WITHOUT consuming the block, for a PROGRAM.
 *
 * A program linking this file gets its own copy of it, with its own cwdbuf,
 * and nothing initialises that copy to anything but "/" -- so every relative
 * path the program resolves lands at the root of the card, whatever directory
 * it was launched from. That is not a theoretical hazard: kalk's /SS wrote
 * every sheet to the card root while the user was sitting in /KALK.
 *
 * The block stays armed on purpose. It is the prompt this program eventually
 * exits back to that is entitled to consume it, and a program that took it
 * would send that prompt back to the root instead of the launch directory --
 * trading one wrong directory for another. */
void kfs_carry_adopt(void);

#endif /* X816_KFS_H */
