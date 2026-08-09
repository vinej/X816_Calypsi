#!/usr/bin/env bash
# A sheet saved to the card as CSV and read back, and checked for being the
# SAME SHEET rather than the expected bytes.
#
# Checking a CSV writer against a string proves it writes what somebody
# expected; what a spreadsheet needs is that saving and loading is the
# identity. So the test builds a sheet with one of everything awkward in it --
# a label with a comma, a label that looks like a number, a label that looks
# like a formula, a doubled quote, a fraction that is not exact in binary, a
# gap between two full cells -- then saves, clears, loads, and compares.
#
# It also PRINTS the file, because a writer and a reader that are wrong in the
# same direction round-trip perfectly. The decoder checks the text against the
# shape the Prog8 port writes, which is the other half of the verdict.
#
#   ./run-sheet.sh              build and run
#   ./run-sheet.sh --negative   make quotes stop meaning text, to prove this
#                               test can fail
#
# The negative control targets sheet.h's one real decision: that a quoted
# field loads as a LABEL. Undo it and "2024" comes back as a number -- a sheet
# that still LOOKS right, with a column quietly changed from text to figures.
# If the round trip cannot see that, it is not testing what it claims to.
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
SRC=sheet.c
if [ "${1:-}" = "--negative" ]; then
    NEG=1
    echo "negative control: quotes stop meaning text, so a quoted 2024 must"
    echo "  come back as a number and the round trip must notice"
    sed 's/if (len || was_quoted) {/if (len) { was_quoted = false;/' \
        sheet.c > "$OUT/sheet.c"
    grep -q 'was_quoted = false;$' "$OUT/sheet.c" || {
        echo "the negative control patch did not apply -- the loader changed shape"
        exit 1; }
    SRC="$OUT/sheet.c"
fi

LDSCRIPT=$RT/x816-kalk.scm
calypsi_banner

cc816 sheettest.c   "$OUT/sheettest.o" -I . || exit 1
cc816 "$SRC"        "$OUT/sheet.o" -I . || exit 1
cc816 cell.c        "$OUT/cell.o"     || exit 1
cc816 expr.c        "$OUT/expr.o"     || exit 1
cc816 fmt.c         "$OUT/fmt.o"      || exit 1
cc816 $RT/fp.c      "$OUT/fp.o"       || exit 1
cc816 $RT/shell.c   "$OUT/shell.o"    || exit 1
cc816 $RT/console.c "$OUT/console.o"  || exit 1
cc816 $RT/font8x8.c "$OUT/font.o"     || exit 1
cc816 $RT/fat32.c   "$OUT/fat32.o"    || exit 1
cc816 $RT/kfs.c     "$OUT/kfs.o"      || exit 1
cc816 $RT/goshell.c "$OUT/goshell.o"  || exit 1
as816 $RT/fpcall.s  "$OUT/fpcall.o" -I "$X16LIB" || exit 1
as816 $RT/kcall.s   "$OUT/kcall.o"    || exit 1
as816 $RT/x816hdr.s "$OUT/hdr.o"      || exit 1
as816 $RT/smc.s     "$OUT/smc.o"      || exit 1
as816 $RT/exec.s    "$OUT/exec.o"     || exit 1
as816 $RT/font_cp437.s "$OUT/fontcp.o" || exit 1
as816 $RT/ccursor.s "$OUT/ccursor.o"  || exit 1
ln816 "$OUT/SHEETTEST" "$OUT/hdr.o" "$OUT/sheettest.o" "$OUT/sheet.o" \
      "$OUT/cell.o" "$OUT/expr.o" "$OUT/fmt.o" "$OUT/fpcall.o" "$OUT/fp.o" \
      "$OUT/kcall.o" "$OUT/shell.o" "$OUT/console.o" "$OUT/font.o" \
      "$OUT/fontcp.o" "$OUT/smc.o" "$OUT/exec.o" "$OUT/ccursor.o" \
      "$OUT/fat32.o" "$OUT/kfs.o" "$OUT/goshell.o" || exit 1

python - "$WOUT/card.img" "$WOUT/SHEETTEST.raw" <<'PY' || exit 1
import sys
from pyfatfs.PyFat import PyFat
from pyfatfs.PyFatFS import PyFatFS
img, prog = sys.argv[1], sys.argv[2]
with open(img, "wb") as f:
    f.truncate(64 * 1024 * 1024)
fat = PyFat()
fat.mkfs(img, fat_type=PyFat.FAT_TYPE_FAT32, sector_size=512, label="X816SHET")
fat.close()
fs = PyFatFS(img)
with fs.open("/ST.BIN", "wb") as g:
    g.write(open(prog, "rb").read())
fs.close()
PY

cp ../shell/kernel.bin "$OUT/kernel.bin" || exit 1

SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout 240 \
    "$EMU/build/x16emu.exe" -boot "$(cygpath -m "$CORE/boot/boot.rom")" \
    -sdcard "$WOUT/card.img" \
    -load "F00000,$WOUT/kernel.bin" \
    -autokeys 'st\n' \
    -warp -gif "$WOUT/out.gif" >/dev/null 2>&1

# The card is read back on the HOST as well. The program printing its own file
# proves it can read what it wrote; opening the image here proves the bytes on
# the card are a CSV file that something else could also read, which is the
# entire point of choosing CSV over a private format.
python - "$WOUT/card.img" > "$OUT/onhost.txt" 2>/dev/null <<'PY'
import sys
from pyfatfs.PyFatFS import PyFatFS
try:
    fs = PyFatFS(sys.argv[1])
    with fs.open("/SHEET.CSV", "rb") as f:
        sys.stdout.write(f.read().decode("latin-1"))
    fs.close()
except Exception as e:
    print("HOSTREADFAIL", e)
PY

python - "$WOUT/out.gif" "$RT/font_cp437.s" "$NEG" "$OUT/onhost.txt" <<'PY'
import sys, re, io
from PIL import Image, ImageFile
ImageFile.LOAD_TRUNCATED_IMAGES = True

gif, fontinc, negative, hostfile = (sys.argv[1], sys.argv[2],
                                    sys.argv[3] == "1", sys.argv[4])

vals = []
for line in io.open(fontinc, encoding='utf-8'):
    m = re.match(r'\s*\.byte\s+(.*)$', line.split(';')[0])
    if m:
        vals += [int(x.strip().lstrip('$'), 16)
                 for x in m.group(1).split(',') if x.strip()]
glyph, inverse = {}, {}
for _c in range(0x20, 0x7F):
    bits = tuple(vals[_c * 8:(_c + 1) * 8])
    glyph[bits] = chr(_c)
    inverse[tuple((~b) & 0xFF for b in bits)] = chr(_c)

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
    for col in range(80):
        bits = []
        for y in range(8):
            b = 0
            for x in range(8):
                if px[col * 8 + x, r * 8 + y] != (0, 0, 0):
                    b |= 0x80 >> x
            bits.append(b)
        t = tuple(bits)
        out += glyph.get(t) or inverse.get(t) or '?'
    return out.rstrip()

rows = [row_text(r) for r in range(60)]
body = "\n".join(rows)

def dump():
    for i, r in enumerate(rows):
        if r:
            print(f"  {i:2}: {r}")

host = io.open(hostfile, encoding='latin-1').read()

if negative:
    if "SHEET CSV OK" in body:
        print("FAIL (negative control): quotes were made to stop meaning text")
        print("  and every cell still came back identical -- so the round trip")
        print("  cannot see a label silently turning into a number.")
        dump()
        sys.exit(1)
    print("PASS (negative control): with quotes no longer meaning text the")
    print("  round trip breaks, so it is testing what it claims to")
    dump()
    sys.exit(0)

if "SHEET CSV OK" not in body:
    if "NO CARD" in body:
        sys.exit("FAIL: the program found no card")
    if "SHEET CSV FAILED" in body:
        print("FAIL: an assertion in the program failed")
    else:
        print("FAIL: no verdict -- the program did not finish")
    dump()
    sys.exit(1)

if "HOSTREADFAIL" in host or not host.strip():
    print("FAIL: the card has no readable /SHEET.CSV on it -- the program")
    print("  agreed with itself, but nothing else can read the file")
    print(f"  host said: {host.strip()[:200]!r}")
    sys.exit(1)

# The shape, checked on the host's copy of the bytes. These are the things a
# different CSV reader would depend on, and none of them is visible from a
# round trip that only ever talks to itself.
problems = []
lines = host.replace("\r\n", "\n").split("\n")
if '"Gadget, Deluxe"' not in host:
    problems.append('a label containing a comma is not quoted')
if '"2024"' not in host:
    problems.append('a label that looks like a number is not quoted, so it '
                    'would read back as one')
if '"+not a formula"' not in host:
    problems.append('a label that looks like a formula is not quoted')
if '"say ""hi"""' not in host:
    problems.append('a quote inside a label is not doubled')
if "+B1*3" not in host:
    problems.append('a formula was not saved as its source')
# NOT a check that the file says "0.1". A tenth is not exact in MFLPT, so the
# writer emits all nine digits it has -- 9.99999999e-02 -- which is ugly, and
# is the right trade: six significant digits would reload as a different
# number. What matters is that the field parses back to the same value, and
# Python is a fair judge of that.
fields = [f for l in lines for f in l.split(",")]
def num(f):
    try:
        return float(f)
    except ValueError:
        return None
if not any(num(f) is not None and abs(num(f) - 0.1) < 1e-8 for f in fields):
    problems.append('0.1 is not in the file as a number a reader can parse '
                    'back to a tenth')
if "-12.5" not in host:
    problems.append('a negative was not written')
if "123456789" not in host:
    problems.append('a nine-digit integer was not written plainly')
if not any(l.startswith("Item,10,,") for l in lines):
    problems.append('the empty cell between two full ones lost its comma')

if problems:
    print("FAIL: the file round-trips but is not the CSV it should be")
    for p in problems:
        print(f"    {p}")
    print("\n  /SHEET.CSV as the host reads it:")
    for l in lines:
        print(f"    {l}")
    sys.exit(1)

# The structural edits, and what they cost. sheet.h claims the price is set by
# the watermark and the row map rather than by the grid; these two numbers are
# that claim, and the same insert on a sheet holding three cells is what makes
# them mean something.
timing = [r.strip() for r in rows if "insert a row," in r]
if len(timing) != 2:
    print("FAIL: the structural-edit timings are missing from the screen")
    dump()
    sys.exit(1)

print("PASS: a sheet saved, cleared, reloaded and identical -- commas,")
print("      doubled quotes, labels that look like numbers and like formulas,")
print("      a formula's source, an inexact fraction, and a gap that stayed")
print("      a gap. The bytes are readable off the card by something that is")
print("      not this program.")
print()
print("  a row inserted, and what bounds it:")
for t in timing:
    print(f"    {t}")
print("    -- rows and columns move within the WATERMARK, and a row nobody")
print("       has written to is skipped without reading a cell of it")
print()
print("  /SHEET.CSV:")
for l in lines:
    if l:
        print(f"    {l}")
PY
