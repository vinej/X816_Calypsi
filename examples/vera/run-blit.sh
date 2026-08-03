#!/usr/bin/env bash
# blit816 conformance (doc/BLIT816.md), plus the firmware write-protect
# (doc/KERNEL.md section 3).
#
# blittest.c runs on the machine and paints its verdict: GREEN is all tests
# passed, any other full-screen colour names the failing test (the map below
# matches the #defines in blittest.c).
#
# 2026-08-02: the sprite-reach probe (VERA816 attribute bits [5:4] reaching
# above 128 KB) was removed with the feature when VRAM went back to stock.
#
# The firmware side: fwpat.bin is loaded at $F0:0000 with -load -- the bypass
# path the contract requires to keep working -- and the program then proves a
# CPU store to that region is silently dropped. The program does NOT assume
# this pattern: it reads back whatever is there and requires it unchanged, so
# the same binary makes the same assertion from the demo card, where the
# resident kernel occupies that region instead.
#
#   ./run-blit.sh                build and run
#   ./run-blit.sh --negative     expect a wrong fill value, to prove the test
#                                can fail rather than always painting green
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
SRC=blittest.c
NEGATIVE=0
if [ "${1:-}" = "--negative" ]; then
    NEGATIVE=1
    # Break the FILL expectation specifically, so the screen must come up in
    # test 2's colour rather than merely non-green.
    sed 's/^#define FILL_EXPECT.*/#define FILL_EXPECT 0x99/' blittest.c > "$OUT/neg.c"
    SRC="$OUT/neg.c"
    echo "negative control: expecting YELLOW (test 2, FILL)"
fi

cc816 "$SRC" "$OUT/t.o"   || exit 1
as816 $RT/x816hdr.s "$OUT/hdr.o" || exit 1
ln816 "$OUT/BLITTEST" "$OUT/hdr.o" "$OUT/t.o" || exit 1
cp "$OUT/BLITTEST.raw" "$OUT/blittest.bin" || exit 1

# The firmware pattern blittest.c's test 7 asserts on. Loaded at $F0:0000 by
# -load -- the loader path that must BYPASS the write-protect.
printf '\xc3\x5a\xa5\x3c' > "$OUT/fwpat.bin"

SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout 60 \
    "$EMU/build/x16emu.exe" -boot "$(cygpath -m "$CORE/boot/boot.rom")" \
    -load "010000,$WOUT/blittest.bin" \
    -load "F00000,$WOUT/fwpat.bin" \
    -warp -gif "$WOUT/out.gif" >/dev/null 2>&1

python - "$WOUT/out.gif" "$NEGATIVE" <<'PY'
import sys, collections
from PIL import Image, ImageFile
ImageFile.LOAD_TRUNCATED_IMAGES = True

gif, negative = sys.argv[1], sys.argv[2] == "1"

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
top, cnt = collections.Counter(rgb.get_flattened_data()).most_common(1)[0]

GREEN = (0, 204, 85)
WHY = {GREEN:            None,
       (136, 0, 0):      "test 1 -- BLT_ID / busy (no blitter answered at DCSEL=33)",
       (238, 238, 119):  "test 2 -- FILL (value, bounds or pointer readback)",
       (0, 0, 170):      "test 3 -- misaligned COPY",
       (204, 68, 204):   "test 4 -- the doubling idiom",
       (170, 255, 238):  "test 5 -- LEN=0 no-op",
       (255, 255, 255):  "test 6 -- wrap at the top of VRAM ($1FFFE -> $00001)",
       (221, 136, 85):   "test 7 -- firmware region (-load, read, store drop)"}

if negative:
    if top == (238, 238, 119):
        print("PASS (negative control): the deliberate wrong FILL expectation")
        print("      failed exactly test 2 -- the test can fail")
        sys.exit(0)
    sys.exit("FAIL (negative control): expected YELLOW (test 2), screen is %r"
             % (top,))

if top not in WHY:
    sys.exit("unrecognised screen colour %r -- the test did not finish" % (top,))
if WHY[top]:
    sys.exit("FAIL on the machine: " + WHY[top])

print("PASS: blitter conformance green on the machine")
print("    1  BLT_ID $B6, busy idles 0")
print("    2  FILL odd addr/odd len, bounds tight, pointers read back one-past-end")
print("    3  COPY misaligned src/dst, LEN=257, SRC/DST/LEN readback")
print("    4  doubling idiom 16->32->64 (ascending copy, disjoint overlap)")
print("    5  LEN=0 starts nothing, parameters left as programmed")
print("    6  wrap $1FFFE->$00001: both ends land, one-past-end through the wrap")
print("    7  firmware: -load lands at $F0:0000, reads open, CPU store dropped")
PY
