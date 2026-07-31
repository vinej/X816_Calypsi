; ============================================================================
; smc.s -- the SMC keyboard transaction, in assembly.
;
; WHY THIS IS NOT C
; -----------------
; It was C, and Calypsi 5.18 miscompiled it. In the bit-bang loop
;
;       for (i = 0; i < 8; i++) {
;           if (b & 0x80) sda_rel(); else sda_low();
;           b = (uint8_t)(b << 1);
;           ...
;       }
;
; b and i are both 8-bit locals and landed in adjacent stack slots, b at
; $01,S and i at $02,S. The shift of b compiled to
;
;       tsc / inc a / tax / asl 0x0000,x         with M = 0
;
; -- a SIXTEEN-bit read-modify-write on an EIGHT-bit object. So every shift of
; b also shifted the loop counter sitting next to it, and fed b's bit 7 into
; it. The counter passed 8 after three iterations; each byte went out three or
; four bits wide instead of eight; and the slave saw $90 where $84 was meant.
; It NACKed, so every read returned nothing and the keyboard was simply dead
; while the screen kept working perfectly.
;
; None of that is visible in the C and it produced no diagnostic. The same
; hazard applies to ANY 8-bit local shifted in place, so the fix is not to
; reshuffle the C until the code generator happens to behave -- it is to stop
; generating that code at all. What follows is a direct port of X816_Core
; boot/kbd.s, which has been green on real DE10-Nano hardware since the
; keyboard was first brought up.
;
; CALLING CONVENTION
; ------------------
; Calypsi, --code-model=large: entered by jsl, returns by rtl, with M=0 and
; X=0 (16-bit A and index registers) at both boundaries. A uint8_t result
; comes back in the low byte of A; callers mask it themselves, but this
; zero-extends anyway so the value is well defined either way.
;
; DBR is 0 under --data-model=small, which is what makes the absolute $9Fxx
; addressing below correct -- the compiler's own code reaches the VIA the same
; way.
;
; X and Y are both preserved. No direct-page location is touched: Calypsi
; keeps its virtual registers there, so a zero-page scratch byte -- which is
; how kbd.s did it -- would corrupt the caller. The byte being shifted lives
; in Y and the bit counter in X instead, which is also why nothing here needs
; a read-modify-write on memory at all.
; ============================================================================

              .rtmodel version, "1"
              .rtmodel core, "65816"
              .rtmodel codeModel, "large"
; No dataModel attribute: this file emits no data. Asserting one would make it
; refuse to link against a program built with a different model, for no gain.
; Same reasoning as x816hdr.s.

              .public smc_getkey_raw

VIA1_PA:      .equ    0x9F01          ; ORA / IRA
VIA1_DDRA:    .equ    0x9F03
SDA:          .equ    0x01            ; PA0
SCL:          .equ    0x02            ; PA1
NOT_SDA:      .equ    0xFE
NOT_SCL:      .equ    0xFD
; The bus addresses, pre-shifted. Written out rather than computed as
; (SMC_ADDR << 1) so nothing here depends on the assembler's expression
; syntax -- this file cannot be exercised by the C conformance runs.
SMC_ADDR:     .equ    0x42            ; 7-bit address
SMC_WRITE:    .equ    0x84            ; 0x42 << 1
SMC_READ:     .equ    0x85            ; (0x42 << 1) | 1
SMC_GETKEY:   .equ    0x07

              .section code

; ----------------------------------------------------------------------------
; uint8_t smc_getkey_raw(void);
;
; Runs one complete GETKEY transaction and returns the byte the SMC served:
; $00 when the key FIFO is empty, otherwise an IBM System/2 keycode with bit 7
; set on release.
;
; NOTE the full STOP between the command and the read, NOT a repeated START.
; X816_Core rtl/smc_x16.sv documents why: the real SMC firmware early-returns
; for a one-byte write, leaving the command armed for a separate read
; transaction. A repeated START never arms it and every read comes back $FE.
; ----------------------------------------------------------------------------
smc_getkey_raw:
              phy                             ; both are callee-saved, and both
              phx                             ; get used as scratch below
              sep     #0x30                   ; 8-bit A, X and Y throughout

              ; ORA = 0 once. The whole open-drain scheme rests on it: a line
              ; is driven low by switching the pin to an OUTPUT, which drives
              ; whatever ORA holds.
              stz     VIA1_PA

              jsr     i2c_start
              lda     #SMC_WRITE
              jsr     i2c_write
              lda     #SMC_GETKEY
              jsr     i2c_write
              jsr     i2c_stop                ; the command stays armed

              jsr     i2c_start
              lda     #SMC_READ
              jsr     i2c_write
              jsr     i2c_read_nak
              tay                             ; hold it across the STOP
              jsr     i2c_stop
              tya

              rep     #0x30                   ; 16-bit again, as C expects
              and     #0x00FF                 ; zero-extend the result
              plx
              ply
              rtl

; ----------------------------------------------------------------------------
; I2C primitives. Open drain via DDRA: a line is driven LOW by making the pin
; an output, and RELEASED by making it an input so the pull-up takes it high.
; A line is never driven high -- that is what open drain means, and the SMC
; drives SDA itself during ACK and during reads.
;
; All of these run with M=1 and X=1 (8-bit A/X/Y) and expect it. They clobber
; A only; X and Y belong to the callers below.
; ----------------------------------------------------------------------------
sda_low:
              lda     VIA1_DDRA
              ora     #SDA
              sta     VIA1_DDRA
              rts
sda_rel:
              lda     VIA1_DDRA
              and     #NOT_SDA
              sta     VIA1_DDRA
              rts
scl_low:
              lda     VIA1_DDRA
              ora     #SCL
              sta     VIA1_DDRA
              rts
scl_rel:
              lda     VIA1_DDRA
              and     #NOT_SCL
              sta     VIA1_DDRA
              rts

; START: SDA falls while SCL is high.
i2c_start:
              jsr     sda_rel
              jsr     scl_rel
              jsr     sda_low
              jsr     scl_low
              rts

; STOP: SDA rises while SCL is high.
i2c_stop:
              jsr     sda_low
              jsr     scl_rel
              jsr     sda_rel
              rts

; i2c_write -- send A, MSB first, then give the slave its ACK slot.
; Y holds the byte as it shifts out, X counts the bits.
i2c_write:
              tay
              ldx     #8
i2c_write_bit:
              tya
              asl     a                       ; MSB -> carry
              tay
              bcc     i2c_write_zero
              jsr     sda_rel
              bra     i2c_write_clk
i2c_write_zero:
              jsr     sda_low
i2c_write_clk:
              jsr     scl_rel
              jsr     scl_low
              dex
              bne     i2c_write_bit

              ; ACK slot: release SDA and give the slave one clock to pull it
              ; low. The answer is not checked -- kbd.s does not check it
              ; either, and there is nothing useful to do about a NACK here.
              jsr     sda_rel
              jsr     scl_rel
              jsr     scl_low
              rts

; i2c_read_nak -- read one byte MSB first, answer NACK, return it in A.
i2c_read_nak:
              ldy     #0
              ldx     #8
              jsr     sda_rel                 ; let the slave drive SDA
i2c_read_bit:
              tya
              asl     a
              tay                             ; make room for the next bit
              jsr     scl_rel
              lda     VIA1_PA
              and     #SDA
              beq     i2c_read_zero
              iny                             ; shifted-in 1; bit 0 is clear
i2c_read_zero:
              jsr     scl_low
              dex
              bne     i2c_read_bit

              ; NACK: leave SDA released for one more clock.
              jsr     sda_rel
              jsr     scl_rel
              jsr     scl_low
              tya
              rts
