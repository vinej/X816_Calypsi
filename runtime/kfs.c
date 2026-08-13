/* ==========================================================================
 * kfs.c -- the kernel's filesystem entries.
 *
 * The interface, and why every function here takes no arguments, is in kfs.h.
 * This file is the implementation: a handle table, the working directory, and
 * the marshalling between the kernel ABI's 24-bit pointers and fat32.c, which
 * works in near pointers because it is ordinary library code.
 *
 * WHY THE STAGING BUFFER
 * ----------------------
 * fat32_read and fat32_write take NEAR buffers -- they are library functions
 * and a library has no business assuming its caller lives outside bank $00.
 * The ABI's buffers are 24-bit, because a program loaded at $07:0000 has to be
 * able to read into its own memory. So a transfer goes through `stage` in
 * chunks: the alternative is teaching fat32.c about far pointers, which would
 * push a kernel concern into the mechanism layer for no gain.
 *
 * fat32_read_far exists and is faster, but it moves WHOLE CLUSTERS and starts
 * at the current position -- right for `load`, wrong for a general read of
 * `count` bytes at an arbitrary offset.
 * ========================================================================== */

#include "kfs.h"
#include "kernel.h"
#include "fat32.h"

uint16_t kfs_c, kfs_x, kfs_y;
uint16_t kfs_carry;

/* ---- state --------------------------------------------------------------
 *
 * Small and fixed. A handle table that grows is a memory allocator wearing a
 * disguise, and doc/KERNEL.md section 3 is explicit that buffers scaling with
 * the number of open files do not belong in bank $00.
 */
static fat32_file files[KFS_FILES];
static uint8_t    fmode[KFS_FILES];     /* 0 = free, else KFS_READ+1 / KFS_WRITE+1 */
static fat32_dir  dirs[KFS_DIRS];
static uint8_t    dused[KFS_DIRS];

static char cwdbuf[KFS_PATH] = { '/', '\0' };
static char pathbuf[KFS_PATH];          /* resolved, for the current call */
static char argbuf[KFS_PATH];           /* the caller's path, copied in    */
static uint8_t stage[128];

static bool mounted;

/* ---- far access ---------------------------------------------------------
 *
 * Everything the ABI hands over is a 24-bit address. These are the only place
 * that is turned back into a pointer, so a mistake about bank handling has one
 * home rather than fifteen.
 */
static uint8_t __far *
farp(uint32_t a)
{
    return (uint8_t __far *)a;
}

static uint32_t
argptr(void)
{
    /* C:X, the ABI's 24-bit pointer: offset in C, bank in the LOW BYTE of X.
       Masking matters -- a caller with rubbish in the high byte of X would
       otherwise address a bank that does not exist. */
    return (uint32_t)kfs_c | ((uint32_t)(kfs_x & 0x00FF) << 16);
}

static uint16_t
fget16(uint32_t a)
{
    uint8_t __far *p = farp(a);
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t
fget32(uint32_t a)
{
    uint8_t __far *p = farp(a);
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void
fput32(uint32_t a, uint32_t v)
{
    uint8_t __far *p = farp(a);
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* ---- results ------------------------------------------------------------
 *
 * Every entry leaves through one of these two, so no path can return without
 * having decided what carry says. The ABI promises carry always reports, even
 * for a call that cannot currently fail.
 */
static uint16_t
ok(uint16_t v)
{
    kfs_carry = 0;
    return v;
}

static uint16_t
err(uint16_t code)
{
    kfs_carry = 1;
    return code;
}

bool
kfs_ready(void)
{
    if (mounted)
        return true;
    if (fat32_mount()) {
        mounted = true;
        return true;
    }
    return false;
}

const char *
kfs_cwd(void)
{
    return cwdbuf;
}

/* ---- path resolution ---------------------------------------------------- */

bool
kfs_abspath(const char *arg, char *out)
{
    uint16_t    n = 0;
    const char *p = arg;

    if (*p == '/') {
        out[n++] = '/';
        p++;
    } else {
        uint16_t i = 0;
        while (cwdbuf[i] && n < KFS_PATH - 1)
            out[n++] = cwdbuf[i++];
    }

    while (*p) {
        const char *seg;
        uint16_t    seglen = 0;

        while (*p == '/')
            p++;
        if (!*p)
            break;

        seg = p;
        while (*p && *p != '/') {
            p++;
            seglen++;
        }

        if (seglen == 1 && seg[0] == '.')
            continue;                             /* "." changes nothing */

        /* ".." at the root stays at the root. A path that walks off the top of
           the tree is the classic way a shell reads something it should not. */
        if (seglen == 2 && seg[0] == '.' && seg[1] == '.') {
            while (n > 1 && out[n - 1] != '/')
                n--;
            if (n > 1)
                n--;
            continue;
        }

        if (n > 1 && n < KFS_PATH - 1)
            out[n++] = '/';
        while (seglen) {
            if (n >= KFS_PATH - 1)
                return false;                     /* too long: refuse */
            out[n++] = *seg++;
            seglen--;
        }
    }

    if (n == 0)
        out[n++] = '/';
    out[n] = '\0';
    return true;
}

/* Copy the caller's 24-bit path into argbuf, then resolve it into pathbuf.
   Returns false if it does not fit, which is a refusal and not a truncation --
   a truncated path names a DIFFERENT file, and deleting that would be worse
   than failing. */
static bool
resolve(uint32_t addr)
{
    uint8_t __far *p = farp(addr);
    uint16_t       i = 0;

    while (i < KFS_PATH - 1 && p[i])
        i++;
    if (p[i])
        return false;
    argbuf[i] = '\0';
    while (i) {
        i--;
        argbuf[i] = (char)p[i];
    }
    return kfs_abspath(argbuf, pathbuf);
}

/* ---- handle table ------------------------------------------------------- */

static fat32_file *
fileh(uint16_t h)
{
    if (h < 1 || h > KFS_FILES)
        return 0;
    if (fmode[h - 1] == 0)
        return 0;
    return &files[h - 1];
}

static fat32_dir *
dirh(uint16_t h)
{
    if (h < KFS_DIRBIT || h >= KFS_DIRBIT + KFS_DIRS)
        return 0;
    if (dused[h - KFS_DIRBIT] == 0)
        return 0;
    return &dirs[h - KFS_DIRBIT];
}

/* ---- files -------------------------------------------------------------- */

uint16_t
kfs_open(void)
{
    uint16_t mode = kfs_y;
    uint16_t i;

    if (!kfs_ready())
        return err(KERR_IO);
    if (!resolve(argptr()))
        return err(KERR_BADARG);

    for (i = 0; i < KFS_FILES; i++)
        if (fmode[i] == 0)
            break;
    if (i == KFS_FILES)
        return err(KERR_NOSPACE);   /* no handle left, not no disk space */

    if (mode == KFS_WRITE) {
        if (!fat32_create(pathbuf, &files[i]))
            return err(KERR_IO);
    } else {
        if (!fat32_open(pathbuf, &files[i]))
            return err(KERR_NOTFOUND);
    }
    fmode[i] = (uint8_t)(mode + 1);
    return ok(i + 1);
}

uint16_t
kfs_close(void)
{
    fat32_file *f = fileh(kfs_c);

    if (!f)
        return err(KERR_BADARG);
    /* fat32_close is what makes a written file durable -- the size lives in
       the directory entry, and until that is written the file is whatever
       length it was before. */
    if (!fat32_close(f)) {
        fmode[kfs_c - 1] = 0;
        return err(KERR_IO);
    }
    fmode[kfs_c - 1] = 0;
    return ok(0);
}

uint16_t
kfs_read(void)
{
    uint32_t    blk = argptr();
    fat32_file *f   = fileh(fget16(blk));
    uint32_t    dst;
    uint32_t    want;
    uint32_t    done = 0;

    if (!f)
        return err(KERR_BADARG);
    dst  = fget32(blk + 2) & 0x00FFFFFFUL;
    want = fget32(blk + 6);

    fat32_clearerr();
    while (want) {
        uint16_t chunk = (want > sizeof stage) ? (uint16_t)sizeof stage
                                               : (uint16_t)want;
        uint16_t got   = fat32_read(f, stage, chunk);
        uint16_t i;

        if (got == 0)
            break;                          /* end of file -- or see below */
        for (i = 0; i < got; i++)
            farp(dst + done + i)[0] = stage[i];
        done += got;
        want -= got;
        if (got < chunk)
            break;
    }

    /* A short count has TWO causes and the ABI must not conflate them.
       End of file is SUCCESS: carry clear, C = bytes moved (0 at EOF). A
       device failure mid-transfer is KERR_IO with carry set -- the bytes
       that did arrive are still counted in the block at +10, so a caller
       can tell how much of its buffer is real. */
    fput32(blk + 10, done);
    if (fat32_ioerr())
        return err(KERR_IO);
    return ok((uint16_t)done);
}

uint16_t
kfs_write(void)
{
    uint32_t    blk = argptr();
    uint16_t    h   = fget16(blk);
    fat32_file *f   = fileh(h);
    uint32_t    src;
    uint32_t    want;
    uint32_t    done = 0;

    if (!f)
        return err(KERR_BADARG);
    if (fmode[h - 1] != KFS_WRITE + 1)
        return err(KERR_BADARG);            /* opened read-only */
    src  = fget32(blk + 2) & 0x00FFFFFFUL;
    want = fget32(blk + 6);

    while (want) {
        uint16_t chunk = (want > sizeof stage) ? (uint16_t)sizeof stage
                                               : (uint16_t)want;
        uint16_t put;
        uint16_t i;

        for (i = 0; i < chunk; i++)
            stage[i] = farp(src + done + i)[0];
        put = fat32_write(f, stage, chunk);
        done += put;
        want -= put;
        if (put < chunk)
            break;                          /* disk full or an I/O error */
    }

    fput32(blk + 10, done);
    return ok((uint16_t)done);
}

uint16_t
kfs_seek(void)
{
    uint32_t    blk = argptr();
    fat32_file *f   = fileh(fget16(blk));
    uint16_t    whence;
    uint32_t    off;
    uint32_t    target;

    if (!f)
        return err(KERR_BADARG);
    whence = farp(blk + 2)[0];
    off    = fget32(blk + 4);

    if (whence == KFS_SET)
        target = off;
    else if (whence == KFS_CUR)
        target = f->pos + off;
    else if (whence == KFS_END)
        target = f->size + off;
    else
        return err(KERR_BADARG);

    if (target > f->size)
        return err(KERR_BADARG);

    /* Seeking is re-walking the cluster chain, which fat32.c only does
       forwards; going backwards means starting over. Correct either way, and
       the cost is a chain walk rather than a data transfer. */
    if (target < f->pos) {
        f->pos         = 0;
        f->cluster     = f->first_cluster;
        f->cluster_off = 0;
    }
    while (f->pos < target) {
        uint32_t gap   = target - f->pos;
        uint16_t chunk = (gap > sizeof stage) ? (uint16_t)sizeof stage
                                              : (uint16_t)gap;
        if (fat32_read(f, stage, chunk) != chunk)
            return err(KERR_IO);
    }

    fput32(blk + 8, f->pos);
    return ok((uint16_t)f->pos);
}

uint16_t
kfs_size(void)
{
    fat32_file *f = fileh(kfs_c);

    if (!f)
        return err(KERR_BADARG);
    /* 32 bits across C and X, the same shape section 5.5 uses for a 24-bit
       address -- a parameter block for one number no caller can misread is
       ceremony. */
    kfs_x = (uint16_t)(f->size >> 16);
    return ok((uint16_t)f->size);
}

uint16_t
kfs_delete(void)
{
    if (!kfs_ready())
        return err(KERR_IO);
    if (!resolve(argptr()))
        return err(KERR_BADARG);
    /* fat32_unlink also refuses a DIRECTORY, which is not "not found". */
    if (!fat32_unlink(pathbuf)) {
        uint32_t clus;
        bool     isdir;
        if (!fat32_stat(pathbuf, &clus, 0, &isdir))
            return err(KERR_NOTFOUND);
        return err(isdir ? KERR_BADARG : KERR_IO);
    }
    return ok(0);
}

uint16_t
kfs_rename(void)
{
    uint32_t blk = argptr();
    uint32_t newp;
    uint16_t i;

    if (!kfs_ready())
        return err(KERR_IO);
    if (!resolve(fget32(blk) & 0x00FFFFFFUL))
        return err(KERR_BADARG);

    /* The new name is a bare 8.3 name, not a path: renaming ACROSS directories
       means rewriting the entry somewhere else, which is a different operation
       from editing one in place. So it goes into argbuf verbatim, unresolved. */
    newp = fget32(blk + 4) & 0x00FFFFFFUL;
    for (i = 0; i < 13; i++) {
        argbuf[i] = (char)farp(newp + i)[0];
        if (argbuf[i] == '\0')
            break;
    }
    if (i == 13)
        return err(KERR_BADARG);

    if (!fat32_rename(pathbuf, argbuf))
        return err(KERR_IO);
    return ok(0);
}

/* ---- directories -------------------------------------------------------- */

uint16_t
kfs_diropen(void)
{
    uint16_t i;

    if (!kfs_ready())
        return err(KERR_IO);
    if (!resolve(argptr()))
        return err(KERR_BADARG);

    for (i = 0; i < KFS_DIRS; i++)
        if (dused[i] == 0)
            break;
    if (i == KFS_DIRS)
        return err(KERR_NOSPACE);

    if (!fat32_opendir(pathbuf, &dirs[i]))
        return err(KERR_NOTFOUND);
    dused[i] = 1;
    return ok(i + KFS_DIRBIT);
}

uint16_t
kfs_dirnext(void)
{
    fat32_dir   *d = dirh(kfs_c);
    fat32_dirent e;
    uint32_t     buf;
    uint16_t     i;

    if (!d)
        return err(KERR_BADARG);
    /* X:Y here, not C:X -- C is spent on the handle. Same 24-bit shape, just
       shifted along one register. */
    buf = (uint32_t)kfs_x | ((uint32_t)(kfs_y & 0x00FF) << 16);

    /* End of directory is carry set with KERR_NOTFOUND, which is how a caller
       tells "no more entries" from "that was not a directory handle": the
       latter is KERR_BADARG and happens on the FIRST call, not the last. */
    if (!fat32_readdir(d, &e))
        return err(KERR_NOTFOUND);

    for (i = 0; i < 13; i++)
        farp(buf + i)[0] = (uint8_t)e.name[i];
    farp(buf + 13)[0] = e.is_dir ? 1 : 0;
    fput32(buf + 14, e.size);
    return ok(0);
}

uint16_t
kfs_dirclose(void)
{
    if (!dirh(kfs_c))
        return err(KERR_BADARG);
    dused[kfs_c - KFS_DIRBIT] = 0;
    return ok(0);
}

/* ---- the working directory ---------------------------------------------- */

/* pathbuf already holds the resolved path. Returns 0 or a KERR_ code, so the
   ABI entry and the prompt's entry refuse for exactly the same reasons. */
static uint16_t
commit_cwd(void)
{
    uint32_t clus;
    bool     isdir;
    uint16_t i;

    /* The root always exists and has no entry to stat. */
    if (!(pathbuf[0] == '/' && pathbuf[1] == '\0')) {
        if (!fat32_stat(pathbuf, &clus, 0, &isdir))
            return KERR_NOTFOUND;
        if (!isdir)
            return KERR_BADARG;
    }

    for (i = 0; pathbuf[i] && i < KFS_PATH - 1; i++)
        cwdbuf[i] = pathbuf[i];
    cwdbuf[i] = '\0';
    return 0;
}

/* The prompt's door. Same validation, same refusals, different way in. */
bool
kfs_chdir_path(const char *arg)
{
    if (!kfs_ready())
        return false;
    if (!kfs_abspath(arg, pathbuf))
        return false;
    return commit_cwd() == 0;
}

uint16_t
kfs_chdir(void)
{
    uint16_t e;

    if (!kfs_ready())
        return err(KERR_IO);
    if (!resolve(argptr()))
        return err(KERR_BADARG);
    e = commit_cwd();
    return e ? err(e) : ok(0);
}

/* ---- the carry-over block ------------------------------------------------
 *
 * Why it exists and where it lives is in kfs.h. Here it is only two short
 * copies through a __far pointer, because the block is at an absolute address
 * and not an object the compiler knows about -- which is the whole point.
 */
void
kfs_carry_save(void)
{
    uint8_t __far *p = farp(KFS_CARRY_BASE);
    uint16_t       i;

    for (i = 0; i + 1 < KFS_PATH && cwdbuf[i]; i++)
        p[KFS_CARRY_PATH + i] = (uint8_t)cwdbuf[i];
    p[KFS_CARRY_PATH + i] = 0;

    /* The magic goes down LAST. It is what says the rest of the block is
       meant, so writing it first would leave a window in which a reset
       published a half-copied path. */
    p[0] = 'X';
    p[1] = 'C';
    p[2] = 'W';
    p[3] = 'D';
}

void
kfs_carry_adopt(void)
{
    uint8_t __far *p = farp(KFS_CARRY_BASE);
    uint16_t       i;

    if (!(p[0] == 'X' && p[1] == 'C' && p[2] == 'W' && p[3] == 'D'))
        return;

    for (i = 0; i + 1 < KFS_PATH && p[KFS_CARRY_PATH + i]; i++)
        cwdbuf[i] = (char)p[KFS_CARRY_PATH + i];
    cwdbuf[i] = '\0';

    /* cwdbuf is absolute by construction everywhere else, and kfs_abspath
       assumes it: a relative or empty string here would make every relative
       path resolve against nothing. Refuse it rather than carry it. */
    if (cwdbuf[0] != '/') {
        cwdbuf[0] = '/';
        cwdbuf[1] = '\0';
    }
}

void
kfs_carry_restore(void)
{
    kfs_carry_adopt();

    /* Consumed, and only HERE -- kfs_carry_adopt deliberately leaves the block
       armed. Every launch re-arms it, so clearing costs nothing a program can
       see, and it is what keeps a RESET at an idle prompt from reviving the
       directory some earlier program was launched from. */
    farp(KFS_CARRY_BASE)[0] = 0;
}

uint16_t
kfs_getcwd(void)
{
    uint32_t buf = argptr();
    uint16_t i;

    for (i = 0; cwdbuf[i]; i++)
        farp(buf + i)[0] = (uint8_t)cwdbuf[i];
    farp(buf + i)[0] = 0;
    return ok(i);
}

/* fat32.c answers true or false, and the ABI promises a REASON. Asking the
   filesystem what is actually at the path is the only way to turn one into the
   other -- and it matters more than it looks: a blanket code sends whoever
   reads it looking in the wrong place. This is exactly how a failed mkdir on
   hardware read as "already exists" when it could equally have been a card
   that would not mount. */
static uint16_t
why_not(bool want_dir)
{
    uint32_t clus;
    bool     isdir;

    if (fat32_stat(pathbuf, &clus, 0, &isdir))
        return want_dir && !isdir ? KERR_BADARG : KERR_EXISTS;
    return KERR_IO;
}

uint16_t
kfs_mkdir(void)
{
    if (!kfs_ready())
        return err(KERR_IO);
    if (!resolve(argptr()))
        return err(KERR_BADARG);
    if (!fat32_mkdir(pathbuf))
        return err(why_not(false));
    return ok(0);
}

uint16_t
kfs_rmdir(void)
{
    uint32_t clus;
    bool     isdir;

    if (!kfs_ready())
        return err(KERR_IO);
    if (!resolve(argptr()))
        return err(KERR_BADARG);
    /* fat32_rmdir refuses a non-empty directory, and that refusal is the
       reason this cannot just be FS_DELETE: freeing the chain would strand
       every file inside with nothing pointing at it. But it also refuses a
       path that is not there and one that is a file, and a caller told
       NOTEMPTY for those goes looking for contents that do not exist. */
    if (!fat32_rmdir(pathbuf)) {
        if (!fat32_stat(pathbuf, &clus, 0, &isdir))
            return err(KERR_NOTFOUND);
        return err(isdir ? KERR_NOTEMPTY : KERR_BADARG);
    }
    return ok(0);
}
