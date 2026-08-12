#!/usr/bin/env bash
# Resident editor smoke test.
#
# This enters the resident editor through the real entry point, exits through a
# smoke-only X816 key shim, and proves the shell console is restored.
#
# Requires Pillow:  pip install pillow
set -eu

cd "$(dirname "$0")"
bash build.sh >/dev/null

. ../../runtime/calypsi.sh

OUT_GIF=${OUT_GIF:-$PWD/out-edit-smoke.gif}
rm -f "$OUT_GIF"
trap 'rm -f "$OUT_GIF"' EXIT

PYTHON=${PYTHON:-python}
if ! command -v "$PYTHON" >/dev/null 2>&1; then
    PYTHON=/c/Users/jyv/AppData/Local/Programs/Python/Python312/python.exe
fi
POWERSHELL=${POWERSHELL:-powershell.exe}
EMU_W=$(cygpath -m "$EMU/build/x16emu.exe")
BOOT_W=$(cygpath -m "$CORE/boot/boot.rom")
KERNEL_W=$(cygpath -m "$PWD/kernel.bin")
GIF_W=$(cygpath -m "$OUT_GIF")
FONT_W=$(cygpath -m "$RT/font_cp437.s")
CAPTURE_W=$(cygpath -m "$PWD/run-edit-capture.ps1")

"$POWERSHELL" -NoProfile -ExecutionPolicy Bypass -File "$CAPTURE_W" \
    -Emu "$EMU_W" -Boot "$BOOT_W" -Kernel "$KERNEL_W" -Gif "$GIF_W" >/dev/null

"$PYTHON" - "$GIF_W" "$FONT_W" <<'PY'
import os, re, sys, io
from collections import Counter
from PIL import Image, ImageFile
ImageFile.LOAD_TRUNCATED_IMAGES = True

gif, fontinc = sys.argv[1:]
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
    for col in range(60):
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
        out += glyph.get(tuple(bits), "?")
    return out.rstrip()

def frame_rows(im):
    rows = []
    for r in range(60):
        rows.append(frame_row(im, r))
    return rows

def frame_is_editor_clear(im):
    rgb = im.convert("RGB")
    w, h = rgb.size
    px = rgb.load()
    blue = 0
    total = 0
    for y in range(0, h, 8):
        for x in range(0, w, 8):
            r, g, b = px[x, y]
            if b > 120 and r < 80 and g < 80:
                blue += 1
            total += 1
    return blue * 2 > total

im = Image.open(gif)
n = 0
seen_editor = False
seen_footer2 = False
while True:
    try:
        im.seek(n); im.load()
        header = frame_row(im, 0).upper()
        footer = frame_row(im, 58).upper()
        footer2 = frame_row(im, 59).upper()
        seen_editor = seen_editor or (("X16EDIT" in header and "NEW BUFFER" in header) \
                                      or ("GET HELP" in footer and "WRITE OUT" in footer))
        seen_footer2 = seen_footer2 or ("EXIT" in footer2 and "OPEN FILE" in footer2)
        n += 1
    except (EOFError, OSError, IndexError):
        break
if n == 0:
    sys.exit("FAIL: no decodable frame")

im.seek(n - 1)
im.load()
last_rows = frame_rows(im)

rows = last_rows
body = "\n".join(rows).upper()
if not seen_editor:
    sys.exit("FAIL: readable resident editor screen did not appear")
if not seen_footer2:
    sys.exit("FAIL: resident editor second footer row did not appear")
if "EDITOR RESIDENT; PORT IN PROGRESS" in body:
    sys.exit("FAIL: shell still called the old resident probe")
if ">" not in body:
    sys.exit("FAIL: prompt did not return after editor exit")
if "> EDIT" in body:
    sys.exit("FAIL: edit command did not echo")

print("PASS: resident editor rendered, exited, and restored the shell")
for r in rows:
    if r:
        print("   ", r)
PY
