#!/usr/bin/env bash
# Resident editor allocator smoke test.
set -eu

cd "$(dirname "$0")"
bash build.sh >/dev/null

. ../../runtime/calypsi.sh

OUT_GIF=${OUT_GIF:-$PWD/out-editmem-smoke.gif}
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
    -Keys "editmem" >/dev/null

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
    return [frame_row(im, r) for r in range(60)]

im = Image.open(gif)
n = 0
while True:
    try:
        im.seek(n); im.load()
        n += 1
    except (EOFError, OSError, IndexError):
        break
if n == 0:
    sys.exit("FAIL: no decodable frame")

im.seek(n - 1)
im.load()
rows = frame_rows(im)
body = "\n".join(rows).upper()

if "FAIL" in body:
    sys.exit("FAIL: resident editor allocator smoke reported failure")
if "OK" not in body:
    sys.exit("FAIL: resident editor allocator smoke verdict not visible")
if ">" not in body:
    sys.exit("FAIL: prompt did not return after allocator smoke")

print("PASS: resident editor allocator smoke passed")
for r in rows:
    if r:
        print("   ", r)
PY
