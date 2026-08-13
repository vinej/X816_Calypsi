#!/usr/bin/env bash
# An ordinary `edit` must IGNORE whatever is in the smoke hand-off bytes.
#
#   ./run-edithandoff.sh              build and run
#   ./run-edithandoff.sh --negative   ask for a smoke run on purpose, and
#                                     require it to still happen
#
# WHY THIS EXISTS. $0007FE selects a smoke path inside the editor -- 2, 3 and
# 4 each render something and RETURN instead of running -- and $0007FF makes a
# typed 'x' mean Ctrl+X. Both are fixed addresses in BANK $00, which belongs to
# the CALLER, and nothing initialised them: k_edit wrote the filename pointer
# beside them and left these two alone.
#
# A caller really does use that memory. durexForth's text input buffer is
# TIB = $0600, 512 bytes -- $0600 to $07FF -- so the hand-off block is the last
# eight bytes of it, and whatever was last typed at the Forth prompt chose the
# editor's behaviour. Reported from hardware as `s" name" edit` opening the
# file and returning to the console at once.
#
# So this pokes the worst case in deliberately, with the shell's own `poke`,
# and requires `edit` to open anyway. k_edit clears both bytes now, which is
# why it is k_edit and not cmd_edit: durexForth and SuperBasic go through the
# slot, not through the shell.
#
# Requires Pillow.
set -eu

cd "$(dirname "$0")"
bash build.sh >/dev/null

. ../../runtime/calypsi.sh

NEG=0
if [ "${1:-}" = "--negative" ]; then
    NEG=1
    echo "negative control: asking for a smoke run through editsmk, which is"
    echo "entitled to one -- it must still return to the prompt"
fi

OUT_GIF=${OUT_GIF:-$PWD/out-edithandoff.gif}
SCRATCH=${SCRATCH:-$PWD/out-edithandoff-card.img}
rm -f "$OUT_GIF" "$SCRATCH"
trap 'rm -f "$OUT_GIF" "$SCRATCH"' EXIT
cp "$CORE/boot/fat32.img" "$SCRATCH"

PYTHON=${PYTHON:-python}
if ! command -v "$PYTHON" >/dev/null 2>&1; then
    PYTHON=/c/Users/jyv/AppData/Local/Programs/Python/Python312/python.exe
fi
POWERSHELL=${POWERSHELL:-powershell.exe}
CAPTURE_W=$(cygpath -m "$PWD/run-edit-capture.ps1")

# Poke the hand-off bytes to the values that used to divert an ordinary edit,
# then edit a real file. 2 is "render and return"; 1 in $07FF turns a typed
# 'x' into Ctrl+X.
if [ "$NEG" = "1" ]; then
    KEYS='poke 7fe 2\npoke 7ff 1\neditsmk'
else
    KEYS='poke 7fe 2\npoke 7ff 1\nedit /hello.txt'
fi

"$POWERSHELL" -NoProfile -ExecutionPolicy Bypass -File "$CAPTURE_W" \
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
# The prompt is CP437 $AF, the chevron from the boot mark, NOT '>'.
glyph[tuple(vals[0xAF * 8:0xB0 * 8])] = ">"


def frame_row(im, r):
    px = im.convert("RGB").load()
    out = ""
    for col in range(80):
        colors = []
        for y in range(8):
            for x in range(8):
                colors.append(px[col * 8 + x, r * 8 + y])
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
rows = [frame_row(im, r) for r in range(12)]
up = "\n".join(rows).upper()

# In the editor: the header names the file and the text is on screen. Back at
# the shell: a prompt. Both are asserted, because "no prompt" alone would also
# be true of a machine that had crashed.
in_editor = "/HELLO.TXT" in up and "ELLO FROM FAT32 ON X816!" in up
at_prompt = any(r.strip().startswith(">") for r in rows)


def dump():
    print("    final screen:")
    for i, r in enumerate(rows):
        if r:
            print("      %2d: %s" % (i, r))


if neg:
    if in_editor and not at_prompt:
        print("FAIL (negative control): editsmk did NOT take the smoke path -- "
              "k_edit_raw is clearing the request it is supposed to honour, so "
              "the positive check below proves nothing")
        dump()
        sys.exit(1)
    print("PASS (negative control): a caller entitled to a smoke run still "
          "gets one, so the positive case is about the CLEAR and not about "
          "the mechanism being dead")
    sys.exit(0)

if not in_editor:
    print("FAIL: `edit` did not open the file with $07FE poked to 2 -- the "
          "hand-off byte still diverts an ordinary edit into a smoke path, "
          "which is what durexForth's TIB was doing by accident")
    dump()
    sys.exit(1)

print("PASS: an ordinary `edit` opened the file and stayed in the editor with "
      "the smoke hand-off bytes poked to 2 and 1")
dump()
PY
