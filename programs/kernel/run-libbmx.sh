#!/usr/bin/env bash
# storage/bmx.asm over the kernel, round-tripped on a real card.
#
# bmx was the library's heaviest KERNAL user (MACPTR port streaming, READST
# at every stage, the channel dance). It now moves bytes with fio_* through
# a bounce buffer, and libbmx.s proves the bytes: save a known VRAM pattern
# to a file, wipe VRAM, load it back, compare every byte. A chunking mistake
# at a 255-byte seam or a mis-seeked palette gap is a mismatched pixel here,
# not a plausible-looking screen.
#
#   ./run-libbmx.sh              build and run
#   ./run-libbmx.sh --negative   break the LIBRARY, not the test, and require
#                                the test to notice
#
# THE NEGATIVE CONTROL BREAKS THE CODE UNDER TEST: it patches the library
# copy so bmx_load's bounce pump never stores to the VERA port. The load
# still reports success -- every fio_read really did read -- but the pixels
# never arrive, and test 3's byte compare must catch it. If it does not, the
# compare is reading something other than what the load wrote.
#
# Requires Pillow:  pip install pillow
set -u

# The toolchain, the memory map and the -O0 rule come from one place --
# runtime/calypsi.sh -- so this script cannot drift from the build that ships.
. "$(dirname "$0")/../../runtime/calypsi.sh"
cd "$(dirname "$0")"
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
WOUT=$(cygpath -m "$OUT" 2>/dev/null || echo "$OUT")

INC="$X16LIB"
NEG=0
if [ "${1:-}" = "--negative" ]; then
    NEG=1
    echo "negative control: bmx_load's pump will not reach the port,"
    echo "                  so test 3 (BLUE) must fail"
    # A patched COPY OF THE WHOLE LIBRARY, as run-libirq.sh does: the
    # include path decides which library the test believes in.
    cp -r "$X16LIB" "$OUT/lib"
    python - "$OUT/lib/storage/bmx.s" <<'PY'
import re, sys
p = sys.argv[1]
s = open(p, newline='').read()
hits = re.findall(r'sta\s+VERA_DATA0', s)
# Two, since bmx_palptr: the pixel pump and the palette pump that also
# fills the caller's buffer. Both are silenced -- test 3's byte compare
# is still the one that must notice.
assert len(hits) == 2, f"expected two port stores in the pumps, found {len(hits)}"
s = re.sub(r'sta\s+VERA_DATA0', 'nop', s)
open(p, 'w', newline='').write(s)
PY
    [ $? -eq 0 ] || exit 1
    INC="$OUT/lib"
fi

as816 libbmx.s "$OUT/t.o" -I "$INC"  || exit 1
cc816 $RT/fat32.c "$OUT/fat32.o"   || exit 1
cc816 $RT/kfs.c "$OUT/kfs.o"     || exit 1
cc816 $RT/goshell.c "$OUT/gosh.o"    || exit 1
cc816 $RT/console.c "$OUT/console.o" || exit 1
as816 $RT/ccursor.s    "$OUT/ccur.o"     || exit 1
cc816 $RT/font8x8.c "$OUT/font.o"    || exit 1
as816 $RT/x816hdr.s "$OUT/hdr.o"    || exit 1
as816 $RT/smc.s "$OUT/smc.o"    || exit 1
as816 $RT/exec.s "$OUT/exec.o"   || exit 1
as816 $RT/font_cp437.s "$OUT/fontcp.o" || exit 1
as816 $RT/kerntab.s "$OUT/tab.o"    || exit 1
as816 $RT/kirq.s      "$OUT/kirq.o"   -I "$RT" || exit 1
cc816 $RT/kexec.c "$OUT/kexec.o"   || exit 1
cc816 $RT/kmem.c  "$OUT/kmem.o"    || exit 1

ln816 "$OUT/LIBBMX" "$OUT/hdr.o" "$OUT/t.o" "$OUT/fat32.o" "$OUT/kfs.o" "$OUT/kexec.o" "$OUT/kmem.o" "$OUT/gosh.o" "$OUT/console.o" "$OUT/ccur.o" "$OUT/font.o" "$OUT/smc.o" "$OUT/exec.o" "$OUT/fontcp.o" "$OUT/tab.o" "$OUT/kirq.o" || exit 1
cp "$OUT/LIBBMX.raw" "$OUT/libbmx.bin" || exit 1

cp "$CORE/boot/fat32.img" "$OUT/scratch.img" || exit 1

SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout -s KILL 60 \
    "$EMU/build/x16emu.exe" -boot "$(cygpath -m "$CORE/boot/boot.rom")" \
    -sdcard "$WOUT/scratch.img" \
    -load "010000,$WOUT/libbmx.bin" \
    -warp -gif "$WOUT/out.gif" >/dev/null 2>&1

python - "$WOUT/out.gif" "$NEG" <<'PY'
import sys
from PIL import Image, ImageFile
ImageFile.LOAD_TRUNCATED_IMAGES = True

gif, neg = sys.argv[1], sys.argv[2] == '1'

WHICH = {
    (0x00, 0xCC, 0x55): (0, 'green'),
    (0x88, 0x00, 0x00): (1, 'bmx_save'),
    (0xEE, 0xEE, 0x77): (2, 'bmx_load / published header'),
    (0x00, 0x00, 0xAA): (3, 'VRAM byte compare after wipe + reload'),
    (0xCC, 0x44, 0xCC): (4, 'junk not refused as FORMAT'),
    (0xAA, 0xFF, 0xEE): (5, 'missing file not refused as IO'),
    (0xFF, 0xFF, 0xFF): (6, 'bmx_palptr: the caller-held palette round trip'),
}
REASON = {
    (0x00, 0xCC, 0x55): 'no code',
    (0x00, 0x00, 0x00): 'BMX_ERR_IO',
    (0x66, 0x44, 0x00): 'BMX_ERR_FORMAT',
    (0xAA, 0xFF, 0x66): 'BMX_ERR_PACKED',
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
top    = rgb.getpixel((w // 2, h // 4))
bottom = rgb.getpixel((w // 2, h * 3 // 4))

code, what = WHICH.get(top, (99, 'unrecognised colour %r' % (top,)))
if code != 0:
    why = REASON.get(bottom, 'unrecognised colour %r' % (bottom,))
    print('FAIL: on screen, test %d -- %s, and the reason was %s' %
          (code, what, why))
    sys.exit(0 if neg else 1)
if neg:
    print('FAIL: the negative control passed, so the test proves nothing')
    sys.exit(1)
print('PASS: bmx save/load round trip over fio_* -- header, palette gap,')
print('      chunked pixel pump, both refusal paths, and bmx_palptr')
PY
