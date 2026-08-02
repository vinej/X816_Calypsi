; ============================================================================
; membench.s -- how fast can this machine move memory?
;
; The question this answers: x16lib is 65C02 code with eight-bit registers,
; and mem_copy/mem_fill walk 24-bit pointers a byte at a time with a `jsr` per
; increment. Is it worth rewriting them 16-bit -- or, better, on the 65816's
; own block-move instruction? Three implementations of each, measured.
;
;   LIBCOPY / LIBFILL   x16lib's own, exactly as it ships
;   W16COPY / W16FILL   a 16-bit word loop through [dp],y
;   MVNCOPY / MVNFILL   MVN, the 65816 block move
;
; THE SIX RESULTS ARE PRINTED UNLABELLED, IN THIS ORDER:
;
;     LIBCOPY  W16COPY  MVNCOPY  LIBFILL  W16FILL  MVNFILL
;
; each four hex digits of milliseconds for 4 x 32 KB. Two attempts at printing
; the names were abandoned -- one walking a 24-bit pointer, one indexing a
; table with `lda long:names,x` -- and both flooded the screen instead of
; terminating. The cause was not found. It is a cosmetic feature on a
; benchmark, the order is deterministic and stated here and in
; run-membench.sh, so the labels are not worth more build cycles. If someone
; wants them, start by checking whether `say`'s loop ever sees its NUL.
;
; MEASURED WITH THE KERNEL'S MILLISECOND CLOCK (TIME_GET, $9F90). That clock
; is why this test can exist at all: before it there was no timebase on this
; machine that a program could read, so "is it worth it" had never been
; anything but an argument.
;
; WHAT THE NUMBERS DO AND DO NOT MEAN
;
; In the emulator this measures INSTRUCTION cost -- the 65816's published
; cycle counts, which is exactly the quantity the 8-bit/16-bit/MVN question
; turns on. It does NOT measure SDRAM wait states, because the emulator's
; memory is uniform. On hardware the absolute numbers will be larger; the
; ORDERING should not change, because all three variants touch the same bytes
; the same number of times and differ only in how many instructions they spend
; doing it. IRQTEST.BIN and this both ship on the card for that reason.
;
; MVN'S TWO CATCHES, both of which a real mem_copy has to handle:
;   * X and Y are 16-bit offsets that WRAP WITHIN THEIR BANK. A move crossing
;     a bank boundary must be split. This benchmark stays inside one bank.
;   * MVN sets DBR to the destination bank and leaves it there. x16lib needs
;     DBR = $00 for every one of its variables, so it must be put back --
;     forgetting is the kind of silent wrongness this tree keeps a file of.
; ============================================================================

              .rtmodel version, "1"
              .rtmodel core, "65816"
              .rtmodel codeModel, "large"
              .rtmodel dataModel, "small"

#define X16_USE_MEM 1

#include "x16.s"

              .extern kern_install, con_init, goshell_on_esc

; The two scratch blocks. Fixed flat addresses in SDRAM rather than MEM_ALLOC
; results, so the measurement is over the same addresses every run and cannot
; drift with the allocator. Nothing else is running.
SRC:          .equ 0x300000
DST:          .equ 0x310000
LEN:          .equ 0x8000             ; 32 KB, comfortably inside one bank
ITER:         .equ 4                  ; enough that even MVN takes ~100 ms

; K_CON_PUTC and K_TIME_GET come from the LIBRARY's core/const_kernel.s and
; are already table ADDRESSES, so they are jsl'd directly. The generated
; x816_contract.inc defines the same names as call NUMBERS and must not be
; included here as well -- see irqhelp.s for what that costs.
              .section near,bss
t0:           .space 4                ; TIME_GET at the start of a run
dt:           .space 2                ; ...and the elapsed milliseconds
len16:        .space 2
; The variant under test, for `jmp (vec)`. NOT X16_T0/T1, which is where it
; was first put: mem_fill's very first instruction is `sta X16_T0` -- the
; library's T registers are ITS scratch, and the second iteration of a timed
; run jumped through whatever mem_fill had left there. Anything a library
; routine can see is the wrong place to keep a pointer across a call to it.
vec:          .space 2
iter:         .space 2
fillw:        .space 2

              .section code
              .public main

; ---------------------------------------------------------------------------
; Timing. Both halves are 16-bit; the clock is 32 bits but a run is never more
; than a minute, so only the low half is kept.
; ---------------------------------------------------------------------------
t_start:
              rep     #0x30
              jsl     K_TIME_GET
              sta     long:t0
              txa
              sta     long:t0 + 2
              sep     #0x30
              rts

t_stop:
              rep     #0x30
              jsl     K_TIME_GET
              sec
              sbc     long:t0
              sta     long:dt
              sep     #0x30
              rts

; ---------------------------------------------------------------------------
; Printing: four hex digits and a newline. Hex, not decimal, because a 32-bit
; decimal divide in 8-bit assembly is more code than everything else here and
; the run script can convert.
; ---------------------------------------------------------------------------
putc:                                 ; A = character, 8-bit in
              and     #0xFF
              sta     long:fillw      ; borrow: staging for the 16-bit call
              lda     #0              ; (no `stz long:` -- absolute long is an
              sta     long:fillw + 1  ;  accumulator-only mode on this core)
              rep     #0x30
              lda     long:fillw
              jsl     K_CON_PUTC
              sep     #0x30
              rts

putnib:                               ; A = 0..15
              and     #0x0F
              cmp     #10
              bcc     putnib_dec
              adc     #('A' - 10 - 1) ; carry is set, hence the extra -1
              bra     putc
putnib_dec:
              adc     #'0'
              bra     putc

puthex16:                             ; dt -> four digits
              lda     long:dt + 1
              lsr     a
              lsr     a
              lsr     a
              lsr     a
              jsr     .word0 (putnib)
              lda     long:dt + 1
              jsr     .word0 (putnib)
              lda     long:dt
              lsr     a
              lsr     a
              lsr     a
              lsr     a
              jsr     .word0 (putnib)
              lda     long:dt
              jsr     .word0 (putnib)
              ; CON_PUTC intercepts $0D as RETURN (column 0) and $0A as
              ; NEWLINE. Return alone made every result overwrite the last.
              lda     #13
              jsr     .word0 (putc)
              lda     #10
              jsr     .word0 (putc)
              rts

; ---------------------------------------------------------------------------
; The three copies. Each moves LEN bytes from SRC to DST, ITER times.
; ---------------------------------------------------------------------------

; x16lib's own, through the published interface.
libcopy:
              lda     #.byte0 (SRC)
              sta     X16_P0
              lda     #.byte1 (SRC)
              sta     X16_P1
              lda     #.byte2 (SRC)
              sta     X16_P2
              lda     #.byte0 (DST)
              sta     X16_P3
              lda     #.byte1 (DST)
              sta     X16_P4
              lda     #.byte2 (DST)
              sta     X16_P5
              lda     #.byte0 (LEN)
              sta     X16_P6
              lda     #.byte1 (LEN)
              sta     X16_P7
              jsr     .word0 (mem_copy)
              rts

; A 16-bit word loop. [dp],y is the only long-indirect indexed mode there is,
; so the pointers live in the library's own parameter block and Y steps by two.
w16copy:
              lda     #.byte0 (SRC)
              sta     X16_P0
              lda     #.byte1 (SRC)
              sta     X16_P1
              lda     #.byte2 (SRC)
              sta     X16_P2
              lda     #.byte0 (DST)
              sta     X16_P3
              lda     #.byte1 (DST)
              sta     X16_P4
              lda     #.byte2 (DST)
              sta     X16_P5
              rep     #0x30
              ldy     ##0
w16copy_loop:
              lda     [X16_P0],y
              sta     [X16_P3],y
              iny
              iny
              cpy     len16
              bne     w16copy_loop
              sep     #0x30
              rts

; MVN. The bank operands are immediate bytes inside the instruction, so for
; runtime banks they are patched in place -- the standard technique, and the
; reason the opcode is emitted as raw bytes: assemblers disagree about whether
; `mvn a,b` means source-first or destination-first, and the encoding does not.
; $54, THEN THE DESTINATION BANK, THEN THE SOURCE BANK.
mvncopy:
              lda     #.byte2 (DST)
              sta     long:mvncopy_op + 1
              lda     #.byte2 (SRC)
              sta     long:mvncopy_op + 2
              rep     #0x30
              lda     ##(LEN - 1)             ; MVN moves C+1 bytes
              ldx     ##.word0 (SRC)
              ldy     ##.word0 (DST)
mvncopy_op:
              .byte   0x54, 0, 0
              sep     #0x30
              lda     #0                      ; MVN left DBR = the destination
              pha                             ; bank; x16lib needs $00 back
              plb
              rts

; ---------------------------------------------------------------------------
; The three fills. Each writes LEN bytes of $A5 at DST, ITER times.
; ---------------------------------------------------------------------------
libfill:
              lda     #.byte0 (DST)
              sta     X16_P0
              lda     #.byte1 (DST)
              sta     X16_P1
              lda     #.byte2 (DST)
              sta     X16_P2
              lda     #.byte0 (LEN)
              sta     X16_P3
              lda     #.byte1 (LEN)
              sta     X16_P4
              lda     #0xA5
              jsr     .word0 (mem_fill)
              rts

w16fill:
              lda     #.byte0 (DST)
              sta     X16_P0
              lda     #.byte1 (DST)
              sta     X16_P1
              lda     #.byte2 (DST)
              sta     X16_P2
              rep     #0x30
              ldy     ##0
              lda     ##0xA5A5
w16fill_loop:
              sta     [X16_P0],y
              iny
              iny
              cpy     len16
              bne     w16fill_loop
              sep     #0x30
              rts

; MVN as a fill: write one byte, then move the block onto itself shifted by
; one. Each byte read has just been written, so the value propagates. C is
; LEN-2 because MVN moves C+1 bytes and one byte was placed by hand.
mvnfill:
              lda     #.byte0 (DST)
              sta     X16_P0
              lda     #.byte1 (DST)
              sta     X16_P1
              lda     #.byte2 (DST)
              sta     X16_P2
              lda     #0xA5
              sta     [X16_P0]
              lda     #.byte2 (DST)
              sta     long:mvnfill_op + 1
              sta     long:mvnfill_op + 2     ; source and destination banks
              rep     #0x30                   ; are the same block
              lda     ##(LEN - 2)
              ldx     ##.word0 (DST)
              ldy     ##.word0 (DST) + 1
mvnfill_op:
              .byte   0x54, 0, 0
              sep     #0x30
              lda     #0
              pha
              plb
              rts

; ---------------------------------------------------------------------------
; run -- time ITER passes of the routine whose address is in `vec`, and
; print the result. Entered 8-bit.
; ---------------------------------------------------------------------------
run:
              lda     #ITER
              sta     long:iter
              lda     #0
              sta     long:iter + 1
              jsr     .word0 (t_start)
run_loop:
              jsr     .word0 (run_call)
              lda     long:iter
              dec     a
              sta     long:iter
              bne     run_loop
              jsr     .word0 (t_stop)
              jsr     .word0 (puthex16)
              rts
run_call:
              jmp     (vec)                   ; the variant under test

main:
              jsl     con_init
              jsl     kern_install

              rep     #0x30
              lda     ##LEN
              sta     long:len16
              sep     #0x30

              lda     #.byte0 (libcopy)
              sta     long:vec
              lda     #.byte1 (libcopy)
              sta     long:vec + 1
              jsr     .word0 (run)

              lda     #.byte0 (w16copy)
              sta     long:vec
              lda     #.byte1 (w16copy)
              sta     long:vec + 1
              jsr     .word0 (run)

              lda     #.byte0 (mvncopy)
              sta     long:vec
              lda     #.byte1 (mvncopy)
              sta     long:vec + 1
              jsr     .word0 (run)

              lda     #.byte0 (libfill)
              sta     long:vec
              lda     #.byte1 (libfill)
              sta     long:vec + 1
              jsr     .word0 (run)

              lda     #.byte0 (w16fill)
              sta     long:vec
              lda     #.byte1 (w16fill)
              sta     long:vec + 1
              jsr     .word0 (run)

              lda     #.byte0 (mvnfill)
              sta     long:vec
              lda     #.byte1 (mvnfill)
              sta     long:vec + 1
              jsr     .word0 (run)

              lda     #'.'
              jsr     .word0 (putc)                    ; the run script's end marker
              rep     #0x30
              jsl     goshell_on_esc                   ; ESC returns to the prompt
              rtl


n_libfill:    .byte   "LIBFILL", 0

; The library's CODE, emitted last so this file's own routines come first --
; the same placement libmem.s uses.
#include "x16_code.s"
