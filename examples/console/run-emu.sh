#!/usr/bin/env bash
# Build the console test and run it in X816_Emulator, then report pass/fail.
#
# This one checks more than a screen colour. On success the test leaves TEXT on
# screen rather than painting green, because the glyphs are the one thing a
# person can actually verify -- so the check decodes the framebuffer 8x8 block
# by block against the font and compares the result with what the test printed.
#
# That closes a real gap: the six programmatic tests inside contest.c read back
# VRAM *cell* contents, which are tile indices. They say nothing about whether
# the font uploaded correctly or whether the tile mode is set up right. A
# console that wrote the correct indices and displayed garbage would pass all
# six.
#
#   ./run-emu.sh                 build and run
#   ./run-emu.sh --negative      break the scroll test, to prove it can fail
#
# Requires Pillow for the framebuffer decode:  pip install pillow
set -u

CALYPSI=${CALYPSI:-/c/calypsi/calypsi-65816-5.18}
EMU=${EMU:-/c/quartus/projects/X816_Emulator}
CORE=${CORE:-/c/quartus/projects/X816_core}
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
WOUT=$(cygpath -m "$OUT" 2>/dev/null || echo "$OUT")

SRC=contest.c
if [ "${1:-}" = "--negative" ]; then
    # Break the scroll check specifically, so the colour has to name that test
    # rather than merely being non-green.
    sed 's/cell(0, CON_ROWS - 3) != .P./cell(0, CON_ROWS - 3) != 0x21/' contest.c > "$OUT/neg.c"
    SRC="$OUT/neg.c"
    echo "negative control: expecting CYAN (test 5, scroll)"
fi

"$CALYPSI/bin/cc65816" --core=65816 --code-model=large --data-model=small -O0 \
    -I ../../runtime "$SRC" -o "$OUT/t.o" || exit 1
"$CALYPSI/bin/cc65816" --core=65816 --code-model=large --data-model=small -O0 \
    -I ../../runtime ../../runtime/console.c -o "$OUT/console.o" || exit 1
"$CALYPSI/bin/cc65816" --core=65816 --code-model=large --data-model=small -O0 \
    -I ../../runtime ../../runtime/font8x8.c -o "$OUT/font.o" || exit 1
"$CALYPSI/bin/as65816" --core=65816 ../../runtime/x816hdr.s -o "$OUT/hdr.o" || exit 1
"$CALYPSI/bin/ln65816" ../../runtime/x816-lib.scm "$OUT/hdr.o" "$OUT/t.o" \
    "$OUT/console.o" "$OUT/font.o" "$CALYPSI/lib/clib-lc-sd.a" \
    -o "$OUT/CONTEST.elf" --output-format raw \
    --program-root __x816_root_section --rtattr exit=simplified || exit 1
cp "$OUT/CONTEST.raw" "$OUT/contest.bin" || exit 1

SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout 40 \
    "$EMU/build/x16emu.exe" -boot "$(cygpath -m "$CORE/boot/boot.rom")" \
    -load "010000,$WOUT/contest.bin" -warp -gif "$WOUT/out.gif" >/dev/null 2>&1

python - "$WOUT/out.gif" "$CORE/boot/font8x8.inc" <<'PY'
import sys, re, io, collections
from PIL import Image, ImageFile
ImageFile.LOAD_TRUNCATED_IMAGES = True

# The same font the console uploaded, so the decode is against ground truth.
vals = []
for line in io.open(sys.argv[2], encoding='utf-8'):
    m = re.match(r'\s*\.byte\s+(.*)$', line.split(';')[0])
    if m:
        vals += [int(x.strip().lstrip('$'), 16) for x in m.group(1).split(',') if x.strip()]
glyph = {tuple(vals[i:i+8]): chr(0x20 + i // 8) for i in range(0, len(vals), 8)}

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
rgb = im.convert('RGB')
px = rgb.load()
counts = collections.Counter(rgb.get_flattened_data())
(top, cnt), = counts.most_common(1)
frac = cnt / (rgb.width * rgb.height)

FAIL = {(136, 0, 0):     "test 1 -- a character did not land where addressed",
        (238, 238, 119): "test 2 -- cls did not clear or did not home",
        (0, 0, 170):     "test 3 -- no wrap at the right margin",
        (204, 68, 204):  "test 4 -- newline / return / backspace",
        (170, 255, 238): "test 5 -- scrolling lost or misplaced a line",
        (255, 255, 255): "test 6 -- an unprintable was not filtered"}

# A pass is text mode, which is >99% BLACK background -- so "one colour fills
# the screen" is not by itself a failure. Only a known failure colour is.
if frac > 0.99 and top in FAIL:
    print("FAIL:", FAIL[top])
    sys.exit(1)
if frac > 0.999 and top == (0, 0, 0):
    print("FAIL: screen is entirely blank -- nothing was printed")
    sys.exit(1)

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

want = ["X816 CONSOLE OK",
        "80X60 TEXT, VERA TILE MODE, SMC KEYBOARD",
        "",
        "ALL SIX CONSOLE TESTS PASSED."]
got = [row_text(r) for r in range(4)]
if got != want:
    print("FAIL: the glyphs on screen are not what was printed")
    for i, (g, w) in enumerate(zip(got, want)):
        if g != w:
            print(f"  row {i}: got {g!r}\n         want {w!r}")
    sys.exit(1)

print("PASS: six checks, and the glyphs decode back to exactly what was printed")
for g in got:
    if g:
        print("   ", g)
PY
