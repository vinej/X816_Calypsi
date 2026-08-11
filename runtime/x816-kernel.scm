;;; ==========================================================================
;;; x816-kernel.scm -- ln65816 memory map for the RESIDENT KERNEL image.
;;;
;;; The kernel is the shell linked to live in the FIRMWARE REGION
;;; (X816_Core doc/KERNEL.md section 3): code in banks $F0-$FF, HPS-loaded as
;;; games/X816/boot2.rom and WRITE-PROTECTED by the core, with its state and
;;; direct page in the kernel's bank-0 claim at $2000-$2FFF. That is what
;;; makes the $00:FE00 jump table durable: `run` erases $01:0000, and the
;;; thunks the table points at are no longer there.
;;;
;;; Differences from x816-lib.scm (the loadable-program map):
;;;
;;;   * DirectPage at $2000. Every kernel entry switches D here (kerntab.s
;;;     KENTER), which swaps in the kernel's OWN Calypsi pseudo-registers --
;;;     including its C stack pointer -- and restores the caller's D on exit.
;;;     The ABI's "the kernel switches to its own direct page" (KERNEL.md
;;;     section 4) is implemented by this address.
;;;   * stack/cstack/data/zdata all inside $2000-$2FFF -- the kernel budget of
;;;     KERNEL.md section 3.1. The hardware stack here is the KERNEL's prompt
;;;     stack; a program EXECed by the kernel sets its own from its own map.
;;;   * Code (and cdata/idata -- the large code model reads them far) in the
;;;     firmware region, magic header at $F0:0000, entry $F0:0004 -- the same
;;;     image shape boot/boot.s checks at PROG_BASE, relocated.
;;;   * No heap: nothing in the kernel allocates.
;;;
;;;   ln65816 x816-kernel.scm x816hdr.o <kernel objects> clib-lc-sd.a \
;;;           -o KERNEL.elf --output-format raw \
;;;           --program-root __x816_root_section --rtattr exit=simplified
;;; ==========================================================================

(define memories
  '(;; --- kernel direct page: pseudo-registers only ------------------------
    ;; Page-aligned (unaligned D costs a cycle on every dp access). Bounded
    ;; so growth is a link error, same policy as x816-lib.scm.
    ;;
    ;; Stops at $209F, not $20FF: $20A0-$20FF is the CARRY-OVER BLOCK (see
    ;; runtime/kfs.h), and it is deliberately NOT a section the linker can
    ;; fill. It holds the working directory ACROSS a K_EXIT, which restarts
    ;; this image through cstartup -- and anything the linker places is
    ;; something cstartup initialises, which is exactly what that block must
    ;; not be. Same arrangement as the jump table at $00:FE00, for the same
    ;; kind of reason.
    ;;
    ;; It is inside the kernel's claim on purpose: every program map already
    ;; carves $2000-$2FFF out, so the LOADABLE prompt reaches the same bytes
    ;; without needing a placement rule of its own.
    (memory DirectPage (address (#x002000 . #x00209f))
            (section (registers ztiny)))

    ;; --- kernel state: KERNEL.md section 3.1 gives $2000-$2FFF ------------
    ;; $2100-$2FFF after the direct page. Hardware stack + C stack + data.
    ;; If this overflows, the design rule applies: move the offender into the
    ;; firmware region (const) or an SDRAM buffer -- do not grow the claim
    ;; without updating KERNEL.md 3.1 and MEMORY_MAP.md together.
    (memory KernRAM (address (#x002100 . #x002fff))
            (section stack cstack data zdata znear near))

    ;; $00:FE00-$00:FEFF -- the jump table, written by kern_install at boot.
    ;; Not a linker section, same as x816-lib.scm.

    ;; --- the firmware region: banks $F0-$FF, write-protected --------------
    (memory FwHeader (address (#xf00000 . #xf00007))
            (section x816hdr))
    (memory FwCode (address (#xf00008 . #xffffff))
            (section code farcode cfar chuge
                     cdata idata switch data_init_table reset))

    ;; --- the kernel's own far data: bank $C0 -------------------------------
    ;; Unused by the small data model; present so a stray `far` object gets a
    ;; defined home instead of a link error nobody understands.
    ;;
    ;; IT USED TO BE AT $EF:0000, WHICH IS INSIDE THE VERA2 FRAMEBUFFER
    ;; ($E0:0000-$EF:FFFF, X816_VFB_BASE..X816_VFB_LAST). Nothing had broken
    ;; only because the small data model never placed anything here -- the
    ;; moment it did, a `far` object and the bitmap layer would have been the
    ;; same bytes. Worse, $E0-$EF is not even ordinary memory: flat_sdram.sv
    ;; maps it TWO BYTES PER SDRAM WORD (map_addr, keyed on cpu_a[23:20]) so
    ;; the scanout engine reads two pixels per access, and where a framebuffer
    ;; sits WITHIN the region is program-chosen through VERA2_DISPL/M/H. No
    ;; address in it is safe by construction.
    ;;
    ;; Bank $C0 is the first bank of the kernel writable-data region
    ;; (X816_KDATA_BASE, doc/MEMORY_MAP.md 1.1) -- ordinary one-byte-per-word
    ;; SDRAM the kernel owns, carved out of MEM_ALLOC's arena by
    ;; X816_HEAP_END. The editor's page pool starts one bank above, at
    ;; X816_EDIT_BASE, so the two cannot meet.
    (memory FarRAM (address (#xc00000 . #xc0ffff))
            (section far zfar huge zhuge))

    (block stack  (size #x0300))    ; kernel prompt stack
    (block cstack (size #x0300))    ; kernel C stack

    (base-address _DirectPageStart DirectPage 0)
    ))
