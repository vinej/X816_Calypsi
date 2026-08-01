/* ==========================================================================
 * fwtest.c -- FAT32 WRITE conformance.
 *
 * Writes files from X816, then the host verifies the resulting image with
 * pyfatfs -- an INDEPENDENT implementation. That direction matters: a writer
 * checked only with our own reader proves the two agree, not that either is
 * right. The reader was brought up the same way round.
 *
 * On screen:
 *   GREEN    every test passed
 *   RED      test 1: create + write + close a small file
 *   YELLOW   test 2: a file spanning several clusters
 *   BLUE     test 3: truncate an existing file
 *   MAGENTA  test 4: read back what was written, through our own reader
 *   CYAN     test 5: unlink
 *   WHITE    mount failed -- nothing else could run
 *
 * The result code also goes to $00:0400.
 * ========================================================================== */

#include "fat32.h"

#define VERA_ADDR_L     (*(volatile unsigned char *)0x9F20)
#define VERA_ADDR_M     (*(volatile unsigned char *)0x9F21)
#define VERA_ADDR_H     (*(volatile unsigned char *)0x9F22)
#define VERA_DATA0      (*(volatile unsigned char *)0x9F23)
#define VERA_CTRL       (*(volatile unsigned char *)0x9F25)
#define VERA_DC_VIDEO   (*(volatile unsigned char *)0x9F29)
#define VERA_DC_HSCALE  (*(volatile unsigned char *)0x9F2A)
#define VERA_DC_VSCALE  (*(volatile unsigned char *)0x9F2B)
#define VERA_L0_CONFIG  (*(volatile unsigned char *)0x9F2D)
#define VERA_L0_TILEB   (*(volatile unsigned char *)0x9F2F)

#define RESULT (*(volatile unsigned char *)0x0400)

/* 3000 bytes: more than one 512-byte cluster on the test image, so writing it
   has to allocate a chain and follow it rather than filling one cluster. */
#define BIG_LEN 3000

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

/* Position-dependent, so a chain followed to the wrong cluster produces wrong
   bytes rather than plausible ones. Same trick boot/mkfat32.py uses. */
static unsigned char
pattern(unsigned int i)
{
    return (unsigned char)((i * 7u + 13u) & 0xFFu);
}

int
main(void)
{
    unsigned char fail = 0;
    fat32_file    f;
    unsigned char buf[64];
    unsigned int  i;
    unsigned int  n;

    if (!fat32_mount()) {
        RESULT = 6;
        paint(0x01);
        for (;;) ;
    }

    /* ---- 1: create, write, close --------------------------------------- */
    if (!fail) {
        static char msg[] = "HELLO FROM X816\n";
        static char path[] = "/WROTE.TXT";
        unsigned int len = 0;
        while (msg[len]) len++;

        if (!fat32_create(path, &f))
            fail = 1;
        else if (fat32_write(&f, (unsigned char *)msg, (unsigned int)len) != len)
            fail = 1;
        else if (!fat32_close(&f))
            fail = 1;
    }

    /* ---- 2: a file bigger than one cluster ----------------------------- */
    if (!fail) {
        static char path[] = "/BIGW.BIN";
        if (!fat32_create(path, &f)) {
            fail = 2;
        } else {
            unsigned int written = 0;
            while (written < BIG_LEN && !fail) {
                unsigned int chunk = BIG_LEN - written;
                if (chunk > sizeof buf)
                    chunk = sizeof buf;
                for (i = 0; i < chunk; i++)
                    buf[i] = pattern(written + i);
                if (fat32_write(&f, buf, (unsigned int)chunk) != chunk)
                    fail = 2;
                written += chunk;
            }
            if (!fail && !fat32_close(&f))
                fail = 2;
        }
    }

    /* ---- 3: truncate an existing file ---------------------------------- */
    /* Rewriting /WROTE.TXT shorter must leave the SHORT content, not the old
       tail. A truncate that only updates the size leaves the rest readable. */
    if (!fail) {
        static char path[] = "/WROTE.TXT";
        static char msg2[] = "SHORT\n";
        unsigned int len = 0;
        while (msg2[len]) len++;

        if (!fat32_create(path, &f))
            fail = 3;
        else if (fat32_write(&f, (unsigned char *)msg2, (unsigned int)len) != len)
            fail = 3;
        else if (!fat32_close(&f))
            fail = 3;
    }

    /* ---- 4: read back through our own reader --------------------------- */
    if (!fail) {
        static char path[] = "/BIGW.BIN";
        unsigned long total = 0;
        if (!fat32_open(path, &f) || f.size != BIG_LEN) {
            fail = 4;
        } else {
            while ((n = fat32_read(&f, buf, sizeof buf)) != 0) {
                for (i = 0; i < n; i++) {
                    if (buf[i] != pattern((unsigned int)(total + i))) {
                        fail = 4;
                        break;
                    }
                }
                total += n;
                if (fail)
                    break;
            }
            if (!fail && total != BIG_LEN)
                fail = 4;
        }
    }

    /* ---- 5: unlink ----------------------------------------------------- */
    if (!fail) {
        static char path[] = "/GONE.TXT";
        static char msg3[] = "DELETE ME\n";
        unsigned int len = 0;
        while (msg3[len]) len++;

        if (!fat32_create(path, &f))
            fail = 5;
        else if (fat32_write(&f, (unsigned char *)msg3, (unsigned int)len) != len)
            fail = 5;
        else if (!fat32_close(&f))
            fail = 5;
        else if (!fat32_unlink(path))
            fail = 5;
        else if (fat32_open(path, &f))      /* must be gone */
            fail = 5;
    }

    RESULT = fail;
    switch (fail) {
    case 0:  paint(0x05); break;    /* green  */
    case 1:  paint(0x02); break;    /* red    */
    case 2:  paint(0x07); break;    /* yellow */
    case 3:  paint(0x06); break;    /* blue   */
    case 4:  paint(0x04); break;    /* magenta*/
    default: paint(0x03); break;    /* cyan   */
    }
    for (;;)
        ;
}
