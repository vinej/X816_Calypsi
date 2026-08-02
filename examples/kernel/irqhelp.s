; ============================================================================
; irqhelp.s -- the assembly half of irqtest.c.
;
; Interrupt handlers CANNOT be written in C here, and that is a property of the
; ABI rather than a limitation of the compiler. runtime/kirq.s calls a handler
; with D = $0000, DBR = $00 and 16-bit registers, and expects it to finish with
; `rtl`. A Calypsi C function expects its own direct page and returns with rts
; or rtl depending on the code model -- and would run its prologue against
; pseudo-registers at $0000 that belong to whatever the interrupt cut off. So
; the handlers are here, they are four instructions each, and they touch
; nothing but their own counters.
;
; The install routines are here for a second reason: passing a 24-bit CODE
; address from C means knowing how Calypsi represents a function pointer in the
; large code model, which is exactly the kind of thing runtime/kcall.s's header
; records getting wrong twice. `.word0` and `.byte2` are the assembler's own
; relocation operators for "low 16 bits of" and "bank of", so the address the
; test installs is computed by the linker and cannot be a guess.
; ============================================================================

              .rtmodel version, "1"
              .rtmodel core, "65816"
              .rtmodel codeModel, "large"

              .public irqh_vsync_count, irqh_brk_count
              .public irqh_prev_lo, irqh_prev_bank
              .public irqh_vsync_lo, irqh_vsync_bank
              .public irqh_vsync2_lo, irqh_vsync2_bank
              .public irqh_install_vsync, irqh_install_vsync2, irqh_clear_vsync
              .public irqh_install_brk, irqh_do_brk
              .public irqh_set_bad_slot, irqh_bad_carry, irqh_bad_code

#include "x816_contract.inc"

              .section near,bss
irqh_vsync_count: .space 2      ; bumped by the VSYNC handlers
irqh_brk_count:   .space 2      ; bumped by the BRK handler
irqh_prev_lo:     .space 2      ; what IRQ_SET reported as the previous handler
irqh_prev_bank:   .space 2
irqh_vsync_lo:    .space 2      ; ...and the addresses to compare it against,
irqh_vsync_bank:  .space 2      ; published so C never has to build one
irqh_vsync2_lo:   .space 2
irqh_vsync2_bank: .space 2
irqh_bad_carry:   .space 2      ; the refusal path: carry and code from IRQ_SET
irqh_bad_code:    .space 2

              .section code

; ---------------------------------------------------------------------------
; The handlers. Entered by jsl from kirq_call with 16-bit A/X/Y, D = $0000 and
; DBR = $00; every register is free. `long:` addressing is used anyway, so
; these stay correct if the calling convention's D ever changes.
;
; irqh_vsync and irqh_vsync2 bump the SAME counter on purpose. The test tells
; them apart by the ADDRESS IRQ_SET hands back, not by the effect, so that
; "the previous handler is reported correctly" is a check about the ABI and not
; a check about which counter moved.
; ---------------------------------------------------------------------------
irqh_vsync:
              rep     #0x30
              lda     long:irqh_vsync_count
              inc     a
              sta     long:irqh_vsync_count
              rtl

irqh_vsync2:
              rep     #0x30
              lda     long:irqh_vsync_count
              inc     a
              sta     long:irqh_vsync_count
              rtl

irqh_brk:
              rep     #0x30
              lda     long:irqh_brk_count
              inc     a
              sta     long:irqh_brk_count
              rtl

; ---------------------------------------------------------------------------
; irqh_setvec -- the shared body: A = slot, X:Y = handler, and the previous
; handler IRQ_SET reports is parked where C can read it.
;
; NOTE THE TARGET. The generated x816_contract.inc defines K_IRQ_SET as the
; call NUMBER (49), because that is what a C caller passes to kern_call. The
; library's own core/const_kernel.asm defines the same name as an ADDRESS
; (X816_KERN + 49*4), because assembly jsl's straight through the table. Both
; are right for their side and the names collide, so an assembly caller that
; took the contract's version and wrote `jsl K_IRQ_SET` would jump to $000031
; -- a real address, in the direct page, executing whatever is there. That
; happened while writing this file. The arithmetic below is the fix, and it
; is spelled out rather than hidden in an equate so the next reader sees which
; of the two K_IRQ_SET means what.
; ---------------------------------------------------------------------------
irqh_setvec:
              jsl     KERN_TABLE + K_IRQ_SET * KERN_ENTRY_SIZE
              sta     long:irqh_prev_lo
              txa
              and     ##0x00FF
              sta     long:irqh_prev_bank
              rtl

; void irqh_install_vsync(void);   -- and publish the address it installed
irqh_install_vsync:
              rep     #0x30
              lda     ##.word0 (irqh_vsync)
              sta     long:irqh_vsync_lo
              lda     ##.byte2 (irqh_vsync)
              sta     long:irqh_vsync_bank
              ldy     ##.byte2 (irqh_vsync)
              ldx     ##.word0 (irqh_vsync)
              lda     ##KIRQ_VSYNC
              jsl     irqh_setvec
              rtl

; void irqh_install_vsync2(void);
irqh_install_vsync2:
              rep     #0x30
              lda     ##.word0 (irqh_vsync2)
              sta     long:irqh_vsync2_lo
              lda     ##.byte2 (irqh_vsync2)
              sta     long:irqh_vsync2_bank
              ldy     ##.byte2 (irqh_vsync2)
              ldx     ##.word0 (irqh_vsync2)
              lda     ##KIRQ_VSYNC
              jsl     irqh_setvec
              rtl

; void irqh_clear_vsync(void);   -- installing 0 is how a slot is emptied
irqh_clear_vsync:
              rep     #0x30
              ldy     ##0
              ldx     ##0
              lda     ##KIRQ_VSYNC
              jsl     irqh_setvec
              rtl

; void irqh_install_brk(void);
irqh_install_brk:
              rep     #0x30
              ldy     ##.byte2 (irqh_brk)
              ldx     ##.word0 (irqh_brk)
              lda     ##KIRQ_BRK
              jsl     irqh_setvec
              rtl

; ---------------------------------------------------------------------------
; void irqh_do_brk(void);
;
; Execute a BRK and come back. The 65816 pushes the address of the byte AFTER
; the signature byte, so the dispatcher's rti resumes at the rtl below -- which
; is the real check: not merely that the handler ran, but that the whole
; prologue/dispatch/epilogue put the machine back exactly as it found it. If
; the stack discipline in kirq.s were off by one byte, this would return
; somewhere else and the test would never reach its verdict.
; ---------------------------------------------------------------------------
irqh_do_brk:
              brk
              .byte   0                       ; BRK's signature byte
              rtl

; ---------------------------------------------------------------------------
; void irqh_set_bad_slot(void);
;
; IRQ_SET with a slot number one past the end. Must come back carry-set with
; KERR_BADARG -- the in-test negative: an IRQ_SET that accepted any number
; would write past kirq_vec into whatever follows it in bank $00.
; ---------------------------------------------------------------------------
irqh_set_bad_slot:
              rep     #0x30
              ldy     ##0
              ldx     ##0
              lda     ##KIRQ_SLOTS            ; one past the last legal slot
              jsl     KERN_TABLE + K_IRQ_SET * KERN_ENTRY_SIZE
              sta     long:irqh_bad_code
              lda     ##0
              rol     a                       ; carry -> bit 0, no branch
              sta     long:irqh_bad_carry
              rtl
