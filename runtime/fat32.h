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
 * Read-only for now, matching doc/KERNEL.md section 9's order: FAT32 read,
 * then write.
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

/* Reads up to `len` bytes into a near buffer. Returns the count, 0 at EOF. */
uint16_t fat32_read(fat32_file *f, uint8_t *dst, uint16_t len);

/* Reads whole clusters straight to a flat 24-bit address using the block
   device's DMA, which is the fast path: no per-byte cost through the window.
   Returns bytes transferred. `dest` must be cluster-aligned in the sense that
   the caller accepts whole clusters; the final partial cluster is included. */
uint32_t fat32_read_far(fat32_file *f, uint32_t dest, uint32_t len);

#endif /* FAT32_H */
