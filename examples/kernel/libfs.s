; ============================================================================
; libfs.s -- the converted x16lib storage API, on a real card.
;
; Everything here goes through the LIBRARY -- fio_open, fio_read, dir_next --
; which goes through system/x816kernel.asm, which goes through $00:FE00. That
; whole stack is what is under test. kfstest.c already proved the kernel
; entries themselves; what is unproven is the crossing: 65C02 code running
; 8-bit calling a 16-bit ABI, with the register width switched back on every
; path including the error ones.
;
; A missed `sep` does not crash. It leaves the machine executing 65C02 code
; with 16-bit registers, reading and writing one byte too many, and the
; symptom appears somewhere else entirely. So the checks below are arranged so
; that a width leak shows up as a wrong answer here rather than as damage
; later: every call is followed by something that reads memory the library
; wrote.
;
;   GREEN    every test passed
;   RED      1: fio_mkdir
;   YELLOW   2: fio_open write / fio_write / fio_close
;   BLUE     3: fio_size and fio_read read back what was written
;   MAGENTA  4: dir_open / dir_next / dir_close
;   CYAN     5: fio_getc, a byte at a time
;   ORANGE   6: fio_delete / fio_rmdir and the refusals
;
; The number also lands at $00:0400.
;
; Runs 8-bit: x16lib is 65C02 code, so A/X/Y must be 8 bits wide before any
; call into it. cstartup leaves them 16-bit for C, and kern_install is C.
; ============================================================================

              .rtmodel version, "1"
              .rtmodel core, "65816"
              .rtmodel codeModel, "large"
              .rtmodel dataModel, "small"

#define X16_USE_FILEIO 1
#define X16_USE_DIR    1

#include "x16.s"

              .extern kern_install

RESULT:       .equ 0x0400

C_GREEN:      .equ 0x05
C_RED:        .equ 0x02
C_YELLOW:     .equ 0x07
C_BLUE:       .equ 0x06
C_MAGENTA:    .equ 0x04
C_CYAN:       .equ 0x03
C_ORANGE:     .equ 0x08

; The VERA registers come from the library's own core/const_vera.asm --
; defining them again here would be a second source of truth for an address
; that is not ours to choose.
VERA_DC_VID:  .equ 0x9F29
VERA_DC_HS:   .equ 0x9F2A
VERA_DC_VS:   .equ 0x9F2B
VERA_L0_CFG:  .equ 0x9F2D
VERA_L0_TB:   .equ 0x9F2F

LEN:          .equ 40

              .section code, noreorder
              .public main
main:
              ; The table is stamped by C, which runs 16-bit. Everything
              ; after this is the library's world.
              jsl     kern_install
              sep     #0x30

; ---- 1: make the directory -------------------------------------------------
; A success here also proves the card mounted; every later test would fail the
; same way otherwise, so it is worth its own colour.
              lda     #1
              sta     failno
              bra     f1_go
f1:
              jmp     .word0 (fail)
f1_go:
              lda     #.byte0 (d_lt)
              sta     dp:X16_P0
              lda     #.byte1 (d_lt)
              sta     dp:X16_P1
              stz     dp:X16_P2
              jsr     .word0 (fio_mkdir)
              bcs     f1

; ---- 2: create, write, close ----------------------------------------------
              lda     #2
              sta     failno
              bra     f2_go
f2:
              jmp     .word0 (fail)
f2_go:
              ldx     #0
fillsrc:
              txa
              clc
              adc     #65                     ; 'A' upwards, not a constant:
              sta     src,x                   ; a buffer of one repeated byte
              inx                             ; reads back correctly even if
              cpx     #LEN                    ; the file is misaligned
              bne     fillsrc

              jsr     .word0 (open_write)
              bcs     f2
              sta     handle

              lda     handle
              sta     dp:X16_P0
              lda     #.byte0 (src)
              sta     dp:X16_P1
              lda     #.byte1 (src)
              sta     dp:X16_P2
              stz     dp:X16_P3               ; bank $00
              lda     #LEN
              sta     dp:X16_P4
              stz     dp:X16_P5
              jsr     .word0 (fio_write)
              bcs     f2
              lda     dp:X16_P6
              cmp     #LEN
              bne     f2
              lda     dp:X16_P7
              bne     f2

              ; Closing is what makes it durable: the size lives in the
              ; directory entry, and until that is written the file is
              ; whatever length it was before.
              lda     handle
              jsr     .word0 (fio_close)
              bcs     f2

; ---- 3: size, and read it back ---------------------------------------------
              lda     #3
              sta     failno
              bra     f3_go
f3:
              jmp     .word0 (fail)
f3_go:
              jsr     .word0 (open_read)
              bcs     f3
              sta     handle

              lda     handle
              jsr     .word0 (fio_size)
              bcs     f3
              lda     dp:X16_P4
              cmp     #LEN
              bne     f3
              lda     dp:X16_P5               ; the upper three bytes must be
              ora     dp:X16_P6               ; zero -- a width leak in
              ora     dp:X16_P7               ; kern_size shows up right here
              bne     f3

              bra     f3b_go
f3b:
              jmp     .word0 (fail)
f3b_go:
              ldx     #0
clrdst:
              stz     dst,x
              inx
              cpx     #LEN
              bne     clrdst

              lda     handle
              sta     dp:X16_P0
              lda     #.byte0 (dst)
              sta     dp:X16_P1
              lda     #.byte1 (dst)
              sta     dp:X16_P2
              stz     dp:X16_P3
              lda     #LEN
              sta     dp:X16_P4
              stz     dp:X16_P5
              jsr     .word0 (fio_read)
              bcs     f3b
              lda     dp:X16_P6
              cmp     #LEN
              bne     f3b

              ldx     #0
cmploop:
              lda     dst,x
              cmp     src,x
              bne     f3b
              inx
              cpx     #LEN
              bne     cmploop

              ; At the end of the file a further read returns zero bytes and
              ; still SUCCEEDS. End of file is not an error, and a library
              ; that reported it as one would make every read loop wrong.
              lda     handle
              sta     dp:X16_P0
              lda     #LEN
              sta     dp:X16_P4
              stz     dp:X16_P5
              jsr     .word0 (fio_read)
              bcs     f3b
              lda     dp:X16_P6
              ora     dp:X16_P7
              bne     f3b

              lda     handle
              jsr     .word0 (fio_close)
              bcs     f3b

; ---- 4: enumerate ----------------------------------------------------------
              lda     #4
              sta     failno
              bra     f4_go
f4:
              jmp     .word0 (fail)
f4_go:
              lda     #.byte0 (d_lt)
              sta     dp:X16_P0
              lda     #.byte1 (d_lt)
              sta     dp:X16_P1
              lda     #3                      ; a non-zero "length": the
              sta     dp:X16_P2               ; argument survives for source
              lda     #8                      ; compatibility and is ignored
              sta     dp:X16_P3               ; along with the device
              jsr     .word0 (dir_open)
              bcs     f4

              ; "." and ".." are NOT hidden. A directory without them is
              ; unnavigable from inside, so seeing them is part of proving
              ; the kernel seeded the cluster properly.
              jsr     .word0 (next_entry)
              bcc     f4
              lda     #.byte0 (e_dot)
              ldx     #.byte1 (e_dot)
              jsr     .word0 (name_is)
              bcs     f4
              jsr     .word0 (dir_type)
              cmp     #DIR_TYPE_DIR
              bne     f4

              jsr     .word0 (next_entry)
              bcc     f4
              lda     #.byte0 (e_dot2)
              ldx     #.byte1 (e_dot2)
              jsr     .word0 (name_is)
              bcs     f4

              jsr     .word0 (next_entry)
              bcc     f4
              lda     #.byte0 (e_file)
              ldx     #.byte1 (e_file)
              jsr     .word0 (name_is)
              bcs     f4
              jsr     .word0 (dir_type)
              cmp     #DIR_TYPE_PRG
              bne     f4
              jsr     .word0 (dir_size)
              lda     dp:X16_P4
              cmp     #LEN
              bne     f4

              ; The end of the listing is carry CLEAR here -- the library
              ; keeps the sense every caller in the tree was written against,
              ; which is the opposite of the kernel's. Without this check the
              ; loop could run forever and the test would still be green.
              jsr     .word0 (next_entry)
              bcs     f4

              jsr     .word0 (dir_close)

; ---- 5: one byte at a time -------------------------------------------------
              lda     #5
              sta     failno
              bra     f5_go
f5:
              jmp     .word0 (fail)
f5_go:
              jsr     .word0 (open_read)
              bcs     f5
              sta     handle

              ; The index lives in memory, not in X. A kernel call goes
              ; through 16-bit registers and comes back having used X and Y
              ; for the ABI's arguments, so nothing survives in them across
              ; one -- which is worth stating because holding a loop counter
              ; there is exactly what a 65C02 programmer does by reflex.
              stz     idx
getcloop:
              lda     handle
              jsr     .word0 (fio_getc)
              bcs     f5                      ; carry set before the end
              ldx     idx
              cmp     src,x
              bne     f5
              inc     idx
              lda     idx
              cmp     #LEN
              bne     getcloop

              ; Past the end it must REFUSE, not keep answering. A CBM CHRIN
              ; answers $0D forever past the end of a file, which is exactly
              ; the behaviour that made every parser in the old library check
              ; READST after every single byte.
              lda     handle
              jsr     .word0 (fio_getc)
              bcc     f5

              lda     handle
              jsr     .word0 (fio_close)

; ---- 6: delete, and the refusals -------------------------------------------
              lda     #6
              sta     failno
              bra     f6_go
f6:
              jmp     .word0 (fail)
f6_go:
              ; A non-empty directory must be refused. This is the negative
              ; control the other five need: a library that returned success
              ; unconditionally would be green all the way to here.
              lda     #.byte0 (d_lt)
              sta     dp:X16_P0
              lda     #.byte1 (d_lt)
              sta     dp:X16_P1
              stz     dp:X16_P2
              jsr     .word0 (fio_rmdir)
              bcc     f6

              lda     #.byte0 (f_a)
              sta     dp:X16_P0
              lda     #.byte1 (f_a)
              sta     dp:X16_P1
              stz     dp:X16_P2
              jsr     .word0 (fio_delete)
              bcs     f6

              lda     #.byte0 (d_lt)
              sta     dp:X16_P0
              lda     #.byte1 (d_lt)
              sta     dp:X16_P1
              stz     dp:X16_P2
              jsr     .word0 (fio_rmdir)
              bcs     f6

              ; And the file really is gone.
              jsr     .word0 (open_read)
              bcc     f6

              stz     failno
fail:
              lda     failno
              sta     RESULT
              tax
              lda     colours,x
              jsr     .word0 (paint)
halt:
              bra     halt

; ---- helpers ---------------------------------------------------------------

; open /LT/A.TXT for reading; carry as fio_open leaves it, A = handle
open_read:
              lda     #.byte0 (f_a)
              sta     dp:X16_P0
              lda     #.byte1 (f_a)
              sta     dp:X16_P1
              stz     dp:X16_P2
              lda     #FIO_READ
              sta     dp:X16_P3
              jmp     .word0 (fio_open)

open_write:
              lda     #.byte0 (f_a)
              sta     dp:X16_P0
              lda     #.byte1 (f_a)
              sta     dp:X16_P1
              stz     dp:X16_P2
              lda     #FIO_WRITE
              sta     dp:X16_P3
              jmp     .word0 (fio_open)

; one step of the iterator into namebuf; carry SET if an entry came back
next_entry:
              lda     #.byte0 (namebuf)
              sta     dp:X16_P0
              lda     #.byte1 (namebuf)
              sta     dp:X16_P1
              lda     #16
              sta     dp:X16_P2
              jmp     .word0 (dir_next)

; A/X = the expected name; carry SET if namebuf differs. Compares the
; terminator too, so a name that merely starts with the expected text fails.
name_is:
              sta     dp:X16_T0
              stx     dp:X16_T1
              ldy     #0
ni_loop:
              lda     (dp:X16_T0),y
              cmp     namebuf,y
              bne     ni_bad
              cmp     #0
              beq     ni_ok
              iny
              bne     ni_loop
ni_bad:
              sec
              rts
ni_ok:
              clc
              rts

; A = palette index; fills the 320x240 layer with it
paint:
              sta     dp:X16_T2
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
              ldx     #240
p_row:
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
              .byte   C_MAGENTA, C_CYAN, C_ORANGE
failno:       .byte   0
handle:       .byte   0
idx:          .byte   0
d_lt:         .byte   "/LT", 0
f_a:          .byte   "/LT/A.TXT", 0
e_dot:        .byte   ".", 0
e_dot2:       .byte   "..", 0
e_file:       .byte   "A.TXT", 0
namebuf:      .space  16, 0
src:          .space  LEN, 0
dst:          .space  LEN, 0

#include "x16_code.s"
