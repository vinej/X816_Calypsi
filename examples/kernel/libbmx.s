; ============================================================================
; libbmx.s -- storage/bmx.asm over the kernel, round-tripped on a real card.
;
; bmx was the library's heaviest KERNAL user: MACPTR streaming into a fixed
; port, READST at every stage, the channel open/close dance. All of that is
; now fio_* handles and a bounce buffer, and THIS is the test that the
; plumbing still moves the right bytes to the right places: a known pattern
; is saved from VRAM to a file, the VRAM is wiped, the file is loaded back,
; and every byte is compared. A chunking mistake -- an off-by-one in the
; bounce loop, a lost byte at a 255-byte seam, the gap seek landing short --
; shows up as a mismatched pixel, not as a plausible screen.
;
; READING THE SCREEN (as libfs.s: top band = the test, bottom = why)
;
;   GREEN    every test passed
;   RED      1: bmx_save of a 64x32x8 pattern (palette 8 entries from 16)
;   YELLOW   2: bmx_load back, and the header fields it publishes
;   BLUE     3: every VRAM byte identical after wipe + reload
;   MAGENTA  4: a 16-byte junk file is refused as BMX_ERR_FORMAT
;   CYAN     5: a missing file is refused as BMX_ERR_IO
;
;   bottom = bmx_lasterr: BLACK 1 = IO, BROWN 2 = FORMAT, LTGREEN 3 = PACKED
;
; The test number lands at $00:0400 and the error code at $00:0401.
;
; Runs 8-bit: x16lib is 65C02 code. Cleans up before it starts, as libfs.s
; does, so a run after a crashed run still starts from a known card.
; ============================================================================

              .rtmodel version, "1"
              .rtmodel core, "65816"
              .rtmodel codeModel, "large"
              .rtmodel dataModel, "small"

; ONLY the bmx gate: X16_USE_BMX pulling X16_USE_FILEIO through x16_code.s
; is itself under test -- the fio_* calls below fail to assemble if the
; dependency chain drops it.
#define X16_USE_BMX 1

#include "x16.s"

              .extern kern_install, goshell_on_esc, con_init

RESULT:       .equ 0x0400

C_GREEN:      .equ 0x05
C_RED:        .equ 0x02
C_YELLOW:     .equ 0x07
C_BLUE:       .equ 0x06
C_MAGENTA:    .equ 0x04
C_CYAN:       .equ 0x03
C_BLACK:      .equ 0x00
C_WHITE:      .equ 0x01
C_BROWN:      .equ 0x09
C_LTGREEN:    .equ 0x0D

VERA_DC_VID:  .equ 0x9F29
VERA_DC_HS:   .equ 0x9F2A
VERA_DC_VS:   .equ 0x9F2B
VERA_L0_CFG:  .equ 0x9F2D
VERA_L0_TB:   .equ 0x9F2F

; 64x32 at 8bpp, stride = width: one contiguous 2 KB block at VRAM $08000
; (clear of the console's tilemap at $00000 and font at $04000).
IMG_W:        .equ 64
IMG_H:        .equ 32
IMG_BYTES:    .equ IMG_W * IMG_H

              .section code, noreorder
              .public main
main:
              jsl     con_init
              jsl     kern_install
              sep     #0x30

; ---- 0: remove anything a previous run left behind -------------------------
              jsr     .word0 (del_bmx)
              jsr     .word0 (del_bad)

; ---- 1: save a known pattern ------------------------------------------------
              lda     #1
              sta     failno
              bra     f1_go
f1:
              jmp     .word0 (fail)
f1_go:
              jsr     .word0 (aim_img_w)      ; port 0 at $08000, INC_1
              lda     #3                      ; v = 3; v += 7 per byte. Chosen
              sta     dp:X16_T6               ; so the sequence's period (256)
              jsr     .word0 (fill_pattern)   ; is coprime with the 255-byte
                                              ; bounce chunk: every seam lands
                                              ; on a different value
              ; 8 palette entries from index 16, values v = 1; v += 5
              jsr     .word0 (aim_pal16)
              lda     #1
              sta     dp:X16_T6
              ldy     #16                     ; 8 entries = 16 bytes
pal_fill:
              lda     dp:X16_T6
              sta     VERA_DATA0
              clc
              adc     #5
              sta     dp:X16_T6
              dey
              bne     pal_fill

              lda     #IMG_W                  ; describe the image for bmx_save
              sta     bmx_width
              stz     bmx_width+1
              lda     #IMG_H
              sta     bmx_height
              stz     bmx_height+1
              lda     #8
              sta     bmx_bpp
              lda     #16
              sta     bmx_palstart
              lda     #8
              sta     bmx_palcount
              stz     bmx_palcount+1
              lda     #IMG_W
              sta     bmx_stride
              stz     bmx_stride+1
              lda     #7
              sta     bmx_border

              jsr     .word0 (name_bmx)       ; A/X/Y + P4/P5/P6 for $08000
              jsr     .word0 (bmx_save)
              bcs     f1

; ---- 2: load it back, and believe the header --------------------------------
              lda     #2
              sta     failno
              bra     f2_go
f2:
              jmp     .word0 (fail)
f2_go:
              jsr     .word0 (aim_img_w)      ; wipe: every byte becomes $CC,
              ldy     #IMG_H                  ; so a load that writes nothing
wipe_row:                                     ; cannot pass test 3
              ldx     #IMG_W
wipe_col:
              lda     #0xCC
              sta     VERA_DATA0
              dex
              bne     wipe_col
              dey
              bne     wipe_row

              lda     #0xAA                   ; poison the published fields:
              sta     bmx_width               ; test 2 must see the FILE's
              sta     bmx_width+1             ; values, not the ones test 1
              sta     bmx_height              ; left behind
              sta     bmx_height+1
              sta     bmx_bpp

              jsr     .word0 (name_bmx)
              jsr     .word0 (bmx_load)
              bcs     f2
              lda     bmx_width
              cmp     #IMG_W
              bne     f2
              lda     bmx_width+1
              bne     f2
              lda     bmx_height
              cmp     #IMG_H
              bne     f2
              lda     bmx_height+1
              bne     f2
              lda     bmx_bpp
              cmp     #8
              bne     f2

; ---- 3: every byte back where it was ----------------------------------------
              lda     #3
              sta     failno
              bra     f3_go
f3:
              jmp     .word0 (fail)
f3_go:
              jsr     .word0 (aim_img_r)      ; port 0 at $08000 for reading
              lda     #3
              sta     dp:X16_T6
              stz     dp:X16_T0               ; 16-bit count in T0/T1
              stz     dp:X16_T1
cmp_loop:
              lda     VERA_DATA0
              cmp     dp:X16_T6
              bne     f3
              lda     dp:X16_T6
              clc
              adc     #7
              sta     dp:X16_T6
              inc     dp:X16_T0
              bne     cmp_next
              inc     dp:X16_T1
cmp_next:
              lda     dp:X16_T1
              cmp     #.byte1 (IMG_BYTES)
              bne     cmp_loop
              lda     dp:X16_T0
              cmp     #.byte0 (IMG_BYTES)
              bne     cmp_loop

; ---- 4: junk is not a BMX ----------------------------------------------------
              lda     #4
              sta     failno
              bra     f4_go
f4:
              jmp     .word0 (fail)
f4_go:
              ; 16 bytes of $AA: long enough to be a header, wrong magic
              lda     #.byte0 (p_bad)
              sta     dp:X16_P0
              lda     #.byte1 (p_bad)
              sta     dp:X16_P1
              stz     dp:X16_P2
              lda     #FIO_WRITE
              sta     dp:X16_P3
              jsr     .word0 (fio_open)
              bcs     f4
              sta     handle
              sta     dp:X16_P0
              lda     #.byte0 (junk)
              sta     dp:X16_P1
              lda     #.byte1 (junk)
              sta     dp:X16_P2
              stz     dp:X16_P3
              lda     #16
              sta     dp:X16_P4
              stz     dp:X16_P5
              jsr     .word0 (fio_write)
              bcs     f4
              lda     handle
              jsr     .word0 (fio_close)
              bcs     f4

              lda     #.byte0 (n_bad)
              ldx     #.byte1 (n_bad)
              ldy     #7
              jsr     .word0 (set_name_regs)
              jsr     .word0 (bmx_load)
              bcc     f4                      ; accepting junk is the failure
              jsr     .word0 (bmx_lasterr)
              cmp     #2                      ; BMX_ERR_FORMAT
              bne     f4

; ---- 5: a missing file is an IO refusal --------------------------------------
              lda     #5
              sta     failno
              bra     f5_go
f5:
              jmp     .word0 (fail)
f5_go:
              lda     #.byte0 (n_nope)
              ldx     #.byte1 (n_nope)
              ldy     #8
              jsr     .word0 (set_name_regs)
              jsr     .word0 (bmx_load)
              bcc     f5
              jsr     .word0 (bmx_lasterr)
              cmp     #1                      ; BMX_ERR_IO
              bne     f5

; ---- done: leave the card as it was found ------------------------------------
              jsr     .word0 (del_bmx)
              jsr     .word0 (del_bad)

              stz     failno
fail:
              sta     errno                   ; captured FIRST, as libfs.s
              lda     failno
              sta     RESULT
              bne     fail_err
              stz     errno                   ; a pass ends on a deliberate
fail_err:                                     ; refusal; see libfs.s
              lda     errno
              sta     RESULT+1
              lda     failno
              tax
              lda     colours,x
              sta     dp:X16_T4
              ldx     errno
              cpx     #4
              bcc     fail_code
              ldx     #0
fail_code:
              lda     errcolours,x
              sta     dp:X16_T5
              jsr     .word0 (paint)
halt:
              rep     #0x30
              jsl     goshell_on_esc
              sep     #0x30
              bra     halt

; ---- helpers ---------------------------------------------------------------

; A/X/Y = name for bmx_*; also parks the VRAM target in P4/P5/P6
set_name_regs:
              sta     dp:X16_P0
              stx     dp:X16_P1
              sty     dp:X16_P2
              stz     dp:X16_P3               ; device: ignored
              stz     dp:X16_P4               ; VRAM bank 0
              stz     dp:X16_P5               ; $08000
              lda     #0x80
              sta     dp:X16_P6
              lda     dp:X16_P0
              ldx     dp:X16_P1
              ldy     dp:X16_P2
              rts

name_bmx:
              lda     #.byte0 (n_bmx)
              ldx     #.byte1 (n_bmx)
              ldy     #5
              jmp     .word0 (set_name_regs)

; bmx_load/bmx_save read the name from X16_P0/P1 with the length in P2 --
; set_name_regs leaves exactly that, plus the name back in A/X/Y for the
; callers that take it there.

; port 0 at VRAM $08000, INC_1, for writing or reading
aim_img_w:
aim_img_r:
              stz     VERA_CTRL
              stz     VERA_ADDR_L
              lda     #0x80
              sta     VERA_ADDR_M
              lda     #0x10                   ; bank 0 | INC_1
              sta     VERA_ADDR_H
              rts

; port 0 at the palette, entry 16 ($1FA00 + 32), INC_1
aim_pal16:
              stz     VERA_CTRL
              lda     #0x20
              sta     VERA_ADDR_L
              lda     #0xFA
              sta     VERA_ADDR_M
              lda     #0x11                   ; bank 1 | INC_1
              sta     VERA_ADDR_H
              rts

; IMG_BYTES of v += 7 through the aimed port, starting at X16_T6
fill_pattern:
              stz     dp:X16_T0
              stz     dp:X16_T1
fp_loop:
              lda     dp:X16_T6
              sta     VERA_DATA0
              clc
              adc     #7
              sta     dp:X16_T6
              inc     dp:X16_T0
              bne     fp_next
              inc     dp:X16_T1
fp_next:
              lda     dp:X16_T1
              cmp     #.byte1 (IMG_BYTES)
              bne     fp_loop
              lda     dp:X16_T0
              cmp     #.byte0 (IMG_BYTES)
              bne     fp_loop
              rts

; failures ignored: on a clean card there is nothing to delete
del_bmx:
              lda     #.byte0 (p_bmx)
              sta     dp:X16_P0
              lda     #.byte1 (p_bmx)
              sta     dp:X16_P1
              stz     dp:X16_P2
              jmp     .word0 (fio_delete)
del_bad:
              lda     #.byte0 (p_bad)
              sta     dp:X16_P0
              lda     #.byte1 (p_bad)
              sta     dp:X16_P1
              stz     dp:X16_P2
              jmp     .word0 (fio_delete)

; X16_T4 = top band colour, X16_T5 = bottom. Same painter as libfs.s.
paint:
              stz     VERA_CTRL
              lda     #0x11
              sta     VERA_DC_VID
              lda     #0x40
              sta     VERA_DC_HS
              sta     VERA_DC_VS
              lda     #0x07
              sta     VERA_L0_CFG
              stz     VERA_L0_TB
              stz     VERA_CTRL
              stz     VERA_ADDR_L
              stz     VERA_ADDR_M
              lda     #0x10
              sta     VERA_ADDR_H
              lda     dp:X16_T4
              sta     dp:X16_T2
              ldx     #240
p_row:
              cpx     #120
              bne     p_band
              lda     dp:X16_T5
              sta     dp:X16_T2
p_band:
              ldy     #0
p_col:
              lda     dp:X16_T2
              sta     VERA_DATA0
              iny
              bne     p_col
              lda     dp:X16_T2               ; 320 = 256 + 64
              ldy     #64
p_tail:
              sta     VERA_DATA0
              dey
              bne     p_tail
              dex
              bne     p_row
              rts

              .section data,data
colours:      .byte   C_GREEN, C_RED, C_YELLOW, C_BLUE
              .byte   C_MAGENTA, C_CYAN
; Indexed by bmx_lasterr, which has three codes and 0.
errcolours:   .byte   C_GREEN                 ; 0 no code
              .byte   C_BLACK                 ; 1 BMX_ERR_IO
              .byte   C_BROWN                 ; 2 BMX_ERR_FORMAT
              .byte   C_LTGREEN               ; 3 BMX_ERR_PACKED
failno:       .byte   0
errno:        .byte   0
handle:       .byte   0
; names for bmx_* as (address, length); paths for fio_delete NUL-terminated
n_bmx:        .byte   "B.BMX", 0
n_bad:        .byte   "BAD.BMX", 0
n_nope:       .byte   "NOPE.BMX", 0
p_bmx:        .byte   "/B.BMX", 0
p_bad:        .byte   "/BAD.BMX", 0
junk:         .space  16, 0xAA

#include "x16_code.s"
