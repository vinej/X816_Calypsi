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
              .extern kexec
              .extern kmem_alloc, kmem_free

; The interrupt and clock entries live in kirq.s and are whole thunks in
; themselves: none of them calls C, so none needs KENTER/KLEAVE and there is
; nothing here to adapt. The generated table below reaches them by name.
              .extern k_irq_set, k_time_get, k_time_set, k_irq_frames

; KERN_TABLE, KERN_ENTRIES, KERR_NOSYS, KERN_DP and X816_FW_ENTRY, all from
; the generated contract -- the same table that produces the K_* numbers in
; kernel.h and the body of kern_proto below. KERN_DP is the kernel's direct
; page (KERNEL.md sections 3.1 and 4), which is also where Calypsi keeps the C
; runtime's pseudo-registers and C stack pointer, so switching D switches the
; whole C runtime; X816_FW_ENTRY is what K_EXIT restarts through.
#include "x816_contract.inc"

              .section code

; ----------------------------------------------------------------------------
; KENTER / KLEAVE -- the caller<->kernel context switch around every thunk
; that reaches C code.
;
; A caller is an arbitrary program with its own D (its Calypsi pseudo-
; registers) and its own DBR. The kernel's C runs on the kernel's direct page
; (KERN_DP -- see x816-kernel.scm) with DBR = $00 (small data model, kernel
; state in bank 0). The ABI promises D and DBR are preserved, and that the
; caller never has to know the kernel uses direct page at all.
;
; Register discipline: the thunk's arguments arrive in C, X and Y, so the
; switch must not touch them. `pea ##0 / plb / plb` sets DBR to zero one
; pushed byte at a time without going through A; `pea ##KERN_DP / pld` does D.
; On the way out pld/plb touch N and Z but NOT carry, which is what lets
; KLEAVE run after the carry flag has been given its ABI meaning.
;
; THE SWITCH IS ASSEMBLY-TIME CONDITIONAL, and has to be. In the RESIDENT
; kernel (assembled with -DKERNEL_RESIDENT, linked by x816-kernel.scm) the
; kernel's runtime lives at KERN_DP, initialised by the kernel's own cstartup
; -- the switch is what isolates it from the caller. In a PRIVATE COPY
; (kerntest and friends link this file into their own image) the "kernel" C
; IS the caller's runtime: same pseudo-registers, same C stack -- switching D
; there would hand the C code an uninitialised stack pointer. So the private
; build expands KENTER to nothing and KLEAVE to rtl, byte-identical to the
; thunks every existing test already proved.
; ----------------------------------------------------------------------------
#ifdef KERNEL_RESIDENT
KENTER        .macro
              phb                             ; caller DBR
              phd                             ; caller D
              pea     #KERN_DP
              pld                             ; D   = kernel direct page
              pea     #0
              plb                             ; DBR = $00 (kernel data bank)
              plb                             ; second byte of the pea, also 0
              .endm

KLEAVE        .macro
              pld                             ; caller D   (carry untouched)
              plb                             ; caller DBR (carry untouched)
              rtl
              .endm
#else
KENTER        .macro
              ; private copy: caller context IS the kernel context
              .endm

KLEAVE        .macro
              rtl
              .endm
#endif

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
              cpx     ##(KERN_ENTRIES * KERN_ENTRY_SIZE)
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
              KENTER
              jsl     con_putc
              clc
              KLEAVE

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
              KENTER                          ; _Dp below is the KERNEL's _Dp,
              sta     dp:.tiny _Dp            ; offset within the bank
              txa
              and     ##0x00FF
              sta     dp:.tiny (_Dp+2)        ; bank
              jsl     con_puts_far
              clc
              KLEAVE

; Both return SIXTEEN bits and the mask that used to be here threw the top
; byte away -- which is the byte that says "this is F1, not the CP437 glyph at
; $70". con_getkey already returns 0 for nothing waiting, so there is nothing
; to clean up.
k_con_getc:
              KENTER
              jsl     con_getc
              clc
              KLEAVE

k_con_getkey:
              KENTER
              jsl     con_getkey
              clc
              KLEAVE

k_con_cls:
              KENTER
              jsl     con_cls
              clc
              KLEAVE

; C = column, X = row. Second argument goes on the stack -- pushed AFTER
; KENTER, so the callee's frame offsets are unchanged.
k_con_gotoxy:
              KENTER
              phx
              jsl     con_gotoxy
              plx
              clc
              KLEAVE

; -> C = column, X = row. con_gety's result is parked on the stack and pulled
; straight into X, which costs nothing and avoids caring whether con_getx
; preserves X.
k_con_getxy:
              KENTER
              jsl     con_gety
              and     ##0x00FF
              pha
              jsl     con_getx
              and     ##0x00FF
              plx
              clc
              KLEAVE

; C = column, X = row, Y = glyph. Three arguments: the first stays in A and
; the other two are pushed in reverse, so they land at $04,S and $06,S in the
; order C declares them.
k_con_putraw:
              KENTER
              phy
              phx
              jsl     con_putraw
              plx
              ply
              clc
              KLEAVE

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
              KENTER
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
              KLEAVE
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
              KENTER
              sta     long:kfs_c
              jsl     kfs_size
              pha
              lda     long:kfs_x              ; high half, parked by kfs_size
              tax
              lda     long:kfs_carry
              lsr     a
              pla
              KLEAVE

; ----------------------------------------------------------------------------
; MEM_ALLOC (40): C:X = a 32-bit SIZE in, C:X = a 24-bit ADDRESS out.
;
; The one entry besides FS_SIZE that returns something in X, so it cannot use
; the KFS macro -- that macro spends X on the way in and never reloads it. The
; shape is k_fs_size's, and the reason is the same: the result is wider than
; the accumulator.
;
; kmem_alloc parks the bank in kfs_x on BOTH paths (zero on failure), so X is
; never left holding the size the caller passed in, which would look exactly
; like a plausible bank number.
; ----------------------------------------------------------------------------
k_mem_alloc:
              KENTER
              sta     long:kfs_c              ; size, low 16
              txa
              sta     long:kfs_x              ; size, high 16
              jsl     kmem_alloc
              pha
              lda     long:kfs_x              ; bank of the result
              tax
              lda     long:kfs_carry
              lsr     a
              pla
              KLEAVE

; MEM_FREE (41): C:X = address, nothing to return but carry.
k_mem_free:   KFS     kmem_free

; Everything not implemented in this build. A clean refusal, not a crash.
; No KENTER: it touches no kernel state.
k_nosys:
              sec
              lda     ##KERR_NOSYS
              rtl

; ----------------------------------------------------------------------------
; K_EXEC (32): C:X = path. The KFS window carries the pointer to kexec.c,
; which stages the image and calls exec.s -- on success it never returns and
; the new program owns the machine; on failure the macro's carry path reports.
; ----------------------------------------------------------------------------
k_exec:       KFS     kexec

; ----------------------------------------------------------------------------
; K_EXIT (33): return the machine to the resident kernel by restarting it at
; its firmware entry -- console re-init, card re-mount, prompt. Only valid
; when the firmware actually carries the kernel: a test image running its own
; private copy of this table has nothing at $F0:0004, so check the magic and
; refuse cleanly if it is absent (the same four bytes boot/boot.s checks).
; The exit status in C is accepted and discarded -- v1 semantics.
; ----------------------------------------------------------------------------
k_exit:
              sep     #0x20
              lda     long:X816_FW_BASE+0
              cmp     #X816_MAGIC_0           ; 'X'
              bne     k_exit_nofw
              lda     long:X816_FW_BASE+1
              cmp     #X816_MAGIC_1           ; '8'
              bne     k_exit_nofw
              lda     long:X816_FW_BASE+2
              cmp     #X816_MAGIC_2           ; '1'
              bne     k_exit_nofw
              lda     long:X816_FW_BASE+3
              cmp     #X816_MAGIC_3           ; '6'
              bne     k_exit_nofw
              rep     #0x30
              jmp     long:X816_FW_ENTRY      ; restart the resident kernel
k_exit_nofw:
              rep     #0x30
              sec
              lda     ##KERR_NOSYS
              rtl

; ----------------------------------------------------------------------------
; The prototype table: 64 entries of `jmp long:thunk`, four bytes each.
;
; Order IS the call numbering in kernel.h -- and it is now the SAME SOURCE.
; The body below is generated from X816_core/tools/contract.py, which also
; emits the K_* numbers; a slot's position here and its number there cannot
; disagree, which is exactly what two hand-kept lists could not promise.
; Adding a call means adding a row to that table, not editing this file.
; ----------------------------------------------------------------------------
kern_proto:
#include "x816_kerntab.inc"
kern_proto_end:

; -> C = (major << 8) | minor. Implemented here rather than in C because there
; is nothing to call: it is the one entry that is purely a constant.
k_sys_version:
              lda     ##0x0001                ; 0.1
              clc
              rtl
