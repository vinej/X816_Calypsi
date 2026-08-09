#!/usr/bin/env bash
# Formulas: kalk.c's grammar, evaluated against a sheet with values in it.
#
# Thirty-six cases, and several are here because a spreadsheet parser can be
# wrong in ways that still produce a plausible number:
#
#   PRECEDENCE. 1+2*3 is 7. VisiCalc evaluated strictly left to right and
#   would say 9; kalk.c has ordinary precedence. A parser that walked left to
#   right agrees on every single-operator formula and diverges only on real
#   ones -- so a test with nothing but A1+A2 in it would pass either way.
#
#   OPERAND ORDER. 10-4 against 4-10. These differ only in which way round the
#   accumulator is fed, which is exactly why the float package carries both
#   f_sub and f_rsub, and getting it backwards stays invisible until a
#   subtraction is nested.
#
#   WHAT COUNTS AS A VALUE. The fixture puts a LABEL in the middle of A1..A5,
#   so @SUM is 120 over four values, @COUNT is 4 and @AVERAGE is 30. An
#   @AVERAGE that divided by the size of the range would say 24 -- a perfectly
#   plausible number, and the classic spreadsheet lie.
#
#   ERROR BEATS NA. MFLPT has no NaN to carry a failure inside the value, so
#   the precedence between the two lives in the evaluator and is worth pinning.
#
# The last case caught a real bug: @LOG10 came back ERROR because the name
# scanner accepted only letters and read the name as "LOG", which then failed
# to match anything. A correctly spelled function reported as misspelled.
#
#   ./run-expr.sh              build and run
#   ./run-expr.sh --negative   make * and + bind equally, to prove this fails
#
# Requires Pillow and pyfatfs:  pip install pillow pyfatfs
set -u

. "$(dirname "$0")/../../runtime/calypsi.sh"
cd "$(dirname "$0")"
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
WOUT=$(cygpath -m "$OUT" 2>/dev/null || echo "$OUT")

[ -f ../shell/kernel.bin ] || {
    echo "../shell/kernel.bin missing -- run: sh ../shell/build.sh"; exit 1; }

NEG=0
SRC=expr.c
if [ "${1:-}" = "--negative" ]; then
    NEG=1
    echo "negative control: term() also accepts + and -, collapsing precedence"
    # With '+' and '-' consumed by term(), 1+2*3 evaluates left to right and
    # gives 9 -- VisiCalc's answer, and the wrong one here.
    sed "s/if (op != '\*' \&\& op != '\/')/if (op != '*' \&\& op != '\/' \&\& op != '+')/" \
        expr.c > "$OUT/expr.c"
    SRC="$OUT/expr.c"
fi

LDSCRIPT=$RT/x816-kalk.scm

cc816 exprtest.c    "$OUT/exprtest.o" || exit 1
cc816 "$SRC"        "$OUT/expr.o" -I . || exit 1
cc816 cell.c        "$OUT/cell.o"     || exit 1
cc816 fmt.c         "$OUT/fmt.o"      || exit 1
cc816 $RT/fp.c      "$OUT/fp.o"       || exit 1
cc816 $RT/shell.c   "$OUT/shell.o"    || exit 1
cc816 $RT/console.c "$OUT/console.o"  || exit 1
cc816 $RT/font8x8.c "$OUT/font.o"     || exit 1
cc816 $RT/fat32.c   "$OUT/fat32.o"    || exit 1
cc816 $RT/kfs.c     "$OUT/kfs.o"      || exit 1
as816 $RT/fpcall.s  "$OUT/fpcall.o" -I "$X16LIB" || exit 1
as816 $RT/kcall.s   "$OUT/kcall.o"    || exit 1
as816 $RT/x816hdr.s "$OUT/hdr.o"      || exit 1
as816 $RT/smc.s     "$OUT/smc.o"      || exit 1
as816 $RT/exec.s    "$OUT/exec.o"     || exit 1
as816 $RT/font_cp437.s "$OUT/fontcp.o" || exit 1
as816 $RT/ccursor.s "$OUT/ccursor.o"  || exit 1
ln816 "$OUT/EXPRTEST" "$OUT/hdr.o" "$OUT/exprtest.o" "$OUT/expr.o" \
      "$OUT/cell.o" "$OUT/fmt.o" "$OUT/fpcall.o" "$OUT/fp.o" "$OUT/kcall.o" \
      "$OUT/shell.o" "$OUT/console.o" "$OUT/font.o" "$OUT/fontcp.o" \
      "$OUT/smc.o" "$OUT/exec.o" "$OUT/ccursor.o" "$OUT/fat32.o" \
      "$OUT/kfs.o" || exit 1

python - "$WOUT/card.img" "$WOUT/EXPRTEST.raw" <<'PY' || exit 1
import sys
from pyfatfs.PyFat import PyFat
from pyfatfs.PyFatFS import PyFatFS
img, prog = sys.argv[1], sys.argv[2]
with open(img, "wb") as f:
    f.truncate(64 * 1024 * 1024)
fat = PyFat()
fat.mkfs(img, fat_type=PyFat.FAT_TYPE_FAT32, sector_size=512, label="X816EXPR")
fat.close()
fs = PyFatFS(img)
with fs.open("/ET.BIN", "wb") as g:
    g.write(open(prog, "rb").read())
fs.close()
PY

cp ../shell/kernel.bin "$OUT/kernel.bin" || exit 1

SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout 180 \
    "$EMU/build/x16emu.exe" -boot "$(cygpath -m "$CORE/boot/boot.rom")" \
    -sdcard "$WOUT/card.img" \
    -load "F00000,$WOUT/kernel.bin" \
    -autokeys 'et\n' \
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

rows = [row_text(r) for r in range(48)]
body = "\n".join(rows)

def fail(msg):
    print("FAIL:", msg)
    for r in rows:
        if r:
            print("  ", r)
    sys.exit(1)

if "X816 FORMULAS" not in body:
    fail("no banner -- the program did not reach main()")
if "MEM_ALLOC REFUSED" in body:
    fail("the allocator refused -- kernel not resident?")

if negative:
    if "FORMULAS OK" in body:
        fail("precedence was collapsed and every case still passed -- this "
             "test cannot tell 1+2*3 from (1+2)*3")
    print("PASS (negative control): collapsing precedence is caught")
    sys.exit(0)

if "FAILED AT CASE" in body:
    fail("at least one formula did not evaluate as kalk.c would")
if "FORMULAS OK" not in body:
    fail("no verdict -- the run stopped part way, so a formula hung")
if body.count("  ok") < 36:
    fail(f"only {body.count('  ok')} cases reported ok, expected 36")

print("PASS: 36 formulas -- precedence, absolute refs, ranges either way")
print("      round, five aggregates over a range with a gap in it, the")
print("      value functions, and every way a formula can fail")
for r in rows:
    if r:
        print("   ", r)
PY
