;;; ==========================================================================
;;; x816-lib.scm -- ln65816 memory map for a program that links the converted
;;; X16 assembly library.
;;;
;;; Follows X816_Core doc/MEMORY_MAP.md exactly:
;;;
;;;   bank $00      direct page, stack, and the library's variables
;;;                 ("OS variables" in the map -- 3,831 bytes for the WHOLE
;;;                 library, and a program enables only a few modules)
;;;   $01:0000+     the program: library code and user code, in SDRAM
;;;
;;; The split is forced, not stylistic. x16lib is 16-bit-bank code in three
;;; ways at once: data through DBR, internal jsr through PBR, and I/O at
;;; $9F00-$9FFF through DBR as well. Keeping code and data together in an
;;; SDRAM bank would need DBR to be that bank, and then I/O is unreachable --
;;; 114 of the library's I/O accesses use stz/trb/tsb/stx/sty/bit/ldx, which
;;; have no absolute-long form, so `long:` cannot rescue them.
;;;
;;; With data and I/O both in bank $00 and DBR = $00, every data and I/O
;;; reference works unchanged. Only code references need the low 16 bits of a
;;; bank-$01 address, which the converter emits as Calypsi's `.word0`.
;;;
;;;   ln65816 x816-lib.scm x816hdr.o your.o clib-lc-sd.a -o prog.elf \
;;;           --output-format raw --program-root __x816_root_section
;;; ==========================================================================

(define memories
  '(;; --- bank $00: BRAM, single cycle ------------------------------------
    ;; Direct page is page 0, matching what x16lib assumes ($22-$7F). Keeping
    ;; D page-aligned matters here: an unaligned D costs an extra cycle on
    ;; EVERY direct-page access on this core.
    ;; Stops at $21, one byte below X16_ZP, ON PURPOSE. The C runtime's
    ;; pseudo-registers live at the bottom of the direct page and x16lib's
    ;; X16_P0..X16_T7 are FIXED equates at $22-$31 that the linker knows
    ;; nothing about, so nothing would stop the two overlapping. Measured, the
    ;; runtime uses 20 bytes ($00-$13) even for a program using long, float
    ;; and double, so there are 14 bytes of slack -- but bounding the region
    ;; here turns a future overflow into a link error rather than into memory
    ;; that two owners quietly share.
    (memory DirectPage (address (#x000000 . #x000021))
            (section (registers ztiny)))

    ;; Stack, C stack, heap, and the library's variables and tables.
    ;;
    ;; Stops at $9DFF. $9F00 is the I/O page, and $9E00 is left unallocated
    ;; as a guard: native-mode direct-page arithmetic wraps within the BANK,
    ;; not the page, so an indexed access based at $9E00 can reach $9F4x and
    ;; hit the YM2151.
    (memory LoRAM (address (#x000100 . #x009dff))
            (section stack cstack data zdata heap))

    ;; $9F00-$9FFF is the I/O page -- deliberately absent, and reached with
    ;; ordinary 16-bit absolute addressing because DBR is $00.

    ;; The rest of bank $00, below the vector page the boot overlay owns.
    (memory HiRAM (address (#x00a000 . #x00feff))
            (section znear near))

    ;; --- $01:0000+ : SDRAM, where the loader drops the image -------------
    ;; The eight-byte boot header, then everything that executes. `idata`
    ;; (the initial values for `data`) rides along here and is copied into
    ;; bank $00 at startup by cstartup, driven by data_init_table.
    (memory X816Header (address (#x010000 . #x010007))
            (section x816hdr))
    (memory Code (address (#x010008 . #x0fffff))
            (section code farcode cfar chuge
                     cdata idata switch data_init_table reset))

    ;; Explicitly-placed far data. Unused by the Small data model.
    (memory FarRAM (address (#x100000 . #x1fffff))
            (section far zfar huge zhuge))

    (block stack (size #x1000))
    (block heap  (size #x0800))

    (base-address _DirectPageStart DirectPage 0)
    ))
