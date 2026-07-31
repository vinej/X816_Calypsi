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

              .public smc_getkey_raw, smc_init

; ============================================================================
; The blob that actually runs, and the landing zone it runs FROM.
;
; This routine is copied into bank $00 at startup and executed there, because
; bank $00 is the only zero-wait memory on the machine. Everything above it is
; SDRAM behind a stall: rtl/bank0_ram.sv says bank $00 "removes the stall from
; the paths that execute most often", and X816.sv gates cpu_rdy on
; sdram_ready. A bit-bang loop is nothing BUT instruction fetch -- roughly 25
; bytes per bit -- so running it from SDRAM pays that stall on every one of
; them. That, and not the protocol, is what made typing drop letters.
;
; Dropped, not merely delayed, and the distinction matters: the key FIFO is 16
; entries and a keystroke costs two of them (press and release) while a poll
; drains exactly one. rtl/smc_x16.sv line 505 pushes only
; `if (push_key && !kfifo_full)`, so once typing outruns the drain rate the
; overflow is SILENTLY DISCARDED.
;
; WHY IT IS COPIED RATHER THAN LINKED THERE
; -----------------------------------------
; The HPS loader writes the image to $01:0000 and nothing else, so bank $00
; holds whatever was there before. Calypsi initialises bank $00 `data` from an
; `idata` copy carried in the image -- the mechanism font8x8.c relies on -- but
; the assembler refuses instructions outside a TEXT section, so code cannot use
; it. Hence: assemble the blob into the image like any other code, and copy it
; at startup.
;
; That makes the blob POSITION INDEPENDENT by requirement. It contains no jsr
; and no absolute reference to itself: every branch is relative, and the only
; absolute addresses are the VIA registers, which do not move. The byte-send is
; therefore a MACRO expanded three times rather than a subroutine called three
; times -- a jsr would encode a bank-$01 address and land on whatever happens
; to sit at that offset in bank $00.
; ============================================================================

; ---- the send-a-byte inner loop, inlined -----------------------------------
; A holds the byte. Y shifts it, X counts the bits. Entered with SCL low and
; leaves SCL low after the slave's ACK slot. `n` only makes the labels unique.
I2C_SEND      .macro  n
              tay
              ldx     #8
send\n:
              tya
              asl     a                       ; MSB -> carry
              tay
              bcs     one\n
              lda     #D_BOTH                 ; SDA low, changed while SCL low
              sta     VIA1_DDRA
              lda     #D_SDA                  ; clock high
              sta     VIA1_DDRA
              lda     #D_BOTH                 ; clock low
              sta     VIA1_DDRA
              bra     next\n
one\n:
              lda     #D_SCL                  ; SDA released, SCL still low
              sta     VIA1_DDRA
              lda     #D_IDLE                 ; clock high
              sta     VIA1_DDRA
              lda     #D_SCL                  ; clock low
              sta     VIA1_DDRA
next\n:
              dex
              bne     send\n
              ; ACK slot: release SDA and give the slave one clock to pull it
              ; low. The answer is not checked -- boot/kbd.s does not check it
              ; either, and there is nothing useful to do about a NACK here.
              lda     #D_SCL
              sta     VIA1_DDRA
              lda     #D_IDLE
              sta     VIA1_DDRA
              lda     #D_SCL
              sta     VIA1_DDRA
              .endm

; START: SDA falls while SCL is high. Entered with the bus idle.
I2C_START     .macro
              lda     #D_IDLE
              sta     VIA1_DDRA
              lda     #D_SDA                  ; SDA falls, SCL still high
              sta     VIA1_DDRA
              lda     #D_BOTH
              sta     VIA1_DDRA
              .endm

; STOP: SDA rises while SCL is high.
I2C_STOP      .macro
              lda     #D_BOTH
              sta     VIA1_DDRA
              lda     #D_SDA                  ; SCL rises, SDA still low
              sta     VIA1_DDRA
              lda     #D_IDLE                 ; SDA rises with SCL high
              sta     VIA1_DDRA
              .endm

; ---- landing zone in bank $00 ----------------------------------------------
; `near` is placed in HiRAM ($00:A000-$00:FEFF) by x816-lib.scm. Sized with
; slack so that the blob growing does not silently run off the end.
SMC_RAM_SIZE: .equ    384

              .section near,bss
              .public smc_ram
smc_ram:      .space  SMC_RAM_SIZE

              .section code

; ----------------------------------------------------------------------------
; void smc_init(void);
;
; Copy the blob into bank $00. Call once, from con_init, before any poll.
; ----------------------------------------------------------------------------
smc_init:
              rep     #0x10                   ; 16-bit index
              sep     #0x20                   ; 8-bit A
              ldx     ##0
smc_init_loop:
              lda     long:smc_blob,x         ; absolute long indexed: the
              sta     long:smc_ram,x          ; banks are explicit, so this
              inx                             ; needs no DBR juggling
              cpx     ##(smc_blob_end - smc_blob)
              bne     smc_init_loop
              rep     #0x30                   ; 16-bit again, as C expects
              rtl

; ----------------------------------------------------------------------------
; uint8_t smc_getkey_raw(void);
;
; A tail call into the copy in bank $00: jml does not push, so the blob's own
; rtl returns straight to the C caller and this costs one instruction.
; ----------------------------------------------------------------------------
smc_getkey_raw:
              jmp     long:smc_ram    ; JML: same opcode, Calypsi spelling

; ----------------------------------------------------------------------------
; The blob. Runs from bank $00 after smc_init; the copy in bank $01 is only the
; master image and is never executed.
;
; The byte the SMC serves is $00 when the key FIFO is empty, otherwise an IBM
; System/2 keycode with bit 7 set on release.
;
; NOTE the full STOP between the command and the read, NOT a repeated START.
; rtl/smc_x16.sv documents why: the real SMC firmware early-returns for a
; one-byte write, leaving the command armed for a separate read transaction. A
; repeated START never arms it and every read comes back $FE.
;
; The command is sent explicitly on every poll rather than leaning on the SMC's
; default read operation. That shortcut is correct against the RTL and against
; the emulator, and it killed the keyboard on hardware, because what answers on
; the board is the BITSTREAM: a .rbf built before CMD_SET_DFLT_READ_OP existed
; has no such behaviour, and no emulator can warn about it.
; ----------------------------------------------------------------------------
smc_blob:
              phy
              phx
              sep     #0x30                   ; 8-bit A, X and Y throughout

              ; ORA = 0 once. The whole open-drain scheme rests on it: a line
              ; is driven low by switching the pin to an OUTPUT, which then
              ; drives whatever ORA holds.
              stz     VIA1_PA

              I2C_START
              lda     #SMC_WRITE
              I2C_SEND 1
              lda     #SMC_GETKEY
              I2C_SEND 2
              I2C_STOP                        ; the command stays armed

              I2C_START
              lda     #SMC_READ
              I2C_SEND 3

              ; ---- read one byte, MSB first, and answer NACK ----
              ; SDA stays released throughout so the slave can drive it. The
              ; ninth clock at the end is load-bearing: rtl/smc_x16.sv pops the
              ; FIFO in S_TX_ACK on scl_rise, so without it every poll would
              ; re-serve the same keycode for ever.
              ldy     #0
              ldx     #8
              lda     #D_SCL                  ; release SDA, SCL low
              sta     VIA1_DDRA
read_bit:
              tya
              asl     a
              tay                             ; make room for the next bit
              lda     #D_IDLE                 ; clock high
              sta     VIA1_DDRA
              lda     VIA1_PA
              and     #SDA
              beq     read_zero
              iny                             ; shifted-in 1; bit 0 is clear
read_zero:
              lda     #D_SCL                  ; clock low
              sta     VIA1_DDRA
              dex
              bne     read_bit

              lda     #D_IDLE                 ; NACK, and this ninth clock is
              sta     VIA1_DDRA               ; what advances the FIFO
              lda     #D_SCL
              sta     VIA1_DDRA
              tya                             ; hold the result across the STOP
              tax
              I2C_STOP
              txa

              rep     #0x30                   ; 16-bit again, as C expects
              and     ##0x00FF                ; zero-extend the result
              plx
              ply
              rtl
smc_blob_end:
