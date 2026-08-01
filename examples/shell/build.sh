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

CALYPSI=${CALYPSI:-../../Calypsi/calypsi-65816-5.18}
RT=../../runtime
LIB="$CALYPSI/lib/clib-lc-sd.a"

# -O0 IS MANDATORY: the console, shell and FAT32 layers all reach hardware
# through volatile pointers, and Calypsi 5.18 eliminates volatile reads above
# -O0. See the README.
CFLAGS="--core=65816 --code-model=large --data-model=small -O0 -I $RT"

cd "$(dirname "$0")"

echo "compiling..."
"$CALYPSI/bin/cc65816" $CFLAGS $RT/shell.c    -o shell.o
"$CALYPSI/bin/cc65816" $CFLAGS $RT/fat32.c    -o fat32.o
"$CALYPSI/bin/cc65816" $CFLAGS $RT/console.c  -o console.o
"$CALYPSI/bin/cc65816" $CFLAGS $RT/font8x8.c  -o font8x8.o
"$CALYPSI/bin/cc65816" $CFLAGS shell.c        -o main.o
"$CALYPSI/bin/cc65816" $CFLAGS shtest.c       -o shtest.o
"$CALYPSI/bin/cc65816" $CFLAGS kbdprobe.c     -o probe.o
"$CALYPSI/bin/cc65816" $CFLAGS kbdstat.c      -o stat.o
"$CALYPSI/bin/cc65816" $CFLAGS kbdecho.c      -o echo.o
"$CALYPSI/bin/cc65816" $CFLAGS greentest.c    -o green.o
"$CALYPSI/bin/cc65816" $CFLAGS charmap.c      -o charmap.o
"$CALYPSI/bin/cc65816" $CFLAGS kerntest.c     -o kerntest.o
"$CALYPSI/bin/as65816" --core=65816 $RT/x816hdr.s -o x816hdr.o
"$CALYPSI/bin/as65816" --core=65816 $RT/smc.s     -o smc.o
"$CALYPSI/bin/as65816" --core=65816 $RT/exec.s    -o exec.o
"$CALYPSI/bin/as65816" --core=65816 $RT/font_cp437.s -o fontcp.o
"$CALYPSI/bin/as65816" --core=65816 $RT/kerntab.s -o kerntab.o
"$CALYPSI/bin/as65816" --core=65816 $RT/kcall.s   -o kcall.o

link () {                       # link <ELF-name> <first-object> [extra...]
    local name=$1; shift
    rm -f "$name.raw"
    "$CALYPSI/bin/ln65816" $RT/x816-lib.scm x816hdr.o "$@" "$LIB" \
        -o "$name.elf" --output-format raw \
        --program-root __x816_root_section --rtattr exit=simplified
}

COMMON="shell.o fat32.o console.o font8x8.o fontcp.o smc.o exec.o kerntab.o"

link SHELL    main.o   $COMMON
link SHTEST   shtest.o $COMMON
link KBDPROBE probe.o  console.o font8x8.o fontcp.o smc.o exec.o
link KBDSTAT  stat.o   console.o font8x8.o fontcp.o smc.o exec.o
link KBDECHO  echo.o   console.o font8x8.o fontcp.o smc.o exec.o
link GREEN    green.o
link CHARMAP  charmap.o console.o font8x8.o fontcp.o smc.o exec.o
link KERNTEST kerntest.o console.o font8x8.o fontcp.o smc.o exec.o kerntab.o kcall.o

cp SHELL.raw    shell.bin
cp SHTEST.raw   shtest.bin
cp KBDPROBE.raw kbdprobe.bin
cp KBDSTAT.raw  kbdstat.bin
cp KBDECHO.raw  kbdecho.bin
cp GREEN.raw    greentest.bin
cp CHARMAP.raw  charmap.bin
cp KERNTEST.raw kerntest.bin

for f in shell shtest kbdprobe kbdstat kbdecho greentest charmap kerntest; do
    printf '  %-14s %s bytes\n' "$f.bin" "$(stat -c%s "$f.bin")"
done
