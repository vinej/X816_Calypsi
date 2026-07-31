; ============================================================================
; demo.s -- a program built against the converted X16 assembly library.
;
; Code runs from SDRAM at $01:0000 where the loader drops it; the library's
; variables and tables live in bank $00, reached with 16-bit absolute
; addressing through DBR = $00 -- which is also what makes the I/O page work
; unchanged. runtime/x816-lib.scm places both. Nothing here has to know.
;
; Entry is main(), not __program_start: Calypsi's cstartup runs first and sets
; native mode, the stack, the direct page and DBR = $00, and copies the
; library's initialised tables from the image into bank $00. Doing that by
; hand would mean reimplementing the data-init walk for no gain.
;
; Pick the modules you want with X16_USE_*; only those are assembled in.
; ============================================================================

              .rtmodel version, "1"
              .rtmodel core, "65816"
              .rtmodel codeModel, "large"
              .rtmodel dataModel, "small"

#define X16_USE_MATH 1

; Constants and macros; emits no code. NOTE: no ";" comment may follow a
; preprocessor directive -- the C preprocessor does not know assembler
; comments and warns about "extra tokens".
#include "x16.s"

              .section code, noreorder
              .public main
main:
              sep     #0x20             ; 8-bit A -- x16lib is 8-bit code
              rep     #0x10             ; 16-bit index registers

; Calypsi needs no .a8/.i16 tracking: the immediate WIDTH is written at the
; operand -- "#" is an 8-bit immediate, "##" a 16-bit one. That makes the ca65
; failure mode impossible, where the assembler tracks M/X from .a8/.a16
; DIRECTIVES rather than from rep/sep, and silently emits an 8-bit immediate
; for code running with a 16-bit accumulator.
              ldx     ##0
loop:
              txa
; Two rules apply to hand-written code as well; the converter applies them to
; the library automatically.
;
;   1. A call into the library needs .word0. The code is in bank $01 and jsr
;      is 16-bit, taking its bank from PBR -- the linker has to be told to
;      emit the low 16 bits, or it rejects the 24-bit address outright.
;   2. Your own data belongs in a data section, so it lands in bank $00 where
;      a 16-bit absolute reference through DBR = $00 can reach it.
              jsr     .word0 sin8       ; util/math: A = sin(A), signed byte
              sta     table,x
              inx
              cpx     ##256
              bne     loop

halt:         bra     halt

              .section data,data        ; "data" is a reserved TYPE, hence data,data
table:        .space  256, 0

; The library itself.
#include "x16_code.s"
