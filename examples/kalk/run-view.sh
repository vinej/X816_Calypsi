#!/usr/bin/env bash
# The sheet, drawn: headers, gutter, formats, the cursor, and spilling labels.
#
# This is the first test here judged by LOOKING as well as by assertion, so it
# builds a sheet worth looking at -- the sample order from the Prog8 port, with
# quantities multiplied by prices and a subtotal, all computed in MFLPT rather
# than typed. If the arithmetic, the formatter or the layout is wrong, the
# picture says so.
#
# The two things a renderer gets wrong quietly are both on screen at once:
#
#   SPILL. Row 9 holds an eighteen-character label in a nine-wide column with
#   an empty neighbour, so it runs on. Row 10 holds the SAME label with the
#   neighbour occupied, so it is cut at exactly nine characters. Side by side,
#   the difference is the thing you see; separately, either looks plausible.
#
#   ALIGNMENT. Numbers right, labels left, column letters centred over their
#   columns. A column drifted by one place is obvious in a picture and nearly
#   invisible in a list of strings.
#
# The decoder also reads REVERSED cells, because the cursor is drawn by
# re-emitting the cell's characters with the attribute swapped -- a decoder
# that only knew normal glyphs would report the cursor as unreadable and could
# not tell a highlight from a blank.
#
#   ./run-view.sh              build and run
#   ./run-view.sh --negative   stop labels spilling, to prove this can fail
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
SRC=view.c
if [ "${1:-}" = "--negative" ]; then
    NEG=1
    echo "negative control: labels never spill, so row 9 must be cut short"
    sed 's/for (look = col + 1; look < KALK_COLS; look++)/for (look = KALK_COLS; look < KALK_COLS; look++)/' \
        view.c > "$OUT/view.c"
    SRC="$OUT/view.c"
fi

LDSCRIPT=$RT/x816-kalk.scm

cc816 viewtest.c    "$OUT/viewtest.o" || exit 1
cc816 "$SRC"        "$OUT/view.o" -I . || exit 1
cc816 cell.c        "$OUT/cell.o"     || exit 1
cc816 fmt.c         "$OUT/fmt.o"      || exit 1
cc816 $RT/fp.c      "$OUT/fp.o"       || exit 1
cc816 $RT/shell.c   "$OUT/shell.o"    || exit 1
cc816 $RT/console.c "$OUT/console.o"  || exit 1
cc816 $RT/font8x8.c "$OUT/font.o"     || exit 1
cc816 $RT/fat32.c   "$OUT/fat32.o"    || exit 1
cc816 $RT/kfs.c     "$OUT/kfs.o"      || exit 1
# goshell: ESC from a finished demo restarts the resident kernel, so a
# card full of these can be run one after another.
cc816 $RT/goshell.c "$OUT/goshell.o" || exit 1
as816 $RT/fpcall.s  "$OUT/fpcall.o" -I "$X16LIB" || exit 1
as816 $RT/kcall.s   "$OUT/kcall.o"    || exit 1
as816 $RT/x816hdr.s "$OUT/hdr.o"      || exit 1
as816 $RT/smc.s     "$OUT/smc.o"      || exit 1
as816 $RT/exec.s    "$OUT/exec.o"     || exit 1
as816 $RT/font_cp437.s "$OUT/fontcp.o" || exit 1
as816 $RT/ccursor.s "$OUT/ccursor.o"  || exit 1
ln816 "$OUT/VIEWTEST" "$OUT/hdr.o" "$OUT/viewtest.o" "$OUT/view.o" \
      "$OUT/cell.o" "$OUT/fmt.o" "$OUT/fpcall.o" "$OUT/fp.o" "$OUT/kcall.o" \
      "$OUT/shell.o" "$OUT/console.o" "$OUT/font.o" "$OUT/fontcp.o" \
      "$OUT/smc.o" "$OUT/exec.o" "$OUT/ccursor.o" "$OUT/fat32.o" \
      "$OUT/kfs.o" "$OUT/goshell.o" || exit 1

python - "$WOUT/card.img" "$WOUT/VIEWTEST.raw" <<'PY' || exit 1
import sys
from pyfatfs.PyFat import PyFat
from pyfatfs.PyFatFS import PyFatFS
img, prog = sys.argv[1], sys.argv[2]
with open(img, "wb") as f:
    f.truncate(64 * 1024 * 1024)
fat = PyFat()
fat.mkfs(img, fat_type=PyFat.FAT_TYPE_FAT32, sector_size=512, label="X816VIEW")
fat.close()
fs = PyFatFS(img)
with fs.open("/VT.BIN", "wb") as g:
    g.write(open(prog, "rb").read())
fs.close()
PY

cp ../shell/kernel.bin "$OUT/kernel.bin" || exit 1

SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout 180 \
    "$EMU/build/x16emu.exe" -boot "$(cygpath -m "$CORE/boot/boot.rom")" \
    -sdcard "$WOUT/card.img" \
    -load "F00000,$WOUT/kernel.bin" \
    -autokeys 'vt\n' \
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
    """Decoded row. A reversed cell -- the cursor -- decodes through the
       inverse table, so a highlight reads as its own character rather than
       as an unknown glyph."""
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

def fail(msg):
    print("FAIL:", msg)
    for i, r in enumerate(rows):
        if r:
            print(f"  {i:2}: {r}")
    sys.exit(1)

if "SHEET VIEW OK" not in body:
    if "SHEET VIEW FAILED" in body:
        fail("an assertion in the program failed")
    fail("no verdict -- the program did not finish drawing")

# Row 9 of the sheet is the spill case: an 18-character label in a 9-wide
# column whose neighbour is empty.
spill = [r for r in rows if "This label runs on" in r]
cut   = [r for r in rows if "This labeSTOP" in r]

if negative:
    if spill:
        fail("a label still ran past its column with spilling disabled -- "
             "this test cannot see the difference, so it cannot see a bug")
    print("PASS (negative control): with spill disabled the long label is "
          "cut, and the check notices")
    sys.exit(0)

if not spill:
    fail("the long label did not spill into its empty neighbour")
if not cut:
    fail("the long label was not cut by the occupied neighbour -- spill is "
         "running past cells that are not empty")

# The render cache, from both sides. Row 12 was edited without being declared
# dirty and row 14 with -- so the first must still show the OLD text and the
# second the NEW. Checking only the second would pass just as happily with the
# cache switched off, which is why both are here.
if "CACHE BROKEN" in body:
    fail("a cell edited WITHOUT view_dirty_row still reached the screen -- "
         "the cache is not being consulted, so nothing below is measuring it")
if "CACHE STALE OK" not in body:
    fail("the row edited without view_dirty_row lost its cached text anyway")
if "CACHE NOT INVALIDATED" in body:
    fail("view_dirty_row did not invalidate: the row still shows what was "
         "cached before the edit")
if "CACHE FRESH OK" not in body:
    fail("the row edited WITH view_dirty_row did not redraw")

# The arithmetic behind the picture: 10*4.99 + 25*2.50 + 3*12.75.
for want in ("49.90", "62.50", "38.25", "150.65"):
    if want not in body:
        fail(f"{want} is not on screen -- the totals column is not the "
             f"product of its row")
if "Subtotal" not in body:
    fail("no Subtotal label")

# Headers centred over their columns, and the gutter numbered.
hdr = rows[2]
if "A" not in hdr or "B" not in hdr or "D" not in hdr:
    fail("column letters missing from the header row")
if not any(r.startswith("   1 ") for r in rows):
    fail("the row gutter is not numbered")

print("PASS: a drawn sheet -- centred headers, a numbered gutter, right")
print("      aligned currency, a computed subtotal, and a label that spills")
print("      into an empty neighbour but is cut by a full one")
for i, r in enumerate(rows):
    if r:
        print(f"    {i:2}: {r}")
PY
