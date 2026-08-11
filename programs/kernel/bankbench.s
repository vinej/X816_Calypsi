; ============================================================================
; bankbench.s -- how much does executing from SDRAM cost?
;
; THE QUESTION. On X816, bank $00 is single-cycle BRAM and banks $01+ are
; SDRAM. Program code lives in bank $01, so every instruction byte a program
; fetches is an SDRAM access. doc/AUDIT.md 6.2 measured that indirectly: the
; SAME MVN instruction ran at 12.1 cycles/byte with its opcode in BRAM and
; 30.0 with it in SDRAM -- 2.5x, on identical data.
;
; But MVN is a peculiar instruction: it re-fetches its own three bytes for
; every byte it moves, so it is unusually fetch-heavy and flatters BRAM. This
; measures ORDINARY CODE instead, and the answer sizes two open decisions --
; whether to give user banks BRAM at all, and whether VERA2 becomes portable
; once the CPU stops competing for SDRAM (doc/VERA_MEMORY_REVIEW.md 3).
;
; THE METHOD. One workload, assembled once, run twice: in place in bank $01,
; and again from a copy in bank $00. Same bytes, same data, same alignment --
; only the fetch path differs.
;
; That works because the workload is POSITION INDEPENDENT BY CONSTRUCTION:
; relative branches only, no jsr or jmp inside it, no reference to itself, and
; every data access absolute into bank $00 through DBR = $00. The same
; discipline runtime/exec.s's relocator already runs under, and for the same
; reason.
;
; The data it touches is in bank $00 in BOTH runs, deliberately. That isolates
; the INSTRUCTION FETCH difference, which is the thing under test -- and it is
; also the realistic case, because Calypsi's small data model already puts a
; program's data in bank $00 while its code sits in bank $01.
;
; IN THE EMULATOR THIS MEASURES NOTHING, AND THAT IS EXPECTED. Its memory is
; uniform, so both runs come out equal and the ratio is 1.00x. Only the board
; has two kinds of memory. Same lesson as the MVN stub, which is why that one
; was missed until MEMBENCH.BIN ran on hardware.
; ============================================================================

              .rtmodel version, "1"
              .rtmodel core, "65816"
              .rtmodel codeModel, "large"
              .rtmodel dataModel, "small"

#include "x816_contract.inc"

              .extern kern_install, con_init, kirq_install, goshell_on_esc
              .public main

K_PUTC:       .equ KERN_TABLE + K_CON_PUTC * KERN_ENTRY_SIZE
K_TGET:       .equ KERN_TABLE + K_TIME_GET * KERN_ENTRY_SIZE

; 255 outer passes of a 256-iteration inner loop: ~65k iterations, which is a
; few hundred milliseconds from BRAM and comfortably measurable from SDRAM.
OUTER:        .equ 255
BRAM_ROOM:    .equ 128                ; the copy's landing zone; work is ~30

              .section near,bss
; The landing zone is a `near` object rather than a hard-coded $00:A000, so
; the linker guarantees nothing else is placed on top of it. exec.s does the
; same with x816_exec_ram.
bram_code:    .space  BRAM_ROOM
wk_a:         .space  1               ; the workload's data -- bank $00 for
wk_b:         .space  1               ; both runs, so only fetch differs
wk_c:         .space  1
wk_outer:     .space  1
wk_bank:      .space  1               ; PBR the workload really ran in
t0:           .space  4
dt:           .space  2
fillw:        .space  2

              .section code

; ---------------------------------------------------------------------------
; THE WORKLOAD. Six instructions, sixteen bytes of opcode and operand, three
; reads and one write per inner iteration -- roughly the fetch-to-data ratio
; ordinary 65816 code has, because on this CPU every instruction byte is
; itself a memory access.
;
; NOTHING IN HERE MAY BE ABSOLUTE-WITHIN-THE-CODE. Branches are PC-relative
; and survive the copy; a jsr or jmp to a label would still point into bank
; $01 and the bank-$00 copy would silently measure the bank-$01 original.
; ---------------------------------------------------------------------------
work:
              sep     #0x30
              ; Record the bank we are actually executing in. Without this the
              ; two runs being equal is AMBIGUOUS: a copy that had secretly
              ; jumped back into bank $01 -- because something in here was not
              ; position independent after all -- would also report equal
              ; times, and would do so on hardware too, where it would look
              ; like "SDRAM costs nothing".
              phk
              pla
              sta     wk_bank
              lda     #OUTER
              sta     wk_outer
work_outer:
              ldx     #0
work_inner:
              lda     wk_a
              clc
              adc     wk_b
              eor     wk_c
              sta     wk_a
              inx
              bne     work_inner
              dec     wk_outer
              bne     work_outer
              rep     #0x30
              rtl
work_end:

; ---------------------------------------------------------------------------
; Timing and printing, the shapes membench.s already proved.
; ---------------------------------------------------------------------------
t_start:
              rep     #0x30
              jsl     K_TGET
              sta     long:t0
              txa
              sta     long:t0 + 2
              sep     #0x30
              rts

t_stop:
              rep     #0x30
              jsl     K_TGET
              sec
              sbc     long:t0
              sta     long:dt
              sep     #0x30
              rts

putc:
              and     #0xFF
              sta     long:fillw
              lda     #0
              sta     long:fillw + 1
              rep     #0x30
              lda     long:fillw
              jsl     K_PUTC
              sep     #0x30
              rts

putnib:
              and     #0x0F
              cmp     #10
              bcc     putnib_dec
              adc     #('A' - 10 - 1)
              bra     putc
putnib_dec:
              adc     #'0'
              bra     putc

puthex16:
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
              lda     #13
              jsr     .word0 (putc)
              lda     #10
              jmp     .word0 (putc)

; putbank -- print wk_bank as four hex digits, so the run script can prove
; each half executed where it was meant to.
putbank:
              lda     wk_bank
              sta     long:dt
              lda     #0
              sta     long:dt + 1
              jmp     .word0 (puthex16)

; ---------------------------------------------------------------------------
main:
              jsl     con_init
              jsl     kern_install
              jsl     kirq_install
              sep     #0x30

              ; Copy the workload into bank $00. Byte-wise and by length, the
              ; same way x816_exec_init moves its relocator down.
              rep     #0x10
              sep     #0x20
              ldx     ##0
main_copy:
              lda     long:work,x
              sta     long:bram_code,x
              inx
              cpx     ##(work_end - work)
              bne     main_copy
              sep     #0x30

              ; ---- run 1: in place, bank $01 (SDRAM) ----
              lda     #0
              sta     wk_a
              sta     wk_b
              sta     wk_c
              jsr     .word0 (t_start)
              jsl     work
              jsr     .word0 (t_stop)
              jsr     .word0 (puthex16)     ; time FIRST -- putbank reuses dt
              jsr     .word0 (putbank)

              ; ---- run 2: the copy, bank $00 (BRAM) ----
              lda     #0
              sta     wk_a
              sta     wk_b
              sta     wk_c
              jsr     .word0 (t_start)
              jsl     bram_code
              jsr     .word0 (t_stop)
              jsr     .word0 (puthex16)     ; time FIRST -- putbank reuses dt
              jsr     .word0 (putbank)

              lda     #'.'
              jsr     .word0 (putc)
              rep     #0x30
              jsl     goshell_on_esc
              rtl
