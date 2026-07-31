;;; ==========================================================================
;;; x816-plain.scm -- ln65816 memory map for a plain (bare, no OS) X816
;;; program loaded at $01:0000.
;;;
;;; Mirrors X816_Core doc/MEMORY_MAP.md. The two facts that shape everything
;;; below:
;;;
;;;   * Bank $00 is 64 KB of BRAM and is the machine's only single-cycle
;;;     memory. Banks $01+ are SDRAM and stall on every access.
;;;   * The 65816 forces BOTH the stack and the direct page into bank $00
;;;     regardless of DBR/PBR. They are not merely better off there, they
;;;     cannot be anywhere else.
;;;
;;; That is also why the Small data model is the right one here: Calypsi
;;; requires the near bank to be the same bank as the CPU stack, i.e. bank
;;; $00, which is exactly where X816's fast RAM is. Code uses the Large model
;;; so it can live out in SDRAM and be reached with 24-bit calls.
;;;
;;;     Library: clib-lc-sd.a   (large code, small data)
;;;
;;; Link with:
;;;   ln65816 x816-plain.scm x816hdr.o your.o -o prog.elf \
;;;           --output-format raw --program-root __x816_root_section \
;;;           clib-lc-sd.a
;;; ==========================================================================

(define memories
  '(;; --- bank $00: BRAM, single cycle ------------------------------------
    ;; Direct page is page 0. Keeping D page-aligned matters: on this core
    ;; an unaligned D costs an extra cycle on EVERY direct-page access
    ;; (P65C816.vhd DLNoZero, gated on native mode).
    (memory DirectPage (address (#x000000 . #x0000ff))
            (section (registers ztiny)))

    ;; Stack, C stack, static data and heap. Stops at $9DFF, one page short
    ;; of the I/O page at $9F00 -- $9E00 is deliberately left unallocated as
    ;; a guard, because native-mode direct-page arithmetic wraps within the
    ;; BANK, not the page, so an indexed access based at $9E00 can reach
    ;; $9F4x and hit the YM2151.
    (memory LoRAM (address (#x000100 . #x009dff))
            (section stack cstack data zdata heap))

    ;; $9F00-$9FFF is the I/O page -- deliberately absent from this map.

    ;; The rest of bank $00, up to but not including the vector page at
    ;; $FF00, which the boot overlay owns.
    (memory HiRAM (address (#x00a000 . #x00feff))
            (section znear near))

    ;; --- bank $01: SDRAM, where the loader drops the image ---------------
    ;; The HPS loader adds $01:0000 to the file offset, so file offset 0 is
    ;; the magic and the image is contiguous from there.
    (memory X816Header (address (#x010000 . #x010007))
            (section x816hdr))

    ;; farcode belongs HERE, with the rest of the program, not in a separate
    ;; far region. In the Large code model every C function is emitted into
    ;; `farcode`, not `code` -- mapping it to its own memory at $02:0000 put
    ;; main() a bank away from the entry point, and since the raw writer
    ;; spans lowest to highest byte of actual content, a two-line C program
    ;; produced a 65,815-byte image that was almost entirely padding.
    (memory Code (address (#x010008 . #x0fffff))
            (section code farcode cfar chuge
                     cdata idata switch data_init_table reset))

    ;; --- far data --------------------------------------------------------
    ;; Only for data a module explicitly places far; the Small data model
    ;; does not use it. Uninitialised sections contribute no file content,
    ;; and initialised far data has its initialiser in `idata` above and is
    ;; copied at run time, so nothing here inflates the image.
    (memory FarRAM (address (#x100000 . #x1fffff))
            (section far zfar huge zhuge))

    (block stack (size #x1000))
    (block heap  (size #x0800))

    (base-address _DirectPageStart DirectPage 0)
    ))
