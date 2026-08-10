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
CLEAR=0
CSVMODE=0
INSMODE=0
REPMODE=0
VIEWSRC=view.c
if [ "${1:-}" = "--replicate" ]; then
    REPMODE=1
    echo "replicating a formula, and checking the dollars"
fi
if [ "${1:-}" = "--insert" ]; then
    INSMODE=1
    echo "inserting a row through /IR, and checking the formula followed"
fi
if [ "${1:-}" = "--csv" ]; then
    CSVMODE=1
    echo "saving and reloading through the / menu"
fi
if [ "${1:-}" = "--clear" ]; then
    CLEAR=1
    echo "clearing the sheet with /C, to prove the cache goes with it"
fi
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
cc816 sheet.c       "$OUT/sheet.o" -I . || exit 1
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
      "$OUT/cell.o" "$OUT/expr.o" "$OUT/fmt.o" "$OUT/sheet.o" "$OUT/fpcall.o" "$OUT/fp.o" \
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

#
# Then the / menu, after the burst so a dropped keystroke cannot be blamed on
# it. Each command is chosen so its effect is READABLE on the final screen
# rather than merely non-crashing:
#
#   /F $   makes A1 currency, so the sum grows a $ and two decimals
#   /GC 12 widens every column, which moves every figure right and drops
#          the rightmost column letter off the header
#   /GF I  makes every cell WITHOUT its own format integer, so A2..A9 lose
#          nothing (they are whole) but A1 must KEEP its dollars -- which is
#          the part that proves /F beats /GF rather than being overwritten
#
# The goto comes BEFORE the menu, and that ordering is the test's own bug
# caught once already: /F is "this cell", and after eight entries the cursor
# sits on A10, so a /F typed there formats an empty cell and A1 stays plain.
# Parking on A1 first is what makes /F land on the cell being checked -- and
# it leaves the status line showing A1's source at the end, which is the other
# thing the final frame is read for.
MENU='/f$/gc12\n/gfi'

KEYS="kk\n${PAD}+a2+a3+a4+a5+a6+a7+a8+a9\n11\n22\n33\n44\n55\n66\n77\n88\n${DRAIN}>a1\n${DRAIN}${MENU}"

# /C gets its own run, because a cleared sheet has nothing left for the
# assertions above to check. It is the first caller cell_clear_all has ever
# had, and view.h is explicit that whoever calls it owes a view_dirty_all:
# every cached line describes a sheet that no longer exists. Forgetting that
# leaves the old figures on screen over an empty sheet -- which reads as "the
# clear did nothing", and is precisely the class of bug a render cache adds.
if [ "$CLEAR" = 1 ]; then
    KEYS="kk\n${PAD}+a2+a3+a4+a5+a6+a7+a8+a9\n11\n22\n33\n44\n55\n66\n77\n88\n${DRAIN}/c"
fi

# --csv drives the file commands through the MENU, which is the path a person
# takes and the only one that exercises the filename prompt, the reporting,
# and the render cache being thrown away on load. sheettest.c covers the
# format itself; this covers the three keys in front of it.
#
# Save, CLEAR the sheet, load it back. The two failures it separates are worth
# naming: if the load did not happen the screen stays empty, and if it happened
# but the cache was not invalidated the screen shows lines composed before the
# clear. Both look nothing like the pass.
# --insert drives /IR from the menu. The sheet is A1 = the sum of A2..A9, so
# inserting a row ABOVE it moves everything down one and every reference in
# the formula has to move with it. The sum still reading 396 afterwards is the
# check: if the references were not rewritten the formula would be summing
# A2..A9 while the figures now live in A3..A10, and it would show 385 -- a
# number that looks entirely plausible on screen.
# --replicate is the one that needs a formula carrying BOTH kinds of
# reference. A1..A4 hold 11,22,33,44; D1 holds 2; B1 is +a1*$d$1, so B1 = 22.
# Replicating B1 down to B2...B4 must give +a2*$d$1, +a3*$d$1, +a4*$d$1 -- the
# row follows the copy, the anchored D1 does not -- so the column reads
# 22, 44, 66, 88.
#
# The failures this separates both look like a working sheet: if the dollars
# were ignored the copies would multiply by D2, D3, D4, which are EMPTY, and
# the column would read 22 then three zeros. If they were honoured everywhere
# the column would read 22 four times.
if [ "$REPMODE" = 1 ]; then
    KEYS="kk
${PAD}11
22
33
44
${DRAIN}>d1
2
${DRAIN}>b1
+a1*\$d\$1
${DRAIN}/rb1
${DRAIN}b2...b4
${DRAIN}"
fi

if [ "$INSMODE" = 1 ]; then
    KEYS="kk
${PAD}+a2+a3+a4+a5+a6+a7+a8+a9
11
22
33
44
55
66
77
88
${DRAIN}>a1
${DRAIN}/ir${DRAIN}"
fi

if [ "$CSVMODE" = 1 ]; then
    KEYS="kk\n${PAD}+a2+a3+a4+a5+a6+a7+a8+a9\n11\n22\n33\n44\n55\n66\n77\n88\n${DRAIN}/ssk.csv\n${DRAIN}/c${DRAIN}/slk.csv\n${DRAIN}"
fi

SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout 300 \
    "$EMU/build/x16emu.exe" -boot "$(cygpath -m "$CORE/boot/boot.rom")" \
    -sdcard "$WOUT/card.img" \
    -load "F00000,$WOUT/kernel.bin" \
    -autokeys "$KEYS" \
    -warp -gif "$WOUT/out.gif" >/dev/null 2>&1

python - "$WOUT/out.gif" "$RT/font_cp437.s" "$NEG" "$CLEAR" "$CSVMODE" "$INSMODE" "$REPMODE" <<'PY'
import sys, re, io
from PIL import Image, ImageFile
ImageFile.LOAD_TRUNCATED_IMAGES = True

gif, fontinc = sys.argv[1], sys.argv[2]
negative, clearing = sys.argv[3] == "1", sys.argv[4] == "1"
csvmode = sys.argv[5] == "1"
insmode = sys.argv[6] == "1"
repmode = sys.argv[7] == "1"

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

if repmode:
    # B1 = +a1*$d$1 replicated to B2..B4. The relative row follows the copy,
    # the anchored $D$1 does not, so the column is the A column doubled.
    want = [22, 44, 66, 88]
    trouble = []
    for k, v in enumerate(want):
        body = sheet_row(k)
        parts = body.split() if body else []
        got = parts[1] if len(parts) > 1 else "(nothing)"
        if got != str(v):
            trouble.append(f"B{k + 1} should be {v} and reads {got}")
    if trouble:
        print("FAIL: /R did not adjust the references correctly")
        for t in trouble:
            print(f"    {t}")
        print("  22 then three zeros means the dollars were IGNORED and the")
        print("  copies are multiplying by the empty D2, D3, D4")
        print("  22 four times means they were honoured everywhere and the")
        print("  row never followed the copy")
        dump()
        sys.exit(1)
    print("PASS (/R): a formula replicated down a column -- the relative")
    print("      reference followed the copy and the $D$1 stayed put")
    dump()
    sys.exit(0)

if insmode:
    # A row went in above everything, so the figures are one lower and the
    # formula moved from A1 to A2 -- and must still add up.
    trouble = []
    top = sheet_row(0)
    if top and top.strip():
        trouble.append(f"A1 should be the inserted blank row and shows {top.strip()!r}")
    got = sheet_row(1)
    if not got or got.split()[:1] != ["396"]:
        shown = got.split()[0] if got and got.split() else "(nothing)"
        trouble.append(f"the sum moved to A2 should still be 396 and is {shown} "
                       f"-- the references did not follow their cells")
    for k, v in enumerate([11, 22, 33, 44, 55, 66, 77, 88]):
        body = sheet_row(k + 2)
        shown = body.split()[0] if body and body.split() else "(nothing)"
        if shown != str(v):
            trouble.append(f"A{k + 3} should hold {v} and holds {shown}")
    if trouble:
        print("FAIL: /IR did not move the sheet correctly")
        for t in trouble:
            print(f"    {t}")
        dump()
        sys.exit(1)
    print("PASS (/IR): a row inserted above everything -- the figures moved")
    print("      down, the formula moved with them, and it still sums to 396")
    dump()
    sys.exit(0)

if csvmode:
    # After save, clear, load: the figures must be BACK. An empty screen means
    # the load did not happen; the pre-clear figures with a stale cursor mean
    # it did but the cache was not thrown away.
    want = [396, 11, 22, 33, 44, 55, 66, 77, 88]
    trouble = []
    for k, v in enumerate(want):
        body = sheet_row(k)
        got = body.split()[0] if body and body.split() else "(nothing)"
        if got != str(v):
            trouble.append(f"A{k + 1} should be back as {v} and reads {got}")
    if trouble:
        print("FAIL: the sheet did not come back from the file")
        for t in trouble:
            print(f"    {t}")
        print("  an empty sheet means /SL never ran; the old figures with")
        print("  nothing reloaded would mean the cache outlived the clear")
        dump()
        sys.exit(1)
    print("PASS (/SS and /SL): a sheet saved to the card through the menu,")
    print("      cleared, and loaded back with every figure and the sum intact")
    dump()
    sys.exit(0)

if clearing:
    # Nine cells were filled and then /C cleared the sheet. What must be gone
    # is the FIGURES: if the cache were not invalidated they would still be on
    # screen, drawn from lines describing a sheet that no longer exists, and
    # everything else would look perfectly normal.
    left = []
    for n in range(12):
        body = sheet_row(n)
        if body and body.strip():
            left.append(f"sheet row {n + 1} still shows {body.strip()!r}")
    if left:
        print("FAIL: /C did not clear the screen")
        for p in left:
            print(f"    {p}")
        print("  the sheet was emptied but the cached lines were not thrown")
        print("  away, so the old figures are still being drawn")
        dump()
        sys.exit(1)
    # and the program must still be alive and on A1
    if not rows[0].strip().startswith("A1"):
        print(f"FAIL: after /C the cursor is not at A1: {rows[0].strip()!r}")
        dump()
        sys.exit(1)
    print("PASS (/C): nine filled cells cleared, and the cached lines with")
    print("      them -- the screen is empty and the cursor is back at A1")
    dump()
    sys.exit(0)

problems = []
for i, v in enumerate(TYPED):
    body = sheet_row(i + 1)         # A2 is sheet row 1
    if body is None:
        problems.append(f"sheet row {i + 2} is not on screen at all")
    elif body.split()[:1] != [str(v)]:
        got = body.split()[0] if body.split() else "(nothing)"
        problems.append(f"A{i + 2} should hold {v} and holds {got}")

# A1 was given the currency format by /F $, which in kalk means "%.2f" and
# NOT a dollar sign -- fmt.h records the original's own line, `if (fmt == '$')
# snprintf(t, "%.2f", val)`. The $ is the key you press, not a character that
# appears. Worth pinning precisely because it reads like a bug.
#
# That it still shows two decimals after /GF I is the real point: a cell's own
# format outranks the sheet's, and a /GF that overwrote cell.fmt would leave a
# bare 396 here.
top = sheet_row(0)
want_top = f"{TOTAL}.00"
if top is None or top.split()[:1] != [want_top]:
    got = top.split()[0] if top and top.split() else "(nothing)"
    problems.append(f"A1 should be the sum in the currency format ({want_top}) "
                    f"and is {got}")

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

# /GC 12 widened every column. The header row is where that is unambiguous:
# at the default 9 the gutter plus eight columns reaches H, and at 12 only six
# columns fit, so F is the last letter. A width command that silently did
# nothing would leave H there and everything else on screen would still look
# perfectly reasonable.
hdr = rows[2]
if "H" in hdr:
    problems.append("/GC 12 did not widen the columns -- the header still "
                    f"reaches H: {hdr.strip()!r}")
elif "F" not in hdr:
    problems.append(f"the header lost too many columns after /GC 12: {hdr.strip()!r}")

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
