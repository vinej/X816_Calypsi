#!/usr/bin/env bash
# VERA816 conformance test 8 -- CTRL816.REGWIN register-window relocation.
# Contract: X816_core/doc/VERA816.md section 4.4 (and 2.2, which is why the
# bit exists: no 640x480 framebuffer fits in the 352 KB without crossing the
# stock window position at $1F9C0-$1FFFF).
#
# regwin.c paints the screen in palette entry 1 and then writes that entry
# BLUE through the RELOCATED window first and RED through the STOCK address
# second, so each failure mode ends in its own final picture:
#
#   BLUE    both writes went where section 4.4 says          -- PASS
#   RED     the stock address still reaches the palette      -- leak
#   WHITE   the relocated window reaches nothing             -- dead
#
# plus one sprite programmed entirely through the relocated window (must
# render, probed at ~(72,72)) and one poisoned through the stock address
# (must NOT render -- those bytes are plain VRAM now -- probed at ~(104,72)).
#
# CPU-checkable failures paint their own colour before the verdict:
#   dark grey    VRAMCAP is not 22 -- no VERA816 answered
#   light green  CTRL816 reads the version byte -- no DCSEL-34 bank here
#   brown        REGWIN was written 1 but reads back otherwise
#   grey         the freed $1FA02 is not plain VRAM
#   light red    the relocated window is not write-only ($7FA02 read != 0)
#
#   ./run-regwin.sh              build and run
#   ./run-regwin.sh --negative   leave REGWIN clear: the stock address must
#                                repaint the screen RED and the relocated
#                                window must do nothing, sprite checks
#                                inverted -- proving every probe can fail
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

# -O0 IS MANDATORY: MMIO goes through volatile pointers, and Calypsi 5.18
# eliminates volatile reads above -O0. See the project README.
SRC=regwin.c
NEGATIVE=0
if [ "${1:-}" = "--negative" ]; then
    NEGATIVE=1
    sed 's/^#define SET_REGWIN.*/#define SET_REGWIN      0/' regwin.c > "$OUT/neg.c"
    SRC="$OUT/neg.c"
    echo "negative control: REGWIN stays clear -- stock decode must win"
fi

cc816 "$SRC" "$OUT/t.o"   || exit 1
as816 $RT/x816hdr.s "$OUT/hdr.o" || exit 1
ln816 "$OUT/REGWIN" "$OUT/hdr.o" "$OUT/t.o" || exit 1
cp "$OUT/REGWIN.raw" "$OUT/regwin.bin" || exit 1

SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout 60 \
    "$EMU/build/x16emu.exe" -boot "$(cygpath -m "$CORE/boot/boot.rom")" \
    -load "010000,$WOUT/regwin.bin" \
    -warp -gif "$WOUT/out.gif" >/dev/null 2>&1

python - "$WOUT/out.gif" "$NEGATIVE" <<'PY'
import sys, collections
from PIL import Image, ImageFile
ImageFile.LOAD_TRUNCATED_IMAGES = True

gif, negative = sys.argv[1], sys.argv[2] == "1"

BLUE   = (0, 0, 255)        # entry 1 = $00F, via the relocated window
RED    = (255, 0, 0)        # entry 1 = $F00, via the stock address
WHITE  = (255, 255, 255)    # entry 1 untouched -- relocated window dead
YELLOW = (238, 238, 119)    # default entry 7 -- the sprite pixels
SPR1   = (72, 72)           # sprite via the RELOCATED window (32,32 doubled)
SPR2   = (104, 72)          # sprite via the STOCK address    (48,32 doubled)

WHY = {(51, 51, 51):    "VRAMCAP did not read 22 -- no VERA816 answered",
       (170, 255, 102): "CTRL816 read the version byte -- no DCSEL-34 bank "
                        "in this build (older VERA816?)",
       (102, 68, 0):    "REGWIN was written 1 but did not read back 1",
       (119, 119, 119): "the freed $1FA02 is not plain VRAM",
       (255, 119, 119): "the relocated window is not write-only -- $7FA02 "
                        "read back non-zero"}

im = Image.open(gif)
n = 0
while True:
    try:
        im.seek(n); im.load(); n += 1
    except (EOFError, OSError):
        break
if n == 0:
    sys.exit("no decodable frame -- did the emulator run?")
im.seek(n - 1)
rgb = im.convert('RGB')
px = rgb.load()
top, cnt = collections.Counter(rgb.get_flattened_data()).most_common(1)[0]

if top in WHY:
    sys.exit("FAIL on the machine: " + WHY[top])

if negative:
    bad = []
    if top != RED:
        bad.append("screen is %r, expected RED (stock palette decode)" % (top,))
    if px[SPR2[0], SPR2[1]] != YELLOW:
        bad.append("no sprite at %r -- the stock sprite-attr address did not "
                   "work with REGWIN clear" % (SPR2,))
    if px[SPR1[0], SPR1[1]] == YELLOW:
        bad.append("a sprite rendered at %r -- the RELOCATED window worked "
                   "with REGWIN clear" % (SPR1,))
    if bad:
        print("FAIL (negative control):")
        for m in bad:
            print("   -", m)
        sys.exit(1)
    print("PASS (negative control): with REGWIN clear the stock address")
    print("      repainted the screen red, the stock sprite rendered and the")
    print("      relocated window did nothing -- every probe can fail")
    sys.exit(0)

if top == RED:
    sys.exit("FAIL: the STOCK palette address still reaches the palette with "
             "REGWIN set -- the relocation leaks (check ib_addr_winbank in "
             "top.v and vram_addr_winbank in addr_data.v)")
if top == WHITE:
    sys.exit("FAIL: the RELOCATED palette window reaches nothing -- the "
             "screen never left the default white")
if top != BLUE:
    sys.exit("unrecognised screen colour %r -- the test did not finish" % (top,))

bad = []
if px[SPR1[0], SPR1[1]] != YELLOW:
    bad.append("sprite programmed through $7FC08 did not render at %r "
               "(got %r)" % (SPR1, px[SPR1[0], SPR1[1]]))
if px[SPR2[0], SPR2[1]] == YELLOW:
    bad.append("poisoning $1FC10 produced a sprite at %r -- the stock "
               "sprite-attr address is still live" % (SPR2,))
if bad:
    print("FAIL: palette relocation works but the sprite-attr window is wrong")
    for m in bad:
        print("   -", m)
    sys.exit(1)

print("PASS: CTRL816.REGWIN relocates the windows -- VERA816.md section 8 test 8")
print("    CTRL816 read 0 at reset, took the bit, read it back")
print("    the freed $1FA02 is plain VRAM; $7FA02 is write-only (reads 0)")
print("    palette: BLUE via $7FA02 beat RED via $1FA02 -- writes decode at the")
print("    relocated position ONLY, so the whole 352 KB is paintable as VRAM")
print("    sprite via $7FC08 rendered; poison via $1FC10 stayed inert pixels")
PY
