/* ==========================================================================
 * kfstest.c -- the filesystem half of the kernel jump table, on a real card.
 *
 * Everything goes through $00:FE00 by entry NUMBER. Nothing calls fat32_*
 * directly: that would prove FAT32 works, which examples/fat32 already proves.
 * What is in doubt here is the KERNEL layer -- the handle table, the working
 * directory, the parameter blocks, and whether carry reports the way every
 * caller will rely on.
 *
 * The buffers live in BANK $02, not bank $00, and that is deliberate. A
 * 24-bit buffer address is the whole reason this ABI exists rather than the
 * X16 KERNAL's 16-bit one; a test that read into bank $00 would never
 * exercise it.
 *
 *   GREEN    every test passed
 *   RED      1: FS_MKDIR
 *   YELLOW   2: FS_OPEN write, FS_WRITE, FS_CLOSE
 *   BLUE     3: FS_SIZE and FS_READ read back what was written
 *   MAGENTA  4: DIR_OPEN / DIR_NEXT / DIR_CLOSE
 *   CYAN     5: FS_CHDIR, FS_GETCWD, and a RELATIVE open
 *   ORANGE   6: FS_SEEK
 *   BROWN    7: FS_RENAME
 *   LT RED   8: FS_DELETE, FS_RMDIR, and the refusals that follow
 *
 * The number also lands at $00:0400 for a debugger, and the host side of
 * run-kfs.sh checks the card itself with an independent FAT32 implementation
 * -- an on-screen pass only proves this program agrees with itself.
 * ========================================================================== */

#include "kernel.h"
#include "kfs.h"
#include "console.h"

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

#define SRC   0x020000UL        /* what gets written                        */
#define DST   0x021000UL        /* what gets read back                      */
#define ENT   0x022000UL        /* DIR_NEXT's 18-byte entry buffer          */
#define CWDB  0x022100UL        /* FS_GETCWD's buffer                       */
#define LEN   40

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

static unsigned char __far *
far_ptr(unsigned long a)
{
    return (unsigned char __far *)a;
}

/* The address of a near object, as the ABI's C:X pair. Everything static here
   lives in bank $00, so the bank half is always zero -- but it is passed
   explicitly, because a thunk that ignores the bank byte would pass this test
   and fail on the first string a real program hands it. */
static unsigned int
lo(const void *p)
{
    return (unsigned int)(unsigned long)p;
}

static unsigned int
call1(unsigned int n, unsigned int c)
{
    kern_c = c;
    kern_x = 0;
    kern_y = 0;
    return kern_call(n);
}

static unsigned int
call3(unsigned int n, unsigned int c, unsigned int x, unsigned int y)
{
    kern_c = c;
    kern_x = x;
    kern_y = y;
    return kern_call(n);
}

/* ---- parameter blocks ---------------------------------------------------
 * Built in bank $00 and handed over as C:X like any other pointer. The layout
 * is kfs.h's, and getting a field offset wrong here would look exactly like a
 * kernel bug -- which is why the host side checks the card independently. */
static unsigned char blk[16];

static void
put32(unsigned char at, unsigned long v)
{
    blk[at]     = (unsigned char)v;
    blk[at + 1] = (unsigned char)(v >> 8);
    blk[at + 2] = (unsigned char)(v >> 16);
    blk[at + 3] = (unsigned char)(v >> 24);
}

static void
put16(unsigned char at, unsigned int v)
{
    blk[at]     = (unsigned char)v;
    blk[at + 1] = (unsigned char)(v >> 8);
}

static unsigned long
get32(unsigned char at)
{
    return (unsigned long)blk[at] | ((unsigned long)blk[at + 1] << 8) |
           ((unsigned long)blk[at + 2] << 16) |
           ((unsigned long)blk[at + 3] << 24);
}

/* handle, buffer, count -> the FS_READ / FS_WRITE block. */
static unsigned int
xfer(unsigned int entry, unsigned int h, unsigned long buf, unsigned long n)
{
    put16(0, h);
    put32(2, buf);
    put32(6, n);
    put32(10, 0);
    return call3(entry, lo(blk), 0, 0);
}

/* One step of the iterator: true if an entry came back. */
static unsigned char
dnext(unsigned int h)
{
    /* X:Y, not C:X -- C is spent on the handle. */
    call3(K_DIR_NEXT, h, (unsigned int)ENT, (unsigned int)(ENT >> 16));
    return kern_carry ? 0 : 1;
}

/* Compare the entry buffer's name field, terminator included, so a name that
   merely STARTS with the expected text does not pass. */
static unsigned char
named(const char *want)
{
    unsigned int i;
    for (i = 0; want[i]; i++)
        if (far_ptr(ENT + i)[0] != (unsigned char)want[i])
            return 0;
    return far_ptr(ENT + i)[0] == 0;
}

int
main(void)
{
    static char d_kt[]   = "/KT";
    static char f_a[]    = "/KT/A.TXT";
    static char f_rel[]  = "A.TXT";
    static char f_b[]    = "/KT/B.TXT";
    static char n_b[]    = "B.TXT";
    static char root[]   = "/";
    static char updir[]  = "..";

    unsigned char fail = 0;
    unsigned int  h, r, i;

    con_init();
    kern_install();

    /* A known, non-repeating pattern: a buffer that is all one byte would read
       back correctly even if the file were misaligned by a few bytes. */
    for (i = 0; i < LEN; i++)
        far_ptr(SRC + i)[0] = (unsigned char)('A' + (i % 26));

    /* ---- 1: make the directory ------------------------------------------ */
    /* The card starts without /KT, so a success here also proves the card
       mounted at all -- every later test would fail the same way otherwise. */
    if (!fail) {
        call3(K_FS_MKDIR, lo(d_kt), 0, 0);
        if (kern_carry)
            fail = 1;
    }

    /* ---- 2: create, write, close ----------------------------------------- */
    if (!fail) {
        h = call3(K_FS_OPEN, lo(f_a), 0, KFS_WRITE);
        if (kern_carry || h == 0)
            fail = 2;
        if (!fail) {
            r = xfer(K_FS_WRITE, h, SRC, LEN);
            if (kern_carry || r != LEN || get32(10) != LEN)
                fail = 2;
            /* Closing is what makes it durable: the size lives in the
               directory entry, and until that is written the file is whatever
               length it was before. A test that skipped this would pass on
               screen and leave a zero-length file on the card. */
            call1(K_FS_CLOSE, h);
            if (kern_carry)
                fail = 2;
        }
        /* The same again into /KEEP.TXT, which nothing later deletes. Test 8
           tidies /KT away, so without this the card would end the run looking
           exactly as it started and the host side would have nothing to check.
           An on-screen pass only proves this program agrees with itself. */
        if (!fail) {
            static char f_keep[] = "/KEEP.TXT";
            h = call3(K_FS_OPEN, lo(f_keep), 0, KFS_WRITE);
            if (kern_carry)
                fail = 2;
            else {
                if (xfer(K_FS_WRITE, h, SRC, LEN) != LEN)
                    fail = 2;
                call1(K_FS_CLOSE, h);
                if (kern_carry)
                    fail = 2;
            }
        }
    }

    /* ---- 3: size, and read it back --------------------------------------- */
    if (!fail) {
        h = call3(K_FS_OPEN, lo(f_a), 0, KFS_READ);
        if (kern_carry)
            fail = 3;
        if (!fail) {
            r = call1(K_FS_SIZE, h);
            /* 32 bits across C and X. The high half must be zero for a
               40-byte file; a kernel that left junk in X would be missed by
               checking only the low half. */
            if (kern_carry || r != LEN || kern_x != 0)
                fail = 3;
        }
        if (!fail) {
            for (i = 0; i < LEN; i++)
                far_ptr(DST + i)[0] = 0;
            r = xfer(K_FS_READ, h, DST, LEN);
            if (kern_carry || r != LEN)
                fail = 3;
            for (i = 0; i < LEN && !fail; i++)
                if (far_ptr(DST + i)[0] != far_ptr(SRC + i)[0])
                    fail = 3;
            /* At EOF a further read returns zero bytes and still succeeds --
               end of file is not an error. */
            if (!fail) {
                r = xfer(K_FS_READ, h, DST, LEN);
                if (kern_carry || r != 0)
                    fail = 3;
            }
        }
        call1(K_FS_CLOSE, h);
    }

    /* ---- 4: enumerate ---------------------------------------------------- */
    if (!fail) {
        h = call3(K_DIR_OPEN, lo(d_kt), 0, 0);
        if (kern_carry)
            fail = 4;
        /* "." and ".." come first and are NOT hidden. A directory without them
           is unnavigable from inside, so seeing them is part of proving mkdir
           seeded the cluster properly -- and a kernel that filtered them would
           be deciding policy for every caller. Skipping them is the caller's
           job, one line of it. */
        /* `static char[]` and not a string literal: a literal lands in the far
           constant section, whose address C cannot form on this compiler. */
        static char e_dot[]  = ".";
        static char e_dot2[] = "..";
        static char e_a[]    = "A.TXT";

        if (!fail && (!dnext(h) || !named(e_dot) || far_ptr(ENT + 13)[0] != 1))
            fail = 4;
        if (!fail && (!dnext(h) || !named(e_dot2) || far_ptr(ENT + 13)[0] != 1))
            fail = 4;
        if (!fail && (!dnext(h) || !named(e_a)))
            fail = 4;
        if (!fail && far_ptr(ENT + 13)[0] != 0)
            fail = 4;                   /* flagged as a directory */
        if (!fail && far_ptr(ENT + 14)[0] != LEN)
            fail = 4;                   /* wrong size */
        /* The end of the directory must be carry SET, and it must be
           distinguishable from a bad handle: this is KERR_NOTFOUND, a bad
           handle is KERR_BADARG. Without this the iterator could run forever
           and the test would still be green. */
        if (!fail) {
            r = call3(K_DIR_NEXT, h, (unsigned int)ENT,
                      (unsigned int)(ENT >> 16));
            if (!kern_carry || r != KERR_NOTFOUND)
                fail = 4;
        }
        if (!fail) {
            call1(K_DIR_CLOSE, h);
            if (kern_carry)
                fail = 4;
            /* Closed twice must refuse, or the handle table is not tracking
               anything. */
            call1(K_DIR_CLOSE, h);
            if (!kern_carry)
                fail = 4;
        }
    }

    /* ---- 5: the working directory ---------------------------------------- */
    if (!fail) {
        call3(K_FS_CHDIR, lo(d_kt), 0, 0);
        if (kern_carry)
            fail = 5;
        if (!fail) {
            call3(K_FS_GETCWD, (unsigned int)CWDB,
                  (unsigned int)(CWDB >> 16), 0);
            if (kern_carry || far_ptr(CWDB)[0] != '/' ||
                far_ptr(CWDB + 1)[0] != 'K' || far_ptr(CWDB + 2)[0] != 'T' ||
                far_ptr(CWDB + 3)[0] != 0)
                fail = 5;
        }
        /* The point of a working directory: a RELATIVE name now resolves. */
        if (!fail) {
            h = call3(K_FS_OPEN, lo(f_rel), 0, KFS_READ);
            if (kern_carry)
                fail = 5;
            else
                call1(K_FS_CLOSE, h);
        }
        /* ".." must climb, and must stop at the root rather than walking off
           the top of the tree. */
        if (!fail) {
            call3(K_FS_CHDIR, lo(updir), 0, 0);
            call3(K_FS_CHDIR, lo(updir), 0, 0);
            call3(K_FS_GETCWD, (unsigned int)CWDB,
                  (unsigned int)(CWDB >> 16), 0);
            if (far_ptr(CWDB)[0] != '/' || far_ptr(CWDB + 1)[0] != 0)
                fail = 5;
        }
    }

    /* ---- 6: seek --------------------------------------------------------- */
    if (!fail) {
        h = call3(K_FS_OPEN, lo(f_a), 0, KFS_READ);
        if (kern_carry)
            fail = 6;
        if (!fail) {
            put16(0, h);
            blk[2] = KFS_SET;
            blk[3] = 0;
            put32(4, 10);
            put32(8, 0);
            call3(K_FS_SEEK, lo(blk), 0, 0);
            if (kern_carry || get32(8) != 10)
                fail = 6;
        }
        if (!fail) {
            far_ptr(DST)[0] = 0;
            xfer(K_FS_READ, h, DST, 4);
            for (i = 0; i < 4 && !fail; i++)
                if (far_ptr(DST + i)[0] != far_ptr(SRC + 10 + i)[0])
                    fail = 6;
        }
        /* Backwards, which means restarting the cluster walk. */
        if (!fail) {
            put16(0, h);
            blk[2] = KFS_SET;
            blk[3] = 0;
            put32(4, 2);
            put32(8, 0);
            call3(K_FS_SEEK, lo(blk), 0, 0);
            xfer(K_FS_READ, h, DST, 4);
            for (i = 0; i < 4 && !fail; i++)
                if (far_ptr(DST + i)[0] != far_ptr(SRC + 2 + i)[0])
                    fail = 6;
        }
        /* Past the end must be refused, not clamped: a caller that seeks
           beyond EOF has a bug, and silently landing somewhere else hides
           it. */
        if (!fail) {
            put16(0, h);
            blk[2] = KFS_SET;
            blk[3] = 0;
            put32(4, LEN + 100);
            put32(8, 0);
            call3(K_FS_SEEK, lo(blk), 0, 0);
            if (!kern_carry)
                fail = 6;
        }
        call1(K_FS_CLOSE, h);
    }

    /* ---- 7: rename ------------------------------------------------------- */
    if (!fail) {
        put32(0, (unsigned long)lo(f_a));
        put32(4, (unsigned long)lo(n_b));
        call3(K_FS_RENAME, lo(blk), 0, 0);
        if (kern_carry)
            fail = 7;
        if (!fail) {
            h = call3(K_FS_OPEN, lo(f_a), 0, KFS_READ);
            if (!kern_carry)
                fail = 7;               /* the old name still opens */
        }
        if (!fail) {
            h = call3(K_FS_OPEN, lo(f_b), 0, KFS_READ);
            if (kern_carry)
                fail = 7;
            else
                call1(K_FS_CLOSE, h);
        }
    }

    /* ---- 8: delete, and the refusals ------------------------------------- */
    if (!fail) {
        /* Removing a non-empty directory must be refused, and this is the
           negative control the other seven tests need: without it a kernel
           that returned success unconditionally would be green throughout. */
        call3(K_FS_RMDIR, lo(d_kt), 0, 0);
        if (!kern_carry)
            fail = 8;
        if (!fail) {
            call3(K_FS_DELETE, lo(f_b), 0, 0);
            if (kern_carry)
                fail = 8;
        }
        if (!fail) {
            call3(K_FS_RMDIR, lo(d_kt), 0, 0);
            if (kern_carry)
                fail = 8;
        }
        if (!fail) {
            h = call3(K_FS_OPEN, lo(f_b), 0, KFS_READ);
            if (!kern_carry || h != KERR_NOTFOUND)
                fail = 8;
        }
        (void)root;
    }

    RESULT = fail;
    switch (fail) {
    case 0:  paint(0x05); break;        /* green  */
    case 1:  paint(0x02); break;        /* red    */
    case 2:  paint(0x07); break;        /* yellow */
    case 3:  paint(0x06); break;        /* blue   */
    case 4:  paint(0x04); break;        /* magenta*/
    case 5:  paint(0x03); break;        /* cyan   */
    case 6:  paint(0x08); break;        /* orange */
    case 7:  paint(0x09); break;        /* brown  */
    default: paint(0x0A); break;        /* light red */
    }
    for (;;)
        ;
}
