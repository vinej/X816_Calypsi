; ============================================================================
; x816_glue.s -- __simple_call entry stubs bridging C to the X16 library.
;
; See runtime/x816.h for why these exist. In short, each stub does three
; things the two sides disagree about:
;
;   jsl/rtl <-> jsr/rts    C calls with a 24-bit jsl and expects rtl; the
;                          library returns with rts. The stub is the boundary.
;   width                  Calypsi calls with 16-bit A and keeps the index
;                          registers 16 bits ALWAYS, and a function must
;                          return in that state. x16lib wants 8-bit A/X/Y.
;                          So: sep #0x30 on the way in, rep #0x30 on the way
;                          out -- and the rep is not optional, returning with
;                          8-bit index registers corrupts the caller.
;   bank                   Assembled into `code`, the same section as the
;                          library, so a jsl here leaves PBR on the library's
;                          bank and its internal 16-bit jsr resolve.
;
; PARAMETER LAYOUT, taken from what the compiler actually emits rather than
; from the prose. For `two(0x22, 0x33)` with __simple_call it generates:
;
;       lda ##51        ; second argument
;       pha             ; ...pushed first
;       lda ##34        ; first argument in C
;       jsl long:two
;       pla             ; caller cleans up
;
; So on entry the stack holds the 3-byte jsl return address at 1,s..3,s and
; the second argument's low byte at 4,s. Note LDX has no stack-relative
; addressing mode on the 65816, so a second argument destined for X has to
; come through A.
;
; Return values: A for char, C for int -- and the high byte of C is masked,
; because after sep/rep the accumulator's high half still holds whatever the
; hidden B register had.
; ============================================================================

              .rtmodel version, "1"
              .rtmodel core, "65816"
              .rtmodel codeModel, "large"
              .rtmodel dataModel, "small"

; THE LIBRARY IS INCLUDED HERE, not referenced with .extern.
;
; x16lib's labels are LOCAL to their translation unit -- the converter emits
; plain labels, not .public ones -- so `.extern atan2` does not find the
; library's atan2. It falls through to the C library instead and resolves to
; libm's DOUBLE-PRECISION atan2, which drags in the whole 64-bit float
; machinery. That is not a subtle mis-optimisation: f64_div's constants land
; in `cdata`, which is then referenced with 16-bit absolute addressing from
; bank $01, and the link dies with a pile of out-of-range errors that say
; nothing about the actual cause.
;
; Compiling the glue as part of the library's own translation unit fixes both
; halves at once: the stubs see the real symbols, and no C library name is
; ever asked for.
;
; Select modules the usual way, on the command line:
;     as65816 --core=65816 -I src -DX16_USE_MATH=1 x816_glue.s -o glue.o

#include "x16.s"

              .public x816_sin8, x816_cos8, x816_sin8u, x816_cos8u
              .public x816_rnd8, x816_rnd16, x816_rnd_seed
              .public x816_atan2, x816_lerp8_t

              .section code, noreorder

; ---- one char in, one char out ---------------------------------------------
x816_sin8:
              sep     #0x30
              jsr     .word0 (sin8)
              rep     #0x30
              and     ##0x00FF
              rtl

x816_cos8:
              sep     #0x30
              jsr     .word0 (cos8)
              rep     #0x30
              and     ##0x00FF
              rtl

x816_sin8u:
              sep     #0x30
              jsr     .word0 (sin8u)
              rep     #0x30
              and     ##0x00FF
              rtl

x816_cos8u:
              sep     #0x30
              jsr     .word0 (cos8u)
              rep     #0x30
              and     ##0x00FF
              rtl

; lerp8's endpoints come from direct page (X16_P0/X16_P1), which C writes
; itself -- see the inline wrapper in x816.h. Only t travels in a register.
x816_lerp8_t:
              sep     #0x30
              jsr     .word0 (lerp8)
              rep     #0x30
              and     ##0x00FF
              rtl

; ---- no arguments ----------------------------------------------------------
x816_rnd8:
              sep     #0x30
              jsr     .word0 (rnd8)
              rep     #0x30
              and     ##0x00FF
              rtl

; rnd16 returns A = low, X = high. C wants both halves in the 16-bit
; accumulator, so the high byte is folded in through the hidden B register:
; xba puts A into B, then the 8-bit lda takes the low half, and the following
; rep exposes the pair as one 16-bit value.
x816_rnd16:
              sep     #0x30
              jsr     .word0 (rnd16)
              pha                       ; low half aside
              txa                       ; high half
              xba                       ; ...into B
              pla                       ; low half back into A
              rep     #0x30
              rtl                       ; C = high:low, no masking wanted

; ---- int in, nothing out ---------------------------------------------------
; rnd_seed takes A = low, X = high, which is exactly how a 16-bit C parameter
; already sits: xba swaps the halves so the high byte can go to X.
x816_rnd_seed:
              sep     #0x30
              xba                       ; A = high half (xba ignores the M flag)
              tax                       ; X = high
              xba                       ; A = low half
              jsr     .word0 (rnd_seed)
              rep     #0x30
              rtl

; ---- two chars in, one char out --------------------------------------------
; atan2 wants dx in A and dy in X. dx arrives in A; dy is the second argument,
; so it is on the stack. Y is used as the holding place rather than the stack,
; because a push would shift the offset the second argument sits at.
x816_atan2:
              sep     #0x30
              tay                       ; dx aside
              lda     4,s               ; dy -- 3-byte jsl return address first
              tax
              tya                       ; dx back
              jsr     .word0 (atan2)
              rep     #0x30
              and     ##0x00FF
              rtl

#include "x16_code.s"
