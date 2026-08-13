#!/usr/bin/env bash
# BACKSPACE AND DELETE: the two destructive keys actually delete.
#
#   ./run-editbackspace.sh              build and run
#   ./run-editbackspace.sh --negative   same typing WITHOUT the key under
#                                       test, and require both checks to fail
#
# WHAT THIS PINS. The console's characters are ASCII and the editor's key
# tables are the X16 Kernal's PETSCII-shaped values. Tab, Enter and ESC are
# the same number in both, which HID the two keys that are not: ASCII
# Backspace ($08) missed the editor's default_keyval table and fell through to
# default_insert -- a control byte the CP437 font draws blank, so Backspace
# looked like it typed a space -- and Delete ($7F) would have inserted a glyph
# the same way. x816_kernal_getin now translates $08 -> KEYVAL_BACKSPACE and
# $7F -> KEYVAL_DELETE at the one place every editor input path shares.
#
# -autokeys sends both as IBM key POSITIONS (\b = 15, \D = 76), the numbers
# rtl/smc_x16.sv produces, so this drives the real path.
#
# THE ASSERTION IS THE BUFFER CONTENT, per axis:
#   BACKSPACE: four rights then \b deletes the second 'l' of "Hello", so "qq"
#              typed there gives "Helqq" with "o from..." pushed below by the
#              harness's trailing Enter.
#   DELETE:    four rights then \D deletes the 'o' RIGHT of the cursor, so
#              "qq" gives "Hellqq" with " from..." below -- note the leading
#              space where "o" was, which an insert-not-delete cannot produce.
#
# Requires Pillow.
set -eu

cd "$(dirname "$0")"
bash build.sh >/dev/null

. ../../runtime/calypsi.sh

NEG=0
if [ "${1:-}" = "--negative" ]; then
    NEG=1
    echo "negative control: typing the same text WITHOUT backspace/delete, so"
    echo "nothing is removed and both checks must fail"
fi

if [ "$NEG" = "1" ]; then
    KEYS_B='edit /hello.txt\n\r\r\r\rqq'
    KEYS_D='edit /hello.txt\n\r\r\r\rqq'
else
    KEYS_B='edit /hello.txt\n\r\r\r\r\bqq'
    KEYS_D='edit /hello.txt\n\r\r\r\r\Dqq'
fi

GIF_B=${GIF_B:-$PWD/out-editbackspace-b.gif}
GIF_D=${GIF_D:-$PWD/out-editbackspace-d.gif}
CARD_B=$PWD/out-editbackspace-b-card.img
CARD_D=$PWD/out-editbackspace-d-card.img
rm -f "$GIF_B" "$GIF_D" "$CARD_B" "$CARD_D"
trap 'rm -f "$CARD_B" "$CARD_D"' EXIT

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

capture "$KEYS_B" "$GIF_B" "$CARD_B"
capture "$KEYS_D" "$GIF_D" "$CARD_D"

check() { # $1 = axis (b|d), $2 = gif
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

# The file has to be there at all, or "the char was not deleted" is true of a
# machine that never opened it.
opened = any("FAT32 ON X816!" in r for r in up)

if axis == "b":
    deleted = up[2] == "HELQQ" and up[3].startswith("O FROM FAT32 ON X816!")
    what = "backspace deleted the char LEFT of the cursor"
    where = ("rows 2..3 %r, expected 'Helqq' / 'o from FAT32 on X816!'"
             % (rows[2:4],))
else:
    deleted = up[2] == "HELLQQ" and up[3] == " FROM FAT32 ON X816!"
    what = "delete removed the char RIGHT of the cursor"
    where = ("rows 2..3 %r, expected 'Hellqq' / ' from FAT32 on X816!' "
             "(leading space where the 'o' was)" % (rows[2:4],))


def dump():
    print("    final screen:")
    for i, r in enumerate(rows):
        if r:
            print("      %2d: %s" % (i, r))


if neg:
    if not opened:
        print("FAIL(%s) (negative control): the file never opened, so this "
              "run says nothing about deletion" % axis)
        dump()
        sys.exit(1)
    if deleted:
        print("FAIL(%s) (negative control): the text reads as if the key had "
              "been pressed -- the check is not measuring deletion" % axis)
        dump()
        sys.exit(1)
    print("PASS(%s) (negative control): with the key not pressed, nothing "
          "was deleted" % axis)
    sys.exit(0)

if not opened:
    print("FAIL(%s): the editor never opened /hello.txt" % axis)
    dump()
    sys.exit(1)
if not deleted:
    print("FAIL(%s): %s -- %s" % (axis, what, where))
    dump()
    sys.exit(1)

print("PASS(%s): %s" % (axis, what))
dump()
PY
}

RC=0
check b "$GIF_B" || RC=1
check d "$GIF_D" || RC=1
exit $RC
