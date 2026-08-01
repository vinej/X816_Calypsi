; ============================================================================
; kerntab.s -- the native kernel jump table and its thunks.
;
; The ABI is in runtime/kernel.h and X816_Core doc/KERNEL.md sections 3-5.
; This file is the other half: it builds the table at $00:FE00 and adapts the
; kernel ABI to the C functions underneath.
;
; WHY THE TABLE IS WRITTEN AT RUN TIME
; ------------------------------------
; It lives in bank $00, and the HPS loader only ever writes bank $01 -- bank
; $00 comes up as whatever was there. So the table cannot be placed by the
; linker; kern_install stamps it out of the entry list below. The linker script
; DOES reserve the page (HiRAM stops at $FDFF) so no `near` object can land on
; top of it.
;
; WHY EVERY ENTRY NEEDS A THUNK
; -----------------------------
; The two conventions do not line up:
;
;   kernel ABI   args in C, X, Y; carry clear on success with the result in C,
;                carry set on failure with an error code in C
;   Calypsi C    first argument in A, the rest PUSHED by the caller, caller
;                cleans up; return value in A; carry means nothing
;
; Verified by compiling call sites with -S rather than assumed, which is the
; only way to get this right: the rule is not uniform. Sixteen-bit arguments
; after the first are pushed in reverse (con_putraw's call site pushes the
; glyph, then the row, then puts the column in A), but a __far POINTER ignores
; that entirely and travels in the direct-page pseudo-registers _Dp and _Dp+2
; regardless of position. k_con_puts guessed the stack and printed nothing.
;
; So even a call that cannot fail needs at least a `clc`, because the ABI
; promises carry always reports. That promise is what lets a call grow a
; failure mode later without breaking every program already built.
;
; UNIMPLEMENTED ENTRIES ARE NOT ABSENT
; ------------------------------------
; Every one of the 64 slots is filled, and the ones with no implementation
; point at k_nosys, which returns carry set with KERR_NOSYS. A caller gets a
; clean error instead of a jump into whatever bank $00 happened to contain --
; and the numbering is fixed now, so filling a slot later is not an ABI break.
; ============================================================================

              .rtmodel version, "1"
              .rtmodel core, "65816"
              .rtmodel codeModel, "large"

              .public kern_install

              .extern con_putc, con_puts_far, con_getc, con_getkey
              .extern con_cls, con_gotoxy, con_getx, con_gety, con_putraw
              .extern _Dp                     ; Calypsi's direct-page registers

              .extern kfs_c, kfs_x, kfs_y, kfs_carry
              .extern kfs_open, kfs_close, kfs_read, kfs_write, kfs_seek
              .extern kfs_size, kfs_delete, kfs_rename
              .extern kfs_diropen, kfs_dirnext, kfs_dirclose
              .extern kfs_chdir, kfs_getcwd, kfs_mkdir, kfs_rmdir

KERN_TABLE:   .equ    0xFE00          ; bank $00, one page
KERN_ENTRIES: .equ    64

KERR_NOSYS:   .equ    1

              .section code

; ----------------------------------------------------------------------------
; void kern_install(void);
;
; Copy the 256-byte prototype below into $00:FE00. Entered from C with 16-bit
; A and index registers.
; ----------------------------------------------------------------------------
kern_install:
              phx
              rep     #0x10                   ; 16-bit index
              sep     #0x20                   ; 8-bit A
              ldx     ##0
kern_install_loop:
              lda     long:kern_proto,x
              sta     long:KERN_TABLE,x       ; bank $00 is implied by the
              inx                             ; 24-bit form
              cpx     ##(KERN_ENTRIES * 4)
              bne     kern_install_loop
              rep     #0x30
              plx
              rtl

; ----------------------------------------------------------------------------
; Thunks. Each is entered by jsl through the table with the kernel ABI in
; force, and must leave by rtl with carry meaning what the ABI says.
; ----------------------------------------------------------------------------

; C = character.
k_con_putc:
              jsl     con_putc
              clc
              rtl

; C:X = 24-bit pointer.
;
; A __far pointer is 32 bits in Calypsi and does NOT travel like a 16-bit
; argument: the compiler puts it in the direct-page pseudo-registers, low half
; in _Dp and high half in _Dp+2, whatever the argument position. Confirmed from
; the callee itself, which reads `lda [.tiny _Dp]` with no stack access at all.
;
; The high half is the bank in its low byte and zero above, because the top
; byte of a 24-bit address is zero by definition -- so the mask is what turns
; the ABI's "bank in the low byte of X" into a valid far pointer.
k_con_puts:
              sta     dp:.tiny _Dp            ; offset within the bank
              txa
              and     ##0x00FF
              sta     dp:.tiny (_Dp+2)        ; bank
              jsl     con_puts_far
              clc
              rtl

; Both return SIXTEEN bits and the mask that used to be here threw the top
; byte away -- which is the byte that says "this is F1, not the CP437 glyph at
; $70". con_getkey already returns 0 for nothing waiting, so there is nothing
; to clean up.
k_con_getc:
              jsl     con_getc
              clc
              rtl

k_con_getkey:
              jsl     con_getkey
              clc
              rtl

k_con_cls:
              jsl     con_cls
              clc
              rtl

; C = column, X = row. Second argument goes on the stack.
k_con_gotoxy:
              phx
              jsl     con_gotoxy
              plx
              clc
              rtl

; -> C = column, X = row. con_gety's result is parked on the stack and pulled
; straight into X, which costs nothing and avoids caring whether con_getx
; preserves X.
k_con_getxy:
              jsl     con_gety
              and     ##0x00FF
              pha
              jsl     con_getx
              and     ##0x00FF
              plx
              clc
              rtl

; C = column, X = row, Y = glyph. Three arguments: the first stays in A and
; the other two are pushed in reverse, so they land at $04,S and $06,S in the
; order C declares them.
k_con_putraw:
              phy
              phx
              jsl     con_putraw
              plx
              ply
              clc
              rtl

; ----------------------------------------------------------------------------
; Filesystem thunks. All fifteen are the SAME four steps, so they are one
; macro rather than fifteen chances to marshal something differently:
;
;   park C, X and Y in the ABI window kfs.c reads
;   call a C function that takes no arguments at all
;   turn kfs_carry back into the carry flag
;   rtl with the result still in A
;
; Zero-argument callees are the point. Calypsi's argument passing changes with
; arity and with type, so a thunk that marshals into a natural C signature has
; to encode that table correctly fifteen separate times; two such thunks were
; already written wrong before the rule was measured. This way the marshalling
; is written once and is either right for everything or wrong for everything.
;
; `lsr` is what puts the flag back: bit 0 of kfs_carry falls straight into
; carry, and `pla` afterwards touches N and Z but not C.
; ----------------------------------------------------------------------------
KFS           .macro  fn
              sta     long:kfs_c
              txa
              sta     long:kfs_x
              tya
              sta     long:kfs_y
              jsl     \fn
              pha
              lda     long:kfs_carry
              lsr     a
              pla
              rtl
              .endm

k_fs_open:    KFS     kfs_open
k_fs_close:   KFS     kfs_close
k_fs_read:    KFS     kfs_read
k_fs_write:   KFS     kfs_write
k_fs_seek:    KFS     kfs_seek
k_fs_delete:  KFS     kfs_delete
k_fs_rename:  KFS     kfs_rename
k_dir_open:   KFS     kfs_diropen
k_dir_next:   KFS     kfs_dirnext
k_dir_close:  KFS     kfs_dirclose
k_fs_chdir:   KFS     kfs_chdir
k_fs_getcwd:  KFS     kfs_getcwd
k_fs_mkdir:   KFS     kfs_mkdir
k_fs_rmdir:   KFS     kfs_rmdir

; FS_SIZE is the one that cannot use the macro: it returns 32 bits, low half in
; C and high half in X, and X is the register the macro has already spent.
k_fs_size:
              sta     long:kfs_c
              jsl     kfs_size
              pha
              lda     long:kfs_x              ; high half, parked by kfs_size
              tax
              lda     long:kfs_carry
              lsr     a
              pla
              rtl

; Everything not implemented in this build. A clean refusal, not a crash.
k_nosys:
              sec
              lda     ##KERR_NOSYS
              rtl

; ----------------------------------------------------------------------------
; The prototype table: 64 entries of `jmp long:thunk`, four bytes each.
;
; Order IS the call numbering in kernel.h. Changing a line here changes the
; ABI, so the numbers are written out rather than left implicit in position.
; ----------------------------------------------------------------------------
kern_proto:
              jmp     long:k_con_putc         ;  0 K_CON_PUTC
              jmp     long:k_con_puts         ;  1 K_CON_PUTS
              jmp     long:k_con_getc         ;  2 K_CON_GETC
              jmp     long:k_con_getkey       ;  3 K_CON_GETKEY
              jmp     long:k_con_cls          ;  4 K_CON_CLS
              jmp     long:k_con_gotoxy       ;  5 K_CON_GOTOXY
              jmp     long:k_con_getxy        ;  6 K_CON_GETXY
              jmp     long:k_con_putraw       ;  7 K_CON_PUTRAW
              jmp     long:k_nosys            ;  8
              jmp     long:k_nosys            ;  9
              jmp     long:k_nosys            ; 10
              jmp     long:k_nosys            ; 11
              jmp     long:k_nosys            ; 12
              jmp     long:k_nosys            ; 13
              jmp     long:k_nosys            ; 14
              jmp     long:k_nosys            ; 15
              jmp     long:k_fs_open          ; 16 K_FS_OPEN
              jmp     long:k_fs_close         ; 17 K_FS_CLOSE
              jmp     long:k_fs_read          ; 18 K_FS_READ
              jmp     long:k_fs_write         ; 19 K_FS_WRITE
              jmp     long:k_fs_seek          ; 20 K_FS_SEEK
              jmp     long:k_fs_size          ; 21 K_FS_SIZE
              jmp     long:k_fs_delete        ; 22 K_FS_DELETE
              jmp     long:k_fs_rename        ; 23 K_FS_RENAME
              jmp     long:k_dir_open         ; 24 K_DIR_OPEN
              jmp     long:k_dir_next         ; 25 K_DIR_NEXT
              jmp     long:k_dir_close        ; 26 K_DIR_CLOSE
              jmp     long:k_fs_chdir         ; 27 K_FS_CHDIR
              jmp     long:k_fs_getcwd        ; 28 K_FS_GETCWD
              jmp     long:k_fs_mkdir         ; 29 K_FS_MKDIR
              jmp     long:k_fs_rmdir         ; 30 K_FS_RMDIR
              jmp     long:k_nosys            ; 31
              jmp     long:k_nosys            ; 32 K_EXEC
              jmp     long:k_nosys            ; 33 K_EXIT
              jmp     long:k_nosys            ; 34
              jmp     long:k_nosys            ; 35
              jmp     long:k_nosys            ; 36
              jmp     long:k_nosys            ; 37
              jmp     long:k_nosys            ; 38
              jmp     long:k_nosys            ; 39
              jmp     long:k_nosys            ; 40 K_MEM_ALLOC
              jmp     long:k_nosys            ; 41 K_MEM_FREE
              jmp     long:k_nosys            ; 42
              jmp     long:k_nosys            ; 43
              jmp     long:k_nosys            ; 44
              jmp     long:k_nosys            ; 45
              jmp     long:k_nosys            ; 46
              jmp     long:k_nosys            ; 47
              jmp     long:k_sys_version      ; 48 K_SYS_VERSION
              jmp     long:k_nosys            ; 49
              jmp     long:k_nosys            ; 50
              jmp     long:k_nosys            ; 51
              jmp     long:k_nosys            ; 52
              jmp     long:k_nosys            ; 53
              jmp     long:k_nosys            ; 54
              jmp     long:k_nosys            ; 55
              jmp     long:k_nosys            ; 56
              jmp     long:k_nosys            ; 57
              jmp     long:k_nosys            ; 58
              jmp     long:k_nosys            ; 59
              jmp     long:k_nosys            ; 60
              jmp     long:k_nosys            ; 61
              jmp     long:k_nosys            ; 62
              jmp     long:k_nosys            ; 63
kern_proto_end:

; -> C = (major << 8) | minor. Implemented here rather than in C because there
; is nothing to call: it is the one entry that is purely a constant.
k_sys_version:
              lda     ##0x0001                ; 0.1
              clc
              rtl
