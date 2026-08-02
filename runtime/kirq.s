; ============================================================================
; kirq.s -- the kernel's interrupt front end: vectors, dispatch, and the
;           IRQ_SET / TIME_GET / TIME_SET / IRQ_FRAMES entries.
;
; doc/KERNEL.md section 5.6 in the core repo is the specification. This is the
; whole of the implementation: nothing else in the kernel touches an interrupt.
;
; WHY THE FIRST-LEVEL HANDLER CANNOT LIVE WITH THE REST OF THE KERNEL
; -------------------------------------------------------------------
; The 65816's native vectors at $00:FFE4-$00:FFEF are SIXTEEN bits. The CPU
; jumps into bank $00 for every interrupt, full stop -- there is no bank byte
; in the vector and no register that supplies one. The kernel's code is in the
; firmware region at $F0:0000, which a 16-bit vector cannot name.
;
; So kirq_install stamps a four-byte `jmp long:` trampoline into bank $00 for
; each vector and points the vector at that. Same technique as kern_install and
; x816_exec_init: assemble the real instructions in the code section where the
; assembler can compute the 24-bit operands, then copy the bytes down. Building
; them by hand from a label's bank byte would need an addressing idiom this
; assembler does not have; copying an assembled template needs none.
;
; ONE SLOT PER SOURCE, NOT ONE PER VECTOR
; ---------------------------------------
; The CPU has one IRQ vector with seven devices behind it. Deciding which one
; fired is this file's job -- a program that wants a raster split should not
; have to service the audio FIFO to get it. So IRQ_SET indexes SOURCES
; (KIRQ_VSYNC..KIRQ_YM), and KIRQ_SPURIOUS catches an IRQ that no enabled
; source claimed.
;
; THE ENVIRONMENT A HANDLER IS CALLED IN -- normative
; ---------------------------------------------------
;   native mode, M=0 and X=0 (16-bit A, X and Y)
;   D   = $0000
;   DBR = $00
;   reached by jsl, so it must finish with rtl
;   A, X and Y are free; the dispatcher saved the interrupted code's
;
; D = $0000 rather than the kernel's direct page, and that is a decision worth
; stating. It is what the X16 KERNAL does before it calls CINV, it is what
; x16lib's variables at $22-$31 need, and above all it does not depend on what
; happened to be running when the interrupt landed. A handler wanting its own
; direct page has 16-bit registers to set one; a handler that assumed it got
; the interrupted code's D would work until the interrupt arrived during a
; kernel call. This file uses `long:` addressing throughout for the same
; reason, so its own state is reachable whatever D is.
;
; A HANDLER MUST NOT ENABLE INTERRUPTS. There is one scratch pointer and one
; ISR snapshot; re-entering the dispatcher would use both twice.
;
; WHAT THE DISPATCHER TOUCHES, AND WHY THE LIST IS SHORT
; ------------------------------------------------------
; VERA's ISR and IEN ($9F26/$9F27), the two VIAs' IFR/IER, and the YM's status
; -- and NOTHING else. In particular it never goes near VERA's address
; registers or its data ports at $9F20-$9F24. That is a requirement, not a
; coincidence: an interrupt can land between a console write setting the VERA
; address and the store that uses it, and a dispatcher that reprogrammed the
; address port would corrupt the interrupted write with no trace. Handlers do
; not get this for free -- one that touches CTRL, ADDRSEL/DCSEL or a data port
; must save and restore what it found, exactly as x16lib's irq.asm has always
; warned.
;
; THE STUCK-SOURCE DEFENCE
; ------------------------
; Every IRQ source here is LEVEL sensitive. If one asserts and nothing clears
; it, `rti` returns to an instruction that is immediately interrupted again,
; forever -- a machine that is completely dead with no diagnostic and no way in.
; That is the worst failure this file can produce, so it is designed out rather
; than documented around:
;
;   VSYNC, LINE, SPRCOL   acknowledged by the dispatcher itself, ALWAYS,
;                         before any handler runs and whether or not one is
;                         installed
;   AFLOW                 cannot be acknowledged at all -- it clears only when
;                         something refills the audio FIFO. With no handler
;                         installed the dispatcher clears its ENABLE bit.
;   VIA1, VIA2            only the VIA's own registers can clear it, and the
;                         dispatcher does not know which of seven sources it
;                         was. With no handler it writes $7F to IER, disabling
;                         all of that VIA's interrupts.
;   YM2151                with no handler, register $14 gets $30: both timer
;                         flags reset and both timer IRQ enables cleared.
;
; Each of those records a bit in kirq_disabled, so a test can prove the defence
; fired rather than inferring it from a machine that did not hang. The cost is
; a footgun worth naming: ENABLE A SOURCE AFTER INSTALLING ITS HANDLER, never
; before, or the first interrupt turns the source back off.
; ============================================================================

              .rtmodel version, "1"
              .rtmodel core, "65816"
              .rtmodel codeModel, "large"

              .public kirq_install
              .public k_irq_set, k_time_get, k_time_set, k_irq_frames
              .public kirq_vec, kirq_frames, kirq_disabled, kirq_time_off
              .public kirq_tramp

; KIRQ_* slot numbers, the vector addresses, VERA's and the VIA's interrupt
; registers, X816_TIMER and KERR_BADARG -- all from the generated contract, so
; the slot numbering the library's const_kernel.asm publishes to programs and
; the offsets this file indexes with are the same table.
#include "x816_contract.inc"

; ---------------------------------------------------------------------------
; State. It MUST be in bank $00 -- that is structural, not a preference: the
; CPU's vectors are 16-bit and point into bank $00, and `jmp [abs]` takes its
; pointer from bank $00 whatever the program bank is, so kirq_tramp and
; kirq_vec have nowhere else to be.
;
; WHICH bank-$00 section, though, is assembly-time conditional -- the same
; split kerntab.s makes with KENTER/KLEAVE, and for a related reason.
;
; A PROGRAM linking a private copy has `near` in HiRAM ($00:A000-$00:FDFF)
; with kilobytes spare, so `near` is right there.
;
; The RESIDENT KERNEL does not. Its whole bank-$00 claim is the 4 KB at
; $2000-$2FFF (doc/KERNEL.md 3.1) and, measured before this file existed, its
; `near`/`data`/`zdata`/stacks filled KernRAM to 99% -- 38 bytes free, against
; the 92 needed here. But the kernel's own DIRECT PAGE, the other half of the
; same claim, was 7.8% used: Calypsi's pseudo-registers take 20 bytes of 256
; and the remaining 236 were doing nothing.
;
; So the resident build puts this state in `ztiny`, which x816-kernel.scm
; places in that direct page. It is bank $00, it is already inside the section
; 3.1 claim, and it needed no growth of the claim and no eviction of anything
; else. Nothing below uses direct-page ADDRESSING -- every access is `long:`
; -- so the section is purely about where the linker puts the bytes.
; ---------------------------------------------------------------------------
#ifdef KERNEL_RESIDENT
              .section ztiny,bss
#else
              .section near,bss
#endif

; KIRQ_SLOTS entries of {24-bit handler, one pad byte}. All zero means "no
; handler". The pad is what makes the stride four, so a slot number becomes a
; byte offset with two shifts instead of a multiply -- and it is written as
; zero on every install, so the high byte of a 16-bit read at +2 is the pad
; and not somebody else's data.
kirq_vec:     .space  KIRQ_SLOTS * KIRQ_SLOT_SIZE

; The four `jmp long:` trampolines the CPU vectors point at.
kirq_tramp:   .space  4 * KIRQ_TRAMPOLINE_SIZE

kirq_frames:  .space  2       ; VSYNC frames since install; wraps at 65536
kirq_time_off: .space 4       ; TIME_SET's epoch: reported ms = hardware + this
kirq_disabled: .space 2       ; bitmask: sources the stuck-source defence shut off
kirq_isr:     .space  2       ; VERA ISR & IEN, snapshotted once per dispatch
kirq_claimed: .space  2       ; did any source own this IRQ?
kirq_tgt:     .space  4       ; the slot being dispatched, for `jmp [abs]`
kirq_scr:     .space  16      ; scratch for the 32-bit arithmetic below

              .section code

; ---------------------------------------------------------------------------
; void kirq_install(void);
;
; Point the COP, BRK, NMI and IRQ vectors at this file, clear the table, and
; leave the machine in a defined interrupt state: VERA acknowledged, VSYNC the
; only enabled source, interrupts ON.
;
; ABORT ($00:FFE8) is deliberately left alone. x816.sv ties abort_n high, so
; there is no ABORT source in this machine and boot.s's trap should keep it.
; ---------------------------------------------------------------------------
kirq_install:
              php
              sei                             ; nothing may dispatch through a
                                              ; half-installed table

              ; ---- copy the assembled trampolines into bank $00 ----
              rep     #0x10
              sep     #0x20
              ldx     ##0
kirq_install_copy:
              lda     long:kirq_proto,x
              sta     long:kirq_tramp,x
              inx
              cpx     ##(kirq_proto_end - kirq_proto)
              bne     kirq_install_copy

              ; ---- clear the vector table and the counters ----
              rep     #0x30
              ldx     ##0
              lda     ##0
kirq_install_clear:
              sta     long:kirq_vec,x
              inx
              inx
              cpx     ##(KIRQ_SLOTS * KIRQ_SLOT_SIZE)
              bne     kirq_install_clear

              lda     ##0
              sta     long:kirq_frames
              sta     long:kirq_disabled
              sta     long:kirq_isr
              sta     long:kirq_claimed
              sta     long:kirq_time_off
              sta     long:kirq_time_off + 2

              ; ---- point the CPU vectors at the trampolines ----
              ; Each is a 16-bit address in bank $00, which is exactly what the
              ; vector holds. The trampoline order below is the order in
              ; kirq_proto.
              lda     ##kirq_tramp + 0 * KIRQ_TRAMPOLINE_SIZE
              sta     long:X816_VEC_COP
              lda     ##kirq_tramp + 1 * KIRQ_TRAMPOLINE_SIZE
              sta     long:X816_VEC_BRK
              lda     ##kirq_tramp + 2 * KIRQ_TRAMPOLINE_SIZE
              sta     long:X816_VEC_NMI
              lda     ##kirq_tramp + 3 * KIRQ_TRAMPOLINE_SIZE
              sta     long:X816_VEC_IRQ

              ; ---- a defined VERA interrupt state ----
              ; Acknowledge whatever was already pending BEFORE enabling
              ; anything: a stale VSYNC latched during boot would otherwise
              ; dispatch on the first cli, counting a frame that never
              ; happened.
              sep     #0x20
              lda     #0x0F
              sta     long:X816_VERA_ISR      ; write 1 to clear, all sources
              lda     #X816_VERA_IRQ_VSYNC
              sta     long:X816_VERA_IEN      ; VSYNC only; bit 7 (IRQLINE bit
                                              ; 8) cleared with it, which is
                                              ; correct for scanline 0-255
              rep     #0x30

              plp                             ; caller's I flag back...
              cli                             ; ...and then interrupts ON: the
                                              ; frame counter is the kernel's
                                              ; time base and must be running
              rtl

; ---------------------------------------------------------------------------
; The trampoline template. Assembled here so the assembler computes the 24-bit
; operands; copied into bank $00 by kirq_install and never executed in place.
; ORDER IS THE ABI between this table and the vector writes above.
; ---------------------------------------------------------------------------
kirq_proto:
              jmp     long:kirq_cop
              jmp     long:kirq_brk
              jmp     long:kirq_nmi
              jmp     long:kirq_irq
kirq_proto_end:

; ---------------------------------------------------------------------------
; PROLOGUE / EPILOGUE
;
; `rep #0x30` comes FIRST, before anything is pushed. The CPU does not change
; M or X on an interrupt, so the widths on entry are whatever the interrupted
; code was using -- and a prologue that pushed in one width and pulled in
; another would corrupt the stack for the interrupted program rather than for
; the handler, which is the kind of bug that surfaces three routines away.
; Forcing 16 bits makes every push and pull two bytes. The interrupted widths
; are restored by `rti` out of the P the CPU pushed, so nothing is lost.
; ---------------------------------------------------------------------------
KPROLOGUE     .macro
              rep     #0x30                   ; 16-bit A, X and Y -- see above
              pha
              phx
              phy
              phb
              phd
              pea     #0
              plb
              plb                             ; DBR = $00
              pea     #0
              pld                             ; D   = $0000
              .endm

KEPILOGUE     .macro
              pld
              plb
              ply
              plx
              pla
              rti
              .endm

; ---------------------------------------------------------------------------
; kirq_call -- dispatch one slot. X = slot number * KIRQ_SLOT_SIZE.
;
; Returns with Z SET if the slot was empty and nothing was called. Entered and
; left with 16-bit A/X/Y; the handler is reached by a synthesised jsl, the same
; phk/per/`jmp [abs]` shape kcall.s uses and for the same reason: `jmp [abs]`
; is the only transfer that takes its target from memory, and it pushes
; nothing, so the rtl-shaped return address is pushed by hand. `per` keeps it
; position independent, which matters because this code is linked into bank
; $F0 in the kernel and bank $01 in a test image.
;
; Clobbers A, X and Y -- by design. A handler is entitled to all three.
; ---------------------------------------------------------------------------
kirq_call:
              lda     long:kirq_vec,x
              sta     long:kirq_tgt
              lda     long:kirq_vec + 2,x     ; bank in the low byte, pad above
              sta     long:kirq_tgt + 2
              ora     long:kirq_tgt           ; all 24 bits zero = no handler
              beq     kirq_call_none

              phk
              per     kirq_call_back - 1
              jmp     [kirq_tgt]
kirq_call_back:
              lda     ##1                     ; and Z clear on the way out, so
              rts                             ; the caller sees "claimed"
kirq_call_none:
              lda     ##0                     ; Z set
              rts

; ---------------------------------------------------------------------------
; COP, BRK and NMI. Nothing to acknowledge on any of them: COP and BRK are
; software traps and the SMC's NMI is an edge that x816.sv has already
; stretched and released. With no handler installed each is a plain return,
; which is what boot.s's trap did and is still the right answer -- a BRK with
; nowhere to go should resume, not hang.
; ---------------------------------------------------------------------------
kirq_cop:
              KPROLOGUE
              ldx     ##KIRQ_COP * KIRQ_SLOT_SIZE
              jsr     .word0 (kirq_call)
              KEPILOGUE

kirq_brk:
              KPROLOGUE
              ldx     ##KIRQ_BRK * KIRQ_SLOT_SIZE
              jsr     .word0 (kirq_call)
              KEPILOGUE

kirq_nmi:
              KPROLOGUE
              ldx     ##KIRQ_NMI * KIRQ_SLOT_SIZE
              jsr     .word0 (kirq_call)
              KEPILOGUE

; ---------------------------------------------------------------------------
; IRQ -- the one with seven devices behind it.
; ---------------------------------------------------------------------------
kirq_irq:
              KPROLOGUE
              lda     ##0
              sta     long:kirq_claimed

              ; ---- 8-bit window: read VERA's state and acknowledge ----------
              sep     #0x20
              lda     long:X816_VERA_ISR
              and     long:X816_VERA_IEN
              ; IEN bit 7 is IRQLINE's ninth bit, NOT an enable -- and ISR bits
              ; 7:4 are the sprite collision groups. Masking to the low nibble
              ; is what stops a collision in group 8 being read as "AFLOW and
              ; three others are enabled".
              and     #0x0F
              sta     long:kirq_isr

              ; Acknowledge the three that can be acknowledged, now, before any
              ; handler runs. A source re-asserting inside its own handler must
              ; still be pending at the rti; acking afterwards would drop it.
              ; AFLOW is excluded because writing its bit does nothing.
              and     #(X816_VERA_IRQ_VSYNC | X816_VERA_IRQ_LINE | X816_VERA_IRQ_SPRCOL)
              sta     long:X816_VERA_ISR
              rep     #0x30

              ; ---- VSYNC ----------------------------------------------------
              lda     long:kirq_isr
              and     ##X816_VERA_IRQ_VSYNC
              beq     kirq_irq_no_vsync
              lda     long:kirq_frames        ; the kernel's own frame count
              inc     a                       ; advances whether or not anybody
              sta     long:kirq_frames        ; installed a handler
              lda     ##1
              sta     long:kirq_claimed
              ldx     ##KIRQ_VSYNC * KIRQ_SLOT_SIZE
              jsr     .word0 (kirq_call)
kirq_irq_no_vsync:

              ; ---- LINE -----------------------------------------------------
              lda     long:kirq_isr
              and     ##X816_VERA_IRQ_LINE
              beq     kirq_irq_no_line
              lda     ##1
              sta     long:kirq_claimed
              ldx     ##KIRQ_LINE * KIRQ_SLOT_SIZE
              jsr     .word0 (kirq_call)
kirq_irq_no_line:

              ; ---- SPRCOL ---------------------------------------------------
              lda     long:kirq_isr
              and     ##X816_VERA_IRQ_SPRCOL
              beq     kirq_irq_no_sprcol
              lda     ##1
              sta     long:kirq_claimed
              ldx     ##KIRQ_SPRCOL * KIRQ_SLOT_SIZE
              jsr     .word0 (kirq_call)
kirq_irq_no_sprcol:

              ; ---- AFLOW ----------------------------------------------------
              ; The one that cannot be acknowledged. Refilling the FIFO is the
              ; acknowledge, so only a handler can end it -- and with no
              ; handler the enable bit has to go, or this is an infinite loop.
              lda     long:kirq_isr
              and     ##X816_VERA_IRQ_AFLOW
              beq     kirq_irq_no_aflow
              lda     ##1
              sta     long:kirq_claimed
              ldx     ##KIRQ_AFLOW * KIRQ_SLOT_SIZE
              jsr     .word0 (kirq_call)
              bne     kirq_irq_no_aflow       ; a handler ran; it refilled
              sep     #0x20
              lda     long:X816_VERA_IEN
              and     #~X816_VERA_IRQ_AFLOW & 0xFF
              sta     long:X816_VERA_IEN
              rep     #0x30
              lda     long:kirq_disabled
              ora     ##X816_VERA_IRQ_AFLOW
              sta     long:kirq_disabled
kirq_irq_no_aflow:

              ; ---- VIA #1 ---------------------------------------------------
              ; IFR bit 7 is the VIA's own "one of my enabled sources is
              ; asserting". Which one, only the handler knows -- so with no
              ; handler the whole IER goes ($7F = clear every enable).
              sep     #0x20
              lda     long:X816_VIA1 + X816_VIA_IFR
              rep     #0x30
              and     ##0x0080
              beq     kirq_irq_no_via1
              lda     ##1
              sta     long:kirq_claimed
              ldx     ##KIRQ_VIA1 * KIRQ_SLOT_SIZE
              jsr     .word0 (kirq_call)
              bne     kirq_irq_no_via1
              sep     #0x20
              lda     #0x7F
              sta     long:X816_VIA1 + X816_VIA_IER
              rep     #0x30
              lda     long:kirq_disabled
              ora     ##0x0010                ; bit 4: VIA1 was silenced
              sta     long:kirq_disabled
kirq_irq_no_via1:

              ; ---- VIA #2 ---------------------------------------------------
              sep     #0x20
              lda     long:X816_VIA2 + X816_VIA_IFR
              rep     #0x30
              and     ##0x0080
              beq     kirq_irq_no_via2
              lda     ##1
              sta     long:kirq_claimed
              ldx     ##KIRQ_VIA2 * KIRQ_SLOT_SIZE
              jsr     .word0 (kirq_call)
              bne     kirq_irq_no_via2
              sep     #0x20
              lda     #0x7F
              sta     long:X816_VIA2 + X816_VIA_IER
              rep     #0x30
              lda     long:kirq_disabled
              ora     ##0x0020                ; bit 5: VIA2 was silenced
              sta     long:kirq_disabled
kirq_irq_no_via2:

              ; ---- YM2151 ---------------------------------------------------
              ; Status bits 0 and 1 are the two timer overflows. With no
              ; handler, register $14 gets $30: bits 5:4 reset both flags and
              ; bits 3:2 (the IRQ enables) go to zero with them.
              sep     #0x20
              lda     long:X816_YM
              rep     #0x30
              and     ##0x0003
              beq     kirq_irq_no_ym
              lda     ##1
              sta     long:kirq_claimed
              ldx     ##KIRQ_YM * KIRQ_SLOT_SIZE
              jsr     .word0 (kirq_call)
              bne     kirq_irq_no_ym
              sep     #0x20
              lda     #X816_YM_TIMER_REG
              sta     long:X816_YM            ; register select
              lda     #0x30
              sta     long:X816_YM + 1        ; reset both flags, disable both
              rep     #0x30
              lda     long:kirq_disabled
              ora     ##0x0040                ; bit 6: the YM was silenced
              sta     long:kirq_disabled
kirq_irq_no_ym:

              ; ---- nobody owned it ------------------------------------------
              ; Not necessarily a fault: the line is a wired AND of four
              ; devices, and a source can drop between the CPU latching the
              ; interrupt and this code reading the registers. The slot exists
              ; so that a machine which starts taking interrupts nothing
              ; explains has somewhere to put a diagnostic.
              lda     long:kirq_claimed
              bne     kirq_irq_done
              ldx     ##KIRQ_SPURIOUS * KIRQ_SLOT_SIZE
              jsr     .word0 (kirq_call)
kirq_irq_done:
              KEPILOGUE

; ============================================================================
; The kernel entries. None of these calls C, touches the kernel's direct page
; or reads anything through DBR, so none of them needs kerntab.s's
; KENTER/KLEAVE -- same reasoning as k_nosys and k_sys_version. They are
; reached by jsl through $00:FExx and leave by rtl with carry meaning what
; doc/KERNEL.md section 4 says.
; ============================================================================

; ---------------------------------------------------------------------------
; K_IRQ_SET (49): C = KIRQ_ slot, X = handler low 16, Y = handler bank.
;   -> carry clear, C:X = the PREVIOUS handler (C = low 16, X = bank)
;      carry set, C = KERR_BADARG for a slot that does not exist
;
; The previous handler is returned because chaining is the normal case: a
; program that wants to add a VSYNC action without taking the source away from
; whoever had it installs its own and calls the old one. Returning it costs
; nothing here and cannot be reconstructed by the caller afterwards.
;
; C:X = 0 means there was no previous handler. A slot is cleared by installing
; 0, which is why zero is not a legal handler address -- nothing can be
; executing at $00:0000 anyway, that is the direct page.
; ---------------------------------------------------------------------------
k_irq_set:
              rep     #0x30
              cmp     ##KIRQ_SLOTS
              bcs     k_irq_set_bad
              asl     a
              asl     a                       ; slot -> byte offset (stride 4)
              sta     long:kirq_scr           ; ...parked: X and Y are the
              txa                             ; handler and cannot be spent
              sta     long:kirq_scr + 2       ; new handler, low 16
              tya
              and     ##0x00FF                ; the bank is the LOW byte of Y;
              sta     long:kirq_scr + 4       ; zero above it becomes the pad
              lda     long:kirq_scr
              tax                             ; there is no `ldx long:`

              lda     long:kirq_vec,x         ; read the old one out first
              sta     long:kirq_scr + 6
              lda     long:kirq_vec + 2,x
              and     ##0x00FF
              sta     long:kirq_scr + 8

              ; The store is two 16-bit writes and the dispatcher reads all
              ; four bytes. An interrupt landing between them would find half
              ; of the new pointer and half of the old one and jump into the
              ; join, so the pair is atomic against dispatch.
              php
              sei
              lda     long:kirq_scr + 2
              sta     long:kirq_vec,x
              lda     long:kirq_scr + 4
              sta     long:kirq_vec + 2,x     ; bank, and the pad back to zero
              plp

              lda     long:kirq_scr + 8
              tax                             ; X = previous bank
              lda     long:kirq_scr + 6       ; C = previous low 16
              clc
              rtl
k_irq_set_bad:
              sec
              lda     ##KERR_BADARG
              rtl

; ---------------------------------------------------------------------------
; K_TIME_GET (50): -> C = milliseconds low 16, X = high 16. Never fails.
;
; The hardware counter at $9F90 plus TIME_SET's offset. Two 16-bit reads get
; all 32 bits COHERENTLY, and the order is not free: reading $9F90 latches bits
; 31:8 in the core, so the 16-bit read there returns ms[15:0] and the 16-bit
; read at $9F92 returns the top half of the SAME value. Reading them the other
; way round returns a value that was never true whenever the low half carries
; between the two.
; ---------------------------------------------------------------------------
k_time_get:
              rep     #0x30
              lda     long:X816_TIMER         ; MUST be first -- it latches
              clc
              adc     long:kirq_time_off
              tay                             ; park the low half
              lda     long:X816_TIMER + 2     ; the latched high half
              adc     long:kirq_time_off + 2
              tax
              tya
              clc
              rtl

; ---------------------------------------------------------------------------
; K_TIME_SET (51): C = milliseconds low 16, X = high 16. Never fails.
;
; The hardware counter is free-running and read-only -- it cannot be set, and
; making it settable would have cost a write path into a register whose whole
; value is that nothing can perturb it. So TIME_SET moves the EPOCH instead:
; it records (wanted - hardware), and TIME_GET adds it back. 32-bit two's
; complement wraps correctly, so setting a time earlier than the hardware
; count works without a sign anywhere.
; ---------------------------------------------------------------------------
k_time_set:
              rep     #0x30
              sta     long:kirq_scr           ; wanted, low 16
              txa
              sta     long:kirq_scr + 2       ; wanted, high 16
              lda     long:X816_TIMER         ; latches, as above
              sta     long:kirq_scr + 4
              lda     long:X816_TIMER + 2
              sta     long:kirq_scr + 6
              sec                             ; wanted - hardware
              lda     long:kirq_scr
              sbc     long:kirq_scr + 4
              sta     long:kirq_time_off
              lda     long:kirq_scr + 2       ; lda/sta leave carry alone, so
              sbc     long:kirq_scr + 6       ; the borrow crosses correctly
              sta     long:kirq_time_off + 2
              lda     ##0
              clc
              rtl

; ---------------------------------------------------------------------------
; K_IRQ_FRAMES (52): -> C = VSYNC frames since kirq_install. Never fails.
;
; Sixteen bits and it wraps, which is the intended contract: unsigned
; subtraction wraps with it, so `now - then` is the elapsed count across the
; wrap and nothing has to handle the boundary.
;
; This is NOT derivable from TIME_GET and that is why it earns an ABI slot.
; Frames are locked to the display and milliseconds are not, so anything that
; must not tear -- a raster effect, a page flip -- needs the frame count, while
; anything measuring duration wants the clock. They also disagree during an SD
; transfer: the millisecond counter keeps running (that is the point of it) and
; the frame count does not, because the CPU is frozen and cannot service VSYNC.
; ---------------------------------------------------------------------------
k_irq_frames:
              rep     #0x30
              lda     long:kirq_frames
              clc
              rtl
