#!/usr/bin/env bash
# The console CURSOR: the first thing built on IRQ_SET, and the first thing in
# the tree that touches VERA from inside an interrupt.
#
# Five checks -- see curtest.c's header. The one that matters is 5: printing
# drives VERA's port 0 a byte at a time while the handler fires sixty times a
# second and drives port 1, and if the handler disturbed CTRL or port 0's
# address, characters would land in the wrong cells RARELY and only while the
# cursor was on.
#
#   ./run-cur.sh              build and run
#   ./run-cur.sh --negative   stop the cursor blinking, to prove this can fail
#
# THE NEGATIVE CONTROL PATCHES THE CODE UNDER TEST, NOT THE TEST. It makes
# ccur_hide a no-op, so the cursor is drawn once and never undrawn -- which is
# exactly the thing check 1 exists to reject, since "the attribute is reversed"
# is true of a cursor that has stopped working. Check 1 must go RED.
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

CCUR=$RT/ccursor.s
NEG=0
if [ "${1:-}" = "--negative" ]; then
    NEG=1
    # Make ccur_hide return before it draws anything: the cursor is then
    # switched on once and never switched off again.
    sed 's/^ccur_hide:$/ccur_hide:\n              rts/' \
        "$RT/ccursor.s" > "$OUT/ccursor_neg.s"
    if ! grep -A1 '^ccur_hide:$' "$OUT/ccursor_neg.s" | grep -q 'rts'; then
        echo "negative control: the patch did not apply -- ccursor.s moved."  >&2
        echo "  Refusing to run: an unpatched 'negative' run paints green and" >&2
        echo "  proves nothing, which is exactly the trap this guard exists"   >&2
        echo "  for (doc/AUDIT.md, and run-libmem.sh's first version)."        >&2
        exit 1
    fi
    CCUR="$OUT/ccursor_neg.s"
    echo "negative control: expecting RED (check 1, the cursor actually blinks)"
fi

cc816 curtest.c        "$OUT/t.o"       || exit 1
cc816 $RT/kmem.c       "$OUT/kmem.o"    || exit 1
cc816 $RT/kfs.c        "$OUT/kfs.o"     || exit 1
cc816 $RT/fat32.c      "$OUT/fat32.o"   || exit 1
cc816 $RT/kexec.c      "$OUT/kexec.o"   || exit 1
cc816 $RT/goshell.c    "$OUT/gosh.o"    || exit 1
cc816 $RT/console.c    "$OUT/console.o" || exit 1
cc816 $RT/font8x8.c    "$OUT/font.o"    || exit 1
as816 $RT/x816hdr.s    "$OUT/hdr.o"     || exit 1
as816 $RT/smc.s        "$OUT/smc.o"     || exit 1
as816 $RT/exec.s       "$OUT/exec.o"    || exit 1
as816 $RT/font_cp437.s "$OUT/fontcp.o"  || exit 1
as816 $RT/kerntab.s    "$OUT/tab.o"     || exit 1
as816 $RT/kcall.s      "$OUT/kcall.o"   || exit 1
as816 $RT/kirq.s       "$OUT/kirq.o"   -I "$RT" || exit 1
as816 "$CCUR"          "$OUT/ccur.o"   -I "$RT" || exit 1

ln816 "$OUT/CURTEST" "$OUT/hdr.o" "$OUT/t.o" "$OUT/kmem.o" "$OUT/kfs.o" \
      "$OUT/fat32.o" "$OUT/kexec.o" "$OUT/gosh.o" "$OUT/console.o" \
      "$OUT/font.o" "$OUT/smc.o" "$OUT/exec.o" "$OUT/fontcp.o" \
      "$OUT/tab.o" "$OUT/kcall.o" "$OUT/kirq.o" "$OUT/ccur.o" || exit 1
cp "$OUT/CURTEST.raw" "$OUT/curtest.bin" || exit 1

SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout -s KILL 120 \
    "$EMU/build/x16emu.exe" -boot "$(cygpath -m "$CORE/boot/boot.rom")" \
    -load "010000,$WOUT/curtest.bin" \
    -warp -gif "$WOUT/out.gif" >/dev/null 2>&1

python - "$WOUT/out.gif" "$NEG" <<'PY'
import sys, collections
from PIL import Image, ImageFile
ImageFile.LOAD_TRUNCATED_IMAGES = True

gif, neg = sys.argv[1], sys.argv[2] == '1'

# The VERA default palette, doubled from 4 bits per channel to 8 -- the same
# indices irqtest.c paints. Keep these in step with the paint() calls there.
WHICH = {
    (0x00, 0xCC, 0x55): (0, 'green -- every check passed'),
    (0x88, 0x00, 0x00): (1, 'the cursor cell takes BOTH attribute values: it blinks'),
    (0xEE, 0xEE, 0x77): (2, 'the glyph under the cursor is never disturbed'),
    (0x00, 0x00, 0xAA): (3, 'it follows: the old cell settles, the new one blinks'),
    (0xCC, 0x44, 0xCC): (4, 'ccur_off leaves the cell normal and static'),
    (0xAA, 0xFF, 0xEE): (5, 'text printed while it blinks is not corrupted'),
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
(top, cnt), = collections.Counter(rgb.get_flattened_data()).most_common(1)
frac = cnt / (rgb.width * rgb.height)

if frac < 0.99:
    print('FAIL: the screen is not one colour (%.0f%% is %r) -- the test did '
          'not reach its verdict. An interrupt source left unacknowledged '
          'livelocks the machine, and that is what this looks like.'
          % (frac * 100, top))
    sys.exit(1)

code, what = WHICH.get(top, (99, 'unrecognised colour %r' % (top,)))
if code != 0:
    print('FAIL: check %d -- %s' % (code, what))
    if neg and code != 1:
        print('  ...but the negative control was supposed to break check 1, '
              'not check %d. The patch hit something else.' % code)
        sys.exit(1)
    sys.exit(0 if neg else 1)
if neg:
    print('FAIL: the negative control painted GREEN, so check 1 proves nothing')
    sys.exit(1)
print('PASS: the console cursor -- it blinks, keeps the glyph under it, '
      'follows the console, stops cleanly, and does not corrupt text printed '
      'underneath it')
PY
