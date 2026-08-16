#!/usr/bin/env bash
# Build the X816 desktop launcher.
#
#     sh build.sh        ->  x.bin
#
set -eu

cd "$(dirname "$0")"
. ../../runtime/calypsi.sh
calypsi_banner

echo "compiling..."
cc816 desktop.c        desktop.o
as816 "$RT/x816hdr.s"  x816hdr.o
as816 "$RT/kcall.s"    kcall.o

ln816 X x816hdr.o desktop.o kcall.o
cp X.raw x.bin

printf '  %-12s %s bytes\n' "x.bin" "$(stat -c%s x.bin)"
