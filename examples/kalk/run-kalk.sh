#!/usr/bin/env bash
# The spreadsheet itself, typed at.
#
# kalk.c was committed as work in progress with a known fault -- keystrokes
# dropped under sustained typing, a typed 11 landing as 1 -- and no test. This
# is that test: it types a formula and then a column of figures underneath it,
# with nothing slowing the typing down, and checks that every entry arrived and
# that the formula agrees with them.
#
# WHY A FORMULA FIRST. Typing a number into a sheet with no formulas repaints
# two rows, which was never slow. The expensive path is a commit whose
# recalculation moves a value somewhere else, because that repaints all 56
# rows -- so the sum at A1 is not decoration, it is what makes every one of the
# entries below it take the slow path. Without it this test types into the
# cheap case and proves nothing about the fault it exists for.
#
# The figures are two digits and all different, so a dropped keystroke cannot
# hide: 11 arriving as 1 fails, and a lost Return merges two entries into one
# four-digit cell that matches neither. The sum fails if any of them is wrong.
#
#   ./run-kalk.sh              build, type, check
#   ./run-kalk.sh --negative   defeat the render cache, to show this can fail
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
VIEWSRC=view.c
if [ "${1:-}" = "--negative" ]; then
    NEG=1
    echo "negative control: every row a cache miss, so every commit repaints cold"
    sed 's|return (fresh\[row >> 3\] & (uint8_t)(1u << (row & 7))) != 0;|return false;  /* negative control: never a hit */|' \
        view.c > "$OUT/view.c"
    grep -q 'negative control' "$OUT/view.c" || {
        echo "the negative control patch did not apply -- row_fresh has changed shape"
        exit 1; }
    VIEWSRC="$OUT/view.c"
fi

LDSCRIPT=$RT/x816-kalk.scm
calypsi_banner

cc816 kalk.c        "$OUT/kalk.o"     || exit 1
cc816 "$VIEWSRC"    "$OUT/view.o" -I . || exit 1
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
ln816 "$OUT/KALK" "$OUT/hdr.o" "$OUT/kalk.o" "$OUT/view.o" \
      "$OUT/cell.o" "$OUT/expr.o" "$OUT/fmt.o" "$OUT/fpcall.o" "$OUT/fp.o" \
      "$OUT/kcall.o" "$OUT/shell.o" "$OUT/console.o" "$OUT/font.o" \
      "$OUT/fontcp.o" "$OUT/smc.o" "$OUT/exec.o" "$OUT/ccursor.o" \
      "$OUT/fat32.o" "$OUT/kfs.o" "$OUT/goshell.o" || exit 1

python - "$WOUT/card.img" "$WOUT/KALK.raw" <<'PY' || exit 1
import sys
from pyfatfs.PyFat import PyFat
from pyfatfs.PyFatFS import PyFatFS
img, prog = sys.argv[1], sys.argv[2]
with open(img, "wb") as f:
    f.truncate(64 * 1024 * 1024)
fat = PyFat()
fat.mkfs(img, fat_type=PyFat.FAT_TYPE_FAT32, sector_size=512, label="X816KALK")
fat.close()
fs = PyFatFS(img)
with fs.open("/KK.BIN", "wb") as g:
    g.write(open(prog, "rb").read())
fs.close()
PY

cp ../shell/kernel.bin "$OUT/kernel.bin" || exit 1

# THE PADDING IS NOT OPTIONAL, and what it is made of matters.
#
# -autokeys starts typing 500 ms after reset and never waits for anything. The
# shell spends far longer than that loading a 40 KB image off the SD card, and
# nothing drains the SMC FIFO while it does -- so the first second of typing
# lands in a 16-event buffer that overflows, and the characters that survive
# are the ones nearest the end. This test found that the hard way: it typed
# +a2+a3+a4+a5... and kalk received +a2+aa5..., which parses as column AA and
# sums to 297 instead of 396. That is the harness losing keys before the
# program exists, not the program dropping them, and padding is the only
# honest fix -- shortening the typing would just move the race.
#
# BACKSPACE is the character to pad with, because it is the one key that is
# harmless BOTH ways round: kalk ignores it outside an edit (it is below 0x20,
# so the printable test rejects it) and it is a no-op at the start of one
# (entry_len is already zero and the decrement is guarded). Padding with a
# printable character would open an entry; padding with ESC would quit to the
# shell if the characters before it were the ones lost. Losing padding is
# free and receiving it is free, which is the whole requirement.
PAD=$(printf '\b%.0s' $(seq 1 120))     # ~6 s of emulated time to load in

# Lower case is deliberate: expr.c's reference parser upper-cases what it
# reads, so a2 and A2 are the same cell -- and an unshifted character is two
# SMC events where a shifted one is four, which types twice as fast and is
# therefore twice the stress. The + signs have to be shifted; there is no
# other way to reach one.
#
# The trailing >a1 is not decoration either: it parks the cursor back on the
# formula so the status line shows its SOURCE, which is the only way to see
# which characters a dropped keystroke actually cost. A wrong total on its own
# says something went missing; the source says what.
#
# and it is padded AWAY from the burst on purpose. The eight entries above are
# the measurement; the goto after them is the instrument reading it. Typed
# immediately, the goto goes in while the FIFO is still at its deepest and
# gets eaten -- which fails the test for the instrument's sake and says
# nothing about whether the entries arrived. A short drain separates the two,
# the way a person pausing before navigating would.
DRAIN=$(printf '\b%.0s' $(seq 1 20))

KEYS="kk\n${PAD}+a2+a3+a4+a5+a6+a7+a8+a9\n11\n22\n33\n44\n55\n66\n77\n88\n${DRAIN}>a1\n"

SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout 300 \
    "$EMU/build/x16emu.exe" -boot "$(cygpath -m "$CORE/boot/boot.rom")" \
    -sdcard "$WOUT/card.img" \
    -load "F00000,$WOUT/kernel.bin" \
    -autokeys "$KEYS" \
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
    """The cursor cell is drawn in reverse, so its characters decode through
       the inverse table -- otherwise the cell the typing landed in is exactly
       the one that reads as unknown."""
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

def dump():
    for i, r in enumerate(rows):
        if r:
            print(f"  {i:2}: {r}")

# Sheet row N is drawn at screen row 3 + N and carries N+1 in its gutter, so
# the gutter is what identifies a line rather than its position -- if the view
# ever scrolled, matching on position would silently check the wrong cells.
def sheet_row(n):
    want = f"{n + 1:4} "
    for r in rows:
        if r.startswith(want):
            return r[5:]
    return None

help_row = next((r for r in rows if "arrows move" in r), None)
if help_row is None:
    print("FAIL: the help line is missing -- kalk did not start")
    dump()
    sys.exit(1)

# The help line must END where its string does. Padding a row by writing
# `s[i] ? s[i] : ' '` keeps indexing past the terminator and prints whatever
# the linker put next, which is how "MEM_ALLOC REFU" -- the first words of an
# unrelated error message -- ended up on the end of the help line. Nothing but
# a screen shows that, so the screen is where it is checked.
if not help_row.rstrip().endswith("ESC quit"):
    print("FAIL: the help line has text after it that is not part of it:")
    print(f"    {help_row!r}")
    print("  something is reading past the end of the string it is drawing")
    dump()
    sys.exit(1)

TYPED = [11, 22, 33, 44, 55, 66, 77, 88]
TOTAL = sum(TYPED)          # 396

problems = []
for i, v in enumerate(TYPED):
    body = sheet_row(i + 1)         # A2 is sheet row 1
    if body is None:
        problems.append(f"sheet row {i + 2} is not on screen at all")
    elif body.split()[:1] != [str(v)]:
        got = body.split()[0] if body.split() else "(nothing)"
        problems.append(f"A{i + 2} should hold {v} and holds {got}")

top = sheet_row(0)
if top is None or top.split()[:1] != [str(TOTAL)]:
    got = top.split()[0] if top and top.split() else "(nothing)"
    problems.append(f"A1 should be the sum {TOTAL} and is {got}")

# The formula's SOURCE, character for character, off the status line -- the
# cursor was parked back on A1 for exactly this. The sum catches a dropped
# keystroke only when it changes the answer, and it does not always: losing a
# digit can turn a2 into a valid reference to somewhere else that happens to
# be empty. Comparing the text catches every loss, including the ones that
# still evaluate.
FORMULA = "+a2+a3+a4+a5+a6+a7+a8+a9"
status = rows[0] if rows else ""
if FORMULA not in status:
    shown = status.replace("READY", "").strip()
    problems.append(f"A1's source should be {FORMULA} and reads {shown!r}")

if negative:
    if not problems:
        print("FAIL (negative control): with the render cache defeated every")
        print("  commit repaints cold, and the typing STILL arrived intact --")
        print("  so this test cannot see the fault it was written for.")
        dump()
        sys.exit(1)
    print("PASS (negative control): cache defeated, and the typing broke")
    for p in problems:
        print(f"    {p}")
    dump()
    sys.exit(0)

if problems:
    print("FAIL: typing was not received intact")
    for p in problems:
        print(f"    {p}")
    dump()
    sys.exit(1)

print("PASS: a formula and eight figures typed at full speed, all received")
print(f"      A2..A9 hold {', '.join(str(v) for v in TYPED)}")
print(f"      and A1 sums them to {TOTAL}")
dump()
PY
