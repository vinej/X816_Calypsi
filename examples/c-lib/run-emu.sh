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

CALYPSI=${CALYPSI:-/c/calypsi/calypsi-65816-5.18}
EMU=${EMU:-/c/quartus/projects/X816_Emulator}
CORE=${CORE:-/c/quartus/projects/X816_core}
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

"$CALYPSI/bin/cc65816" --core=65816 --code-model=large --data-model=small -O2     -I ../../runtime "$SRC" -o "$OUT/t.o" || exit 1
"$CALYPSI/bin/as65816" --core=65816 -I ../../src -DX16_USE_MATH=1     ../../runtime/x816_glue.s -o "$OUT/glue.o" || exit 1
"$CALYPSI/bin/as65816" --core=65816 ../../runtime/x816hdr.s -o "$OUT/hdr.o" || exit 1
"$CALYPSI/bin/ln65816" ../../runtime/x816-lib.scm "$OUT/hdr.o" "$OUT/t.o" \
    "$OUT/glue.o" "$CALYPSI/lib/clib-lc-sd.a" -o "$OUT/CTEST.elf" --output-format raw \
    --program-root __x816_root_section --rtattr exit=simplified || exit 1
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
