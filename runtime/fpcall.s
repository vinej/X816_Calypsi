; ============================================================================
; fpcall.s -- call x16lib's floating point from C.
;
; The interface, and why it is shaped the way it is, is in runtime/fp.h. This
; file is the other half: one thunk per entry, each narrowing the registers to
; eight bits, calling, and widening again.
;
; WHY EVERY ENTRY NEEDS A THUNK
; -----------------------------
; Two conventions that do not line up, exactly as kerntab.s has:
;
;   x16lib    65C02 code. A/X/Y EIGHT bits. Pointer arguments arrive as
;             A = low byte, Y = high byte. Reached with `jsr .word0 (name)`,
;             a 16-bit call inside bank $01, so entries end in RTS.
;   Calypsi C A/X/Y SIXTEEN bits, first argument in A, return value in A,
;             and a `jsl` from anywhere, so this file's entries end in RTL.
;
; Getting the width wrong does not fail loudly -- an 8-bit routine entered with
; a 16-bit accumulator reads one byte too many on its first `lda #` and walks
; off into whatever follows. That is why programs/kernel/libfs.s opens by
; saying "Runs 8-bit" and does it once for the whole program: an assembly
; caller can. A C caller cannot, so it happens here instead, per call.
;
; THIS FILE SOURCES THE LIBRARY. `#include "x16.s"` emits x16lib's code, and
; the header of every module says it must be sourced EXACTLY ONCE per image.
; So exactly one object in a link may include it, and this is that object.
; Anything else wanting a different gate adds it to the list below rather than
; including x16.s a second time.
;
; PBR MUST BE $01 when the `jsr` runs, because the library is bank-$01 code
; called 16-bit. A `jsl` into this file sets PBR to this file's bank, and this
; file is in `code`, which x816-kalk.scm and x816-lib.scm both place in bank
; $01 with the library. So the requirement is satisfied by placement rather
; than by anything at run time -- which is worth stating, because a linker map
; that moved `code` elsewhere would break every call here silently.
;
; D AND DBR are already what the library needs and are not touched: D is
; $0000, so the library's fixed direct-page equates ($22-$31 for X16_ZP,
; $32-$61 for the float block) resolve where it expects; DBR is $00, which is
; the bank its (dp),y operand fetches reach. Both are the small data model's
; own settings, which is the deeper reason this bridge is only possible in
; that model.
; ============================================================================

              .rtmodel version, "1"
              .rtmodel core, "65816"
              .rtmodel codeModel, "large"
              .rtmodel dataModel, "small"

; The float module and nothing else. Every other gate stays off: x16_code.s
; emits what is asked for, and a spreadsheet has no use for the sprite engine.
;
; TWO includes, and the order is not a style choice. x16.s is constants and
; macros and emits NO CODE -- it has to come before anything that uses them.
; x16_code.s is where the routines actually land, and it goes at the BOTTOM of
; the file, which is the arrangement every assembly caller in the tree uses
; (programs/kernel/libfs.s, libmem.s). Including only the first gives a clean
; assembly and an undefined f_add at link time.
#define X16_USE_FLOAT 1
#include "x16.s"

; ----------------------------------------------------------------------------
; A two-byte landing pad for the register-width crossing.
;
; It exists because the split happens across a `sep`: the argument arrives as a
; 16-bit accumulator and has to leave as A = low, Y = high, and there is no
; instruction that halves the accumulator into two registers. Writing it wide
; and reading it back narrow is the shortest way across, and it costs a store
; and two loads.
;
; `zdata` puts it in bank $00 where DBR already points. Not the direct page:
; that is 98 bytes spoken for between Calypsi's pseudo-registers and the
; library's own block, and two bytes of scratch do not earn a place in it.
; ----------------------------------------------------------------------------
              .section zdata,bss
fp_arg:       .space  2

              .section code

; ----------------------------------------------------------------------------
; The four shapes. Every entry in fp.h is one of them.
; ----------------------------------------------------------------------------

; FPPTR -- one pointer argument, no result. The bulk of the interface.
FPPTR         .macro  fn
              sta     .word0 (fp_arg)         ; 16-bit A: the whole pointer
              sep     #0x30                   ; 8-bit A, X, Y -- x16lib's width
              lda     .word0 (fp_arg)         ; low
              ldy     .word0 (fp_arg+1)       ; high
              jsr     .word0 (\fn)
              rep     #0x30
              rtl
              .endm

; FPNIL -- no argument, no result. FAC is both operand and answer.
FPNIL         .macro  fn
              sep     #0x30
              jsr     .word0 (\fn)
              rep     #0x30
              rtl
              .endm

; FPGET -- no argument, a 16-bit result the library returns as A = low,
; X = high. Reassembled through the same landing pad, in the other direction.
FPGET         .macro  fn
              sep     #0x30
              jsr     .word0 (\fn)
              sta     .word0 (fp_arg)
              stx     .word0 (fp_arg+1)
              rep     #0x30
              lda     .word0 (fp_arg)
              rtl
              .endm

; ---- moving values in and out ---------------------------------------------
              .public fp_load, fp_store, fp_zero
fp_load:      FPPTR   f_load
fp_store:     FPPTR   f_store
fp_zero:      FPNIL   f_zero

; f_from_s16 wants A = low and X = HIGH -- not Y, which is what every pointer
; entry wants. Spelled out rather than macro'd for that one difference.
              .public fp_from_s16, fp_from_u8, fp_to_s16
fp_from_s16:
              sta     .word0 (fp_arg)
              sep     #0x30
              lda     .word0 (fp_arg)
              ldx     .word0 (fp_arg+1)
              jsr     .word0 (f_from_s16)
              rep     #0x30
              rtl

fp_from_u8:
              sep     #0x30                   ; the low byte is the argument
              jsr     .word0 (f_from_u8)
              rep     #0x30
              rtl

fp_to_s16:    FPGET   f_to_s16

; ---- strings --------------------------------------------------------------
;
; INPUT AND OUTPUT DO NOT USE THE SAME BANK, and this is the one asymmetry in
; the whole interface.
;
; f_to_str writes into a bank $00 buffer and hands back a bank $00 address, so
; C can read it directly. f_from_str READS FROM THE PROGRAM BANK: its fetch is
; `phb / phk / plb` then `lda (fpstr),y`, which points DBR at PBR. util/float.s
; explains why -- in x16lib's world the thing you parse is a literal, and a
; literal is initialised data the loader put in the image at PBR. Its header
; says what to do about anything else in as many words: "To parse one assembled
; at run time in a bank $00 buffer, copy it into the image first."
;
; A spreadsheet only ever parses strings assembled at run time. So the buffer
; below lives in `code` -- which is to say, in the image, in bank $01, which is
; BRAM and writable -- and fp.c copies into it before calling. Getting this
; wrong does not fail: float.s records that it "parsed garbage without ever
; failing -- it just answered the wrong number", and that is exactly what this
; bridge did on its first run, returning zero for "2.5".
              .public fp_parse

; The staging buffer, IN THE IMAGE. 48 bytes is well past any decimal a cell
; can hold: nine significant digits, a sign, a point and an exponent is under
; twenty, and f_from_str stops at the first character it cannot use anyway.
fp_strin:     .space  48, 0

; Its 24-bit address, published where C can read it. C cannot form a pointer
; into bank $01 by itself in this data model -- a near pointer is 16 bits and
; the compiler builds even a __far one from a 16-bit immediate -- so the
; address has to be handed over as data. fp.c casts this to `uint8_t __far *`,
; which is the same move shell.c's far_ptr() makes.
              .section data,data
              .public fp_strin_addr
fp_strin_addr:
              .long   fp_strin

              .section code

; No argument: the buffer is fixed and fp.c has already filled it. Carry is
; the library's failure report and C has no way to see it, so it becomes the
; bool fp.h promises -- 1 parsed, 0 did not. That is the test a cell uses to
; tell a number from a label, so it has to be exact: carry set means not a
; single digit was consumed.
fp_parse:
              sep     #0x30
              lda     #.byte0 (fp_strin)
              ldy     #.byte1 (fp_strin)
              jsr     .word0 (f_from_str)
              rep     #0x30
              lda     ##0
              bcs     fp_parse_out            ; carry set: nothing parsed
              lda     ##1
fp_parse_out:
              rtl

              .public fp_to_str, fp_to_str_trim

fp_to_str:      FPGET f_to_str
fp_to_str_trim: FPGET f_to_str_trim

; ---- arithmetic ------------------------------------------------------------
              .public fp_add, fp_sub, fp_mul, fp_div, fp_rsub, fp_rdiv
              .public fp_pow, fp_rpow
fp_add:       FPPTR   f_add
fp_sub:       FPPTR   f_sub
fp_mul:       FPPTR   f_mul
fp_div:       FPPTR   f_div
fp_rsub:      FPPTR   f_rsub
fp_rdiv:      FPPTR   f_rdiv
fp_pow:       FPPTR   f_pow
fp_rpow:      FPPTR   f_rpow

; ---- tests -----------------------------------------------------------------
              .public fp_sgn, fp_cmp
; Both sign entries return an 8-bit $01/$00/$FF and C is promised an int8_t,
; so the top byte has to carry the sign rather than whatever the accumulator's
; high half happened to hold. Written out rather than macro'd: a macro that
; defines a label can only be expanded once before the labels collide.
fp_sgn:
              sep     #0x30
              jsr     .word0 (f_sgn)
              sta     .word0 (fp_arg)
              ldx     #0
              bpl     fp_sgn_pos
              ldx     #0xFF                   ; sign-extend $FF to $FFFF
fp_sgn_pos:
              stx     .word0 (fp_arg+1)
              rep     #0x30
              lda     .word0 (fp_arg)
              rtl

; f_cmp takes a pointer AND returns a sign, so it is neither macro. It also
; leaves FAC alone, which is the property that lets a sort call it in a loop.
fp_cmp:
              sta     .word0 (fp_arg)
              sep     #0x30
              lda     .word0 (fp_arg)
              ldy     .word0 (fp_arg+1)
              jsr     .word0 (f_cmp)
              sta     .word0 (fp_arg)
              ldx     #0
              bpl     fp_cmp_pos
              ldx     #0xFF
fp_cmp_pos:
              stx     .word0 (fp_arg+1)
              rep     #0x30
              lda     .word0 (fp_arg)
              rtl

; ---- unary -----------------------------------------------------------------
              .public fp_abs, fp_neg, fp_int, fp_sqrt
              .public fp_sin, fp_cos, fp_tan, fp_atan, fp_ln, fp_exp
fp_abs:       FPNIL   f_abs
fp_neg:       FPNIL   f_neg
fp_int:       FPNIL   f_int
fp_sqrt:      FPNIL   f_sqrt
fp_sin:       FPNIL   f_sin
fp_cos:       FPNIL   f_cos
fp_tan:       FPNIL   f_tan
fp_atan:      FPNIL   f_atan
fp_ln:        FPNIL   f_ln
fp_exp:       FPNIL   f_exp

; ----------------------------------------------------------------------------
; The library code itself, LAST. x16.s at the top of this file is symbols and
; macros only; this is where f_add and the other 2,000 lines actually land.
; One translation unit in the image may do this -- see the header.
; ----------------------------------------------------------------------------
#include "x16_code.s"
