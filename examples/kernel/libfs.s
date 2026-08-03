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
; READING THE SCREEN
;
; All green is a pass. Anything else is TWO bands: the top half names the test
; that failed and the bottom half is the kernel's error code, because on
; hardware there is no debugger to ask and one colour cannot carry both. A red
; top with a grey bottom is "mkdir refused, and the reason was I/O" -- a card
; that would not mount. A red top with a white bottom is "mkdir refused because
; the name is already taken", which is a different fault entirely and used to
; look identical.
;
;   top      bottom (KERR_)
;   GREEN    every test passed
;   RED      1: fio_mkdir
;   YELLOW   2: fio_open write / fio_write / fio_close
;   BLUE     3: fio_size and fio_read read back what was written
;   MAGENTA  4: dir_open / dir_next / dir_close
;   CYAN     5: fio_getc, a byte at a time
;   ORANGE   6: fio_delete / fio_rmdir and the refusals
;   DKGREY   7: the dos_* compatibility layer -- mkdir/chdir/rename/
;               delete/rmdir with the OLD (address, length) signatures,
;               dos_lasterr carrying the KERR_ code, and an overlong
;               name refused up front with BADARG
;   LTGREY   8: the redesigned storage/load -- fs_save of 20 KB of
;               bank $00, fs_load back into bank $03 (two chunks, so
;               the full-ask path runs), CRCs agree, fs_vload lands
;               the same bytes in VRAM, and a missing file refuses
;
;            bottom band: 0 none, 1 NOSYS, 2 NOTFOUND, 3 NOSPACE, 4 BADARG,
;            5 IO, 6 EXISTS, 7 NOTEMPTY
;
; The test number lands at $00:0400 and the error code at $00:0401.
;
; IT CLEANS UP BEFORE IT STARTS, not only after. A test that only works on a
; pristine card fails for the wrong reason the second time it is run, and that
; is exactly what happened on hardware: a green run, a reset, and then a red
; screen that meant "/LT is still there" while looking like "mkdir is broken".
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
#define X16_USE_DOS    1
#define X16_USE_LOAD   1
#define X16_USE_MEM    1

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
C_BLACK:      .equ 0x00
C_WHITE:      .equ 0x01
C_BROWN:      .equ 0x09
C_LTGREEN:    .equ 0x0D
C_LTBLUE:     .equ 0x0E
C_GREY:       .equ 0x0C
C_PINK:       .equ 0x0A
C_DKGREY:     .equ 0x0B
C_LTGREY:     .equ 0x0F

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
              ; con_init before anything else, and not only for the screen: it
              ; is what copies the exec relocator into bank $00. Without it ESC
              ; reached a handover blob that had never been installed and the
              ; machine walked into whatever bank $00 happened to contain --
              ; silently, because nothing had touched VERA by then.
              jsl     con_init
              ; The table is stamped by C, which runs 16-bit. Everything
              ; after this is the library's world.
              jsl     kern_install
              sep     #0x30

; ---- 0: remove anything a previous run left behind -------------------------
; Failures here are IGNORED on purpose: on a clean card there is nothing to
; delete and both calls refuse, which is correct and is not this test's
; business. The point is only that test 1 starts from a known state.
              lda     #.byte0 (f_a)
              sta     dp:X16_P0
              lda     #.byte1 (f_a)
              sta     dp:X16_P1
              stz     dp:X16_P2
              jsr     .word0 (fio_delete)
              lda     #.byte0 (d_lt)
              sta     dp:X16_P0
              lda     #.byte1 (d_lt)
              sta     dp:X16_P1
              stz     dp:X16_P2
              jsr     .word0 (fio_rmdir)
              ; and test 7's leavings, if a previous run died inside it
              lda     #.byte0 (p_ltdr)
              sta     dp:X16_P0
              lda     #.byte1 (p_ltdr)
              sta     dp:X16_P1
              stz     dp:X16_P2
              jsr     .word0 (fio_delete)
              lda     #.byte0 (p_ltds)
              sta     dp:X16_P0
              lda     #.byte1 (p_ltds)
              sta     dp:X16_P1
              stz     dp:X16_P2
              jsr     .word0 (fio_delete)
              lda     #.byte0 (d_ltd)
              sta     dp:X16_P0
              lda     #.byte1 (d_ltd)
              sta     dp:X16_P1
              stz     dp:X16_P2
              jsr     .word0 (fio_rmdir)
              lda     #.byte0 (p_ld)          ; and test 8's file
              sta     dp:X16_P0
              lda     #.byte1 (p_ld)
              sta     dp:X16_P1
              stz     dp:X16_P2
              jsr     .word0 (fio_delete)

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

; ---- 7: the dos_* compatibility layer --------------------------------------
; The OLD signatures -- A/X = name, Y = length, no NUL -- through the new
; plumbing. This is what filepick and every +xm_dos_* call site uses, so the
; register shapes here are the ones that must keep working.
              lda     #7
              sta     failno
              bra     f7_go
f7:
              jmp     .word0 (fail)
f7_go:
              ; make a directory, old-style
              lda     #.byte0 (n_ltd)
              ldx     #.byte1 (n_ltd)
              ldy     #3
              jsr     .word0 (dos_mkdir)
              bcs     f7

              ; making it AGAIN must refuse, and dos_lasterr must say why.
              ; This is the error path a caller that can only see the carry
              ; asks about afterwards.
              lda     #.byte0 (n_ltd)
              ldx     #.byte1 (n_ltd)
              ldy     #3
              jsr     .word0 (dos_mkdir)
              bcc     f7
              jsr     .word0 (dos_lasterr)
              cmp     #KERR_EXISTS
              bne     f7

              ; step inside and put a file there to rename
              lda     #.byte0 (n_ltd)
              ldx     #.byte1 (n_ltd)
              ldy     #3
              jsr     .word0 (dos_chdir)
              bcs     f7
              lda     #.byte0 (f_r83)
              sta     dp:X16_P0
              lda     #.byte1 (f_r83)
              sta     dp:X16_P1
              stz     dp:X16_P2
              lda     #FIO_WRITE
              sta     dp:X16_P3
              jsr     .word0 (fio_open)
              bcs     f7
              jsr     .word0 (fio_close)
              bcs     f7

              ; rename R.BIN to S.BIN: OLD name in X16_P0/P1 with its length
              ; in X16_P2, NEW name in A/X/Y -- both copy loops get exercised
              lda     #.byte0 (f_r83)
              sta     dp:X16_P0
              lda     #.byte1 (f_r83)
              sta     dp:X16_P1
              lda     #5
              sta     dp:X16_P2
              lda     #.byte0 (f_s83)
              ldx     #.byte1 (f_s83)
              ldy     #5
              jsr     .word0 (dos_rename)
              bcs     f7
              bra     f7m_go                  ; the test is longer than a
f7m:                                          ; branch reaches; relay, as
              jmp     .word0 (fail)           ; f1..f6 do at their heads
f7m_go:

              ; deleting S.BIN succeeding is the proof the rename happened;
              ; deleting it AGAIN must refuse
              lda     #.byte0 (f_s83)
              ldx     #.byte1 (f_s83)
              ldy     #5
              jsr     .word0 (dos_delete)
              bcs     f7m
              lda     #.byte0 (f_s83)
              ldx     #.byte1 (f_s83)
              ldy     #5
              jsr     .word0 (dos_delete)
              bcc     f7m

              ; back out and remove the directory
              lda     #.byte0 (n_root)
              ldx     #.byte1 (n_root)
              ldy     #1
              jsr     .word0 (dos_chdir)
              bcs     f7m
              lda     #.byte0 (n_ltd)
              ldx     #.byte1 (n_ltd)
              ldy     #3
              jsr     .word0 (dos_rmdir)
              bcs     f7m

              ; a name too long for the path buffer is refused up front with
              ; BADARG -- nothing reaches the kernel, and dos_lasterr agrees
              lda     #.byte0 (longbuf)
              ldx     #.byte1 (longbuf)
              ldy     #90
              jsr     .word0 (dos_delete)
              bcc     f7m
              jsr     .word0 (dos_lasterr)
              cmp     #KERR_BADARG
              bne     f7m

; ---- 8: the redesigned storage/load -----------------------------------------
; fs_save writes 20 KB of bank $01 exactly as it lies -- the program's own
; code, chosen because it is the one large region that does NOT change
; under the test's feet. (The first attempt saved bank $00's application
; area and compared CRCs later; the region contains this test's own
; variables and the runtime's fat32 state, all of which mutate with every
; fio call in between, so the CRCs disagreed by construction.) fs_load
; brings it back into bank $03, which takes one full $4000 ask and one
; short one, so both arms of the read loop run. The two blocks must then
; CRC identically, fs_vload must land the same bytes in VRAM, and a file
; that does not exist must refuse.
              lda     #8
              sta     failno
              bra     f8_go
f8:
              jmp     .word0 (fail)
f8_go:
              jsr     .word0 (name_ld)        ; save $01:1000..$01:5FFF
              lda     #1
              sta     dp:X16_P4
              stz     dp:X16_P5
              lda     #0x10
              sta     dp:X16_P6
              stz     dp:X16_T6               ; end $6000, exclusive
              lda     #0x60
              sta     dp:X16_T7
              jsr     .word0 (fs_save)
              bcs     f8

              jsr     .word0 (name_ld)        ; load it back at $03:0000
              lda     #3
              sta     dp:X16_P4
              stz     dp:X16_P5
              stz     dp:X16_P6
              jsr     .word0 (fs_load)
              bcs     f8
              cpx     #0x00                   ; one past the end = $03:5000
              bne     f8
              cpy     #0x50
              bne     f8
              lda     dp:X16_P4
              cmp     #3
              bne     f8

              bra     f8b_go                  ; relay: the test outgrew a branch
f8b:
              jmp     .word0 (fail)
f8b_go:
              stz     dp:X16_P0               ; CRC of the source block
              lda     #0x10
              sta     dp:X16_P1
              lda     #1
              sta     dp:X16_P2
              stz     dp:X16_P3               ; count $5000
              lda     #0x50
              sta     dp:X16_P4
              jsr     .word0 (mem_crc)
              sta     t_crc
              stx     t_crc+1
              stz     dp:X16_P0               ; CRC of the loaded copy
              stz     dp:X16_P1
              lda     #3
              sta     dp:X16_P2
              stz     dp:X16_P3
              lda     #0x50
              sta     dp:X16_P4
              jsr     .word0 (mem_crc)
              cmp     t_crc
              bne     f8b
              cpx     t_crc+1
              bne     f8b

              jsr     .word0 (name_ld)        ; the same bytes into VRAM $08000
              stz     dp:X16_P4               ; VRAM bank 0
              stz     dp:X16_P5
              lda     #0x80
              sta     dp:X16_P6
              jsr     .word0 (fs_vload)
              bcs     f8b

              bra     f8c_go
f8c:
              jmp     .word0 (fail)
f8c_go:
              stz     VERA_CTRL               ; port 0 at $08000 to read back
              stz     VERA_ADDR_L
              lda     #0x80
              sta     VERA_ADDR_M
              lda     #0x10
              sta     VERA_ADDR_H
              ; compared against the bank-$03 copy through mem_peek's long
              ; pointer -- the ONE way 8-bit code reaches another bank, and
              ; the copy is the thing test 8a just proved equal to the source
              stz     dp:X16_P0
              stz     dp:X16_P1
              lda     #3
              sta     dp:X16_P2
v8_byte:
              lda     VERA_DATA0
              sta     t_v
              jsr     .word0 (mem_peek)
              cmp     t_v
              bne     f8c
              inc     dp:X16_P0
              bne     v8_byte
              inc     dp:X16_P1
              lda     dp:X16_P1
              cmp     #0x50                   ; $5000 bytes done
              bne     v8_byte

              jsr     .word0 (name_nof)       ; and a missing file refuses
              lda     #3
              sta     dp:X16_P4
              stz     dp:X16_P5
              stz     dp:X16_P6
              jsr     .word0 (fs_load)
              bcc     f8c

              lda     #.byte0 (p_ld)          ; leave the card as found
              sta     dp:X16_P0
              lda     #.byte1 (p_ld)
              sta     dp:X16_P1
              stz     dp:X16_P2
              jsr     .word0 (fio_delete)
              bcs     f8c

              stz     failno
fail:
              ; A is whatever the failing call returned, which for a refusal is
              ; the KERR_ code. Captured FIRST: everything below touches A.
              sta     errno
              lda     failno
              sta     RESULT
              bne     fail_err
              ; A PASS has no reason, and the last thing a passing run does is
              ; a deliberate refusal -- so errno still holds NOTFOUND from the
              ; open that was supposed to fail. Left alone it painted a green
              ; screen with a fault code underneath it.
              stz     errno
fail_err:
              lda     errno
              sta     RESULT+1
              lda     failno
              tax
              lda     colours,x
              sta     dp:X16_T4
              ldx     errno
              cpx     #8
              bcc     fail_code
              ldx     #0                      ; a code outside the table reads
fail_code:                                    ; as "none" rather than as junk
              lda     errcolours,x
              sta     dp:X16_T5
              jsr     .word0 (paint)
halt:
              ; ESC reloads the shell. Without it, reading the next result on
              ; hardware means power-cycling the board -- `run` loaded this
              ; over the prompt. Sixteen-bit for the crossing back into C, and
              ; it only comes back if the card could not be read, in which case
              ; the colour on screen is still the answer.
              rep     #0x30
              jsl     goshell_on_esc
              sep     #0x30
              bra     halt

; ---- helpers ---------------------------------------------------------------

; the (address, length) name for the fs_* calls of test 8
name_ld:
              lda     #.byte0 (n_ld)
              sta     dp:X16_P0
              lda     #.byte1 (n_ld)
              sta     dp:X16_P1
              lda     #6
              sta     dp:X16_P2
              rts
name_nof:
              lda     #.byte0 (n_nof)
              sta     dp:X16_P0
              lda     #.byte1 (n_nof)
              sta     dp:X16_P1
              lda     #8
              sta     dp:X16_P2
              rts

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

; X16_T4 = the top band's colour, X16_T5 = the bottom band's. A pass paints
; both the same, so green stays green and nothing has to special-case it.
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
              cpx     #120                    ; the lower half of the screen
              bne     p_band                  ; carries the reason
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
              .byte   C_MAGENTA, C_CYAN, C_ORANGE, C_DKGREY, C_LTGREY
; Indexed by the KERR_ code. Deliberately NOT the same palette as above -- if
; the two bands shared colours a glance could not tell which half it was
; looking at.
errcolours:   .byte   C_GREEN                 ; 0 no code
              .byte   C_BLACK                 ; 1 NOSYS
              .byte   C_BROWN                 ; 2 NOTFOUND
              .byte   C_LTGREEN               ; 3 NOSPACE
              .byte   C_LTBLUE                ; 4 BADARG
              .byte   C_GREY                  ; 5 IO
              .byte   C_WHITE                 ; 6 EXISTS
              .byte   C_PINK                  ; 7 NOTEMPTY
failno:       .byte   0
errno:        .byte   0
handle:       .byte   0
idx:          .byte   0
d_lt:         .byte   "/LT", 0
f_a:          .byte   "/LT/A.TXT", 0
; test 7's names. The n_ ones go to dos_* as (address, length) pairs -- the
; NULs are not part of the name, they are here so a debugger dump reads.
n_ltd:        .byte   "LTD", 0
n_root:       .byte   "/", 0
f_r83:        .byte   "R.BIN", 0
f_s83:        .byte   "S.BIN", 0
d_ltd:        .byte   "/LTD", 0
p_ltdr:       .byte   "/LTD/R.BIN", 0
p_ltds:       .byte   "/LTD/S.BIN", 0
n_ld:         .byte   "LD.BIN", 0
n_nof:        .byte   "NOFILE.X", 0
p_ld:         .byte   "/LD.BIN", 0
t_crc:        .word   0
t_v:          .byte   0
longbuf:      .space  90, 0
e_dot:        .byte   ".", 0
e_dot2:       .byte   "..", 0
e_file:       .byte   "A.TXT", 0
namebuf:      .space  16, 0
src:          .space  LEN, 0
dst:          .space  LEN, 0

#include "x16_code.s"
