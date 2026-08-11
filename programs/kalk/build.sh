#!/usr/bin/env bash
# Build the shippable kalk binaries.
#
#     sh build.sh        ->  kalk.bin, kbench.bin
#
# This exists because kalk had NO build script: every one of the run-*.sh
# harnesses compiled the program into a temporary directory, ran it in the
# emulator and deleted it again. That is fine for a test and useless for a
# release -- there was no kalk.bin anywhere on disk to put on a card, which is
# why the spreadsheet never shipped.
#
# The recipe is the one run-kalk.sh and run-bench.sh use, with the same
# x816-kalk.scm memory map (kalk needs its own: the cell arena does not fit the
# ordinary program map). Keep the three in step -- if a source file is added
# here it must be added there too, and the harnesses are what would catch it.
set -eu

cd "$(dirname "$0")"
. ../../runtime/calypsi.sh

LDSCRIPT=$RT/x816-kalk.scm
calypsi_banner

echo "compiling..."
cc816 kalk.c        kalk.o
cc816 view.c        view.o -I .
cc816 cell.c        cell.o
cc816 expr.c        expr.o
cc816 sheet.c       sheet.o -I .
cc816 fmt.c         fmt.o
cc816 benchtest.c   benchtest.o -I .
cc816 $RT/fp.c      fp.o
cc816 $RT/shell.c   shell.o
cc816 $RT/console.c console.o
cc816 $RT/font8x8.c font.o
cc816 $RT/fat32.c   fat32.o
cc816 $RT/kfs.c     kfs.o
cc816 $RT/kmem.c    kmem.o
cc816 $RT/goshell.c goshell.o
as816 $RT/fpcall.s  fpcall.o -I "$X16LIB"
as816 $RT/kcall.s   kcall.o
as816 $RT/x816hdr.s hdr.o
as816 $RT/smc.s     smc.o
as816 $RT/exec.s    exec.o
as816 $RT/font_cp437.s fontcp.o
as816 $RT/ccursor.s ccursor.o

COMMON="view.o cell.o fmt.o fpcall.o fp.o kcall.o shell.o console.o font.o
        fontcp.o smc.o exec.o ccursor.o fat32.o kfs.o kmem.o goshell.o"

ln816 KALK  hdr.o kalk.o expr.o sheet.o $COMMON
ln816 KBENCH hdr.o benchtest.o $COMMON

cp KALK.raw   kalk.bin
cp KBENCH.raw kbench.bin

for f in kalk kbench; do
    printf '  %-12s %s bytes\n' "$f.bin" "$(stat -c%s "$f.bin")"
done
