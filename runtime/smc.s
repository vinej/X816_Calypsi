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
; It NACKed, every read returned nothing, and the keyboard was dead while the
; screen kept working perfectly.
;
; None of that is visible in the C and it produced no diagnostic. The same
; hazard applies to ANY 8-bit local shifted in place, so the fix is not to
; reshuffle the C until the code generator behaves -- it is to stop generating
; that code at all.
;
; SPEED, AND WHY IT MATTERS HERE
; ------------------------------
; The first working version was a direct port of X816_Core boot/kbd.s, and
; typing at it felt sluggish. Note what the symptom could NOT be: the SMC holds
; key events in a 16-entry FIFO, so a slow poller cannot LOSE a keypress, only
; add latency to each one. The cost of a single poll is therefore the whole
; problem.
;
; The speed here comes from ONE change: no jsr/rts per bus edge, and no
; read-modify-write of DDRA. The four states DDRA ever holds are known at every
; point, so every edge is a two-instruction `lda #imm / sta VIA1_DDRA` -- about
; 6 cycles against ~22 for a called helper that has to read DDRA back first.
;
; A SECOND optimisation was tried and REVERTED, and the reason is worth keeping.
; The SMC's default read operation powers up as CMD_GET_KEYCODE_FAST, so a bare
; read with no command write returns a keycode and halves the traffic again.
; That is correct against rtl/smc_x16.sv and against X816_Emulator, both of
; which agree -- and it killed the keyboard outright on real hardware, because
; what answers on the board is the BITSTREAM, not the RTL source. A .rbf built
; before CMD_SET_DFLT_READ_OP existed simply does not have that behaviour, and
; no emulator can warn about it because the emulator always models current RTL.
; The command is therefore sent explicitly on every poll. One byte per poll is
; cheap insurance against a class of bug that costs a hardware round trip to
; find and cannot be reproduced off the board.
;
; Making the edges FASTER is safe against this slave: smc_x16.sv samples SDA and
; SCL through a 3-stage synchroniser on cpu_clk and detects an edge from a pulse
; one clock wide, while the shortest pulse here is six. It drives each data bit
; on the SYNCHRONISED scl_fall, ~3 clocks after the real edge, and the master
; here needs ~28 cycles from that edge to the sample -- so the margin is nearly
; an order of magnitude. Anything that stretches these edges, such as SDRAM
; waits fetching this code from bank $01, only adds to it.
;
; CALLING CONVENTION
; ------------------
; Calypsi, --code-model=large: entered by jsl, returns by rtl, with M=0 and
; X=0 (16-bit A and index registers) at both boundaries. A uint8_t result comes
; back in the low byte of A; callers mask it themselves, but this zero-extends
; anyway so the value is well defined either way.
;
; DBR is 0 under --data-model=small, which is what makes the absolute $9Fxx
; addressing below correct -- the compiler's own code reaches the VIA the same
; way.
;
; TWO Calypsi assembler details, both of which fail SILENTLY if you get them
; wrong. `#` is an EIGHT-bit immediate and `##` a sixteen-bit one -- the width
; comes from the SYNTAX, NOT from the rep/sep state -- so `and #0x00FF` after a
; rep assembles to two bytes, the CPU then takes the following opcode as its
; operand, and execution walks off into the stack. And every jsr goes through
; .word0: this code links into bank $01, jsr takes a 16-bit target, and the
; bank comes from PBR. Without it the linker rejects the file outright, which
; is at least a loud failure. Disassemble the linked image before believing a
; clean build here.
;
; X and Y are both preserved. No direct-page location is touched: Calypsi keeps
; its virtual registers there, so a zero-page scratch byte -- which is how
; kbd.s did it -- would corrupt the caller. The byte being shifted lives in Y
; and the bit counter in X, which is also why nothing here needs a
; read-modify-write on memory at all.
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

; DDRA states. A 1 bit makes the pin an OUTPUT, and ORA is 0, so 1 = drive the
; line LOW and 0 = release it to the pull-up. A line is never driven high: that
; is what open drain means, and the SMC drives SDA itself during ACK and reads.
D_IDLE:       .equ    0x00            ; SDA released, SCL released
D_SDA:        .equ    0x01            ; SDA low,      SCL released
D_SCL:        .equ    0x02            ; SDA released, SCL low
D_BOTH:       .equ    0x03            ; SDA low,      SCL low

SMC_WRITE:    .equ    0x84            ; 0x42 << 1
SMC_READ:     .equ    0x85            ; (0x42 << 1) | 1
SMC_GETKEY:   .equ    0x07            ; CMD_GET_KEYCODE

              .section code

; ----------------------------------------------------------------------------
; uint8_t smc_getkey_raw(void);
;
; One read transaction. The byte the SMC serves is $00 when the key FIFO is
; empty, otherwise an IBM System/2 keycode with bit 7 set on release.
;
; The FIFO only advances when the master clocks the ninth (ACK/NACK) bit --
; rtl/smc_x16.sv pops in S_TX_ACK on scl_rise -- so i2c_read_nak's trailing
; clock is load-bearing rather than politeness. Without it every poll would
; re-serve the same keycode for ever.
; ----------------------------------------------------------------------------
smc_getkey_raw:
              phy
              phx
              sep     #0x30

              stz     VIA1_PA

              ; The command is sent EXPLICITLY every poll, and that is not an
              ; oversight. Relying on the SMC's default read operation saves a
              ; whole byte per poll and is correct against rtl/smc_x16.sv as it
              ; stands -- but "as it stands" is the problem: what answers on the
              ; board is the BITSTREAM, and a .rbf built before
              ; CMD_SET_DFLT_READ_OP existed would return nothing at all here.
              ; A dead keyboard is far worse than a byte per poll, and the
              ; emulator cannot warn about it because it always models current
              ; RTL. Speed comes from the edges below instead, which are
              ; entirely master-side and depend on no SMC feature whatsoever.
              jsr     .word0 (i2c_start)
              lda     #SMC_WRITE
              jsr     .word0 (i2c_write)
              lda     #SMC_GETKEY
              jsr     .word0 (i2c_write)
              jsr     .word0 (i2c_stop)       ; the command stays armed

              jsr     .word0 (i2c_start)
              lda     #SMC_READ
              jsr     .word0 (i2c_write)
              jsr     .word0 (i2c_read_nak)
              tay                             ; hold it across the STOP
              jsr     .word0 (i2c_stop)
              tya

              rep     #0x30
              and     ##0x00FF                ; zero-extend the result
              plx
              ply
              rtl

; ----------------------------------------------------------------------------
; Bus primitives. All run with M=1 and X=1 (8-bit A/X/Y) and expect it.
; ----------------------------------------------------------------------------

; START: SDA falls while SCL is high.
i2c_start:
              lda     #D_IDLE
              sta     VIA1_DDRA
              lda     #D_SDA                  ; SDA falls, SCL still high
              sta     VIA1_DDRA
              lda     #D_BOTH
              sta     VIA1_DDRA
              rts

; STOP: SDA rises while SCL is high.
i2c_stop:
              lda     #D_BOTH
              sta     VIA1_DDRA
              lda     #D_SDA                  ; SCL rises, SDA still low
              sta     VIA1_DDRA
              lda     #D_IDLE                 ; SDA rises with SCL high
              sta     VIA1_DDRA
              rts

; i2c_write -- send A, MSB first, then give the slave its ACK slot.
; Y holds the byte as it shifts out, X counts the bits. Entered with SCL low.
i2c_write:
              tay
              ldx     #8
i2c_write_bit:
              tya
              asl     a                       ; MSB -> carry
              tay
              bcs     i2c_write_one
              lda     #D_BOTH                 ; SDA low, changed while SCL low
              sta     VIA1_DDRA
              lda     #D_SDA                  ; clock high
              sta     VIA1_DDRA
              lda     #D_BOTH                 ; clock low
              sta     VIA1_DDRA
              bra     i2c_write_next
i2c_write_one:
              lda     #D_SCL                  ; SDA released, SCL still low
              sta     VIA1_DDRA
              lda     #D_IDLE                 ; clock high
              sta     VIA1_DDRA
              lda     #D_SCL                  ; clock low
              sta     VIA1_DDRA
i2c_write_next:
              dex
              bne     i2c_write_bit

              ; ACK slot: release SDA and give the slave one clock to pull it
              ; low. The answer is not checked -- kbd.s does not check it
              ; either, and there is nothing useful to do about a NACK here.
              lda     #D_SCL
              sta     VIA1_DDRA
              lda     #D_IDLE
              sta     VIA1_DDRA
              lda     #D_SCL
              sta     VIA1_DDRA
              rts

; i2c_read_nak -- read one byte MSB first, answer NACK, return it in A.
; SDA stays released throughout so the slave can drive it.
i2c_read_nak:
              ldy     #0
              ldx     #8
              lda     #D_SCL                  ; release SDA, SCL low
              sta     VIA1_DDRA
i2c_read_bit:
              tya
              asl     a
              tay                             ; make room for the next bit
              lda     #D_IDLE                 ; clock high
              sta     VIA1_DDRA
              lda     VIA1_PA
              and     #SDA
              beq     i2c_read_zero
              iny                             ; shifted-in 1; bit 0 is clear
i2c_read_zero:
              lda     #D_SCL                  ; clock low
              sta     VIA1_DDRA
              dex
              bne     i2c_read_bit

              ; NACK, and this ninth clock is what advances the FIFO.
              lda     #D_IDLE
              sta     VIA1_DDRA
              lda     #D_SCL
              sta     VIA1_DDRA
              tya
              rts
