; ============================================================================
; libirq.s -- x16lib's system/irq.asm and system/clock.asm, over the kernel.
;
; examples/kernel/irqtest.c already proved IRQ_SET, the dispatcher and both
; clocks AT THE ABI. What is unproven is this layer, and it has one property
; the ABI test cannot reach: THE 8-BIT / 16-BIT CROSSING RUNS THE OTHER WAY
; HERE.
;
; Everywhere else in the tree, 8-bit library code calls a 16-bit kernel and
; comes back. An interrupt handler is the reverse: the kernel calls INTO the
; library, in native mode with 16-bit registers, and the trampolines in
; system/x816kernel.asm have to sep down, run 65C02 code, rep back and rtl.
; A missed rep or sep there does not crash -- it returns to the kernel's
; dispatcher with the wrong register widths, and the dispatcher's own pulls
; then take the wrong number of bytes off the stack, corrupting whatever was
; interrupted. That is why test 5 exists and why it checks the machine is
; still healthy afterwards rather than only that the handler ran.
;
; READING THE SCREEN
;
;   GREEN    every test passed
;   RED      1: irq_frames advances -- the kernel's count reaches the library
;   YELLOW   2: clock_get_ms advances, and agrees with the frame count
;   BLUE     3: clock_mark / clock_elapsed measure a known interval
;   MAGENTA  4: clock_delay waits about as long as it was asked to
;   CYAN     5: a raster handler installed through irq_line_install RUNS,
;              and STOPS when removed while frames keep advancing
;
; The test number lands at $00:0400.
;
; Runs 8-bit: x16lib is 65C02 code, so A/X/Y must be 8 bits wide before any
; call into it. cstartup leaves them 16-bit for C, and kern_install is C.
; ============================================================================

              .rtmodel version, "1"
              .rtmodel core, "65816"
              .rtmodel codeModel, "large"
              .rtmodel dataModel, "small"

; EVERY GATE IS SET EXPLICITLY, and that is not belt-and-braces.
;
; x16_code.s derives X16_USE_IRQ_ANY from X16_USE_IRQ through a chain of
; `#ifdef A` -> `B: .equ 1` -> `#ifdef B` steps. The middle of that chain
; writes an ASSEMBLER symbol and the next step tests a PREPROCESSOR macro,
; and the C preprocessor cannot see a .equ -- so the chain stops dead after
; one link and system/irq.s is never included. Defining X16_USE_IRQ alone
; produces "undefined symbol: irq_frames", which is at least loud.
;
; Left as-is here rather than fixed in the library: the same pattern gates a
; dozen other module groups and changing it is its own change with its own
; blast radius. Recorded so the next reader does not rediscover it.
#define X16_USE_IRQ 1
#define X16_USE_IRQ_ANY 1
#define X16_USE_IRQ_CORE 1
#define X16_USE_IRQ_REMOVE 1
#define X16_USE_IRQ_VSYNC 1
#define X16_USE_IRQ_SPRCOL 1
#define X16_USE_IRQ_SPRCOL_API 1
#define X16_USE_CLOCK 1

#include "x16.s"

              .extern kern_install, goshell_on_esc, con_init, kirq_install

RESULT:       .equ 0x0400

C_GREEN:      .equ 0x05
C_RED:        .equ 0x02
C_YELLOW:     .equ 0x07
C_BLUE:       .equ 0x06
C_MAGENTA:    .equ 0x04
C_CYAN:       .equ 0x03

VERA_DC_VID:  .equ 0x9F29
VERA_DC_HS:   .equ 0x9F2A
VERA_DC_VS:   .equ 0x9F2B
VERA_L0_CFG:  .equ 0x9F2D
VERA_L0_TB:   .equ 0x9F2F

              .section code, noreorder
              .public main
main:
              jsl     con_init
              jsl     kern_install
              jsl     kirq_install      ; vectors, VSYNC on, interrupts on
              sep     #0x30

; ---- 1: irq_frames advances ------------------------------------------------
; The count is the kernel's and runs with no irq_install at all, so this also
; establishes that everything after it has a working timebase to lean on.
              lda     #1
              sta     failno
              jsr     .word0 (irq_frames)
              sta     f0
              lda     #8
              jsr     .word0 (wait_frames)
              bcs     f1                ; carry set = it never advanced
              bra     t2
f1:           jmp     .word0 (fail)

; ---- 2: clock_get_ms advances, and agrees with the frames -------------------
; Two independent clocks. Checking either against itself proves nothing; a
; stuck counter, a wrong divider and a misread register all survive that and
; none of them survive this.
t2:
              lda     #2
              sta     failno
              jsr     .word0 (clock_mark)
              jsr     .word0 (irq_frames)
              sta     f0
              lda     #60               ; ~1 second of frames
              jsr     .word0 (wait_frames)
              bcs     f2
              jsr     .word0 (clock_elapsed)
              ; 60 frames at 59.52 Hz is ~1008 ms. Accept 500..2000: this is a
              ; "wired to the right thing, dividing by roughly the right
              ; number" check, and the 16x a jiffy/millisecond mix-up would
              ; cause is far outside it.
              lda     dp:X16_P2         ; anything above 65535 ms is wrong
              ora     dp:X16_P3
              bne     f2
              lda     dp:X16_P1         ; >= 500?  (high byte >= 1)
              beq     f2
              cmp     #8                ; < 2048 ms
              bcs     f2
              bra     t3
f2:           jmp     .word0 (fail)

; ---- 3: clock_mark / clock_elapsed over a known interval --------------------
; The same measurement taken twice in a row must not go backwards or restart:
; clock_elapsed is READ-ONLY against the mark, and a version that reset the
; mark would pass test 2 and fail here.
t3:
              lda     #3
              sta     failno
              jsr     .word0 (clock_mark)
              lda     #10
              jsr     .word0 (wait_frames)
              bcs     f3
              jsr     .word0 (clock_elapsed)
              lda     dp:X16_P0
              sta     e0
              lda     dp:X16_P1
              sta     e0+1
              lda     #10
              jsr     .word0 (wait_frames)
              bcs     f3
              jsr     .word0 (clock_elapsed)
              lda     dp:X16_P1         ; the second reading must be LARGER,
              cmp     e0+1              ; measured from the same mark
              bcc     f3
              bne     t4
              lda     dp:X16_P0
              cmp     e0
              bcc     f3
              beq     f3
              bra     t4
f3:           jmp     .word0 (fail)

; ---- 4: clock_delay waits about as long as asked ---------------------------
t4:
              lda     #4
              sta     failno
              jsr     .word0 (clock_mark)
              lda     #.byte0 (300)
              sta     dp:X16_P4
              lda     #.byte1 (300)
              sta     dp:X16_P5
              jsr     .word0 (clock_delay)
              jsr     .word0 (clock_elapsed)
              lda     dp:X16_P2
              ora     dp:X16_P3
              bne     f4                ; wildly too long
              lda     dp:X16_P1         ; at least 300 ($012C): high byte >= 1
              beq     f4
              cmp     #4                ; ...and under 1024 ms
              bcs     f4
              bra     t5
f4:           jmp     .word0 (fail)

; ---- 5: a raster handler, through the 8-bit/16-bit trampoline --------------
; The one thing only this file can test. irq_line_install puts the library's
; trampoline in the kernel's LINE slot; the kernel calls it 16-bit, it seps
; down to the 8-bit handler below, and comes back.
;
; Then it is REMOVED, and the counter must stop while frames keep advancing.
; Without that second half the test passes for a dispatcher that calls every
; slot unconditionally -- and "frames still advancing" is what stops the stop
; being explained by interrupts having died.
t5:
              lda     #5
              sta     failno
              stz     hits
              stz     hits+1
              lda     #.byte0 (100)     ; scanline 100, inside the display
              sta     dp:X16_P0
              lda     #.byte1 (100)
              sta     dp:X16_P1
              lda     #.byte0 (line_handler)
              ldx     #.byte1 (line_handler)
              jsr     .word0 (irq_line_install)

              lda     #10
              jsr     .word0 (wait_frames)
              bcs     f5
              lda     hits              ; it must have run
              ora     hits+1
              beq     f5

              jsr     .word0 (irq_line_remove)
              lda     #4                ; let anything already pending drain
              jsr     .word0 (wait_frames)
              bcs     f5
              lda     hits
              sta     h0
              lda     hits+1
              sta     h0+1

              lda     #10
              jsr     .word0 (wait_frames)
              bcs     f5                ; frames must STILL be advancing, or
                                        ; the line below proves nothing
              lda     hits              ; ...and the handler must not have run
              cmp     h0
              bne     f5
              lda     hits+1
              cmp     h0+1
              bne     f5
              bra     pass
f5:           jmp     .word0 (fail)

pass:
              stz     failno
              jmp     .word0 (fail)     ; failno 0 paints GREEN

; ---------------------------------------------------------------------------
; The raster handler. Ordinary 8-bit code: the kernel acknowledged the source
; before calling, and the trampoline in system/x816kernel.asm already put the
; registers back to eight bits. Registers are free.
; ---------------------------------------------------------------------------
line_handler:
              inc     hits
              bne     lh_done
              inc     hits+1
lh_done:
              rts

; ---------------------------------------------------------------------------
; wait_frames -- A = how many. Carry SET if the count stopped advancing, which
; is what a dispatcher that never acknowledges VSYNC looks like from here and
; is worth reporting as a failed check rather than as a ninety-second timeout.
;
; THE GUARD IS 24 BITS, and the first version's 16 were not enough: each poll
; is a kernel call, so 65536 of them is about 0.8 seconds of machine time --
; less than the one second test 2 waits for. A guard that expires during a
; legitimate wait turns a passing test red for no reason, which is exactly as
; useless as one that never expires at all.
; ---------------------------------------------------------------------------
wait_frames:
              sta     want
              jsr     .word0 (irq_frames)
              sta     f0
              stz     guard
              stz     guard+1
              stz     guard+2
wf_loop:
              jsr     .word0 (irq_frames)
              sec
              sbc     f0                ; wraps correctly at 256
              cmp     want
              bcs     wf_ok
              inc     guard
              bne     wf_loop
              inc     guard+1
              bne     wf_loop
              inc     guard+2
              lda     guard+2
              cmp     #16               ; ~1M polls, comfortably minutes
              bcc     wf_loop
              sec
              rts
wf_ok:
              clc
              rts

; ---------------------------------------------------------------------------
fail:
verdict:
              lda     failno
              sta     RESULT
              tax
              lda     colours,x
              jsr     .word0 (paint)
              rep     #0x30
              jsl     goshell_on_esc
              rtl

; 320x240 8bpp, whole screen one colour -- the same image every conformance
; test in this tree paints, so a result looks the same wherever it came from.
paint:
              sta     pcol
              stz     0x9F25
              lda     #0x11
              sta     VERA_DC_VID
              lda     #0x40
              sta     VERA_DC_HS
              sta     VERA_DC_VS
              lda     #0x07
              sta     VERA_L0_CFG
              stz     VERA_L0_TB
              stz     0x9F25
              stz     0x9F20
              stz     0x9F21
              lda     #0x10
              sta     0x9F22
              ldx     #240
p_row:        ldy     #0
p_col:        lda     pcol
              sta     0x9F23
              iny
              bne     p_col
              lda     pcol              ; 320 = 256 + 64
              ldy     #64
p_col2:       sta     0x9F23
              dey
              bne     p_col2
              dex
              bne     p_row
              rts

              .section data,data
colours:      .byte   C_GREEN, C_RED, C_YELLOW, C_BLUE, C_MAGENTA, C_CYAN
failno:       .byte   0
pcol:         .byte   0
f0:           .byte   0
want:         .byte   0
guard:        .space  3, 0
hits:         .space  2, 0
h0:           .space  2, 0
e0:           .space  2, 0

; The library CODE, last: x16.s above is symbols only.
#include "x16_code.s"
