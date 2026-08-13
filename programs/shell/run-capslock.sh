#!/usr/bin/env bash
# CAPS LOCK: a toggle that upper-cases letters, inverts Shift, and releases.
#
#   ./run-capslock.sh              build and run
#   ./run-capslock.sh --negative   type the same letters WITHOUT touching
#                                  Caps Lock, and require the check to fail
#
# WHAT THIS PINS. Caps Lock used to do nothing at all: the keymap deliberately
# left the lock keys at 0, so position 30 came back as an unmatched
# KEY_SPECIAL event and every consumer dropped it. con_getkey now treats it as
# a TOGGLE -- state flips on the press edge, both edges are consumed -- and
# flips the case of letters, composing with Shift the way a PC does: caps
# alone types upper case, caps+Shift types lower. Handled in the console so
# the shell, the editor, Forth and BASIC all inherit it from one place.
#
# Position 30 is measured, not assumed: rtl/smc_x16.sv maps PS/2 $58 there,
# and -autokeys' \C sends exactly that number.
#
# THE ASSERTION IS CASE-SENSITIVE and covers all three claims in one line.
# Typing   ab \C ab AB \C ab   must produce "abABabab":
#     ab   caps off            -> ab      (baseline)
#     ab   caps ON             -> AB      (letters upper-cased)
#     AB   caps ON, Shift held -> ab      (Shift inverted, not ignored)
#     ab   caps off again      -> ab      (the toggle releases)
# A caps that sticks, ignores Shift, or never engages produces a different
# string. The harness's trailing Enter pushes the file text to the next row.
#
# Requires Pillow.
set -eu

cd "$(dirname "$0")"
bash build.sh >/dev/null

. ../../runtime/calypsi.sh

NEG=0
if [ "${1:-}" = "--negative" ]; then
    NEG=1
    echo "negative control: the same letters with Caps Lock never pressed,"
    echo "so the case pattern must differ and the check must fail"
fi

if [ "$NEG" = "1" ]; then
    KEYS='edit /hello.txt\nababABab'
else
    KEYS='edit /hello.txt\nab\CabAB\Cab'
fi

OUT_GIF=${OUT_GIF:-$PWD/out-capslock.gif}
SCRATCH=$PWD/out-capslock-card.img
rm -f "$OUT_GIF" "$SCRATCH"
trap 'rm -f "$SCRATCH"' EXIT
cp "$CORE/boot/fat32.img" "$SCRATCH"

PYTHON=${PYTHON:-python}
if ! command -v "$PYTHON" >/dev/null 2>&1; then
    PYTHON=/c/Users/jyv/AppData/Local/Programs/Python/Python312/python.exe
fi
POWERSHELL=${POWERSHELL:-powershell.exe}

"$POWERSHELL" -NoProfile -ExecutionPolicy Bypass -File "$PWD/run-edit-capture.ps1" \
    -Emu "$(cygpath -m "$EMU/build/x16emu.exe")" \
    -Boot "$(cygpath -m "$CORE/boot/boot.rom")" \
    -Kernel "$(cygpath -m "$PWD/kernel.bin")" \
    -Gif "$(cygpath -m "$OUT_GIF")" -Sdcard "$(cygpath -m "$SCRATCH")" \
    -Keys "$KEYS" -Seconds 30 >/dev/null

"$PYTHON" - "$OUT_GIF" "$(cygpath -m "$RT/font_cp437.s")" "$NEG" <<'PY'
import os, re, sys, io
from collections import Counter
from PIL import Image, ImageFile
ImageFile.LOAD_TRUNCATED_IMAGES = True

gif, fontinc, neg = sys.argv[1], sys.argv[2], sys.argv[3] == "1"
if not os.path.exists(gif):
    sys.exit("FAIL: emulator did not create GIF")

vals = []
for line in io.open(fontinc, encoding="utf-8"):
    m = re.match(r"\s*\.byte\s+(.*)$", line.split(";")[0])
    if m:
        vals += [int(x.strip().lstrip("$"), 16)
                 for x in m.group(1).split(",") if x.strip()]
glyph = {}
for c in range(0x20, 0x7F):
    glyph[tuple(vals[c * 8:(c + 1) * 8])] = chr(c)


def frame_row(im, r):
    px = im.convert("RGB").load()
    out = ""
    for col in range(80):
        colors = [px[col * 8 + x, r * 8 + y] for y in range(8) for x in range(8)]
        bg = Counter(colors).most_common(1)[0][0]
        bits = []
        for y in range(8):
            b = 0
            for x in range(8):
                if px[col * 8 + x, r * 8 + y] != bg:
                    b |= 0x80 >> x
            bits.append(b)
        ch = glyph.get(tuple(bits))
        if ch is None:
            ch = glyph.get(tuple((~b) & 0xFF for b in bits), "?")
        out += ch
    return out.rstrip()


def frame_count(im):
    n = 0
    while True:
        try:
            im.seek(n); im.load(); n += 1
        except (EOFError, OSError, IndexError):
            break
    return n


im = Image.open(gif)
n = frame_count(im)
if n == 0:
    sys.exit("FAIL: no decodable frame")
im.seek(n - 1)
im.load()
rows = [frame_row(im, r) for r in range(8)]

# CASE-SENSITIVE on purpose -- the whole claim is about case. The `opened`
# check keeps its case too: the file's own mixed case proves the decode
# distinguishes cases at all.
opened = any("Hello from FAT32 on X816!" in r for r in rows)
correct = rows[2] == "abABabab"


def dump():
    print("    final screen:")
    for i, r in enumerate(rows):
        if r:
            print("      %2d: %s" % (i, r))


if neg:
    if not opened:
        print("FAIL (negative control): the file never opened, so this run "
              "says nothing about Caps Lock")
        dump()
        sys.exit(1)
    if correct:
        print("FAIL (negative control): the caps pattern appeared with Caps "
              "Lock never pressed -- the check is not measuring the toggle")
        dump()
        sys.exit(1)
    print("PASS (negative control): without Caps Lock the case pattern "
          "differs, so the positive check is about the toggle")
    sys.exit(0)

if not opened:
    print("FAIL: the editor never opened /hello.txt")
    dump()
    sys.exit(1)
if not correct:
    print("FAIL: caps toggle/invert/release pattern wrong -- row 2 is %r, "
          "expected 'abABabab' (ab, caps ab->AB, caps+shift AB->ab, off ab)"
          % rows[2])
    dump()
    sys.exit(1)

print("PASS: Caps Lock engages, upper-cases letters, inverts Shift, and "
      "releases")
dump()
PY
