#!/usr/bin/env bash
# VERA FX affine mode: a conformance test, and a measurement.
#
# There was no FX test in this tree at all -- line draw, polygon fill, the
# 32-bit cache and affine mode all arrived working from upstream and have been
# carried unexercised. This covers affine, which is the part doc/VERA816.md
# section 9 proposes to change (FX_BASEX), and it exists for two reasons:
#
#   1. a GUARD, so that widening the FX base registers is not done blind;
#   2. a MEASUREMENT of the affine fill rate, because the case for widening
#      them rests on whether affine is fast enough to be worth having and
#      that number had only ever been estimated on paper.
#
# It runs at 320x240 with its map and tile data BELOW 128 KB, so it needs no
# RTL change and says nothing about FX_BASEX either way. It measures what is
# there now.
#
# The correctness checks compare FX against a plain-C tilemap walk written
# from the documented semantics, not from the same expressions the RTL uses --
# otherwise the test would only prove that two copies of one reading agree.
# The screen reports the cycles-per-pixel it measured.
#
#   ./run-fx.sh              build and run
#   ./run-fx.sh --negative   break the C reference by one bit, to prove the
#                            comparison can fail
#
# Requires Pillow:  pip install pillow
set -u

# The toolchain, the memory map and the -O0 rule come from one place --
# runtime/calypsi.sh -- so this script cannot drift from the build that ships.
# It also sets EMU, CORE, RT and X16LIB, and cc816 refuses -O1+ silently.
. "$(dirname "$0")/../../runtime/calypsi.sh"
cd "$(dirname "$0")"
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
WOUT=$(cygpath -m "$OUT" 2>/dev/null || echo "$OUT")

SRC=fxtest.c
NEG=0
if [ "${1:-}" = "--negative" ]; then
    NEG=1
    # Perturb the REFERENCE, not FX. If the walks still pass after this, they
    # are not actually comparing anything.
    sed 's/\^ 0x80);/^ 0x81);/' fxtest.c > "$OUT/neg.c"
    if ! grep -q "0x81" "$OUT/neg.c"; then
        echo "negative control: the patch did not apply -- refusing to run" >&2
        exit 1
    fi
    SRC="$OUT/neg.c"
    echo "negative control: the C reference is off by one bit, so the affine"
    echo "                  walks must disagree with it"
fi

cc816 "$SRC"           "$OUT/t.o"       || exit 1
cc816 $RT/console.c    "$OUT/console.o" || exit 1
cc816 $RT/font8x8.c    "$OUT/font.o"    || exit 1
cc816 $RT/fat32.c      "$OUT/fat32.o"   || exit 1
cc816 $RT/goshell.c    "$OUT/gosh.o"    || exit 1
as816 $RT/x816hdr.s    "$OUT/hdr.o"     || exit 1
as816 $RT/smc.s        "$OUT/smc.o"     || exit 1
as816 $RT/exec.s       "$OUT/exec.o"    || exit 1
as816 $RT/font_cp437.s "$OUT/fontcp.o"  || exit 1

ln816 "$OUT/FXTEST" "$OUT/hdr.o" "$OUT/t.o" "$OUT/console.o" "$OUT/font.o" \
      "$OUT/fat32.o" "$OUT/gosh.o" "$OUT/smc.o" "$OUT/exec.o" \
      "$OUT/fontcp.o" || exit 1
cp "$OUT/FXTEST.raw" "$OUT/fxtest.bin" || exit 1

SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout -s KILL 90 \
    "$EMU/build/x16emu.exe" -boot "$(cygpath -m "$CORE/boot/boot.rom")" \
    -load "010000,$WOUT/fxtest.bin" \
    -warp -gif "$WOUT/out.gif" >/dev/null 2>&1

python - "$WOUT/out.gif" "$RT/font_cp437.s" "$NEG" <<'PY'
import sys, re, io
from PIL import Image, ImageFile
ImageFile.LOAD_TRUNCATED_IMAGES = True

gif, fontinc, neg = sys.argv[1], sys.argv[2], sys.argv[3] == '1'

vals = []
for line in io.open(fontinc, encoding='utf-8'):
    m = re.match(r'\s*\.byte\s+(.*)$', line.split(';')[0])
    if m:
        vals += [int(x.strip().lstrip('$'), 16)
                 for x in m.group(1).split(',') if x.strip()]
glyph = {}
for _c in range(0x20, 0x7F):
    glyph[tuple(vals[_c * 8:(_c + 1) * 8])] = chr(_c)

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
px = im.convert('RGB').load()

def row_text(r):
    out = ""
    for col in range(60):
        bits = []
        for y in range(8):
            b = 0
            for x in range(8):
                if px[col * 8 + x, r * 8 + y] != (0, 0, 0):
                    b |= 0x80 >> x
            bits.append(b)
        out += glyph.get(tuple(bits), '?')
    return out.rstrip()

rows = [row_text(r) for r in range(10)]
screen = "\n".join(rows).upper()

WHICH = {1: 'FX answered with a constant -- affine produced no texture',
         2: 'identity walk (one pixel per step)',
         3: 'fractional increment -- the sub-pixel stepping Mode 7 needs',
         4: 'two-axis walk (a rotation), including a negative x step',
         5: 'clipping -- outside the map must give tile 0',
         6: 'wrapping -- with clip off the map must repeat'}

if 'X816 VERA FX' not in screen:
    print('FAIL: the test did not reach its verdict. Screen:')
    for r in rows:
        if r:
            print('   ', r)
    sys.exit(1)

m = re.search(r'FAIL AT TEST ([0-9A-F]{2})', screen)
if m:
    code = int(m.group(1), 16)
    what = WHICH.get(code, 'unknown test %d' % code)
    if neg:
        print('PASS (negative control): with the C reference shifted by one '
              'bit the walks disagreed, at test %d -- %s' % (code, what))
        sys.exit(0)
    print('FAIL: test %d -- %s' % (code, what))
    for r in rows:
        if r:
            print('   ', r)
    sys.exit(1)

if neg:
    print('FAIL: the negative control PASSED. The affine walks agree with a '
          'reference that is deliberately wrong, so they are not comparing '
          'anything.')
    sys.exit(1)

print('PASS: VERA FX affine -- identity, fractional and rotated walks all '
      'match an independent C tilemap reference; clip and wrap both correct')
for r in rows:
    if r:
        print('   ', r)
PY
