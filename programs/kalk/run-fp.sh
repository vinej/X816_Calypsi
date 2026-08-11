#!/usr/bin/env bash
# Float conformance: x16lib's arithmetic, driven from C.
#
# Two things under test at once, and they failed independently on the way in:
#
#   1. runtime/fpcall.s -- the first C-to-x16lib bridge in this tree. The
#      library is 65C02 code and needs A/X/Y eight bits wide; C runs them at
#      sixteen. Every assembly caller narrows once for the whole program and
#      says so in its header; C cannot, so the bridge does it per call.
#   2. util/float.s itself, which had no test and is about to carry a
#      spreadsheet. It is 2,000 lines of hand-written mantissa arithmetic.
#
# WHAT THIS ALREADY CAUGHT, so that removing it looks as expensive as it is:
#
#   * f_from_str reads its string from the PROGRAM BANK, not bank $00 -- it is
#     built to parse literals, which live in the image. Handed a bank $00
#     buffer it parsed garbage and reported success. runtime/fp.c stages the
#     string into the image first, which is what util/float.s tells callers to
#     do; without it, every number a user typed became a different number.
#   * the CONSTANT TABLE was in the wrong bank entirely. acme2calypsi.py swept
#     every data run into bank $00 while leaving the phb/phk/plb that reads it
#     behind as code, so f_to_str, f_from_str and every transcendental read
#     bank $01 at a bank $00 offset and returned confident nonsense. Fixed in
#     the converter, which also repaired audio/notes.s, audio/ym.s and
#     input/input.s -- all three had the same split.
#
# Neither failure produced a diagnostic. Both produced numbers.
#
#   ./run-fp.sh              build and run
#   ./run-fp.sh --negative   break an expectation, to prove this can fail
#
# Requires Pillow:  pip install pillow
set -u

. "$(dirname "$0")/../../runtime/calypsi.sh"
cd "$(dirname "$0")"
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
WOUT=$(cygpath -m "$OUT" 2>/dev/null || echo "$OUT")

NEG=0
[ "${1:-}" = "--negative" ] && NEG=1 && \
    echo "negative control: expecting the check to FAIL"

SRC=fptest.c
if [ "$NEG" = 1 ]; then
    # Claim 10/4 is 2.6. The arithmetic is right, so the test must say so.
    sed 's/r_div\[\] = "2.5"/r_div[] = "2.6"/' fptest.c > "$OUT/fptest.c"
    SRC="$OUT/fptest.c"
fi

cc816 "$SRC"        "$OUT/fptest.o"  || exit 1
cc816 $RT/fp.c      "$OUT/fp.o"      || exit 1
cc816 $RT/shell.c   "$OUT/shell.o"   || exit 1
cc816 $RT/console.c "$OUT/console.o" || exit 1
cc816 $RT/font8x8.c "$OUT/font.o"    || exit 1
cc816 $RT/fat32.c   "$OUT/fat32.o"   || exit 1
cc816 $RT/kfs.c     "$OUT/kfs.o"     || exit 1
cc816 $RT/kmem.c     "$OUT/kmem.o"     || exit 1
# goshell: ESC from a finished demo restarts the resident kernel, so a
# card full of these can be run one after another.
cc816 $RT/goshell.c "$OUT/goshell.o" || exit 1
# fpcall.s sources x16lib -- it is the one object in the link that may.
as816 $RT/fpcall.s  "$OUT/fpcall.o" -I "$X16LIB" || exit 1
as816 $RT/x816hdr.s "$OUT/hdr.o"     || exit 1
as816 $RT/smc.s     "$OUT/smc.o"     || exit 1
as816 $RT/exec.s    "$OUT/exec.o"    || exit 1
as816 $RT/font_cp437.s "$OUT/fontcp.o"  || exit 1
as816 $RT/ccursor.s "$OUT/ccursor.o" || exit 1
ln816 "$OUT/FPTEST" "$OUT/hdr.o" "$OUT/fptest.o" "$OUT/fpcall.o" "$OUT/fp.o" \
      "$OUT/shell.o" "$OUT/console.o" "$OUT/font.o" "$OUT/fontcp.o" \
      "$OUT/smc.o" "$OUT/exec.o" "$OUT/ccursor.o" "$OUT/fat32.o" \
      "$OUT/kfs.o" "$OUT/goshell.o" || exit 1

# No card and no resident kernel: this program owns the machine from the
# magic at $01:0000, which is the shortest path to the arithmetic.
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout 120 \
    "$EMU/build/x16emu.exe" -boot "$(cygpath -m "$CORE/boot/boot.rom")" \
    -load "010000,$WOUT/FPTEST.raw" \
    -warp -gif "$WOUT/out.gif" >/dev/null 2>&1

python - "$WOUT/out.gif" "$RT/font_cp437.s" "$NEG" <<'PY'
import sys, re, io
from PIL import Image, ImageFile
ImageFile.LOAD_TRUNCATED_IMAGES = True

gif, fontinc, negative = sys.argv[1], sys.argv[2], sys.argv[3] == "1"

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
    except (EOFError, OSError):
        break
if n == 0:
    sys.exit("no decodable frame -- did the emulator run?")
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

rows = [row_text(r) for r in range(30)]
body = "\n".join(rows)

def fail(msg):
    print("FAIL:", msg)
    for i, r in enumerate(rows):
        if r:
            print(f"  {i}: {r}")
    sys.exit(1)

if not rows[0].startswith("X816 FLOAT BRIDGE"):
    fail("no banner -- the program did not reach main()")

if negative:
    if "FLOAT BRIDGE OK" in body:
        fail("a wrong expectation still passed -- this test cannot detect one")
    print("PASS (negative control): a wrong expectation is reported, "
          "so a real regression would be too")
    sys.exit(0)

# Every case prints its own line; the verdict is the last word. Checking both
# means a suite that silently stopped early cannot look like a pass.
if "FAILED AT CASE" in body:
    fail("at least one case did not match")
if "FLOAT BRIDGE OK" not in body:
    fail("no verdict -- the run stopped part way, so some case hung")
if body.count("  ok") < 18:
    fail(f"only {body.count('  ok')} cases reported ok, expected 18")

print("PASS: 18 cases -- arithmetic, both string directions, the 16-bit round")
print("      trip, five transcendentals and the domain refusals")
for r in rows:
    if r:
        print("   ", r)
PY
