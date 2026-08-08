#!/usr/bin/env bash
# Column formatting: what a cell looks like once it has to fit somewhere.
#
# kalk.c does this with snprintf and inherits C's rules -- "%ld", "%g",
# "%.2f", "%*s". Neither of the printf implementations on this machine is
# usable (the C library's is unreachable in the small data model; the float
# package's own f_to_str goes to EXPONENT FORM below 1, so a column holding
# 0.4 would read 4.00000000e-01), so fmt.c reimplements the two rules that
# matter on top of the digits f_to_str already gets right.
#
# Every expectation below is therefore what printf would say, and the cases
# sit on the boundaries where a hand-written %g goes wrong: the switch to
# exponent form at both ends, the integral-value shortcut that keeps whole
# numbers free of decimal points, and rounding that carries off the top of the
# digit string (999999.6 becomes 1e+06, not 0.0).
#
# THREE OF THEM WERE WRONG THE FIRST TIME, and all three were the test rather
# than the code -- each is now a comment next to the value:
#
#   * 1234567890 does NOT print as an integer. kalk.c guards that path with
#     fabs(val) < 1e9 and this is over it, so it is %g.
#   * /F I TRUNCATES, because kalk.c casts -- (long)123456.789 is 123456.
#   * a bar of 2.5 is THREE stars, because kalk.c compares an integer against
#     the value: 0, 1 and 2 are all less than 2.5. Truncating first gives two,
#     and a bar chart short by one everywhere is not something anyone reports.
#
# It finishes by drawing three rows of a real sheet with a computed subtotal,
# because a table of strings can be right while the columns do not line up.
#
#   ./run-fmt.sh              build and run
#   ./run-fmt.sh --negative   break %g's threshold, to prove this can fail
#
# Requires Pillow:  pip install pillow
set -u

. "$(dirname "$0")/../../runtime/calypsi.sh"
cd "$(dirname "$0")"
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
WOUT=$(cygpath -m "$OUT" 2>/dev/null || echo "$OUT")

NEG=0
SRC=fmt.c
if [ "${1:-}" = "--negative" ]; then
    NEG=1
    echo "negative control: %g keeps four significant digits instead of six"
    sed 's/#define GEN_SIG 6/#define GEN_SIG 4/' fmt.c > "$OUT/fmt.c"
    SRC="$OUT/fmt.c"
fi

LDSCRIPT=$RT/x816-kalk.scm

cc816 fmttest.c     "$OUT/fmttest.o" || exit 1
cc816 "$SRC"        "$OUT/fmt.o" -I . || exit 1
cc816 $RT/fp.c      "$OUT/fp.o"      || exit 1
cc816 $RT/shell.c   "$OUT/shell.o"   || exit 1
cc816 $RT/console.c "$OUT/console.o" || exit 1
cc816 $RT/font8x8.c "$OUT/font.o"    || exit 1
cc816 $RT/fat32.c   "$OUT/fat32.o"   || exit 1
cc816 $RT/kfs.c     "$OUT/kfs.o"     || exit 1
as816 $RT/fpcall.s  "$OUT/fpcall.o" -I "$X16LIB" || exit 1
as816 $RT/x816hdr.s "$OUT/hdr.o"     || exit 1
as816 $RT/smc.s     "$OUT/smc.o"     || exit 1
as816 $RT/exec.s    "$OUT/exec.o"    || exit 1
as816 $RT/font_cp437.s "$OUT/fontcp.o" || exit 1
as816 $RT/ccursor.s "$OUT/ccursor.o" || exit 1
ln816 "$OUT/FMTTEST" "$OUT/hdr.o" "$OUT/fmttest.o" "$OUT/fmt.o" \
      "$OUT/fpcall.o" "$OUT/fp.o" "$OUT/shell.o" "$OUT/console.o" \
      "$OUT/font.o" "$OUT/fontcp.o" "$OUT/smc.o" "$OUT/exec.o" \
      "$OUT/ccursor.o" "$OUT/fat32.o" "$OUT/kfs.o" || exit 1

SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout 120 \
    "$EMU/build/x16emu.exe" -boot "$(cygpath -m "$CORE/boot/boot.rom")" \
    -load "010000,$WOUT/FMTTEST.raw" \
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
    except (EOFError, OSError, IndexError):
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

rows = [row_text(r) for r in range(34)]
body = "\n".join(rows)

def fail(msg):
    print("FAIL:", msg)
    for i, r in enumerate(rows):
        if r:
            print(f"  {i}: {r}")
    sys.exit(1)

if not rows[0].startswith("X816 CELL FORMAT"):
    fail("no banner -- the program did not reach main()")

if negative:
    if "CELL FORMAT OK" in body:
        fail("%g at four significant digits still passed -- this test cannot "
             "see a precision change, so it cannot see a real one")
    print("PASS (negative control): a changed %g precision is caught")
    sys.exit(0)

if "FAILED AT CASE" in body:
    fail("at least one case did not match")
if "CELL FORMAT OK" not in body:
    fail("no verdict -- the run stopped part way")
if body.count("  ok") < 15:
    fail(f"only {body.count('  ok')} cases reported ok, expected 15")
# The drawn sheet is the second half of the point: a computed subtotal, laid
# out in columns. 112.40 is 10 x 4.99 + 25 x 2.50 done in MFLPT, not typed.
if "112.40" not in body:
    fail("the drawn sheet has no computed subtotal -- formatting is right "
         "but the arithmetic behind the column is not")

print("PASS: 15 formats -- %ld and %g reproduced without a printf, $ and %,")
print("      the bar, truncation to the column, and a sheet that adds up")
for r in rows:
    if r:
        print("   ", r)
PY
