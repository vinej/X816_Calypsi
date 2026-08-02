; ============================================================================
; exec.s -- hand the machine over to a program loaded from the card.
;
; THE PROBLEM
; -----------
; A loadable image is linked for $01:0000: that is where the HPS loader drops
; it, where boot/boot.s looks for the "X816" magic, and where Calypsi's absolute
; long addressing expects the code to be. It is not relocatable -- an image
; running anywhere else would call into whatever happened to sit at the
; addresses baked into its own jsl operands.
;
; But the SHELL is at $01:0000 too, for exactly the same reasons. So `run` has
; to overwrite the code that is doing the running.
;
; THE SHAPE
; ---------
; Three steps, and the middle one is why this file exists:
;
;   1. The shell (still intact, at $01:xxxx) reads the whole file into the
;      staging area at $10:0000 using fat32_read_far, which DMAs whole clusters
;      straight to a flat address. Nothing is overwritten yet.
;   2. This blob -- running from bank $00, which the copy does not touch --
;      moves the image down to $01:0000.
;   3. It jumps to $01:0004, the entry point in the image's own header. The
;      shell no longer exists at that point, which is correct: KERNEL.md's EXEC
;      "does not return on success".
;
; Step 2 CANNOT live in bank $01. It would be overwriting itself mid-copy. That
; makes bank $00 a requirement here, not an optimisation -- worth saying plainly
; because a previous attempt to put the SMC bit-banging in bank $00 for speed
; was measured to be worthless and reverted. This is a different argument: not
; "bank $00 is faster" but "the destination is being erased".
;
; So the blob is assembled into the image like any other code and copied to
; bank $00 at startup, which makes it POSITION INDEPENDENT by requirement: no
; jsr, no branch to an absolute address, and no reference to itself. Every
; absolute address it does use -- the staging area, the destination, the length
; variable -- is fixed and does not move when the blob does.
;
; SIZE LIMIT
; ----------
; X is 16 bits, so one pass copies at most 64 KB. Every image in this tree is
; far smaller (the shell itself is currently ~22 KB), and the shell refuses anything
; larger with a clear message rather than truncating it. Lifting the limit means
; an outer loop over banks; the check is in cmd_run, not here.
; ============================================================================

              .rtmodel version, "1"
              .rtmodel core, "65816"
              .rtmodel codeModel, "large"

              .public x816_exec_init, x816_exec, x816_exec_len

; X816_EXEC_STAGE (where the shell parks the image), X816_PROG_BASE (where it
; has to run) and X816_PROG_ENTRY (the jmp in the image's own header). All
; three are shared with shell.c, goshell.c, kexec.c, boot/boot.s, x816.sv and
; both linker scripts, so they come from the generated contract rather than
; from three .equ lines that only this file could see.
#include "x816_contract.inc"

; ---- bank $00 landing zone -------------------------------------------------
; `near` is placed in HiRAM ($00:A000-$00:FEFF) by x816-lib.scm. 64 bytes is
; ample for the loop below and leaves room to grow.
EXEC_RAM_SIZE: .equ   64

              .section near,bss
              .public x816_exec_ram
x816_exec_ram: .space EXEC_RAM_SIZE

; Byte count, written by C before the handover. In bank $00, at a link-time
; address, so the relocated blob can still reach it with plain absolute
; addressing -- the blob moves, this does not.
              .section near,bss
x816_exec_len: .space 2

              .section code

; ----------------------------------------------------------------------------
; void x816_exec_init(void);
;
; Copy the blob into bank $00. Call once, from con_init, long before it is
; needed -- the copy must happen while the source in bank $01 is still intact.
; ----------------------------------------------------------------------------
x816_exec_init:
              rep     #0x10                   ; 16-bit index
              sep     #0x20                   ; 8-bit A
              ldx     ##0
x816_exec_init_loop:
              lda     long:x816_exec_blob,x
              sta     long:x816_exec_ram,x
              inx
              cpx     ##(x816_exec_blob_end - x816_exec_blob)
              bne     x816_exec_init_loop
              rep     #0x30
              rtl

; ----------------------------------------------------------------------------
; void x816_exec(void);   -- DOES NOT RETURN
;
; Set x816_exec_len first. A tail jump into bank $00: nothing is pushed,
; because there is nothing to come back to.
; ----------------------------------------------------------------------------
x816_exec:
              jmp     long:x816_exec_ram

; ----------------------------------------------------------------------------
; The blob. Runs from bank $00; the copy here in bank $01 is only the master
; image and is destroyed by the very copy it performs.
; ----------------------------------------------------------------------------
x816_exec_blob:
              sei                             ; no interrupt may land in the
                                              ; middle of erasing the program
                                              ; that owns the vectors
              rep     #0x10                   ; 16-bit index
              sep     #0x20                   ; 8-bit A
              ldx     ##0
x816_exec_copy:
              lda     long:X816_EXEC_STAGE,x
              sta     long:X816_PROG_BASE,x
              inx
              cpx     x816_exec_len
              bne     x816_exec_copy

              rep     #0x30                   ; hand over as cstartup expects:
                                              ; 16-bit A and index
              jmp     long:X816_PROG_ENTRY
x816_exec_blob_end:

; ----------------------------------------------------------------------------
; void x816_fw_enter(void);   -- DOES NOT RETURN
;
; Hand the machine to the RESIDENT KERNEL at its firmware entry ($F0:0004,
; doc/KERNEL.md section 7). Callers must have checked the firmware magic
; first (goshell.c does): with no kernel loaded this jumps into SDRAM noise.
; ----------------------------------------------------------------------------
              .public x816_fw_enter
x816_fw_enter:
              jmp     long:X816_FW_ENTRY

; ----------------------------------------------------------------------------
; void x816_go(void);   -- DOES NOT RETURN
;
; Enter an image already sitting AT $01:0000 -- the OSD "Load Image" slot --
; in place: no staging, no relocation. With a resident kernel owning boot,
; an OSD-loaded image no longer starts by itself (the firmware wins the magic
; race in boot.s), so the shell's `go` command is the explicit hand-over; it
; checks the magic before calling.
; ----------------------------------------------------------------------------
              .public x816_go
x816_go:
              jmp     long:X816_PROG_ENTRY
