; ============================================================================
; edit_blob.s -- resident X816_Edit ca65 blob.
;
; The editor is still being ported. This file only places the ca65-built blob
; at a firmware bank-local $2000 so its internal absolute JSR/label addresses
; are coherent once the shell is ready to enter it for real.
; ============================================================================

#include "x816_contract.inc"

              .section code

              .public x816_edit_smoke_exit_on_x
              .public x816_edit_from_shell_empty
              .public x816_edit_from_shell_path
              .public x816_edit_shell_path

              .section far,bss
x816_edit_shell_path:
              .space 80

              .section code

x816_edit_smoke_exit_on_x:
              php
              sep     #0x20
              lda     #1
              sta     long:0x0007ff
              plp
              rtl

x816_edit_from_shell_empty:
              php
              rep     #0x30
              lda     ##0
              ldx     ##0
              jsl     long:0x00fe88
              plp
              rtl

; The smoke variants go in by k_edit_raw instead of the K_EDIT slot, because
; the slot now ZEROES the smoke request on the way in -- it has to, since that
; byte lives in bank $00 where a caller's own buffers are (durexForth's TIB is
; $0600-$07FF and covers it). These four commands are the only things entitled
; to ask for a smoke path, and they set the byte immediately before calling.
              .extern k_edit_raw
              .public x816_edit_smoke_empty, x816_edit_smoke_path

x816_edit_smoke_empty:
              php
              rep     #0x30
              lda     ##0
              ldx     ##0
              jsl     k_edit_raw
              plp
              rtl

x816_edit_smoke_path:
              php
              rep     #0x30
              lda     ##.word0 (x816_edit_shell_path)
              ldx     ##.byte2 (x816_edit_shell_path)
              jsl     k_edit_raw
              plp
              rtl

x816_edit_from_shell_path:
              php
              rep     #0x30
              lda     ##.word0 (x816_edit_shell_path)
              ldx     ##.byte2 (x816_edit_shell_path)
              jsl     long:0x00fe88
              plp
              rtl

              .section editblob, noreorder

              .public x816_edit_resident_entry
              .public x816_edit_blob_probe
              .public x816_edit_blob_end

x816_edit_resident_entry:
              .incbin "../../../X816_Edit/build/x816-edit.bin"
x816_edit_blob_end:

; Safe call target for low-risk linkage checks.
x816_edit_blob_probe:
              rtl
