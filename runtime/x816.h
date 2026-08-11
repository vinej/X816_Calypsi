/* ==========================================================================
 * x816.h -- C access to the converted X16 assembly library.
 *
 * Link against runtime/x816_glue.s and the library modules you enable, using
 * runtime/x816-lib.scm. See programs/c-lib.
 *
 * WHY THERE IS GLUE AT ALL
 *
 * C and x16lib disagree on three things at once, and every one of them is
 * silent if you get it wrong:
 *
 *   call    C in the large code model calls with a 24-bit jsl and expects
 *           rtl. The library's routines end in rts. A jsl straight into one
 *           returns to garbage.
 *   width   Calypsi keeps the index registers 16 bits wide and calls with a
 *           16-bit accumulator; a function must RETURN in that state. x16lib
 *           is 65C02 code and needs 8-bit A/X/Y throughout.
 *   bank    The library's internal calls are 16-bit jsr, taking their bank
 *           from PBR. The glue therefore lives in the same `code` section as
 *           the library, so a jsl into it leaves PBR pointing at the right
 *           bank.
 *
 * All of that is what x816_glue.s does. From C it is an ordinary function
 * call.
 *
 * DIRECT PAGE
 *
 * The library's scratch pointers are fixed at $22-$31, and the C runtime's
 * pseudo-registers sit at the bottom of the same direct page. The linker
 * knows about the latter and nothing about the former, so x816-lib.scm bounds
 * the runtime's region at $21 to turn any future overlap into a link error.
 * If you raise X16_ZP, raise X816_ZP_BASE here to match.
 * ========================================================================== */

#ifndef X816_H
#define X816_H

/* Must match X16_ZP in the library (core/const_zp.asm, default $22). */
#define X816_ZP_BASE 0x22u

#define X816_P0 (*(volatile unsigned char *)(X816_ZP_BASE + 0))
#define X816_P1 (*(volatile unsigned char *)(X816_ZP_BASE + 1))
#define X816_P2 (*(volatile unsigned char *)(X816_ZP_BASE + 2))
#define X816_P3 (*(volatile unsigned char *)(X816_ZP_BASE + 3))
#define X816_P4 (*(volatile unsigned char *)(X816_ZP_BASE + 4))
#define X816_P5 (*(volatile unsigned char *)(X816_ZP_BASE + 5))
#define X816_P6 (*(volatile unsigned char *)(X816_ZP_BASE + 6))

/* ---- util/math -- needs X16_USE_MATH in the library build --------------- */

/* Sine and cosine over a 256-step circle.
 * Signed forms return -127..127; the u forms return 1..255, i.e. 128 + the
 * signed value, which is what you want for a volume or a scale factor. */
signed char   __simple_call x816_sin8(unsigned char angle);
signed char   __simple_call x816_cos8(unsigned char angle);
unsigned char __simple_call x816_sin8u(unsigned char angle);
unsigned char __simple_call x816_cos8u(unsigned char angle);

/* xorshift PRNG. Seeding with zero is nudged to 1 by the library, since a
 * xorshift state of zero would produce nothing but zeroes. */
void          __simple_call x816_rnd_seed(unsigned int seed);
unsigned char __simple_call x816_rnd8(void);
unsigned int  __simple_call x816_rnd16(void);

/* Angle of the vector (dx, dy): 0 = east, 64 = down-screen. atan2(0,0) is 0. */
unsigned char __simple_call x816_atan2(signed char dx, signed char dy);

/* Interpolate between two unsigned bytes; exact at both endpoints.
 * The library takes its endpoints in direct page rather than in registers,
 * so this is written in C -- a three-argument trampoline would have to
 * juggle the stack for no benefit. */
unsigned char __simple_call x816_lerp8_t(unsigned char t);

static inline unsigned char x816_lerp8(unsigned char a, unsigned char b,
                                       unsigned char t)
{
    X816_P0 = a;
    X816_P1 = b;
    return x816_lerp8_t(t);
}

/* ---- util/zx0 -- needs X16_USE_ZX0 in the library build ----------------- */

/* RAM to RAM only, and not in place: the match copier reads the output back.
 * Returns one past the last output byte. Addresses are bank-$00 near
 * addresses, which is where the library can reach. */
unsigned int __simple_call x816_zx0_go(void);

static inline unsigned int x816_zx0(unsigned int src, unsigned int dst)
{
    X816_P0 = (unsigned char)src;
    X816_P1 = (unsigned char)(src >> 8);
    X816_P2 = (unsigned char)dst;
    X816_P3 = (unsigned char)(dst >> 8);
    return x816_zx0_go();
}

/* ---- video/palette -- needs X16_USE_PALETTE ----------------------------- */

/* colour is VERA's own $0RGB layout: (R << 8) | (G << 4) | B. */
void __simple_call x816_pal_set(unsigned char index, unsigned int colour);

/* ---- storage/bmx -- needs X16_USE_BMX (pulls FILEIO) --------------------- */

/* 0 on success, else the BMX_ERR_* code (1 IO, 2 FORMAT, 3 PACKED).
 * The name is (address, length), not NUL-terminated -- the library copies
 * and terminates it itself. Loads land palette + pixels; see the module
 * header for the geometry rules. bmx_save is not wrapped yet: it needs the
 * describe-the-image variables set first, and a settergroup is a decision
 * for when a C caller exists. */
unsigned char __simple_call x816_bmx_go(void);
unsigned char __simple_call x816_bmx_hires_go(void);
unsigned char __simple_call x816_bmx_lasterr(void);
unsigned int  __simple_call x816_bmx_width(void);
unsigned int  __simple_call x816_bmx_height(void);

static inline unsigned char x816_bmx_load(const char *name, unsigned char len,
                                          unsigned char vbank,
                                          unsigned int vaddr)
{
    X816_P0 = (unsigned char)(unsigned int)name;
    X816_P1 = (unsigned char)((unsigned int)name >> 8);
    X816_P2 = len;
    X816_P4 = vbank;
    X816_P5 = (unsigned char)vaddr;
    X816_P6 = (unsigned char)(vaddr >> 8);
    return x816_bmx_go();
}

static inline unsigned char x816_bmx_load_hires(const char *name,
                                                unsigned char len)
{
    X816_P0 = (unsigned char)(unsigned int)name;
    X816_P1 = (unsigned char)((unsigned int)name >> 8);
    X816_P2 = len;
    return x816_bmx_hires_go();
}

#endif /* X816_H */
