#!/usr/bin/env bash
# Working-directory conformance: type a program's NAME, and come back to where
# you typed it.
#
# Two behaviours, one run, because they are the same story:
#
#   1. a bare word that is not a built-in runs <word>.BIN out of the WORKING
#      DIRECTORY -- `exiter` in /SUB runs /SUB/EXITER.BIN, with no `run` and
#      no path (runtime/shell.c, try_program)
#   2. the prompt that program exits back to is in /SUB, not at the root.
#      K_EXIT restarts the resident kernel through cstartup, which re-runs the
#      data initialiser table and would otherwise put cwdbuf back to "/" --
#      the carry-over block at $00:20A0 is what survives it (runtime/kfs.h)
#
# Both are read off ONE screen, and the screen proves each of them:
#
#   * row 0 is a fresh "X816" banner and the typed `cd /sub` is GONE. Only a
#     kernel restart clears the screen, so the program ran and exited -- which
#     is the whole of (1), since `exiter` was typed as a bare word.
#   * `pwd` answers /SUB. That is (2); without the carry-over it answers /.
#
# The resident kernel is what is under test, not the loadable prompt: K_EXIT
# only exists when the firmware region carries a kernel, so the image goes to
# $F0:0000 exactly as boot2.rom does.
#
#   ./run-cwd.sh              build artifacts must exist (sh build.sh)
#   ./run-cwd.sh --negative   put EXITER.BIN at the ROOT instead of in /SUB
#                             and expect NO exit -- which proves the lookup is
#                             relative to the working directory and not a
#                             search that would have found it anywhere
#
# Requires Pillow and pyfatfs:  pip install pillow pyfatfs
set -u

. "$(dirname "$0")/../../runtime/calypsi.sh"
cd "$(dirname "$0")"
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
WOUT=$(cygpath -m "$OUT" 2>/dev/null || echo "$OUT")

[ -f kernel.bin ] || { echo "kernel.bin missing -- run: sh build.sh"; exit 1; }
cp kernel.bin "$OUT/kernel.bin" || exit 1

# exiter is built HERE rather than in build.sh: it is a fixture for this test
# and nothing ships it. kcall.o is what lets C reach the jump table at all --
# see kcall.s for why the entry number cannot simply be a jsl operand.
cc816 exiter.c    "$OUT/exiter.o" || exit 1
as816 $RT/kcall.s "$OUT/kcall.o"  || exit 1
as816 $RT/x816hdr.s "$OUT/hdr.o"  || exit 1
ln816 "$OUT/EXITER" "$OUT/hdr.o" "$OUT/exiter.o" "$OUT/kcall.o" || exit 1

NEG=0
[ "${1:-}" = "--negative" ] && NEG=1 && \
    echo "negative control: EXITER.BIN at the root, expecting NO exit"

# A card built here, not the shared fixture: this one needs a PROGRAM in a
# subdirectory, and the conformance image (boot/mkfat32.py) deliberately holds
# awkward data rather than anything runnable.
python - "$WOUT/card.img" "$WOUT/EXITER.raw" "$NEG" <<'PY' || exit 1
import sys
from pyfatfs.PyFat import PyFat
from pyfatfs.PyFatFS import PyFatFS

img, prog, negative = sys.argv[1], sys.argv[2], sys.argv[3] == "1"
# 64 MB, the same size boot/mkfat32.py uses. Not a round number picked for
# looks: below it pyfatfs computes 0 sectors per cluster and refuses to make
# the filesystem at all.
with open(img, "wb") as f:
    f.truncate(64 * 1024 * 1024)
fat = PyFat()
fat.mkfs(img, fat_type=PyFat.FAT_TYPE_FAT32, sector_size=512, label="X816CWD")
fat.close()

fs = PyFatFS(img)
fs.makedir("/SUB")
fs.writetext("/SUB/MARKER.TXT", "sub\n")
with open(prog, "rb") as g:
    image = g.read()
# The negative puts it where the prompt must NOT look from /SUB. A shell that
# searched the root as well would pass the positive check for the wrong
# reason, and this is the only way to tell those apart.
with fs.open("/EXITER.BIN" if negative else "/SUB/EXITER.BIN", "wb") as g:
    g.write(image)
fs.close()
PY

SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout 120 \
    "$EMU/build/x16emu.exe" -boot "$(cygpath -m "$CORE/boot/boot.rom")" \
    -sdcard "$WOUT/card.img" \
    -load "F00000,$WOUT/kernel.bin" \
    -autokeys 'cd /sub\nexiter\npwd\n' \
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

rows = [row_text(r) for r in range(60)]   # the whole 60-row screen: a 34-row
                                          # window used to cut the tail off any
                                          # listing printed below the banner
body = " ".join(rows).upper()

def fail(msg):
    print("FAIL:", msg)
    for i, r in enumerate(rows):
        print(f"  {i}: {r!r}")
    sys.exit(1)

# "X816" sits under the chevron logo now, not alone on row 0. Bounded by the
# prompt row rather than a fixed window: the banner is above it, and anything
# a command prints below could contain the same four characters.
_pi = next((i for i, r in enumerate(rows) if r.startswith(">")), None)
if not any("X816" in r for r in rows[:_pi if _pi is not None else 12]):
    fail("no banner -- the kernel did not come up from the firmware region")

if negative:
    # EXITER.BIN is at the root and the prompt is in /SUB, so the bare word
    # must be refused. A refusal leaves the screen alone, which is how we can
    # still see the `cd /sub` that a real exit would have cleared away.
    if "CD /SUB" not in body:
        fail("the screen was cleared -- something exited, so the bare name "
             "was found OUTSIDE the working directory")
    if "EXITER?" not in body.replace(" ", ""):
        fail("no `?' for the unfound program -- expected the usual refusal")
    print("PASS (negative control): a program at the ROOT is not found from "
          "/SUB -- the lookup is relative to the working directory")
    sys.exit(0)

# A restart is the only thing that clears the console, so the typed `cd /sub`
# being gone IS the proof that `exiter` -- a bare word, no `run`, no path --
# loaded and ran and then handed the machine back.
if "CD /SUB" in body:
    fail("`cd /sub' is still on screen: nothing cleared it, so the bare "
         "`exiter' never ran /SUB/EXITER.BIN")
if "EXITER?" in body.replace(" ", ""):
    fail("`exiter?' on screen -- the bare name was refused, not run")
if "/SUB" not in body:
    fail("`pwd' did not answer /SUB -- the prompt came back at the root, so "
         "the working directory did not survive K_EXIT")

print("PASS: a bare `exiter' ran /SUB/EXITER.BIN out of the working directory,")
print("      and the prompt it exited to was still in /SUB")
for r in rows:
    if r:
        print("   ", r)
PY
