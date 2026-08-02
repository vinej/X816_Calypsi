; ============================================================================
; ccursor.s -- a blinking cursor for the console, on the VSYNC interrupt.
;
; The first thing built on IRQ_SET, and a good demonstration of why the slot
; model is shaped the way it is: this installs in KIRQ_VSYNC and is never
; called for anything else.
;
; WHY IT IS ASSEMBLY. doc/KERNEL.md 5.6 calls handlers with D = $0000,
; DBR = $00 and 16-bit registers, reached by jsl and expected to finish with
; rtl. A Calypsi C function would run its prologue against pseudo-registers at
; whatever D happened to be -- which is the kernel's $2000 in the resident
; build and the caller's $0000 in a program. So the handler is here, and C
; gets ccur_on/ccur_off.
;
; WHY IT USES VERA'S SECOND PORT, AND SAVES THE ADDRESS ANYWAY
;
; doc/KERNEL.md 5.6 is explicit that the dispatcher never touches VERA's
; address registers or data ports, because an interrupt can land between a
; console write setting the address and the store that uses it. A cursor
; cannot honour that -- drawing IS touching them -- so it has to make itself
; transparent instead.
;
; Port 1 is the right choice because console.c drives everything through port
; 0 (ADDRSEL 0). But port 1 is NOT free: con_scroll uses it as the copy
; destination, so an interrupt landing mid-scroll would find a live address
; there. Hence both halves:
;
;   * CTRL is saved and restored, so port 0's selection and address are never
;     disturbed at all -- whatever half-finished con_putc was interrupted
;     resumes with its address exactly as it left it.
;   * Port 1's ADDR_L/M/H are read back and restored, so a scroll in progress
;     also resumes untouched.
;
; That makes the handler invisible to both, which is the property that matters:
; the alternative is a console that corrupts one character in a few thousand,
; only while the cursor happens to blink, which is the kind of bug that gets
; blamed on the hardware.
;
; WHY WRITING THE ATTRIBUTE IS ENOUGH
;
; The cell is two bytes -- glyph then attribute -- and console.c writes the
; SAME attribute everywhere (its ATTR, white on black). So the cursor is drawn
; by writing the reversed attribute to the second byte and undrawn by writing
; the normal one back: the glyph underneath is never touched, and "restore"
; is a constant rather than something that has to be remembered. That is also
; why a scroll needs no special handling -- every cell's attribute is the same
; before and after.
; ============================================================================

              .rtmodel version, "1"
              .rtmodel core, "65816"
              .rtmodel codeModel, "large"

              .public ccur_on, ccur_off

; con_curx/con_cury are console.c's cursor position. They were `static uint8_t
; curx, cury` until this file needed them; nothing else should write them.
              .extern con_curx, con_cury

#include "x816_contract.inc"

K_IRQ_SET_ENTRY: .equ KERN_TABLE + K_IRQ_SET * KERN_ENTRY_SIZE

VERA_ADDR_L:  .equ X816_VERA + 0
VERA_ADDR_M:  .equ X816_VERA + 1
VERA_ADDR_H:  .equ X816_VERA + 2
VERA_DATA1:   .equ X816_VERA + 4
VERA_CTRL:    .equ X816_VERA + 5

; MUST MATCH console.c's ATTR. Two bytes in two files is a duplication worth
; naming: if they drift, the cursor "undraws" to a colour the rest of the
; screen is not using and leaves a trail behind the prompt.
ATTR_NORMAL:  .equ 0x01               ; white on black -- console.c ATTR
ATTR_CURSOR:  .equ 0x10               ; the same two colours, swapped

; VERA's frame is 59.52 Hz, so 30 frames is almost exactly half a second --
; on for half, off for half.
CCUR_BLINK:   .equ 30

; VRAM bank 0, address increment 1: the value console.c's set_addr uses.
ADDR_H_INC1:  .equ 0x10

; ---------------------------------------------------------------------------
; State. Bank $00 either way; which SECTION is conditional for the same reason
; kirq.s's is -- the resident kernel's KernRAM is full and its direct page is
; not. See kirq.s and doc/KERNEL.md 10.
; ---------------------------------------------------------------------------
#ifdef KERNEL_RESIDENT
              .section ztiny,bss
#else
              .section near,bss
#endif
ccur_armed:   .space  1               ; ccur_on/off
ccur_shown:   .space  1               ; is the reversed attribute on screen?
ccur_tick:    .space  1               ; frames since the last toggle
ccur_lastx:   .space  1               ; where it was last drawn -- NOT where
ccur_lasty:   .space  1               ; the console's cursor is now
ccur_attr:    .space  1
ccur_sctrl:   .space  1               ; saved VERA CTRL
ccur_saddr:   .space  3               ; saved port-1 address

              .section code

; ---------------------------------------------------------------------------
; ccur_put -- write A to the attribute byte of the cell at ccur_lastx/lasty,
; leaving VERA exactly as it was found. Entered with 8-bit A.
;
; The saved values go in memory rather than on the stack because this runs
; inside an interrupt on whatever stack was current, and five pushes and five
; pulls in the right order is a thing to get wrong for no gain.
; ---------------------------------------------------------------------------
ccur_put:
              sta     long:ccur_attr
              lda     long:VERA_CTRL
              sta     long:ccur_sctrl
              lda     #1                      ; ADDRSEL 1 -- port 1
              sta     long:VERA_CTRL
              lda     long:VERA_ADDR_L
              sta     long:ccur_saddr
              lda     long:VERA_ADDR_M
              sta     long:ccur_saddr + 1
              lda     long:VERA_ADDR_H
              sta     long:ccur_saddr + 2

              ; The map is 128 cells wide, so a cell is at y*256 + x*2 and the
              ; attribute one byte after it: ADDR_M is the row and ADDR_L the
              ; doubled column, with no multiply. console.c's comment explains
              ; why the map is 128 wide and not 80 -- this is what it buys.
              ; ccur_lastx/lasty, NOT con_curx/cury: this has to be able to
              ; undraw the cell the cursor has just LEFT, and the console's
              ; position has already moved on by then.
              lda     long:ccur_lastx
              asl     a
              inc     a                       ; +1: the attribute, not the glyph
              sta     long:VERA_ADDR_L
              lda     long:ccur_lasty
              sta     long:VERA_ADDR_M
              lda     #ADDR_H_INC1
              sta     long:VERA_ADDR_H
              lda     long:ccur_attr
              sta     long:VERA_DATA1

              lda     long:ccur_saddr         ; put port 1 back
              sta     long:VERA_ADDR_L
              lda     long:ccur_saddr + 1
              sta     long:VERA_ADDR_M
              lda     long:ccur_saddr + 2
              sta     long:VERA_ADDR_H
              lda     long:ccur_sctrl
              sta     long:VERA_CTRL
              rts

ccur_show:
              lda     #ATTR_CURSOR
              jsr     .word0 (ccur_put)
              lda     #1
              sta     long:ccur_shown
              rts

ccur_hide:
              lda     #ATTR_NORMAL
              jsr     .word0 (ccur_put)
              lda     #0
              sta     long:ccur_shown
              rts

; ---------------------------------------------------------------------------
; The VSYNC handler. Entered by jsl from the kernel's dispatcher with 16-bit
; A/X/Y, D = $0000 and DBR = $00; every register is free and the source has
; already been acknowledged.
;
; It re-reads the console's cursor position every frame rather than being told
; when it moves. That costs two loads and removes the need for console.c to
; call anything on every character -- and it makes SCROLLING free, because a
; scroll changes con_cury like anything else.
; ---------------------------------------------------------------------------
ccur_vsync:
              sep     #0x20                   ; 8-bit A; X and Y stay wide
              lda     long:ccur_armed
              beq     ccur_ret

              lda     long:con_curx           ; has it moved since last time?
              cmp     long:ccur_lastx
              bne     ccur_moved
              lda     long:con_cury
              cmp     long:ccur_lasty
              beq     ccur_blink

ccur_moved:
              ; Undraw where it WAS before following it, or the old cell keeps
              ; the reversed attribute and the prompt grows a trail. ccur_hide
              ; already addresses through ccur_lastx/lasty, which still hold
              ; the old cell at this point -- that is why ccur_put reads those
              ; and not the console's live position.
              lda     long:ccur_shown
              beq     ccur_moveto
              jsr     .word0 (ccur_hide)
ccur_moveto:
              lda     long:con_curx
              sta     long:ccur_lastx
              lda     long:con_cury
              sta     long:ccur_lasty
              lda     #0
              sta     long:ccur_tick
              jsr     .word0 (ccur_show)      ; appear at once after a keypress
              bra     ccur_ret

ccur_blink:
              lda     long:ccur_tick
              inc     a
              sta     long:ccur_tick
              cmp     #CCUR_BLINK
              bcc     ccur_ret
              lda     #0
              sta     long:ccur_tick
              lda     long:ccur_shown
              bne     ccur_blink_off
              jsr     .word0 (ccur_show)
              bra     ccur_ret
ccur_blink_off:
              jsr     .word0 (ccur_hide)

ccur_ret:
              rep     #0x30
              rtl

; ---------------------------------------------------------------------------
; void ccur_on(void);  -- start blinking at the console's cursor
; void ccur_off(void); -- stop, and leave the cell as ordinary text
;
; Both are called from C, so both leave by rtl. ccur_on forces lastx to a
; column that cannot occur, which makes the first tick take the "it moved"
; path and draw immediately rather than waiting half a second.
; ---------------------------------------------------------------------------
ccur_on:
              rep     #0x30
              ldy     ##.byte2 (ccur_vsync)
              ldx     ##.word0 (ccur_vsync)
              lda     ##KIRQ_VSYNC
              jsl     K_IRQ_SET_ENTRY
              sep     #0x20
              lda     #0
              sta     long:ccur_shown
              sta     long:ccur_tick
              lda     #0xFF                   ; no such column: force a redraw
              sta     long:ccur_lastx
              lda     #1
              sta     long:ccur_armed
              rep     #0x30
              rtl

ccur_off:
              sep     #0x20
              lda     #0
              sta     long:ccur_armed         ; disarm FIRST: from here the
                                              ; handler returns immediately, so
                                              ; the undraw below cannot race it
              lda     long:ccur_shown
              beq     ccur_off_clear
              jsr     .word0 (ccur_hide)
ccur_off_clear:
              rep     #0x30
              ldy     ##0                     ; and give the slot back
              ldx     ##0
              lda     ##KIRQ_VSYNC
              jsl     K_IRQ_SET_ENTRY
              rtl
