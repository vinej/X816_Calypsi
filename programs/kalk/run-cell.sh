#!/usr/bin/env bash
# Cell-store conformance: 256 x 1024 cells over four megabytes of flat memory.
#
# The addressing is two shifts and an or. That is the whole point of choosing
# 256 columns and a 16-byte cell -- the X16 port needed a bank register, a
# multiply and two lookup tables for the same job -- and it is also the risk:
# one wrong bit in the index and cells alias each other, which reads as a
# recalculation bug for a long time before it reads as an addressing one.
#
# So the cases are chosen to fail on a single wrong index bit: both far
# corners including the very last byte of the allocation, neighbours that
# differ by one in each field, and one cell per power of two in both. Each
# stamp depends on BOTH coordinates, so an aliased cell answers with the other
# cell's number rather than with a plausible zero.
#
# It also covers the two mechanisms that make 262,144 cells affordable rather
# than merely possible, because both are the kind that appear to work while
# doing nothing:
#
#   * the WATERMARK, which bounds every sweep the spreadsheet will ever do
#   * PER-ROW initialisation, which is why a fresh sheet opens instantly
#     instead of zeroing four megabytes
#
# NEEDS THE RESIDENT KERNEL, because the grid comes from MEM_ALLOC. The test
# is put on a card and launched by name, which also exercises the prompt's
# run-a-program-by-typing-it path.
#
#   ./run-cell.sh              build and run
#   ./run-cell.sh --negative   break the addressing, to prove this can fail
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
SRC=cell.c
if [ "${1:-}" = "--negative" ]; then
    NEG=1
    echo "negative control: shifting the column into the row, expecting aliases"
    # (row << 8 | col) << 4  becomes  (row << 8 | col) << 3 -- every other
    # cell now overlaps its neighbour. A test that cannot see this cannot see
    # a real addressing bug either.
    sed 's/| col) << 4)/| col) << 3)/' cell.c > "$OUT/cell.c"
    SRC="$OUT/cell.c"
fi

# The fast-RAM map: code bounded to bank $01, banks $02-$04 claimed as data.
LDSCRIPT=$RT/x816-kalk.scm

cc816 celltest.c    "$OUT/celltest.o" || exit 1
# -I here, not just $RT: the negative control compiles a doctored COPY of
# cell.c out of a temp directory, and the compiler looks for "cell.h" beside
# the source file it was handed.
cc816 "$SRC"        "$OUT/cell.o" -I .    || exit 1
cc816 $RT/fp.c      "$OUT/fp.o"       || exit 1
cc816 $RT/shell.c   "$OUT/shell.o"    || exit 1
cc816 $RT/console.c "$OUT/console.o"  || exit 1
cc816 $RT/font8x8.c "$OUT/font.o"     || exit 1
cc816 $RT/fat32.c   "$OUT/fat32.o"    || exit 1
cc816 $RT/kfs.c     "$OUT/kfs.o"      || exit 1
cc816 $RT/kmem.c     "$OUT/kmem.o"      || exit 1
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
ln816 "$OUT/CELLTEST" "$OUT/hdr.o" "$OUT/celltest.o" "$OUT/cell.o" \
      "$OUT/fpcall.o" "$OUT/fp.o" "$OUT/kcall.o" "$OUT/shell.o" \
      "$OUT/console.o" "$OUT/font.o" "$OUT/fontcp.o" "$OUT/smc.o" \
      "$OUT/exec.o" "$OUT/ccursor.o" "$OUT/fat32.o" "$OUT/kfs.o" "$OUT/goshell.o" || exit 1

python - "$WOUT/card.img" "$WOUT/CELLTEST.raw" <<'PY' || exit 1
import sys
from pyfatfs.PyFat import PyFat
from pyfatfs.PyFatFS import PyFatFS
img, prog = sys.argv[1], sys.argv[2]
# 64 MB: below that pyfatfs computes 0 sectors per cluster and refuses.
with open(img, "wb") as f:
    f.truncate(64 * 1024 * 1024)
fat = PyFat()
fat.mkfs(img, fat_type=PyFat.FAT_TYPE_FAT32, sector_size=512, label="X816CELL")
fat.close()
fs = PyFatFS(img)
with fs.open("/CT.BIN", "wb") as g:
    g.write(open(prog, "rb").read())
fs.close()
PY

cp ../shell/kernel.bin "$OUT/kernel.bin" || exit 1

SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout 180 \
    "$EMU/build/x16emu.exe" -boot "$(cygpath -m "$CORE/boot/boot.rom")" \
    -sdcard "$WOUT/card.img" \
    -load "F00000,$WOUT/kernel.bin" \
    -autokeys 'ct\n' \
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

rows = [row_text(r) for r in range(24)]
body = "\n".join(rows)

def fail(msg):
    print("FAIL:", msg)
    for i, r in enumerate(rows):
        if r:
            print(f"  {i}: {r}")
    sys.exit(1)

if not rows[0].startswith("X816 CELL STORE"):
    fail("no banner -- the program did not reach main()")
if "MEM_ALLOC REFUSED" in body:
    fail("the allocator refused -- kernel not resident, or the arena moved")

if negative:
    if "CELL STORE OK" in body:
        fail("a broken address shift still passed -- this test cannot see "
             "aliasing, so it cannot see a real addressing bug either")
    print("PASS (negative control): a one-bit addressing error is caught")
    sys.exit(0)

if "FAILED AT CASE" in body:
    fail("at least one case did not match")
if "CELL STORE OK" not in body:
    fail("no verdict -- the run stopped part way")
if body.count("  ok") < 10:
    fail(f"only {body.count('  ok')} cases reported ok, expected 10")

print("PASS: 4 MiB of sheet -- corners, neighbours, every index bit, the")
print("      watermark, per-row init and the text arena")
for r in rows:
    if r:
        print("   ", r)
PY
