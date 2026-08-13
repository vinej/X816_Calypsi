#!/usr/bin/env bash
# Resident editor REAL KEYBOARD PATH: open a file, then type into it with the
# emulated SMC keyboard and require both an ordinary character and a KEY THAT
# HAS TO BE DISPATCHED THROUGH A TABLE to do the right thing.
#
#   ./run-editkeys.sh              build and run
#   ./run-editkeys.sh --negative   type nothing after `edit`, and require the
#                                  typed-text checks to FAIL
#
# WHY THIS EXISTS, given run-edittype.sh already says "typed text": that test
# calls keyboard_mode_default directly with a character in A. It proves the
# insert and it proves NOTHING about how a character gets there. Everything
# between the SMC and that call -- x816_kernal_getin, the K_CON_GETKEY thunk,
# KENTER/KLEAVE, con_getkey -- was untested, and that is exactly where the
# editor was dead on hardware: the screen drew, the file loaded, and not one
# keystroke ever reached a handler.
#
# TWO KINDS OF KEY, because they fail for different reasons:
#
#   * 'z' and 'q' are ordinary characters. They only need the key to arrive.
#   * ENTER has to be found in default_keyval and dispatched through
#     default_jmptbl. That table is a constant inside the editor blob, which
#     lives in the firmware bank while the main loop runs with DBR=$00 -- so it
#     was being compared against bank $00 instead. Enter missed the table and
#     was inserted as the character $0D, which the CP437 font draws BLANK. On
#     screen that is indistinguishable from a swallowed keystroke, which is why
#     the check below is on the LINE STRUCTURE and not on "some text appeared".
#
# The sequence typed into the editor is  z z q q z z  ENTER  y y  ENTER, the
# last one being the newline run-edit-capture.ps1 appends to every -autokeys
# string. The editor opens with the cursor at the start of the buffer, so a
# correct machine ends with three lines:
#
#     zzqqzz
#     yy
#     Hello from FAT32 on X816!
#
# Every other outcome is a different bug: one line means Enter was swallowed,
# "zzqqzz yy" on one line means it was inserted as a character, and text
# anywhere but in front of the file's line means the insert point was wrong.
#
# Lower case on purpose: -autokeys types an upper-case letter with Shift held,
# so lower case keeps the failure surface to the path under test.
#
# Requires Pillow:  pip install pillow
set -eu

cd "$(dirname "$0")"
bash build.sh >/dev/null

. ../../runtime/calypsi.sh

NEG=0
if [ "${1:-}" = "--negative" ]; then
    NEG=1
    echo "negative control: opening the file but typing nothing,"
    echo "expecting the typed-text checks to fail"
fi

OUT_GIF=${OUT_GIF:-$PWD/out-editkeys.gif}
SCRATCH=${SCRATCH:-$PWD/out-editkeys-card.img}
rm -f "$OUT_GIF" "$SCRATCH"
if [ "${KEEP_GIF:-0}" = "1" ]; then
    trap 'rm -f "$SCRATCH"' EXIT
else
    trap 'rm -f "$OUT_GIF" "$SCRATCH"' EXIT
fi

# A scratch copy: this test does not save, but the editor is one Ctrl+S away
# from writing and the card must never be the checked-in image.
cp "$CORE/boot/fat32.img" "$SCRATCH"

PYTHON=${PYTHON:-python}
if ! command -v "$PYTHON" >/dev/null 2>&1; then
    PYTHON=/c/Users/jyv/AppData/Local/Programs/Python/Python312/python.exe
fi
POWERSHELL=${POWERSHELL:-powershell.exe}
EMU_W=$(cygpath -m "$EMU/build/x16emu.exe")
BOOT_W=$(cygpath -m "$CORE/boot/boot.rom")
KERNEL_W=$(cygpath -m "$PWD/kernel.bin")
CARD_W=$(cygpath -m "$SCRATCH")
GIF_W=$(cygpath -m "$OUT_GIF")
FONT_W=$(cygpath -m "$RT/font_cp437.s")
CAPTURE_W=$(cygpath -m "$PWD/run-edit-capture.ps1")

KEYS='edit /hello.txt\nzzqqzz\nyy'
[ "$NEG" = "1" ] && KEYS='edit /hello.txt'

"$POWERSHELL" -NoProfile -ExecutionPolicy Bypass -File "$CAPTURE_W" \
    -Emu "$EMU_W" -Boot "$BOOT_W" -Kernel "$KERNEL_W" -Gif "$GIF_W" \
    -Sdcard "$CARD_W" -Keys "$KEYS" -Seconds 30 >/dev/null

"$PYTHON" - "$OUT_GIF" "$FONT_W" "$NEG" <<'PY'
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
# The prompt is CP437 $AF, the chevron from the boot mark, NOT '>'. The
# table above stops at $7E, so decode $AF as '>' and every "the prompt is
# back" assertion below keeps reading as what it means.
glyph[tuple(vals[0xAF * 8:0xB0 * 8])] = ">"


def frame_row(im, r):
    px = im.convert("RGB").load()
    out = ""
    for col in range(80):
        colors = []
        for y in range(8):
            for x in range(8):
                colors.append(px[col * 8 + x, r * 8 + y])
        # Most common colour is the background -- except for glyphs that light
        # more than half their cell ('B', 'R', 'N'), where it is the ink and the
        # pattern comes out inverted. Try the complement before giving up. The
        # editor's cursor is a colour swap on one cell, so that same complement
        # is what keeps the character under it readable.
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
    # Not capped. A warp run records thousands of frames and a scan that stops
    # at a round number then reads "the last frame" reads the middle instead.
    n = 0
    while True:
        try:
            im.seek(n)
            im.load()
            n += 1
        except (EOFError, OSError, IndexError):
            break
    return n


FILETEXT = "HELLO FROM FAT32 ON X816!"

im = Image.open(gif)
n = frame_count(im)
if n == 0:
    sys.exit("FAIL: no decodable frame")

im.seek(n - 1)
im.load()
rows = [frame_row(im, r) for r in range(8)]
up = [r.upper() for r in rows]


def dump():
    print("    final screen:")
    for i, r in enumerate(rows):
        if r:
            print("      %2d: %s" % (i, r))


# The cursor sits on the file line's first character in the finished state, and
# a colour-swapped cell decodes as '?' whenever the swap is not a clean
# inversion -- so the file line is matched from its second character on.
file_ok = FILETEXT[1:] in up[4]
line1_ok = up[2] == "ZZQQZZ"
line2_ok = up[3] == "YY"

if neg:
    if line1_ok or line2_ok:
        print("FAIL (negative control): a typed-text check passed with nothing "
              "typed -- it is not testing what it claims")
        dump()
        sys.exit(1)
    if FILETEXT[1:] not in up[2]:
        print("FAIL (negative control): the file never rendered, so this run "
              "proves nothing about the typing checks either")
        dump()
        sys.exit(1)
    print("PASS (negative control): the file opened and the typed-text checks "
          "correctly did not pass")
    sys.exit(0)

problems = []
if not line1_ok:
    problems.append("row 2 is %r, expected 'zzqqzz': either the keystrokes "
                    "never reached the buffer, or Enter was inserted as a "
                    "character instead of splitting the line" % rows[2])
if not line2_ok:
    problems.append("row 3 is %r, expected 'yy': Enter did not start a new "
                    "line at the insert point" % rows[3])
if not file_ok:
    problems.append("row 4 is %r, expected the loaded text pushed down by two "
                    "new lines" % rows[4])

if problems:
    print("FAIL:")
    for p in problems:
        print("   -", p)
    dump()
    sys.exit(1)

print("PASS: the resident editor read real keystrokes off the SMC, inserted "
      "the characters, and dispatched Enter through its key table")
dump()
PY
