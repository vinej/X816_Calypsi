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

# The toolchain, the memory map and the -O0 rule come from one place --
# runtime/calypsi.sh -- so this script cannot drift from the build that ships.
# It also sets EMU, CORE, RT and X16LIB, and cc816 refuses -O1+ silently.
. "$(dirname "$0")/../../runtime/calypsi.sh"
cd "$(dirname "$0")"
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
WOUT=$(cygpath -m "$OUT" 2>/dev/null || echo "$OUT")

cd "$(dirname "$0")"

as816 libfs.s "$OUT/t.o" -I "$X16LIB"  || exit 1
cc816 $RT/fat32.c "$OUT/fat32.o"   || exit 1
cc816 $RT/kfs.c "$OUT/kfs.o"     || exit 1
cc816 $RT/goshell.c "$OUT/gosh.o"    || exit 1
cc816 $RT/console.c "$OUT/console.o" || exit 1
cc816 $RT/font8x8.c "$OUT/font.o"    || exit 1
as816 $RT/x816hdr.s "$OUT/hdr.o"    || exit 1
as816 $RT/smc.s "$OUT/smc.o"    || exit 1
as816 $RT/exec.s "$OUT/exec.o"   || exit 1
as816 $RT/font_cp437.s "$OUT/fontcp.o" || exit 1
as816 $RT/kerntab.s "$OUT/tab.o"    || exit 1
# kerntab.s's generated table names the four interrupt/clock entries (IRQ_SET,
# TIME_GET, TIME_SET, IRQ_FRAMES), so every image that links the table now
# links kirq.s behind it.
as816 $RT/kirq.s      "$OUT/kirq.o"   -I "$RT" || exit 1
cc816 $RT/kexec.c "$OUT/kexec.o"   || exit 1
cc816 $RT/kmem.c  "$OUT/kmem.o"    || exit 1

ln816 "$OUT/LIBFS" "$OUT/hdr.o" "$OUT/t.o" "$OUT/fat32.o" "$OUT/kfs.o" "$OUT/kexec.o" "$OUT/kmem.o" "$OUT/gosh.o" "$OUT/console.o" "$OUT/font.o" "$OUT/smc.o" "$OUT/exec.o" "$OUT/fontcp.o" "$OUT/tab.o" "$OUT/kirq.o" || exit 1
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
rgb = im.convert('RGB')
w, h = rgb.size

# Two bands now: the top names the test and the bottom carries the kernel's
# error code. Sampled rather than counted -- a most-common colour over a
# half-and-half screen picks a winner by tie-break, which is no answer at all.
top    = rgb.getpixel((w // 2, h // 4))
bottom = rgb.getpixel((w // 2, h * 3 // 4))

REASON = {
    (0x00, 0xCC, 0x55): 'no code',
    (0x00, 0x00, 0x00): 'KERR_NOSYS',
    (0x66, 0x44, 0x00): 'KERR_NOTFOUND',
    (0xAA, 0xFF, 0x66): 'KERR_NOSPACE',
    (0x00, 0x88, 0xFF): 'KERR_BADARG',
    (0x77, 0x77, 0x77): 'KERR_IO',
    (0xFF, 0xFF, 0xFF): 'KERR_EXISTS',
    (0xFF, 0x77, 0x77): 'KERR_NOTEMPTY',
}

code, what = WHICH.get(top, (99, 'unrecognised colour %r' % (top,)))
if code != 0:
    why = REASON.get(bottom, 'unrecognised colour %r' % (bottom,))
    print('FAIL: on screen, test %d -- %s, and the reason was %s' %
          (code, what, why))
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
