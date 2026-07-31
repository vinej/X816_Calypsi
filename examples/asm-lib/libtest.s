; ============================================================================
; libtest.s -- runtime conformance test for the converted X16 library.
;
;   GREEN screen  = every test passed
;   RED           = test 1 failed (sine table contents / data-init)
;   YELLOW        = test 2 failed (library call through .word0)
;   BLUE          = test 3 failed (direct page, dp: addressing)
;   MAGENTA       = test 4 failed (the patched vera_addrsel macro)
;
; The result code is ALSO written to $00:0400 -- 0 = pass, otherwise the
; number of the first test that failed -- for inspection from a debugger or on
; hardware. It is NOT how the emulator run is checked: x16emu only dumps
; memory when the PC reaches $FFFF, and -testbench hooks a PC value X816 never
; reaches, so the automated check reads the screen colour out of a -gif
; capture instead. See run-emu.sh.
;
; Same idea as the core's boot/vramtest.s, but pointed at the toolchain rather
; than the hardware. Everything the converter had to get right shows up as a
; wrong answer here rather than as a link error:
;
;   test 1  the sine table was moved to a `data` section in bank $00, and
;           cstartup's data_init_table walk actually copied it out of the
;           image. Checked with an EOR checksum over all 256 bytes, not a
;           spot check -- and NOT a sum, because a full sine period sums to
;           zero either way and would pass over a table of zeroes.
;   test 2  calls into the library resolve through .word0: code is at
;           $01:xxxx, jsr is 16 bits, and the bank comes from PBR.
;   test 3  dp: addressing reaches the library's zero-page pointers.
;   test 4  the vera_addrsel PATCHES entry -- the one construct in the tree
;           the converter could not translate -- does at run time what ACME's
;           !if generated.
;
; Runs 8-bit: x16lib is 65C02 code, so A/X/Y must be 8 bits wide before any
; call into it. cstartup leaves them 16-bit for C.
; ============================================================================

              .rtmodel version, "1"
              .rtmodel core, "65816"
              .rtmodel codeModel, "large"
              .rtmodel dataModel, "small"

#define X16_USE_MATH 1

#include "x16.s"

SINTAB_EOR:    .equ 0xFE            ; EOR of all 256 sine bytes, from the formula
SIN_0:         .equ 0x00            ; sin8(0)
SIN_32:        .equ 0x5A            ; sin8(32)
SIN_64:        .equ 0x7F            ; sin8(64)   peak
SIN_192:       .equ 0x81            ; sin8(192)  trough

RESULT:        .equ 0x0400          ; bank $00, readable with testbench RQM

; Colours in VERA's default palette
C_GREEN:       .equ 0x05
C_RED:         .equ 0x02
C_YELLOW:      .equ 0x07
C_BLUE:        .equ 0x06
C_MAGENTA:     .equ 0x04

              .section code, noreorder
              .public main
main:
              sep     #0x30             ; 8-bit A/X/Y for the whole test

; ---- test 1: the sine table survived the move to bank $00 ------------------
              ldx     #0
              lda     #0
t1_loop:
              eor     .word0 (math_sintab),x
              inx
              bne     t1_loop
              cmp     #SINTAB_EOR
              beq     t1_ok
              lda     #1
              jmp     .word0 (fail)
t1_ok:

; ---- test 2: calling into the library ------------------------------------
              lda     #0
              jsr     .word0 (sin8)
              cmp     #SIN_0
              bne     t2_bad
              lda     #32
              jsr     .word0 (sin8)
              cmp     #SIN_32
              bne     t2_bad
              lda     #64
              jsr     .word0 (sin8)
              cmp     #SIN_64
              bne     t2_bad
              lda     #192
              jsr     .word0 (sin8)
              cmp     #SIN_192
              beq     t2_ok
t2_bad:       lda     #2
              jmp     .word0 (fail)
t2_ok:

; ---- test 3: direct page ---------------------------------------------------
; lerp8 takes its endpoints in X16_P0/X16_P1, which are direct page. t=0 must
; give exactly a and t=255 exactly b, so a dp: that reached the wrong address
; cannot pass by luck.
              lda     #10
              sta     dp:X16_P0
              lda     #200
              sta     dp:X16_P1
              lda     #0
              jsr     .word0 (lerp8)
              cmp     #10
              bne     t3_bad
              lda     #255
              jsr     .word0 (lerp8)
              cmp     #200
              beq     t3_ok
t3_bad:       lda     #3
              jmp     .word0 (fail)
t3_ok:

; ---- test 4: the patched vera_addrsel macro --------------------------------
; +vera_addr selects a data port with vera_addrsel, which is the macro the
; converter had to patch (ACME's !if generated trb or tsb; as65816 has no
; assembler conditional, so the port literal is pasted into the macro name).
; Writing through port 1 and reading back through port 0 only works if the
; ADDRSEL bit landed correctly both times.
              vera_addr 1, 0x1000, VERA_INC_1
              lda     #0xA5
              sta     VERA_DATA1
              lda     #0x5A
              sta     VERA_DATA1

              vera_addr 0, 0x1000, VERA_INC_1
              lda     VERA_DATA0
              cmp     #0xA5
              bne     t4_bad
              lda     VERA_DATA0
              cmp     #0x5A
              beq     t4_ok
t4_bad:       lda     #4
              jmp     .word0 (fail)
t4_ok:

              lda     #0                ; every test passed
              ; falls into fail, which paints green for 0

; ---- report ----------------------------------------------------------------
; A = 0 for pass, otherwise the failing test number.
fail:
              sta     RESULT
              tax                       ; keep the code for the colour lookup
              lda     .word0 (colours),x
              jsr     .word0 (paint)
halt:         bra     halt

; This table has to be in a DATA section, not beside the code. Code lives at
; $01:xxxx but the read above is 16-bit absolute through DBR = $00, so leaving
; it here would have read bank $00 at the code's offset -- whatever happened
; to be there. It assembles and links either way; only running it tells you.
; The converter applies this rule to the library automatically; hand-written
; code has to follow it too.
              .section data,data
colours:      .byte   C_GREEN, C_RED, C_YELLOW, C_BLUE, C_MAGENTA
              .section code, noreorder

; ----------------------------------------------------------------------------
; paint -- fill a 320x240 8bpp bitmap with the colour in A.
; Same display setup as the core's boot/vramtest.s, so a green screen here
; means the same thing it does there.
; ----------------------------------------------------------------------------
paint:
              sta     dp:X16_T0         ; stash the colour

              stz     VERA_CTRL
              lda     #0x11             ; VGA output + layer 0 enable
              sta     VERA_DC_VIDEO
              lda     #0x40             ; half scale -> 320x240 active
              sta     VERA_DC_HSCALE
              sta     VERA_DC_VSCALE
              lda     #0x07             ; bitmap mode, 8bpp
              sta     VERA_L0_CONFIG
              stz     VERA_L0_TILEBASE  ; bitmap base 0, 320 wide

              stz     VERA_CTRL
              stz     VERA_ADDR_L
              stz     VERA_ADDR_M
              lda     #0x10             ; increment 1, addr[19:16] = 0
              sta     VERA_ADDR_H

              rep     #0x10             ; 16-bit index for the pixel counters
              ldy     ##240
p_line:       ldx     ##320
p_px:         lda     dp:X16_T0
              sta     VERA_DATA0
              dex
              bne     p_px
              dey
              bne     p_line
              sep     #0x10
              rts

#include "x16_code.s"
