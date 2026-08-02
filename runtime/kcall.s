; ============================================================================
; kcall.s -- call the kernel by entry number, from C.
;
; Assembly callers just `jsl $00FExx` with a constant and are done. C cannot:
; the entry number is a variable, and the 65816 has no jsl through a register
; or a pointer. So this synthesises the call.
;
;     kern_c = ...; kern_x = ...; kern_y = ...;
;     result = kern_call(K_CON_PUTC);
;     if (kern_carry) { ... result is an error code ... }
;
; WHY THE ARGUMENTS GO IN GLOBALS AND NOT PARAMETERS
; --------------------------------------------------
; Because Calypsi's C convention is not what a four-argument prototype looks
; like. Disassembling a real call site shows argument one in A, arguments two
; and THREE in direct-page pseudo-registers ($00 and $02), and only the fourth
; pushed:
;
;     PEA $0000     ; arg 4
;     STZ $02       ; arg 3
;     STZ $00       ; arg 2
;     LDA #$0030    ; arg 1
;     JSL kern_call
;
; A two-argument function passes its second on the stack instead, so the rule
; changes with arity. A shim that reverse-engineers it per signature is a shim
; that breaks when the compiler changes its mind -- and it broke here first
; time out, reading arguments from a stack they were never on.
;
; One argument is unambiguous under any of those rules: it is in A. So the
; entry number is the only parameter, and the register arguments travel in
; globals this file owns. Not re-entrant, which costs nothing on a machine
; with one thread and a kernel whose real ABI is registers anyway.
;
; HOW THE CALL IS SYNTHESISED
; ---------------------------
; `jmp [abs]` (JML indirect long) is the only transfer that takes its target
; from memory, and it is a JUMP -- it pushes nothing. The thunk on the other
; end finishes with `rtl`, which pulls a three-byte return address, so that
; address is pushed by hand first:
;
;     phk                 push the program bank
;     per  back-1         push the address, PC-RELATIVE so it stays correct
;                         wherever this code is linked or copied
;     jmp  [target]       transfer; the entry's own `jmp long:` does the rest
;
; `per` is what keeps this position independent. `pea back-1` would bake in a
; link-time address and break the moment the kernel moves to bank $02, which
; is exactly where it is going.
; ============================================================================

              .rtmodel version, "1"
              .rtmodel core, "65816"
              .rtmodel codeModel, "large"

              .public kern_call, kern_carry, kern_c, kern_x, kern_y

#include "x816_contract.inc"

              .section near,bss
; The indirect target lives in bank $00 because `jmp [abs]` reads its pointer
; from bank $00 whatever the program bank is.
kcall_tgt:    .space  3
kern_c:       .space  2               ; -> C on entry, and the result on return
kern_x:       .space  2               ; -> X
kern_y:       .space  2               ; -> Y
kern_carry:   .space  2               ; 1 = the ABI reported failure

              .section code

; uint16_t kern_call(uint16_t n);
kern_call:
              ; A = entry number, the only parameter, so it is in A under any
              ; of Calypsi's argument rules.
              asl     a
              asl     a                       ; entry number -> byte offset
              clc                             ; (KERN_ENTRY_SIZE = 4)
              adc     ##KERN_TABLE
              sta     long:kcall_tgt          ; low 16 bits of the target
              sep     #0x20
              lda     #0
              sta     long:kcall_tgt + 2      ; bank $00
              rep     #0x20

              phk                             ; return bank
              per     kern_call_back - 1      ; return address, PC-relative
              ; Absolute-long addressing is an accumulator-only mode -- there
              ; is no `ldx long:` or `ldy long:` -- so every argument arrives
              ; through A and is transferred out. Loaded last-to-first, which
              ; leaves the one that must stay in A loaded last.
              lda     long:kern_y
              tay
              lda     long:kern_x
              tax
              lda     long:kern_c
              jmp     [kcall_tgt]

kern_call_back:
              ; Result in A, carry as the ABI left it.
              ;
              ; pha and lda both leave carry alone, so it survives long enough
              ; for `rol` to shift it straight into bit 0 -- turning the flag
              ; into the 0 or 1 C can read, with no branch.
              pha                             ; park the result
              lda     ##0
              rol     a                       ; carry -> bit 0
              sta     long:kern_carry
              ; X IS ALSO A RESULT, and this used to be dropped on the floor.
              ; FS_SIZE returns its high 16 bits in X and MEM_ALLOC returns the
              ; bank there, so a C caller had no way to read either -- and
              ; kfstest.c's "the high half must be zero" check was reading back
              ; the zero its own call1() had just written into kern_x, which
              ; means it could not fail. Storing X here is what makes that
              ; assertion real.
              ;
              ; Safe where it is: carry has already been consumed by the rol
              ; above, and txa/sta touch only N and Z.
              txa
              sta     long:kern_x
              pla                             ; result back into A
              sta     long:kern_c             ; also readable as kern_c
              rtl
