; ============================================================================
; libmem.s -- the reshaped x16lib memory API, over the kernel's allocator.
;
; Everything here goes through the LIBRARY -- mem_alloc, mem_fill, mem_copy,
; mem_crc -- which goes through system/x816kernel.asm for the two calls that
; are the kernel's, and does the rest itself. memtest.c already proved
; MEM_ALLOC/MEM_FREE at the ABI; what is unproven is this layer: 65C02 code
; running 8-bit, calling a 16-bit ABI, and a block-move engine that has to
; walk 24-bit pointers by hand because there is no KERNAL to do it.
;
; A missed `sep` does not crash. It leaves the machine executing 65C02 code
; with 16-bit registers, reading and writing one byte too many, and the
; symptom appears somewhere else. So every call below is followed by
; something that reads memory the library just wrote.
;
; WHY THE CRC TEST USES A PUBLISHED VECTOR
; ----------------------------------------
; mem_crc is the one routine here whose answer cannot be checked by reading
; back what was written -- a CRC that agreed with itself would pass any
; round-trip test while computing the wrong function entirely. So it is run
; against "123456789", whose CRC-16/IBM-3740 check value is the published
; $29B1. That is an oracle this tree did not produce.
;
; READING THE SCREEN
;
;   GREEN    every test passed
;   RED      1: mem_alloc -- in the arena, page-aligned
;   YELLOW   2: mem_poke / mem_peek round-trip at 24 bits
;   BLUE     3: mem_fill, checked at the first, middle and last byte
;   MAGENTA  4: a second block is distinct, and mem_copy moves into it
;   CYAN     5: mem_copy with the ranges OVERLAPPING upwards
;   ORANGE   6: mem_crc against the published $29B1
;   BROWN    7: mem_free, and a double free refused
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

#define X16_USE_MEM 1

#include "x16.s"

              .extern kern_install, goshell_on_esc, con_init

RESULT:       .equ 0x0400

C_GREEN:      .equ 0x05
C_RED:        .equ 0x02
C_YELLOW:     .equ 0x07
C_BLUE:       .equ 0x06
C_MAGENTA:    .equ 0x04
C_CYAN:       .equ 0x03
C_ORANGE:     .equ 0x08
C_BROWN:      .equ 0x09

VERA_DC_VID:  .equ 0x9F29
VERA_DC_HS:   .equ 0x9F2A
VERA_DC_VS:   .equ 0x9F2B
VERA_L0_CFG:  .equ 0x9F2D
VERA_L0_TB:   .equ 0x9F2F

BLOCK:        .equ 0x1000       ; 4 KB, several pages, crosses no bank

              .section code, noreorder
              .public main
main:
              ; con_init first: it also copies the exec relocator into bank
              ; $00, which ESC needs later.
              jsl     con_init
              jsl     kern_install
              sep     #0x30

; ---- 1: mem_alloc ----------------------------------------------------------
; In the arena and page-aligned. The kernel promises both (KERNEL.md 5.5) and
; a caller taking a block to use as a direct page depends on the second.
              lda     #1
              sta     failno
              lda     #.byte0 (BLOCK)
              sta     dp:X16_P0
              lda     #.byte1 (BLOCK)
              sta     dp:X16_P1
              stz     dp:X16_P2
              stz     dp:X16_P3
              jsr     .word0 (mem_alloc)
              bcs     f1
              lda     dp:X16_P0         ; page-aligned?
              bne     f1
              lda     dp:X16_P2         ; bank $20 or above?
              cmp     #0x20
              bcc     f1
              jsr     .word0 (save_a1)
              bra     t2
f1:           jmp     .word0 (fail)

; ---- 2: mem_poke / mem_peek ------------------------------------------------
; One byte out and back through a 24-bit pointer. bank_peek/bank_poke used to
; need a bank number and an offset into an 8 KB window; this is the whole
; machine with no window at all.
t2:
              lda     #2
              sta     failno
              jsr     .word0 (p0_is_a1)
              lda     #0x5A
              jsr     .word0 (mem_poke)
              jsr     .word0 (p0_is_a1)
              jsr     .word0 (mem_peek)
              cmp     #0x5A
              bne     f2
              bra     t3
f2:           jmp     .word0 (fail)

; ---- 3: mem_fill -----------------------------------------------------------
; Checked at the first, middle and LAST byte. A fill that stops one short, or
; one that runs one over, both pass a check of the first byte only.
t3:
              lda     #3
              sta     failno
              jsr     .word0 (p0_is_a1)
              lda     #.byte0 (BLOCK)
              sta     dp:X16_P3
              lda     #.byte1 (BLOCK)
              sta     dp:X16_P4
              lda     #0xA5
              jsr     .word0 (mem_fill)

              jsr     .word0 (p0_is_a1)         ; first
              jsr     .word0 (mem_peek)
              cmp     #0xA5
              bne     f3
              jsr     .word0 (p0_is_a1)         ; middle
              lda     #0x08
              sta     dp:X16_P1
              jsr     .word0 (mem_peek)
              cmp     #0xA5
              bne     f3
              jsr     .word0 (p0_is_a1)         ; last, BLOCK-1
              lda     #0xFF
              sta     dp:X16_P0
              lda     #0x0F
              sta     dp:X16_P1
              jsr     .word0 (mem_peek)
              cmp     #0xA5
              bne     f3
              bra     t4
f3:           jmp     .word0 (fail)

; ---- 4: a second block, and a copy into it ---------------------------------
; Two live allocations must not be the same memory. The copy is what proves
; it: block 2 is filled with a different value first, so finding $A5 there
; afterwards means the bytes really moved rather than having been there.
t4:
              lda     #4
              sta     failno
              lda     #.byte0 (BLOCK)
              sta     dp:X16_P0
              lda     #.byte1 (BLOCK)
              sta     dp:X16_P1
              stz     dp:X16_P2
              stz     dp:X16_P3
              jsr     .word0 (mem_alloc)
              bcs     f4
              jsr     .word0 (save_a2)

              jsr     .word0 (p0_is_a2)         ; poison block 2
              lda     #.byte0 (BLOCK)
              sta     dp:X16_P3
              lda     #.byte1 (BLOCK)
              sta     dp:X16_P4
              lda     #0x3C
              jsr     .word0 (mem_fill)

              jsr     .word0 (p0_is_a1)         ; source = block 1
              jsr     .word0 (p3_is_a2)         ; target = block 2
              lda     #.byte0 (BLOCK)
              sta     dp:X16_P6
              lda     #.byte1 (BLOCK)
              sta     dp:X16_P7
              jsr     .word0 (mem_copy)

              jsr     .word0 (p0_is_a2)         ; first byte of block 2
              jsr     .word0 (mem_peek)
              cmp     #0xA5
              bne     f4
              jsr     .word0 (p0_is_a2)         ; and its last
              lda     #0xFF
              sta     dp:X16_P0
              lda     #0x0F
              sta     dp:X16_P1
              jsr     .word0 (mem_peek)
              cmp     #0xA5
              bne     f4
              bra     t5
f4:           jmp     .word0 (fail)

; ---- 5: mem_copy with the ranges overlapping upwards -----------------------
; The case the direction logic exists for. Eight bytes 1..8 at +0, copied to
; +4: a naive forward loop smears the first byte across the range and the
; result is 1,2,3,4,1,2,3,4. Correct is 1..8 landing at +4.
t5:
              lda     #5
              sta     failno
              ldy     #0
t5_seed:
              jsr     .word0 (p0_is_a1)
              tya
              clc
              adc     dp:X16_P0                 ; page-aligned, offset < 12
              sta     dp:X16_P0
              iny
              tya                               ; values 1..8
              jsr     .word0 (mem_poke)
              cpy     #8
              bne     t5_seed

              jsr     .word0 (p0_is_a1)         ; source = +0
              jsr     .word0 (p3_is_a1)
              lda     dp:X16_P3                 ; target = +4
              clc
              adc     #4
              sta     dp:X16_P3
              lda     #8
              sta     dp:X16_P6
              stz     dp:X16_P7
              jsr     .word0 (mem_copy)

              ldy     #0
t5_check:
              jsr     .word0 (p0_is_a1)
              tya
              clc
              adc     #4                        ; read back at +4..+11
              clc
              adc     dp:X16_P0                 ; no carry: a1 is page-aligned
              sta     dp:X16_P0                 ; and the offset is under 12
              jsr     .word0 (mem_peek)         ; mem_peek leaves Y alone
              sty     dp:X16_T4
              inc     dp:X16_T4                 ; expected value is y+1
              cmp     dp:X16_T4
              bne     f5
              iny
              cpy     #8
              bne     t5_check
              bra     t6
f5:           jmp     .word0 (fail)

; ---- 6: mem_crc against a published vector ---------------------------------
t6:
              lda     #6
              sta     failno
              ldy     #0
t6_seed:
              jsr     .word0 (p0_is_a1)
              tya
              clc
              adc     dp:X16_P0
              sta     dp:X16_P0
              lda     digits,y
              jsr     .word0 (mem_poke)
              iny
              cpy     #9
              bne     t6_seed

              jsr     .word0 (p0_is_a1)
              lda     #9
              sta     dp:X16_P3
              stz     dp:X16_P4
              jsr     .word0 (mem_crc)
              cmp     #0xB1                     ; $29B1, low byte
              bne     f6
              cpx     #0x29
              bne     f6
              bra     t7
f6:           jmp     .word0 (fail)

; ---- 7: mem_free, and a double free refused --------------------------------
t7:
              lda     #7
              sta     failno
              jsr     .word0 (p0_is_a1)
              jsr     .word0 (mem_free)
              bcs     f7
              jsr     .word0 (p0_is_a2)
              jsr     .word0 (mem_free)
              bcs     f7
              jsr     .word0 (p0_is_a1)         ; the same block again
              jsr     .word0 (mem_free)
              bcc     f7                        ; MUST be refused
              stz     failno
              bra     verdict
f7:           jmp     .word0 (fail)

; ----------------------------------------------------------------------------
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

; ---- helpers ---------------------------------------------------------------
save_a1:
              lda     dp:X16_P0
              sta     a1
              lda     dp:X16_P1
              sta     a1+1
              lda     dp:X16_P2
              sta     a1+2
              rts
save_a2:
              lda     dp:X16_P0
              sta     a2
              lda     dp:X16_P1
              sta     a2+1
              lda     dp:X16_P2
              sta     a2+2
              rts
p0_is_a1:
              lda     a1
              sta     dp:X16_P0
              lda     a1+1
              sta     dp:X16_P1
              lda     a1+2
              sta     dp:X16_P2
              rts
p0_is_a2:
              lda     a2
              sta     dp:X16_P0
              lda     a2+1
              sta     dp:X16_P1
              lda     a2+2
              sta     dp:X16_P2
              rts
p3_is_a1:
              lda     a1
              sta     dp:X16_P3
              lda     a1+1
              sta     dp:X16_P4
              lda     a1+2
              sta     dp:X16_P5
              rts
p3_is_a2:
              lda     a2
              sta     dp:X16_P3
              lda     a2+1
              sta     dp:X16_P4
              lda     a2+2
              sta     dp:X16_P5
              rts

; 320x240 8bpp, whole screen one colour. Same as the other conformance
; images, so a result looks the same wherever it came from.
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
digits:       .byte   "123456789"
colours:      .byte   C_GREEN, C_RED, C_YELLOW, C_BLUE, C_MAGENTA
              .byte   C_CYAN, C_ORANGE, C_BROWN
a1:           .space  3, 0
a2:           .space  3, 0
failno:       .byte   0
pcol:         .byte   0

; The library CODE, last: x16.s above is symbols only. One translation
; unit, which is what lets a jsr reach mem_fill at all (see c-lib).
#include "x16_code.s"
