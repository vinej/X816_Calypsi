/* ==========================================================================
 * fat32.h -- read-only FAT32, for X816.
 *
 * This is a LIBRARY, not kernel code, and deliberately so. X816_Core
 * doc/KERNEL.md section 2.2 splits policy from mechanism: deciding who owns
 * file handle 3 is policy and belongs to the kernel, but parsing FAT32 is
 * mechanism, and mechanism can live in a library the kernel links. Keeping it
 * here means it can be tested on its own, and a program that wants to read a
 * card without the kernel can.
 *
 * Read AND write, in the order doc/KERNEL.md section 9 asks for. Writing is
 * the part that can destroy a card rather than merely fail, so every update
 * below is read-modify-write of a whole sector and every FAT copy is kept in
 * step -- see the notes in fat32.c.
 *
 * Paths are 8.3, uppercase, '/'-separated and absolute: "/HELLO.TXT",
 * "/SUB/NESTED.TXT". Long filenames are skipped when scanning a directory --
 * their entries carry attribute 0x0F -- so a file created with a long name is
 * still reachable through its short alias.
 * ========================================================================== */

#ifndef FAT32_H
#define FAT32_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint32_t first_cluster;
    uint32_t size;          /* bytes */
    uint32_t pos;           /* bytes consumed so far */
    uint32_t cluster;       /* cluster holding `pos` */
    uint16_t cluster_off;   /* byte offset within that cluster */
    /* Where this file's directory entry lives, so fat32_close can write the
       final size back without searching for it again. Zero lba means the
       handle was opened read-only and has nothing to flush. */
    uint32_t dir_lba;
    uint16_t dir_off;
    bool     dirty;
} fat32_file;

/* Read the boot sector and cache the geometry. Handles both a bare filesystem
   (BPB in sector 0) and a partitioned card (MBR in sector 0, filesystem in
   the first partition) -- a card straight from mkfs looks like the former,
   one formatted by a desktop OS like the latter. */
bool fat32_mount(void);

/* Volume geometry, valid after a successful mount. Exposed because a kernel
   sitting on top will want them, and because a test can assert on them. */
uint32_t fat32_root_cluster(void);
uint16_t fat32_bytes_per_cluster(void);

bool fat32_open(const char *path, fat32_file *f);

/* ---- directories --------------------------------------------------------
 *
 * Enumeration is separate from fat32_open because opening a DIRECTORY and
 * opening a FILE want different things: a file wants a byte stream, a
 * directory wants records. Folding both into fat32_file would give callers a
 * handle whose meaning depends on what it happens to point at.
 *
 * The iterator holds its position as (cluster, sector, offset) rather than a
 * byte count, because that is what walking a cluster chain actually needs and
 * it avoids recomputing the chain on every step.
 */
typedef struct {
    uint32_t clus;          /* cluster currently being scanned */
    uint8_t  sec;           /* sector within that cluster */
    uint16_t off;           /* byte offset within that sector */
    bool     done;
} fat32_dir;

typedef struct {
    char     name[13];      /* "NAME.EXT", NUL-terminated; no padding */
    uint32_t size;          /* bytes; 0 for a directory */
    uint32_t cluster;
    bool     is_dir;
} fat32_dirent;

/* `path` is absolute, "/" for the root. Fails if it names a file. */
bool fat32_opendir(const char *path, fat32_dir *d);

/* Fills `e` with the next real entry and returns true, or returns false at the
   end. Skips deleted entries, long-filename fragments and the volume label,
   so callers see only things they can act on. */
bool fat32_readdir(fat32_dir *d, fat32_dirent *e);

/* Resolve any path to its directory entry without opening it. `path` may name
   a file or a directory; "/" reports the root. Used by cd, and by anything
   that needs to know WHICH of the two a name is before committing to it. */
bool fat32_stat(const char *path, uint32_t *cluster, uint32_t *size,
                bool *is_dir);

/* ---- writing ------------------------------------------------------------
 *
 * fat32_create makes `path` if it is absent and TRUNCATES it if it is not, so
 * it is "open for writing" rather than a separate create. The parent directory
 * must already exist -- no path is created implicitly, because a typo should
 * not silently produce a new directory tree.
 *
 * Nothing is durable until fat32_close: the directory entry carries the size,
 * and until it is written the file is whatever length it was before. Closing
 * a handle twice is harmless.
 */
bool     fat32_create(const char *path, fat32_file *f);
uint16_t fat32_write(fat32_file *f, const uint8_t *src, uint16_t len);
bool     fat32_close(fat32_file *f);

/* Delete a file and release its clusters. Refuses directories -- removing one
   means checking it is empty first, which is fat32_rmdir's job when it
   exists. */
bool     fat32_unlink(const char *path);

/* Rename within the same directory. `newname` is a bare 8.3 name, not a path:
 * moving between directories is a different operation and needs the entry
 * rewritten somewhere else, not edited in place.
 *
 * Works for directories as well as files -- only the name field changes, so
 * the cluster chain and the "." / ".." entries inside are untouched. Refuses
 * if the new name is already taken, because overwriting it would strand the
 * victim's clusters with nothing pointing at them. */
bool     fat32_rename(const char *path, const char *newname);

/* Create a directory. The new cluster is zeroed and seeded with its own "."
 * and ".." entries, which is what distinguishes making a directory from making
 * a file -- a directory without them is unnavigable from inside.
 *
 * Note ".." stores cluster 0 when the parent is the ROOT. FAT32 spells "the
 * root" as zero in that field, and writing the real root cluster number there
 * is a classic way to produce a tree that chkdsk rejects.
 */
bool     fat32_mkdir(const char *path);

/* Remove a directory. REFUSES a non-empty one -- freeing its chain would strand
 * every file inside with nothing pointing at them. "." and ".." do not count
 * as contents. */
bool     fat32_rmdir(const char *path);

/* Reads up to `len` bytes into a near buffer. Returns the count, 0 at EOF. */
uint16_t fat32_read(fat32_file *f, uint8_t *dst, uint16_t len);

/* Reads whole clusters straight to a flat 24-bit address using the block
   device's DMA, which is the fast path: no per-byte cost through the window.
   Returns bytes transferred. `dest` must be cluster-aligned in the sense that
   the caller accepts whole clusters; the final partial cluster is included. */
uint32_t fat32_read_far(fat32_file *f, uint32_t dest, uint32_t len);

/* ---- error vs EOF -------------------------------------------------------
 *
 * fat32_read and fat32_read_far return a SHORT COUNT both at end-of-file and
 * when the device fails mid-transfer, and the count alone cannot say which.
 * The volume keeps one sticky I/O-error flag, set whenever a device transfer
 * fails inside a data path: a sector fetch, a DMA read, a sector write, or a
 * FAT fetch during a chain walk. It is volume-level rather than per-file
 * because there is one card, one device window and no interrupts, so
 * operations never interleave; a caller that needs the distinction brackets
 * its own loop:
 *
 *     fat32_clearerr();
 *     while ((n = fat32_read(&f, buf, sizeof buf)) != 0) ...
 *     if (fat32_ioerr())  ... truncated by a device failure, not EOF ...
 *
 * fat32_mount clears the flag; nothing else clears it implicitly. A short
 * fat32_write with the flag CLEAR is the volume filling up, with it SET a
 * device failure. */
bool fat32_ioerr(void);
void fat32_clearerr(void);

#endif /* FAT32_H */
