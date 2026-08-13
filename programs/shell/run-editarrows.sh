#!/usr/bin/env bash
# CURSOR MOVEMENT: the arrow keys actually move the cursor.
#
#   ./run-editarrows.sh              build and run
#   ./run-editarrows.sh --negative   type the same text WITHOUT the arrows,
#                                    and require both checks to fail
#
# THIS TEST COULD NOT BE WRITTEN UNTIL NOW. -autokeys had no way to send a key
# with no character, and the scripts said so: "an ARROW KEY CANNOT BE SENT.
# Cursor movement ... is not testable end-to-end". So the entire special-key
# path -- arrows, Home, End, PgUp/PgDn, Insert, F1-F8 -- shipped unexercised,
# and the bug it hid could only be found on real hardware. src/keyboard.c now
# takes \l \r \u \d \h \E \P \N and pushes IBM key POSITIONS into the SMC FIFO,
# the same numbers rtl/smc_x16.sv produces, so this drives the real path.
#
# WHAT IT CAUGHT: x816_kernal_getin's `special:` ladder assembled as 8-BIT
# immediates while the machine ran 16-bit, because ca65 tracks the accumulator
# width TEXTUALLY and the character path just above it ends in `.a8`. So
# `and #$00ff` came out `29 FF` instead of `29 FF 00`, the CPU ate the next
# opcode as the immediate's high byte, and every comparison after it decoded
# from the wrong offset. Characters and Enter never reach that label, which is
# exactly why they worked and nothing else did.
#
# THE ASSERTION IS THE INSERT POINT, not "the cursor indicator changed". A
# cursor that moved the wrong distance, or not at all, puts the typed text
# somewhere else.
#
# TWO RUNS, one axis each -- interleaving them makes the final screen depend
# on every step at once, and a failure then names no axis.
#
# HORIZONTAL: four rights from the start of the loaded line put the insert
# after "Hell", so typing "qq" there gives "Hellqq" and the harness's trailing
# Enter pushes "o from..." to the next line.
#
# VERTICAL: type three fresh lines above the file text, then two ups and a
# down must land at the END of "bb" -- cmd_go_up/down preserve the column, and
# from after "cc" (col 3) the ends of these two-char lines are all col 3. The
# "vv" typed there gives "bbvv", and the trailing Enter leaves an empty line
# under it. This proves the whole mem_crs_move_to_line_start / step_left walk.
#
# Requires Pillow.
set -eu

cd "$(dirname "$0")"
bash build.sh >/dev/null

. ../../runtime/calypsi.sh

NEG=0
if [ "${1:-}" = "--negative" ]; then
    NEG=1
    echo "negative control: typing the same text with NO arrows, so the"
    echo "inserts land where the cursor already was and both checks must fail"
fi

if [ "$NEG" = "1" ]; then
    KEYS_H='edit /hello.txt\nqq'
    KEYS_V='edit /hello.txt\naa\nbb\nccvv'
else
    KEYS_H='edit /hello.txt\n\r\r\r\rqq'
    KEYS_V='edit /hello.txt\naa\nbb\ncc\u\u\dvv'
fi

GIF_H=${GIF_H:-$PWD/out-editarrows-h.gif}
GIF_V=${GIF_V:-$PWD/out-editarrows-v.gif}
CARD_H=$PWD/out-editarrows-h-card.img
CARD_V=$PWD/out-editarrows-v-card.img
rm -f "$GIF_H" "$GIF_V" "$CARD_H" "$CARD_V"
trap 'rm -f "$CARD_H" "$CARD_V"' EXIT

PYTHON=${PYTHON:-python}
if ! command -v "$PYTHON" >/dev/null 2>&1; then
    PYTHON=/c/Users/jyv/AppData/Local/Programs/Python/Python312/python.exe
fi
POWERSHELL=${POWERSHELL:-powershell.exe}

capture() { # $1 = keys, $2 = gif, $3 = card
    cp "$CORE/boot/fat32.img" "$3"
    "$POWERSHELL" -NoProfile -ExecutionPolicy Bypass -File "$PWD/run-edit-capture.ps1" \
        -Emu "$(cygpath -m "$EMU/build/x16emu.exe")" \
        -Boot "$(cygpath -m "$CORE/boot/boot.rom")" \
        -Kernel "$(cygpath -m "$PWD/kernel.bin")" \
        -Gif "$(cygpath -m "$2")" -Sdcard "$(cygpath -m "$3")" \
        -Keys "$1" -Seconds 30 >/dev/null
}

capture "$KEYS_H" "$GIF_H" "$CARD_H"
capture "$KEYS_V" "$GIF_V" "$CARD_V"

check() { # $1 = axis (h|v), $2 = gif
    "$PYTHON" - "$2" "$(cygpath -m "$RT/font_cp437.s")" "$NEG" "$1" <<'PY'
import os, re, sys, io
from collections import Counter
from PIL import Image, ImageFile
ImageFile.LOAD_TRUNCATED_IMAGES = True

gif, fontinc, neg, axis = sys.argv[1], sys.argv[2], sys.argv[3] == "1", sys.argv[4]
if not os.path.exists(gif):
    sys.exit("FAIL(%s): emulator did not create GIF" % axis)

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
    sys.exit("FAIL(%s): no decodable frame" % axis)
im.seek(n - 1)
im.load()
rows = [frame_row(im, r) for r in range(8)]
up = [r.upper() for r in rows]

# The file has to be there at all, or "the text is not where the arrows put
# it" is true of a machine that never opened it.
opened = any("FAT32 ON X816!" in r for r in up)

if axis == "h":
    moved = up[2] == "HELLQQ" and up[3].startswith("O FROM FAT32 ON X816!")
    what = "four right arrows put the insert after 'Hell'"
    where = "row 2 %r, expected 'Hellqq' with 'o from FAT32 on X816!' below" % rows[2]
else:
    moved = (up[2] == "AA" and up[3] == "BBVV"
             and up[5].startswith("CCHELLO FROM FAT32 ON X816!"))
    what = "up-up-down landed at the end of 'bb'"
    where = ("rows 2..5 %r, expected 'aa'/'bbvv'/''/'ccHello from...'"
             % (rows[2:6],))


def dump():
    print("    final screen:")
    for i, r in enumerate(rows):
        if r:
            print("      %2d: %s" % (i, r))


if neg:
    if not opened:
        print("FAIL(%s) (negative control): the file never opened, so this "
              "run says nothing about where the insert landed" % axis)
        dump()
        sys.exit(1)
    if moved:
        print("FAIL(%s) (negative control): the text landed as if the arrows "
              "had been typed -- the check is not measuring movement" % axis)
        dump()
        sys.exit(1)
    print("PASS(%s) (negative control): with no arrows the insert lands "
          "where the cursor already was" % axis)
    sys.exit(0)

if not opened:
    print("FAIL(%s): the editor never opened /hello.txt" % axis)
    dump()
    sys.exit(1)
if not moved:
    print("FAIL(%s): %s -- %s" % (axis, what, where))
    dump()
    sys.exit(1)

print("PASS(%s): %s" % (axis, what))
dump()
PY
}

RC=0
check h "$GIF_H" || RC=1
check v "$GIF_V" || RC=1
exit $RC
