#!/usr/bin/env bash
# Build every shell-side binary from source.
#
#     sh build.sh
#
# This exists because `make` cannot spawn the MSYS-style toolchain paths on this
# machine ("CreateProcess failed"), so the Makefile is unusable here and the
# real build was a hand-typed command line. A hand-typed build is one nobody
# else can reproduce and one that gets skipped -- a stale shell.bin shipped in a
# release three times before this script existed.
#
# X816_core/tools/mkrelease.sh calls this, so packaging a release always builds
# first rather than trusting that someone remembered to.
set -eu

cd "$(dirname "$0")"

# The toolchain, the memory map and the -O0 rule all come from one place now.
# The -O0 is not a style choice: the console, shell and FAT32 layers all reach
# hardware through volatile pointers, and Calypsi 5.18 eliminates volatile
# reads above -O0. calypsi.sh REFUSES to compile without it.
. ../../runtime/calypsi.sh
calypsi_banner

echo "compiling..."
cc816 $RT/shell.c    shell.o
cc816 $RT/fat32.c    fat32.o
cc816 $RT/kfs.c      kfs.o
cc816 $RT/goshell.c  goshell.o
cc816 $RT/console.c  console.o
cc816 $RT/font8x8.c  font8x8.o
cc816 $RT/kexec.c    kexec.o
cc816 $RT/kmem.c     kmem.o
cc816 shell.c        main.o
cc816 kernelmain.c   kernelmain.o
cc816 shtest.c       shtest.o
cc816 kbdprobe.c     probe.o
cc816 kbdstat.c      stat.o
cc816 kbdecho.c      echo.o
cc816 greentest.c    green.o
cc816 charmap.c      charmap.o
cc816 keyscan.c      keyscan.o
cc816 kerntest.c     kerntest.o
cc816 ../kernel/kfstest.c kfstest.o
# memtest is built here for the same reason kfstest is: this is the build a
# release runs, and MEMTEST.BIN on the card is the only way MEM_ALLOC/MEM_FREE
# meet the RESIDENT kernel on real hardware -- run-mem.sh links a private copy.
cc816 ../kernel/memtest.c memtest.o
# irqtest is here for the same reason again, and one more: the millisecond
# counter is REAL HARDWARE ($9F90, rtl/ms_timer.sv). The emulator models it,
# but only IRQTEST.BIN on the card meets the actual divider and the actual
# 59.52 Hz VERA frame it is cross-checked against.
cc816 ../kernel/irqtest.c irqtest.o
cc816 ../kernel/curtest.c curtest.o
# The VERA816 conformance test lives with the other VERA work but is built
# here, like kfstest above, because this is the build a release runs -- and
# BLITTEST.BIN ships on the demo card, which is the only way the blitter and
# the sprite widening get exercised on real hardware.
cc816 ../vera/blittest.c  blittest.o
# scanout.c is the same story: VERA816.md section 8 test 5 is a PICTURE, and
# only a real display can be looked at. It ships on the card beside BLITTEST.
cc816 ../vera/scanout.c   scanout.o
# regwin.c: VERA816.md section 8 test 8, the CTRL816.REGWIN window relocation.
cc816 ../vera/regwin.c    regwin.o
# scanout.c a SECOND time with USE_REGWIN=1 -- the same 640x480 test taking
# section 4.4's escape hatch, so the card carries both paths: SCANOUT.BIN
# paints around the register windows and blits the gap, SCANFULL.BIN sets
# CTRL816.REGWIN and paints straight through. Same picture, and the second
# one is the one a real program would write.
cc816 ../vera/scanout.c   scanfull.o -DUSE_REGWIN=1
# ...and a THIRD time at 4bpp: VERA816.md 5.0's other broken mode, based at
# $20000 so it is also the only thing that writes L0_BASEX non-zero. It needs
# no blitter and no REGWIN -- 153,600 bytes at $20000 clear the register
# windows outright -- so it is the simplest of the three to judge by eye.
cc816 ../vera/scanout.c   scan4.o    -DUSE_4BPP=1
# fxtest.c: VERA816.md 8 test 9. The FX guard -- and the affine fill-rate
# measurement 9.1's decision rests on. It prints its numbers on screen, so it
# is worth running on real hardware where the CPU timing is the real one.
cc816 ../vera/fxtest.c    fxtest.o
as816 $RT/x816hdr.s      x816hdr.o
as816 $RT/smc.s          smc.o
as816 $RT/exec.s         exec.o
as816 $RT/font_cp437.s   fontcp.o
as816 $RT/kerntab.s      kerntab.o
# The RESIDENT variant: KENTER/KLEAVE switch to the kernel context at $2000
# (see kerntab.s). Only the kernel image links this one.
as816 $RT/kerntab.s      kerntab_fw.o -DKERNEL_RESIDENT
as816 $RT/kcall.s        kcall.o
# The interrupt front end: CPU vectors, the dispatcher, IRQ_SET and both
# clocks. Linked into every image that installs the kernel table, because
# kerntab.s's generated body names its four entries.
as816 $RT/kirq.s         kirq.o
# The RESIDENT variant, matching kerntab_fw.o: its state goes in the kernel's
# own direct page rather than KernRAM, which was already 99% full. See kirq.s.
as816 $RT/kirq.s         kirq_fw.o -DKERNEL_RESIDENT
# The console cursor: a VSYNC handler, so it must be assembly (see its
# header). Same resident/loadable split as kirq.s, for the same reason.
as816 $RT/ccursor.s      ccursor.o
as816 $RT/ccursor.s      ccursor_fw.o -DKERNEL_RESIDENT
# libfs.s is the library test: it needs the converted x16lib on the include
# path, which nothing else here does.
as816 ../kernel/libfs.s  libfs.o -I "$X16LIB"
# libmem.s: the reshaped storage/mem over the kernel allocator. Same reason
# as libfs -- the LIBRARY layer on real hardware, where run-libmem.sh can only
# reach the emulator.
as816 ../kernel/libmem.s libmem.o -I "$X16LIB"
# libirq.s: the converted system/irq.asm and system/clock.asm. It is the only
# thing in the tree that exercises the 8-bit/16-bit crossing in the INWARD
# direction -- the kernel calling a library handler.
as816 ../kernel/libirq.s libirq.o -I "$X16LIB"
# membench.s: the block-move benchmark. On the card because the 7.0
# cycles/byte figure is an EMULATOR number -- uniform memory, no SDRAM wait
# states. Only the board says what MVN really costs here.
as816 ../kernel/membench.s membench.o -I "$X16LIB" -I "$RT"
# bankbench.s: what executing from SDRAM costs. Only the BOARD can answer
# it -- the emulator has uniform memory and reports 1.00x by construction.
as816 ../kernel/bankbench.s bankbench.o -I "$RT"
# irqhelp.s: the handlers, which cannot be C -- see its header.
as816 ../kernel/irqhelp.s irqhelp.o -I "$RT"

# kerntab.o calls into kfs.o, which calls into fat32.o: the kernel table is
# not linkable without the filesystem behind it.
COMMON="shell.o fat32.o kfs.o console.o font8x8.o fontcp.o smc.o exec.o kerntab.o kexec.o kmem.o kirq.o"

ln816 SHELL    x816hdr.o main.o   $COMMON
ln816 SHTEST   x816hdr.o shtest.o $COMMON
ln816 KBDPROBE x816hdr.o probe.o  console.o font8x8.o fontcp.o smc.o exec.o
ln816 KBDSTAT  x816hdr.o stat.o   console.o font8x8.o fontcp.o smc.o exec.o
ln816 KBDECHO  x816hdr.o echo.o   console.o font8x8.o fontcp.o smc.o exec.o
ln816 GREEN    x816hdr.o green.o
ln816 CHARMAP  x816hdr.o charmap.o console.o font8x8.o fontcp.o smc.o exec.o goshell.o fat32.o
ln816 KEYSCAN  x816hdr.o keyscan.o console.o font8x8.o fontcp.o smc.o exec.o goshell.o fat32.o
# blittest deliberately links NOTHING but the header and the C library: it
# goes straight at the registers rather than through console.c, because
# sharing code with the device under test is how two broken halves agree.
ln816 BLITTEST x816hdr.o blittest.o
ln816 SCANOUT  x816hdr.o scanout.o
ln816 REGWIN   x816hdr.o regwin.o
ln816 SCANFULL x816hdr.o scanfull.o
ln816 SCAN4    x816hdr.o scan4.o
ln816 FXTEST   x816hdr.o fxtest.o console.o font8x8.o fontcp.o smc.o exec.o goshell.o fat32.o
ln816 KERNTEST x816hdr.o kerntest.o console.o font8x8.o fontcp.o smc.o exec.o kerntab.o kexec.o kmem.o kcall.o kfs.o fat32.o goshell.o kirq.o
ln816 MEMTEST  x816hdr.o memtest.o console.o font8x8.o fontcp.o smc.o exec.o kerntab.o kexec.o kmem.o kcall.o kfs.o fat32.o goshell.o kirq.o
ln816 KFSTEST  x816hdr.o kfstest.o  console.o font8x8.o fontcp.o smc.o exec.o kerntab.o kexec.o kmem.o kcall.o kfs.o fat32.o goshell.o kirq.o
ln816 CURTEST  x816hdr.o curtest.o console.o font8x8.o fontcp.o smc.o exec.o kerntab.o kexec.o kmem.o kcall.o kfs.o fat32.o goshell.o kirq.o ccursor.o
ln816 IRQTEST  x816hdr.o irqtest.o irqhelp.o console.o font8x8.o fontcp.o smc.o exec.o kerntab.o kexec.o kmem.o kcall.o kfs.o fat32.o goshell.o kirq.o
ln816 LIBFS    x816hdr.o libfs.o    console.o font8x8.o fontcp.o smc.o exec.o kerntab.o kexec.o kmem.o kfs.o fat32.o goshell.o kirq.o
ln816 BANKBENCH x816hdr.o bankbench.o console.o font8x8.o fontcp.o smc.o exec.o kerntab.o kexec.o kmem.o kcall.o kfs.o fat32.o goshell.o kirq.o
ln816 MEMBENCH x816hdr.o membench.o console.o font8x8.o fontcp.o smc.o exec.o kerntab.o kexec.o kmem.o kcall.o kfs.o fat32.o goshell.o kirq.o
ln816 LIBIRQ   x816hdr.o libirq.o   console.o font8x8.o fontcp.o smc.o exec.o kerntab.o kexec.o kmem.o kfs.o fat32.o goshell.o kirq.o
ln816 LIBMEM   x816hdr.o libmem.o   console.o font8x8.o fontcp.o smc.o exec.o kerntab.o kexec.o kmem.o kfs.o fat32.o goshell.o kirq.o

# The RESIDENT KERNEL: the same shell linked into the firmware region by
# runtime/x816-kernel.scm (magic at $F0:0000, entry $F0:0004, state at
# $2000-$2FFF), with the resident kerntab, the K_EXEC backend and the
# table-installing main. Ships as games/X816/boot2.rom (mkrelease.sh).
LDSCRIPT=$RT/x816-kernel.scm
ln816 KERNEL x816hdr.o kernelmain.o \
             shell.o fat32.o kfs.o console.o font8x8.o fontcp.o smc.o exec.o \
             kerntab_fw.o kexec.o kmem.o kirq_fw.o ccursor_fw.o
LDSCRIPT=$RT/x816-lib.scm       # back to the loadable-program map
cp KERNEL.raw   kernel.bin

cp BLITTEST.raw ../vera/blittest.bin
cp SCANOUT.raw  ../vera/scanout.bin
cp REGWIN.raw   ../vera/regwin.bin
cp SCANFULL.raw ../vera/scanfull.bin
cp SCAN4.raw    ../vera/scan4.bin
cp FXTEST.raw   ../vera/fxtest.bin
cp SHELL.raw    shell.bin
cp SHTEST.raw   shtest.bin
cp KBDPROBE.raw kbdprobe.bin
cp KBDSTAT.raw  kbdstat.bin
cp KBDECHO.raw  kbdecho.bin
cp GREEN.raw    greentest.bin
cp CHARMAP.raw  charmap.bin
cp KEYSCAN.raw  keyscan.bin
cp KERNTEST.raw kerntest.bin
cp KFSTEST.raw  kfstest.bin
cp MEMTEST.raw  memtest.bin
cp IRQTEST.raw  irqtest.bin
cp CURTEST.raw  curtest.bin
cp LIBFS.raw    libfs.bin
cp LIBMEM.raw   libmem.bin
cp LIBIRQ.raw   libirq.bin
cp MEMBENCH.raw membench.bin
cp BANKBENCH.raw bankbench.bin

for f in kernel shell shtest kbdprobe kbdstat kbdecho greentest charmap keyscan kerntest kfstest memtest irqtest curtest libfs libmem libirq membench bankbench; do
    printf '  %-14s %s bytes\n' "$f.bin" "$(stat -c%s "$f.bin")"
done
printf '  %-14s %s bytes\n' "blittest.bin" "$(stat -c%s ../vera/blittest.bin)"
printf '  %-14s %s bytes\n' "scanout.bin"  "$(stat -c%s ../vera/scanout.bin)"
printf '  %-14s %s bytes\n' "regwin.bin"   "$(stat -c%s ../vera/regwin.bin)"
printf '  %-14s %s bytes\n' "scanfull.bin" "$(stat -c%s ../vera/scanfull.bin)"
printf '  %-14s %s bytes
' "scan4.bin"    "$(stat -c%s ../vera/scan4.bin)"
