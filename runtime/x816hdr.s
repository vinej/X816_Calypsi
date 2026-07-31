; ============================================================================
; x816hdr.s -- the 8-byte header every loadable X816 program starts with.
;
; The core's boot ROM (X816_Core boot/boot.s) copies itself out of the way,
; drops the ROM overlay, then looks for the four-byte magic "X816" at
; $01:0000 -- the base of the first SDRAM bank, which is where the HPS loader
; drops the file. If it matches, it jumps to $01:0004.
;
; So the first eight bytes of the image are fixed:
;
;     $01:0000  58 38 31 36        "X816"
;     $01:0004  5C lo hi bk        jmp long:__program_start
;
; The jump is what lets Calypsi's own cstartup do the work. __program_start
; is .pubweak in the toolchain's cstartup.s and already does exactly what
; X816 needs: clc/xce into native mode, rep #$38 for 16-bit registers with
; decimal off, set the stack from .sectionEnd stack, and set D from
; _DirectPageStart. Referencing it here is what pulls cstartup into the link.
;
; Placing the entry point via a jump rather than by trusting the linker to
; put __program_start first is deliberate: link order is not a contract, and
; a program whose entry point silently drifts off $01:0004 fails by running
; whatever happens to be there.
;
; This section is the program root -- link with
;     --program-root __x816_root_section
; which replaces the default __program_root_section (the reset vector, which
; X816 does not use: a loaded program cannot write bank 0's vector page).
; ============================================================================

              .rtmodel version, "1"
              .rtmodel core, "65816"
              .rtmodel codeModel, "large"
; No dataModel attribute: this file emits no data, and asserting one here
; makes the header refuse to link against a program built with a different
; model. The linker enforces agreement among the objects that DO have data.

              .extern __program_start
              .public __x816_root_section

              .section x816hdr, root, noreorder
__x816_root_section:
              .byte   "X816"                  ; magic, $01:0000
              jmp     long:__program_start    ; entry, $01:0004
