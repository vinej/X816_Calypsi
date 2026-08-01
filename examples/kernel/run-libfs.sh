#!/usr/bin/env bash
# The CONVERTED x16lib storage API, end to end on a real card.
#
# libfs.s calls fio_* and dir_* -- the library. Those call
# system/x816kernel.asm -- the crossing from 65C02 8-bit code into the 16-bit
# kernel ABI. That calls $00:FE00. The whole stack is what is under test;
# run-kfs.sh already covers the kernel entries on their own.
#
# As there, two independent verdicts: the on-screen colour, and pyfatfs
# reading the card afterwards. The library creates /LT, fills it, enumerates
# it and removes it, so a leftover /LT means a delete reported success and did
# nothing.
#
#   ./run-libfs.sh              build and run
#   ./run-libfs.sh --negative   corrupt the expectation, to prove it can fail
#
# Requires: pip install pillow pyfatfs
set -u

CALYPSI=${CALYPSI:-../../Calypsi/calypsi-65816-5.18}
EMU=${EMU:-/c/quartus/projects/X816_Emulator}
CORE=${CORE:-/c/quartus/projects/X816_core}
RT=../../runtime
LIB=../../src
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
WOUT=$(cygpath -m "$OUT" 2>/dev/null || echo "$OUT")

cd "$(dirname "$0")"

CFLAGS="--core=65816 --code-model=large --data-model=small -O0 -I $RT"

"$CALYPSI/bin/as65816" --core=65816 -I "$LIB" libfs.s -o "$OUT/t.o"     || exit 1
"$CALYPSI/bin/cc65816" $CFLAGS $RT/fat32.c   -o "$OUT/fat32.o"   || exit 1
"$CALYPSI/bin/cc65816" $CFLAGS $RT/kfs.c     -o "$OUT/kfs.o"     || exit 1
"$CALYPSI/bin/cc65816" $CFLAGS $RT/console.c -o "$OUT/console.o" || exit 1
"$CALYPSI/bin/cc65816" $CFLAGS $RT/font8x8.c -o "$OUT/font.o"    || exit 1
"$CALYPSI/bin/as65816" --core=65816 $RT/x816hdr.s    -o "$OUT/hdr.o"    || exit 1
"$CALYPSI/bin/as65816" --core=65816 $RT/smc.s        -o "$OUT/smc.o"    || exit 1
"$CALYPSI/bin/as65816" --core=65816 $RT/exec.s       -o "$OUT/exec.o"   || exit 1
"$CALYPSI/bin/as65816" --core=65816 $RT/font_cp437.s -o "$OUT/fontcp.o" || exit 1
"$CALYPSI/bin/as65816" --core=65816 $RT/kerntab.s    -o "$OUT/tab.o"    || exit 1

"$CALYPSI/bin/ln65816" $RT/x816-lib.scm "$OUT/hdr.o" "$OUT/t.o" \
    "$OUT/fat32.o" "$OUT/kfs.o" "$OUT/console.o" "$OUT/font.o" \
    "$OUT/smc.o" "$OUT/exec.o" "$OUT/fontcp.o" "$OUT/tab.o" \
    "$CALYPSI/lib/clib-lc-sd.a" -o "$OUT/LIBFS.elf" --output-format raw \
    --program-root __x816_root_section --rtattr exit=simplified || exit 1
cp "$OUT/LIBFS.raw" "$OUT/libfs.bin" || exit 1

cp "$CORE/boot/fat32.img" "$OUT/scratch.img" || exit 1

SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout -s KILL 60 \
    "$EMU/build/x16emu.exe" -boot "$(cygpath -m "$CORE/boot/boot.rom")" \
    -sdcard "$WOUT/scratch.img" \
    -load "010000,$WOUT/libfs.bin" \
    -warp -gif "$WOUT/out.gif" >/dev/null 2>&1

NEG=0
[ "${1:-}" = "--negative" ] && NEG=1 && echo "negative control: expecting FAIL"

python - "$WOUT/out.gif" "$WOUT/scratch.img" "$NEG" <<'PY'
import sys, collections
from PIL import Image, ImageFile
ImageFile.LOAD_TRUNCATED_IMAGES = True

gif, img, neg = sys.argv[1], sys.argv[2], sys.argv[3] == '1'

WHICH = {
    (0x00, 0xCC, 0x55): (0, 'green'),
    (0x88, 0x00, 0x00): (1, 'fio_mkdir'),
    (0xEE, 0xEE, 0x77): (2, 'fio_open write / fio_write / fio_close'),
    (0x00, 0x00, 0xAA): (3, 'fio_size / fio_read'),
    (0xCC, 0x44, 0xCC): (4, 'dir_open / dir_next / dir_close'),
    (0xAA, 0xFF, 0xEE): (5, 'fio_getc'),
    (0xDD, 0x88, 0x85): (6, 'fio_delete / fio_rmdir / refusals'),
}

im = Image.open(gif)
n = 0
while True:
    try:
        im.seek(n); im.load(); n += 1
    except Exception:
        break
if n == 0:
    sys.exit('no decodable frame -- did the emulator run?')
im.seek(n - 1)
colour = collections.Counter(im.convert('RGB').get_flattened_data()).most_common(1)[0][0]

code, what = WHICH.get(colour, (99, 'unrecognised colour %r' % (colour,)))
if code != 0:
    print('FAIL: on screen, test %d -- %s' % (code, what))
    sys.exit(0 if neg else 1)

from pyfatfs.PyFatFS import PyFatFS
fs = PyFatFS(img)
try:
    names = set(fs.listdir('/'))
finally:
    fs.close()
if neg:
    names.add('LT')           # pretend the cleanup failed, to prove the check
if 'LT' in names:
    print('FAIL: /LT survived -- a delete reported success and removed nothing')
    sys.exit(0 if neg else 1)
if neg:
    print('FAIL: the negative control passed, so the check proves nothing')
    sys.exit(1)
print('PASS: x16lib fio_* and dir_* over the native API, and the card agrees')
PY
