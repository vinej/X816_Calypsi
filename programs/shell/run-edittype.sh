#!/usr/bin/env bash
# Resident editor minimal typing smoke test.
set -eu

cd "$(dirname "$0")"
bash build.sh >/dev/null

. ../../runtime/calypsi.sh

OUT_GIF=${OUT_GIF:-$PWD/out-edittype-smoke.gif}
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
    -Emu "$EMU_W" -Boot "$BOOT_W" -Kernel "$KERNEL_W" -Gif "$GIF_W" \
    -Keys 'edittp SHELL.TX' -Seconds 20 >/dev/null

"$PYTHON" - "$OUT_GIF" "$FONT_W" <<'PY'
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
    return [frame_row(im, r) for r in range(60)]

im = Image.open(gif)
n = 0
seen_typed = False
last_rows = []
while n < 500:
    try:
        im.seek(n); im.load()
        rows = [frame_row(im, r) for r in (0, 1, 2, 3, 57, 58, 59)]
        body = "\n".join(rows).upper()
        seen_typed = seen_typed or ("ABC" in body or "A?C" in body)
        last_rows = rows
        n += 1
    except (EOFError, OSError, IndexError):
        break
if n == 0:
    sys.exit("FAIL: no decodable frame")

im.seek(n - 1)
im.load()
rows = frame_rows(im)
body = "\n".join(rows).upper()

if not seen_typed:
    sys.exit("FAIL: typed text did not render in resident editor")
if "SAVE BEFORE EXIT" not in body and ">" not in body:
    sys.exit("FAIL: neither exit prompt nor shell prompt was visible")
if ">" not in body:
    sys.exit("FAIL: prompt did not return after typed editor exit")
if not re.search(r"EDITA.G OK", body):
    sys.exit("FAIL: console filename was not copied by the resident editor")

print("PASS: resident editor typed text, accepted a console filename, and restored the shell")
for r in rows:
    if r:
        print("   ", r)
PY
