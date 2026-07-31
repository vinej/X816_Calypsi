/* ==========================================================================
 * fstest.c -- FAT32 read conformance test.
 *
 *   GREEN    every test passed
 *   RED      test 1: mount failed
 *   YELLOW   test 2: geometry wrong
 *   BLUE     test 3: /HELLO.TXT contents wrong
 *   MAGENTA  test 4: /SUB/NESTED.TXT wrong -- subdirectory walk
 *   CYAN     test 5: /BIG.BIN wrong -- multi-cluster chain
 *   WHITE    test 6: a missing file was reported as found
 *
 * The image is built by X816_Core boot/mkfat32.py using pyfatfs and verified
 * with 7-Zip, so this tests interoperation with an independent FAT32
 * implementation rather than agreement with our own writer.
 *
 * /BIG.BIN is 20000 bytes of (i*7 + 13) & 0xFF. At one sector per cluster
 * that is 40 clusters, so it cannot be read correctly without walking the FAT
 * chain -- and the pattern is position-dependent, so following the chain to
 * the wrong cluster fails rather than returning plausible bytes.
 * NOTE -- why the paths are `static char[]` and not string literals.
 *
 * A string literal, and any `const` array, is emitted into Calypsi's `cdata`
 * section, which PROVIDES BITS: the linker places its contents at its address
 * rather than generating an initialiser for it. On X816 that address has to be
 * in bank $00 for the small data model's 16-bit references to reach it -- but
 * the image is loaded at $01:0000, so nothing can deliver bank-$00 content.
 * A non-const `static char[]` goes into `data` instead, whose initialiser
 * rides in the image as `idata` and is copied into bank $00 by cstartup.
 *
 * This is a real constraint on X816 C, not a quirk of this test; see the
 * README. The alternative is the medium data model, which puts constants in
 * their own bank at the cost of 24-bit pointers throughout.
 * ========================================================================== */

#include <string.h>
#include "fat32.h"
#include "x816_sd.h"

#define VERA_ADDR_L    (*(volatile unsigned char *)0x9F20)
#define VERA_ADDR_M    (*(volatile unsigned char *)0x9F21)
#define VERA_ADDR_H    (*(volatile unsigned char *)0x9F22)
#define VERA_DATA0     (*(volatile unsigned char *)0x9F23)
#define VERA_CTRL      (*(volatile unsigned char *)0x9F25)
#define VERA_DC_VIDEO  (*(volatile unsigned char *)0x9F29)
#define VERA_DC_HSCALE (*(volatile unsigned char *)0x9F2A)
#define VERA_DC_VSCALE (*(volatile unsigned char *)0x9F2B)
#define VERA_L0_CONFIG (*(volatile unsigned char *)0x9F2D)
#define VERA_L0_TILEB  (*(volatile unsigned char *)0x9F2F)

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

static unsigned char buf[600];

int
main(void)
{
    unsigned char fail = 0;
    fat32_file f;

    /* ---- 1: mount ------------------------------------------------------ */
    if (!fat32_mount())
        fail = 1;

    /* ---- 2: geometry --------------------------------------------------- */
    /* The root cluster is 2 and the cluster size is a power of two at least
       one sector. Checking these catches a BPB parsed at the wrong offsets,
       which would otherwise only show up later as garbled file data. */
    if (!fail) {
        unsigned int cb = fat32_bytes_per_cluster();
        if (fat32_root_cluster() != 2 || cb == 0 || (cb & (cb - 1)) != 0)
            fail = 2;
    }

    /* ---- 3: a file in the root ----------------------------------------- */
    if (!fail) {
        static char want[] = "Hello from FAT32 on X816!\n";
        unsigned int n;
        static char path[] = "/HELLO.TXT";
        if (!fat32_open(path, &f) || f.size != sizeof want - 1) {
            fail = 3;
        } else {
            n = fat32_read(&f, buf, sizeof buf);
            if (n != sizeof want - 1 || memcmp(buf, want, n) != 0)
                fail = 3;
        }
    }

    /* ---- 4: a file in a subdirectory ----------------------------------- */
    if (!fail) {
        static char want[] = "nested file\n";
        unsigned int n;
        static char path[] = "/SUB/NESTED.TXT";
        if (!fat32_open(path, &f) || f.size != sizeof want - 1) {
            fail = 4;
        } else {
            n = fat32_read(&f, buf, sizeof buf);
            if (n != sizeof want - 1 || memcmp(buf, want, n) != 0)
                fail = 4;
        }
    }

    /* ---- 5: a multi-cluster file --------------------------------------- */
    /* Read it in 600-byte bites so the reads straddle sector and cluster
       boundaries at every offset rather than landing on them neatly -- an
       off-by-one in the cluster walk survives aligned reads. */
    if (!fail) {
        unsigned long pos = 0;
        unsigned int n;
        static char path[] = "/BIG.BIN";
        if (!fat32_open(path, &f) || f.size != 20000UL) {
            fail = 5;
        } else {
            for (;;) {
                n = fat32_read(&f, buf, sizeof buf);
                if (n == 0)
                    break;
                for (unsigned int i = 0; i < n; i++) {
                    if (buf[i] != (unsigned char)((pos + i) * 7 + 13)) {
                        fail = 5;
                        break;
                    }
                }
                if (fail)
                    break;
                pos += n;
            }
            if (!fail && pos != 20000UL)
                fail = 5;
        }
    }

    /* ---- 6: a missing file must fail ----------------------------------- */
    /* Without this, an open() that returned true unconditionally would pass
       every test above, because they all open files that exist. */
    if (!fail) {
        static char path[] = "/NOPE.XXX";
        if (fat32_open(path, &f))
            fail = 6;
    }

    RESULT = fail;
    switch (fail) {
    case 0:  paint(0x05); break;   /* green   */
    case 1:  paint(0x02); break;   /* red     */
    case 2:  paint(0x07); break;   /* yellow  */
    case 3:  paint(0x06); break;   /* blue    */
    case 4:  paint(0x04); break;   /* magenta */
    case 5:  paint(0x03); break;   /* cyan    */
    default: paint(0x01); break;   /* white   */
    }
    for (;;)
        ;
}
