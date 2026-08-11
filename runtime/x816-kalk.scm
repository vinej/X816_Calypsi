;;; ==========================================================================
;;; x816-kalk.scm -- ln65816 memory map for a program that wants the FAST RAM.
;;;
;;; x816-lib.scm with two changes, both of which follow from one fact the
;;; other maps do not exploit: banks $01-$04 are BRAM and everything above is
;;; SDRAM. X816_core doc/MEMORY_MAP.md measures the difference at about SIX
;;; CPU cycles against one, and 4.47x end to end for code.
;;;
;;; The kernel's allocator cannot help with that. MEM_ALLOC's arena starts at
;;; $20:0100, above the program banks and above the staging area, so every
;;; byte it hands out is SDRAM. Fast memory is claimed HERE or not at all.
;;;
;;;   1. Code stops at the end of bank $01 instead of running to $0F:FFFF.
;;;   2. Banks $02-$04 become FastRAM, 192 KB of single-cycle data.
;;;
;;; WHY BOUNDING CODE COSTS NOTHING
;;; -------------------------------
;;; A loadable image already cannot exceed X816_EXEC_MAX ($FF00, 65,280
;;; bytes): exec.s relocates it in one pass with a 16-bit index, and both
;;; shell.c's load_file and kexec.c refuse anything larger. So an image that
;;; would overflow bank $01 could never have been launched anyway. The only
;;; thing this changes is WHEN you find out -- a link error naming the section
;;; that did not fit, rather than TOO BIG from `run` after a full build.
;;;
;;; WHAT GOES IN FASTRAM
;;; --------------------
;;; Whatever is read most and is small enough to fit: for the spreadsheet, the
;;; rendered-string cache, the recalculation worklist and the parser's
;;; scratch. NOT the cell grid -- that is megabytes and belongs in the arena,
;;; and it is touched sparsely (a screen shows a few thousand of a quarter
;;; million cells). The shape this produces is an ordinary cache hierarchy:
;;; cold bulk in SDRAM, hot derived data in BRAM, working state in bank $00.
;;;
;;; Declare it with __far and it lands in `zfar`, which is bss and contributes
;;; nothing to the image file.
;;;
;;;   THE EMULATOR CANNOT SHOW ANY OF THIS. It has uniform memory and reports
;;;   1.00x by construction -- programs/kernel/bankbench.s says so in its
;;;   header. Only the board can confirm the layout paid off; what this map
;;;   buys before then is that the decision is made at link time and is
;;;   visible in one place.
;;;
;;;   ln65816 x816-kalk.scm x816hdr.o your.o clib-lc-sd.a \
;;;           -o prog.elf --output-format raw \
;;;           --program-root __x816_root_section
;;; ==========================================================================

(define memories
  '(;; --- bank $00: BRAM, single cycle ------------------------------------
    ;; Identical to x816-lib.scm, deliberately: the kernel's claim, the guard
    ;; page below the I/O page and the jump table page are properties of the
    ;; MACHINE, not of a program, and a map that quietly disagreed with the
    ;; others about any of them would be found by a corrupted kernel call.
    (memory DirectPage (address (#x000000 . #x000021))
            (section (registers ztiny)))

    (memory LoStack (address (#x000100 . #x001fff))
            (section stack))

    ;; $00:2000-$00:2FFF -- the resident kernel's claim, carved out.
    (memory LoRAM (address (#x003000 . #x009dff))
            (section cstack data zdata heap))

    ;; $9E00 guard page, $9F00 I/O page -- deliberately absent.
    (memory HiRAM (address (#x00a000 . #x00fdff))
            (section znear near))

    ;; $00:FE00-$00:FEFF -- the kernel jump table, written at run time.

    ;; --- bank $01: the image, and ONLY bank $01 --------------------------
    (memory X816Header (address (#x010000 . #x010007))
            (section x816hdr))
    (memory Code (address (#x010008 . #x01feff))
            (section code farcode cfar chuge
                     cdata idata switch data_init_table reset))

    ;; --- banks $02-$04: 192 KB of BRAM, for data -------------------------
    ;; The whole point of this map. Uninitialised sections only in practice:
    ;; `far` would put its initialiser in `idata` and copy at startup, which
    ;; is fine but pays for itself only if the values matter before first use.
    (memory FastRAM (address (#x020000 . #x04ffff))
            (section zfar far zhuge huge))

    (block stack (size #x1000))
    (block heap  (size #x0800))

    (base-address _DirectPageStart DirectPage 0)
    ))
