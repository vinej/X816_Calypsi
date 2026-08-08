/* kexec.c -- the K_EXEC backend: load an "X816" image and hand it the machine.
 *
 * Entered through kerntab.s with the KFS window convention: zero C arguments,
 * the caller's C/X parked in kfs_c/kfs_x by the thunk. C:X is a 24-bit
 * pointer to a NUL-terminated path, resolved the way fat32_open resolves it.
 * On success this never returns: the image is staged at EXEC_STAGE and
 * relocated over $01:0000 by exec.s exactly as the shell's `run` does. On
 * failure it returns the KERR_ code with kfs_carry set, and the caller keeps
 * the machine.
 *
 * The load is the same two-pass contract as shell.c's load_file: whole
 * clusters by DMA (fat32_read_far), the sub-cluster remainder a byte at a
 * time, and the total verified -- with fat32_ioerr() distinguishing a device
 * failure from end-of-file.
 *
 * K_EXIT lives in kerntab.s: it is a jml to the kernel entry (a full restart
 * of the resident kernel -- console re-init, card re-mount, prompt), which is
 * the defined v1 semantic: open handles do not survive an exit.
 *
 * The WORKING DIRECTORY is the one exception, and it is deliberate. A restart
 * re-runs cstartup, so every kernel variable including cwdbuf goes back to its
 * initialiser -- which meant a program launched from /GAMES dropped you at the
 * root on the way out. The launch directory is parked in the carry-over block
 * here (kfs_carry_save, see kfs.h) and picked up by the prompt on the way in.
 * Handles are NOT carried: an exiting program's open files are its own, and
 * reviving them across a restart would revive whatever state they were left
 * in.
 */

#include <stdint.h>
#include <stdbool.h>

#include "kernel.h"
#include "kfs.h"
#include "fat32.h"

/* The same staging address and cap shell.c's `run` uses. They are no longer
   "the same values as shell.c" -- both files take them from the generated
   x816_contract.h, so the pair cannot drift apart. */
#include "x816_contract.h"

extern void     x816_exec(void);            /* exec.s: does not return */
extern uint16_t x816_exec_len;

static uint8_t __far *
far_ptr(uint32_t a)
{
    return (uint8_t __far *)a;
}

uint16_t
kexec(void)
{
    static fat32_file f;                    /* kernel data, not cstack */
    char path[80];
    uint8_t __far *p;
    uint8_t i;
    uint32_t got;

    /* Fetch the path out of the caller's memory, whatever bank it lives in. */
    p = far_ptr(((uint32_t)(kfs_x & 0xFFu) << 16) | kfs_c);
    for (i = 0; i < sizeof path - 1; i++) {
        path[i] = (char)p[i];
        if (path[i] == '\0')
            break;
    }
    if (i == sizeof path - 1) {
        kfs_carry = 1;
        return KERR_BADARG;                 /* unterminated / oversized path */
    }

    if (!fat32_open(path, &f)) {
        kfs_carry = 1;
        return KERR_NOTFOUND;
    }
    if (f.size == 0 || f.size > X816_EXEC_MAX) {
        kfs_carry = 1;
        return KERR_BADARG;
    }

    fat32_clearerr();
    got = fat32_read_far(&f, X816_EXEC_STAGE, f.size);
    while (got < f.size) {
        uint8_t  buf[64];
        uint16_t n = fat32_read(&f, buf, sizeof buf);
        uint8_t __far *d = far_ptr(X816_EXEC_STAGE + got);
        uint16_t k;
        if (n == 0)
            break;
        for (k = 0; k < n; k++)
            d[k] = buf[k];
        got += n;
    }
    if (got != f.size || fat32_ioerr()) {
        kfs_carry = 1;
        return KERR_IO;
    }

    p = far_ptr(X816_EXEC_STAGE);
    if (p[0] != X816_MAGIC_0 || p[1] != X816_MAGIC_1
        || p[2] != X816_MAGIC_2 || p[3] != X816_MAGIC_3) {
        kfs_carry = 1;
        return KERR_BADARG;                 /* not an X816 image */
    }

    /* Remember where the caller was before the machine changes hands, exactly
       as the prompt's `run` does. K_EXIT restarts the kernel through cstartup
       and cwdbuf comes back as "/" -- the carry-over block (kfs.h) is what
       survives that, and arming it is the launcher's job because the LAUNCH
       directory is what the exiting program should return to. */
    kfs_carry_save();

    x816_exec_len = (uint16_t)f.size;
    x816_exec();                            /* never comes back */
    return 0;                               /* unreachable */
}
