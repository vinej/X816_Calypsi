#!/usr/bin/env bash
# Build LIBTEST and run it in X816_Emulator, then report pass/fail.
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

CALYPSI=${CALYPSI:-../../Calypsi/calypsi-65816-5.18}
EMU=${EMU:-/c/quartus/projects/X816_Emulator}
CORE=${CORE:-/c/quartus/projects/X816_core}
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
WOUT=$(cygpath -m "$OUT" 2>/dev/null || echo "$OUT")

SRC=libtest.s
if [ "${1:-}" = "--negative" ]; then
    sed 's/SINTAB_EOR:    .equ 0xFE/SINTAB_EOR:    .equ 0xAB/' libtest.s > "$OUT/neg.s"
    SRC="$OUT/neg.s"
    echo "negative control: expecting RED"
fi

"$CALYPSI/bin/as65816" --core=65816 -I ../../src "$SRC" -o "$OUT/t.o" || exit 1
"$CALYPSI/bin/as65816" --core=65816 ../../runtime/x816hdr.s -o "$OUT/hdr.o" || exit 1
"$CALYPSI/bin/ln65816" ../../runtime/x816-lib.scm "$OUT/hdr.o" "$OUT/t.o" \
    "$CALYPSI/lib/clib-lc-sd.a" -o "$OUT/LIBTEST.elf" --output-format raw \
    --program-root __x816_root_section --rtattr exit=simplified || exit 1
# The core's OSD only offers .bin ("F1,BIN,Load Image"), and ln65816 always
# names the raw image <stem>.raw, so the copy is not cosmetic.
cp "$OUT/LIBTEST.raw" "$OUT/libtest.bin" || exit 1

SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout 20 \
    "$EMU/build/x16emu.exe" -boot "$(cygpath -m "$CORE/boot/boot.rom")" \
    -load "010000,$WOUT/libtest.bin" -warp -gif "$WOUT/out.gif" >/dev/null 2>&1

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
         (136, 0, 0): ("RED", "test 1 failed: sine table / data-init"),
         (238, 238, 119): ("YELLOW", "test 2 failed: library call via .word0"),
         (0, 0, 170): ("BLUE", "test 3 failed: direct page"),
         (204, 68, 204): ("MAGENTA", "test 4 failed: vera_addrsel patch")}
name, why = NAMES.get(col, ("?", "unrecognised colour %r" % (col,)))
print(f"final frame: {name} {col} at {frac*100:.0f}% -- {why}")
sys.exit(0 if name == "GREEN" else 1)
PY
