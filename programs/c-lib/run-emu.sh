#!/usr/bin/env bash
# Build CTEST and run it in X816_Emulator, then report pass/fail.
#
# The emulator has no usable headless mode for X816 -- -testbench hooks a PC
# value this machine never reaches, and memory is only dumped when the PC hits
# $FFFF -- so the check reads the screen colour out of a -gif capture. SDL's
# dummy video driver keeps it off-screen.
#
#   ./run-emu.sh                 build and run
#   ./run-emu.sh --negative      same, but with a deliberately wrong expected
#                                checksum, to prove the test can actually fail
#
# Requires Pillow for the GIF decode:  pip install pillow
set -u

# The toolchain, the memory map and the -O0 rule come from one place --
# runtime/calypsi.sh -- so this script cannot drift from the build that ships.
# It also sets EMU, CORE, RT and X16LIB, and cc816 refuses -O1+ silently.
. "$(dirname "$0")/../../runtime/calypsi.sh"
cd "$(dirname "$0")"
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
WOUT=$(cygpath -m "$OUT" 2>/dev/null || echo "$OUT")

SRC=ctest.c
if [ "${1:-}" = "--negative" ]; then
    # Break the two-argument test specifically, so a pass/fail is not the only
    # thing proved -- the colour must point at the test that was broken.
    sed 's/x816_atan2(0, 127) != 64/x816_atan2(0, 127) != 65/' ctest.c > "$OUT/neg.c"
    SRC="$OUT/neg.c"
    echo "negative control: expecting BLUE (test 3, two arguments)"
fi

# ctest.c only ever WRITES VERA registers -- it never reads one back -- so the
# volatile-elision hazard that pins everything else to -O0 cannot bite here,
# and -O2 is the point: this is the test that measures the x16lib call glue
# under the optimiser the rest of the tree is waiting to be able to use.
calypsi_optimise -O2 "writes VERA registers, never reads one back"
cc816 "$SRC"          "$OUT/t.o"    || exit 1
as816 $RT/x816_glue.s "$OUT/glue.o" -I "$X16LIB" -DX16_USE_MATH=1 -DX16_USE_PALETTE=1 -DX16_USE_ZX0=1 || exit 1
as816 $RT/x816hdr.s   "$OUT/hdr.o"  || exit 1
ln816 "$OUT/CTEST" "$OUT/hdr.o" "$OUT/t.o" "$OUT/glue.o" || exit 1
# The core's OSD only offers .bin ("F1,BIN,Load Image"), and ln65816 always
# names the raw image <stem>.raw, so the copy is not cosmetic.
cp "$OUT/CTEST.raw" "$OUT/ctest.bin" || exit 1

SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout 20 \
    "$EMU/build/x16emu.exe" -boot "$(cygpath -m "$CORE/boot/boot.rom")" \
    -load "010000,$WOUT/ctest.bin" -warp -gif "$WOUT/out.gif" >/dev/null 2>&1

python - "$WOUT/out.gif" <<'PY'
import sys, collections
from PIL import Image, ImageFile
# The emulator is killed by `timeout`, so its last GIF frame is usually cut
# off mid-write. Tolerate that and fall back to the last frame that decodes.
ImageFile.LOAD_TRUNCATED_IMAGES = True
im = Image.open(sys.argv[1])
n = 0
while True:
    try:
        im.seek(n); im.load(); n += 1
    except (EOFError, OSError):
        break
if n == 0:
    sys.exit("no decodable frame -- did the emulator run?")
im.seek(n - 1)
px = im.convert('RGB')
col, cnt = collections.Counter(px.get_flattened_data()).most_common(1)[0]
frac = cnt / (px.width * px.height)
# VERA's default palette, the five colours the test can paint.
NAMES = {(0, 204, 85): ("GREEN", "all tests passed"),
         (136, 0, 0): ("RED", "test 1 failed: one char in / one char out"),
         (238, 238, 119): ("YELLOW", "test 2 failed: no-arg call, 16-bit return"),
         (0, 0, 170): ("BLUE", "test 3 failed: two arguments, second via stack"),
         (204, 68, 204): ("MAGENTA", "test 4 failed: direct-page arguments"),
         (170, 255, 238): ("CYAN", "test 5 failed: register width not restored")}
name, why = NAMES.get(col, ("?", "unrecognised colour %r" % (col,)))
print(f"final frame: {name} {col} at {frac*100:.0f}% -- {why}")
sys.exit(0 if name == "GREEN" else 1)
PY
