#!/usr/bin/env bash
# Build the shell conformance test and run it in X816_Emulator.
#
# The test drives sh_exec() with canned lines, so it covers the tokeniser, the
# dispatcher, hex parsing and the far-memory commands WITHOUT a keyboard --
# which is the whole reason sh_exec takes a line rather than reading one. The
# keyboard path (con_getkey) is NOT covered here -- run-kbd.sh covers it, and
# it exists because that exact gap let a miscompiled I2C shift reach hardware
# undetected. See runtime/smc.s.
#
# On success the test leaves text on screen instead of painting green, and this
# decodes the framebuffer 8x8 block by block against the font to check the
# glyphs really are what was printed.
#
#   ./run-emu.sh                 build and run
#   ./run-emu.sh --negative      break the hex test, to prove it can fail
#
# Requires Pillow:  pip install pillow
set -u

CALYPSI=${CALYPSI:-../../Calypsi/calypsi-65816-5.18}
EMU=${EMU:-/c/quartus/projects/X816_Emulator}
CORE=${CORE:-/c/quartus/projects/X816_core}
RT=../../runtime
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
WOUT=$(cygpath -m "$OUT" 2>/dev/null || echo "$OUT")

CFLAGS="--core=65816 --code-model=large --data-model=small -O0 -I $RT"

SRC=shtest.c
if [ "${1:-}" = "--negative" ]; then
    # Break the hex test specifically, so the colour must name test 3 rather
    # than merely being non-green.
    sed 's/|| v != 0x010000UL/|| v != 0x999999UL/' shtest.c > "$OUT/neg.c"
    SRC="$OUT/neg.c"
    echo "negative control: expecting BLUE (test 3, hex parsing)"
fi

"$CALYPSI/bin/cc65816" $CFLAGS "$SRC"           -o "$OUT/t.o"       || exit 1
"$CALYPSI/bin/cc65816" $CFLAGS $RT/shell.c      -o "$OUT/shell.o"   || exit 1
"$CALYPSI/bin/cc65816" $CFLAGS $RT/fat32.c      -o "$OUT/fat32.o"   || exit 1
"$CALYPSI/bin/cc65816" $CFLAGS $RT/kfs.c        -o "$OUT/kfs.o"   || exit 1
"$CALYPSI/bin/cc65816" $CFLAGS $RT/console.c    -o "$OUT/console.o" || exit 1
"$CALYPSI/bin/cc65816" $CFLAGS $RT/font8x8.c    -o "$OUT/font.o"    || exit 1
"$CALYPSI/bin/as65816" --core=65816 $RT/x816hdr.s -o "$OUT/hdr.o"   || exit 1
"$CALYPSI/bin/as65816" --core=65816 $RT/smc.s     -o "$OUT/smc.o"   || exit 1
"$CALYPSI/bin/as65816" --core=65816 $RT/exec.s    -o "$OUT/exec.o"  || exit 1
"$CALYPSI/bin/as65816" --core=65816 $RT/font_cp437.s -o "$OUT/fontcp.o" || exit 1
"$CALYPSI/bin/ln65816" $RT/x816-lib.scm "$OUT/hdr.o" "$OUT/t.o" \
    "$OUT/shell.o" "$OUT/fat32.o" "$OUT/kfs.o" "$OUT/console.o" "$OUT/font.o" "$OUT/smc.o" "$OUT/exec.o" "$OUT/fontcp.o" \
    "$CALYPSI/lib/clib-lc-sd.a" -o "$OUT/SHTEST.elf" --output-format raw \
    --program-root __x816_root_section --rtattr exit=simplified || exit 1
cp "$OUT/SHTEST.raw" "$OUT/shtest.bin" || exit 1

SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout 40 \
    "$EMU/build/x16emu.exe" -boot "$(cygpath -m "$CORE/boot/boot.rom")" \
    -load "010000,$WOUT/shtest.bin" -warp -gif "$WOUT/out.gif" >/dev/null 2>&1

python - "$WOUT/out.gif" "$RT/font_cp437.s" <<'PY'
import sys, re, io, collections
from PIL import Image, ImageFile
ImageFile.LOAD_TRUNCATED_IMAGES = True

vals = []
for line in io.open(sys.argv[2], encoding='utf-8'):
    m = re.match(r'\s*\.byte\s+(.*)$', line.split(';')[0])
    if m:
        vals += [int(x.strip().lstrip('$'), 16) for x in m.group(1).split(',') if x.strip()]
# All 256 CP437 glyphs now, but only $20-$7E is mapped back to characters:
# everything else decodes to '?', which stops the many blank glyphs colliding
# with space and makes a stray box character obvious instead of invisible.
glyph = {}
for _c in range(0x20, 0x7F):
    glyph[tuple(vals[_c * 8:(_c + 1) * 8])] = chr(_c)

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

FAIL = {(136, 0, 0):     "test 1 -- tokeniser",
        (238, 238, 119): "test 2 -- argument-count checking",
        (0, 0, 170):     "test 3 -- hex parsing",
        (204, 68, 204):  "test 4 -- far memory (peek/poke/fill)",
        (170, 255, 238): "test 5 -- move, including overlap",
        (255, 255, 255): "test 6 -- unknown command or blank line"}

# A pass is text mode, which is >99% black -- so one colour filling the screen
# is a failure only when it is a known failure colour.
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

want = ["X816 SHELL OK",
        "TOKENISER, DISPATCH, HEX, FAR MEMORY, MOVE",
        "",
        "ALL SIX SHELL TESTS PASSED."]
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
