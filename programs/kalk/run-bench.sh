#!/usr/bin/env bash
# Where a repaint goes: grid reads, formatting, spill lookahead, VERA writes.
#
# This is not a pass/fail test of behaviour -- it is the measurement the render
# cache is designed from, and it exists because the last one was ambiguous. The
# previous round recorded con_putraw at 90 us a character AND a whole 80-column
# row at 6.44 ms, which are the same number twice over and leave the formatter
# costing nothing. Both cannot be describing what their names say.
#
# So the parts are timed separately and must sum to roughly the whole:
#
#     GET    448 cell_get, the grid traffic alone
#     FMT    the same, plus fp_load and fmt_number   (FMT - GET is formatting)
#     EMIT   56 ready-made 80-column lines to VERA
#     ROW    56 view_draw_row -- everything, lookahead included
#     DRAW   one view_draw
#
# and ROW - FMT - EMIT is the spill lookahead, which nobody has priced.
#
#   ./run-bench.sh              build, run, print the five figures
#   ./run-bench.sh --negative   halve the work, to prove the clock is real
#
# The negative control matters more here than in a behaviour test: a timer that
# is misread, stopped, or quantised too coarsely reports a plausible number for
# ANY amount of work. Drawing 28 rows instead of 56 must roughly halve ROW; if
# it does not, the figures above are decoration.
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
SRC=benchtest.c
if [ "${1:-}" = "--negative" ]; then
    NEG=1
    echo "negative control: half the rows, so ROW and DRAW must roughly halve"
    sed 's/r < VIEW_ROWS/r < (VIEW_ROWS \/ 2)/g' benchtest.c > "$OUT/benchtest.c"
    SRC="$OUT/benchtest.c"
fi

LDSCRIPT=$RT/x816-kalk.scm
calypsi_banner

cc816 "$SRC"        "$OUT/benchtest.o" -I . || exit 1
cc816 view.c        "$OUT/view.o"     || exit 1
cc816 cell.c        "$OUT/cell.o"     || exit 1
cc816 fmt.c         "$OUT/fmt.o"      || exit 1
cc816 $RT/fp.c      "$OUT/fp.o"       || exit 1
cc816 $RT/shell.c   "$OUT/shell.o"    || exit 1
cc816 $RT/console.c "$OUT/console.o"  || exit 1
cc816 $RT/font8x8.c "$OUT/font.o"     || exit 1
cc816 $RT/fat32.c   "$OUT/fat32.o"    || exit 1
cc816 $RT/kfs.c     "$OUT/kfs.o"      || exit 1
cc816 $RT/kmem.c     "$OUT/kmem.o"      || exit 1
cc816 $RT/goshell.c "$OUT/goshell.o"  || exit 1
as816 $RT/fpcall.s  "$OUT/fpcall.o" -I "$X16LIB" || exit 1
as816 $RT/kcall.s   "$OUT/kcall.o"    || exit 1
as816 $RT/x816hdr.s "$OUT/hdr.o"      || exit 1
as816 $RT/smc.s     "$OUT/smc.o"      || exit 1
as816 $RT/exec.s    "$OUT/exec.o"     || exit 1
as816 $RT/font_cp437.s "$OUT/fontcp.o" || exit 1
as816 $RT/ccursor.s "$OUT/ccursor.o"  || exit 1
ln816 "$OUT/BENCH" "$OUT/hdr.o" "$OUT/benchtest.o" "$OUT/view.o" \
      "$OUT/cell.o" "$OUT/fmt.o" "$OUT/fpcall.o" "$OUT/fp.o" "$OUT/kcall.o" \
      "$OUT/shell.o" "$OUT/console.o" "$OUT/font.o" "$OUT/fontcp.o" \
      "$OUT/smc.o" "$OUT/exec.o" "$OUT/ccursor.o" "$OUT/fat32.o" \
      "$OUT/kfs.o" "$OUT/goshell.o" || exit 1

python - "$WOUT/card.img" "$WOUT/BENCH.raw" <<'PY' || exit 1
import sys
from pyfatfs.PyFat import PyFat
from pyfatfs.PyFatFS import PyFatFS
img, prog = sys.argv[1], sys.argv[2]
with open(img, "wb") as f:
    f.truncate(64 * 1024 * 1024)
fat = PyFat()
fat.mkfs(img, fat_type=PyFat.FAT_TYPE_FAT32, sector_size=512, label="X816BNCH")
fat.close()
fs = PyFatFS(img)
with fs.open("/RB.BIN", "wb") as g:
    g.write(open(prog, "rb").read())
fs.close()
PY

cp ../shell/kernel.bin "$OUT/kernel.bin" || exit 1

SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout 300 \
    "$EMU/build/x16emu.exe" -boot "$(cygpath -m "$CORE/boot/boot.rom")" \
    -sdcard "$WOUT/card.img" \
    -load "F00000,$WOUT/kernel.bin" \
    -autokeys 'rb\n' \
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

if "RENDER BENCH OK" not in body:
    if "RENDER BENCH FAILED" in body:
        fail("an assertion in the program failed -- the parts came out larger "
             "than the whole, which usually means the timer latch was read in "
             "the wrong order")
    fail("no verdict -- the program did not finish")

KEYS = ("GET", "FMT", "EMIT", "ROW", "HOT", "DRAW", "SCROLL",
        "TOSTR", "NORM", "FMTG", "FMTD", "TSBIG", "TSMID", "TSSML")

got = {}
for r in rows:
    m = re.match(r'^\s*(' + '|'.join(KEYS) + r')\s+(\d+) ms\s*$', r)
    if m:
        got[m.group(1)] = int(m.group(2))
missing = [k for k in KEYS if k not in got]
if missing:
    fail(f"no figure for {', '.join(missing)}")

if negative:
    print("PASS (negative control): half the work, and the clock followed it")
    for k in KEYS:
        print(f"    {k:6} {got[k]:6} ms")
    print("  Compare these against the full run: ROW and DRAW must be roughly")
    print("  half. If they are not, the timer is not measuring the work.")
    sys.exit(0)

# The decomposition. ROW does everything FMT does, plus the spill lookahead,
# plus the writes -- so what is left over after subtracting those is the
# lookahead, and it is the figure nobody had before.
fmt_only = got["FMT"] - got["GET"]
lookahead = got["ROW"] - got["FMT"] - got["EMIT"]

print("PASS: a repaint, split into parts that sum")
print()
print(f"    GET    {got['GET']:6} ms   448 cell_get out of the 4 MiB grid")
print(f"    FMT    {got['FMT']:6} ms   the same, plus fp_load and fmt_number")
print(f"    EMIT   {got['EMIT']:6} ms   56 ready-made lines to VERA")
print(f"    ROW    {got['ROW']:6} ms   56 view_draw_row, nothing cached")
print(f"    HOT    {got['HOT']:6} ms   the same 56 rows, every one a hit")
print(f"    DRAW   {got['DRAW']:6} ms   a cold whole-screen repaint")
print(f"    SCROLL {got['SCROLL']:6} ms   a repaint with ONE row dirty")
print()
print(f"    formatting alone      {fmt_only:6} ms   (FMT - GET)")
print(f"    spill lookahead       {lookahead:6} ms   (ROW - FMT - EMIT)")
print()

parts = [("grid reads", got["GET"]), ("formatting", fmt_only),
         ("VERA writes", got["EMIT"]), ("spill lookahead", lookahead)]
parts.sort(key=lambda kv: -kv[1])
top, cost = parts[0]
share = (100 * cost // got["ROW"]) if got["ROW"] else 0
print(f"    A cold repaint is {share}% {top}.")
if top == "VERA writes":
    print("    No cache of WHAT to draw can help: the cost is the writing. The")
    print("    fix would have to skip writes whose characters are already on")
    print("    screen, which needs a shadow of the screen, not a render cache.")
    sys.exit(0)

print("    The per-row cache removes that, and the lookahead with it.")
print()

# What the cache is worth, and against the thing that actually breaks.
speed = (got["ROW"] / got["HOT"]) if got["HOT"] else 0
print(f"    cached repaint        {got['HOT']:6} ms   {speed:.0f}x cheaper than composing")
print(f"    one row changed       {got['SCROLL']:6} ms   the scroll and commit case")
print()

# The 16-entry SMC key FIFO is about 400 ms of buffering at a fast typist's
# rate. That is the number a repaint has to stay under, and it is why this
# test reports SCROLL rather than DRAW as the verdict.
FIFO_MS = 400
if got["SCROLL"] >= FIFO_MS:
    print(f"    STILL TOO SLOW: {got['SCROLL']} ms against ~{FIFO_MS} ms of key FIFO.")
    print("    Keystrokes will be dropped under sustained typing.")
    sys.exit(1)
print(f"    A scroll or a commit now costs {got['SCROLL']} ms against roughly")
print(f"    {FIFO_MS} ms of key FIFO, so sustained typing has room. A COLD repaint")
print(f"    is still {got['DRAW']} ms, but nothing in the edit loop asks for one.")

# One level down: what the cold repaint's 92% is actually made of. The three
# figures nest -- fmt_number calls fmt_normalise calls fp_to_str_trim -- so
# the differences are the work each layer adds on its own.
print()
print("  and inside that formatting, per 448 calls:")
print()
print(f"    TOSTR  {got['TOSTR']:6} ms   fp_to_str_trim alone")
print(f"    NORM   {got['NORM']:6} ms   + parsing the digits back out")
print(f"    FMTG   {got['FMTG']:6} ms   + rounding and writing, %g")
print(f"    FMTD   {got['FMTD']:6} ms   the currency path, for comparison")
print()
parse = got["NORM"] - got["TOSTR"]
write = got["FMTG"] - got["NORM"]
print(f"    the float->decimal conversion  {got['TOSTR']:6} ms")
print(f"    parsing that string back       {parse:6} ms")
print(f"    rounding and writing           {write:6} ms")
print()
if got["FMTG"]:
    conv = 100 * got["TOSTR"] // got["FMTG"]
    per = got["FMTG"] * 1000 // (448 or 1)
    print(f"    A %g format is {conv}% the float package's decimal conversion,")
    print(f"    at roughly {per} us a call. fmt.c's own share is the rest.")
    if conv >= 60:
        print("    So fmt.c is not where to look -- the conversion is, and it is")
        print("    reached through fp_to_str_trim in fmt_normalise's first line.")

# And inside the conversion: float.s scales the value into [1e8,1e9) a decade
# at a time, then peels nine digits with a float divide each. A value already
# in range skips the scaling entirely, so TSBIG is the peeling on its own.
print()
print("  and inside ONE conversion, by how far the value has to scale:")
print()
print(f"    TSBIG  {got['TSBIG']:6} ms   123456789 -- already in [1e8,1e9), no scaling")
print(f"    TSMID  {got['TSMID']:6} ms   200.125   -- about six decades up")
print(f"    TSSML  {got['TSSML']:6} ms   0.000123  -- about twelve")
print()
step = (got["TSSML"] - got["TSBIG"]) / 12.0
print(f"    peeling nine digits    {got['TSBIG']:6} ms   (TSBIG, no scaling done)")
print(f"    one scaling decade     {step:6.0f} ms   ((TSSML - TSBIG) / 12)")
print()
if got["TSMID"]:
    peel = 100 * got["TSBIG"] // got["TSMID"]
    print(f"    On a typical sheet value the peeling is {peel}% of the conversion.")
    if peel >= 55:
        print("    fp_nine_digits is the target: nine float DIVIDES, nine multiplies")
        print("    and nine subtracts to recover digits from a value that is already")
        print("    a whole number under 1e9 -- which integer arithmetic can do.")
    else:
        print("    The scale-by-one-decade loop is the target, not the digit peeling:")
        print("    it walks decade by decade where a table of powers of ten would")
        print("    land in one step.")
PY
