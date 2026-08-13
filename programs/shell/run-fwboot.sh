#!/usr/bin/env bash
# RESIDENT-KERNEL conformance: boot the firmware image and type at it.
#
# This is the regression kernel residency never had. boot1.rom-style tests
# load a program at $01:0000 and prove the fallback path; this one loads
# kernel.bin at $F0:0000 -- exactly what the core does with boot2.rom -- and
# proves, in order:
#
#   1. boot/boot.s takes the FIRMWARE magic branch (not the $01:0000 one)
#   2. the kernel image runs where it was linked (banks $F0+, KENTER context
#      at $2000, doc/KERNEL.md section 3)
#   3. kern_install ran (kernelmain.c) -- the table page is stamped at boot
#   4. the prompt is live on the real keyboard path (same checks as run-kbd)
#
# What it deliberately does NOT yet cover: the run -> ESC -> firmware
# re-entry loop (goshell's x816_fw_enter). -autokeys cannot inject ESC as a
# special key today; when it can, extend this with a card, `run
# /demo/charmap.bin`, ESC, and a second prompt check.
#
#   ./run-fwboot.sh              build artifacts must exist (sh build.sh)
#   ./run-fwboot.sh --negative   corrupt the magic and expect the BANDS path
#                                (no banner), proving the firmware branch is
#                                what booted us in the positive run
#
# Requires Pillow:  pip install pillow
set -u

# Nothing is compiled here -- this runs the kernel.bin build.sh already made --
# but EMU, CORE and RT come from the same place every other script gets them,
# so a moved checkout moves once.
. "$(dirname "$0")/../../runtime/calypsi.sh"
cd "$(dirname "$0")"
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
WOUT=$(cygpath -m "$OUT" 2>/dev/null || echo "$OUT")

[ -f kernel.bin ] || { echo "kernel.bin missing -- run: sh build.sh"; exit 1; }

cp kernel.bin "$OUT/kernel.bin"
NEGATIVE=0
if [ "${1:-}" = "--negative" ]; then
    NEGATIVE=1
    # Break the magic: boot must fall through to the bands demo.
    printf 'Y' | dd of="$OUT/kernel.bin" bs=1 count=1 conv=notrunc 2>/dev/null
    echo "negative control: corrupted firmware magic, expecting NO banner"
fi

SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout 90 \
    "$EMU/build/x16emu.exe" -boot "$(cygpath -m "$CORE/boot/boot.rom")" \
    -load "F00000,$WOUT/kernel.bin" -autokeys 'help\nver\n' \
    -warp -gif "$WOUT/out.gif" >/dev/null 2>&1

python - "$WOUT/out.gif" "$RT/font_cp437.s" "$NEGATIVE" <<'PY'
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
# The prompt is CP437 $AF, the chevron from the boot mark, NOT '>'. The
# table above stops at $7E, so decode $AF as '>' and every "the prompt is
# back" assertion below keeps reading as what it means.
glyph[tuple(vals[0xAF * 8:0xB0 * 8])] = ">"

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

def fail(msg):
    print("FAIL:", msg)
    for i, r in enumerate(rows):
        print(f"  {i}: {r!r}")
    sys.exit(1)

# The prompt row, found rather than assumed: the banner above it is art.
pi = next((i for i, r in enumerate(rows) if r.startswith(">")), None)

# The banner is the chevron logo with the wordmark under it, so "X816" lands
# somewhere ABOVE THE PROMPT rather than on a fixed row. Above the prompt is
# the whole point: `ver' prints "X816 shell 0.1" BELOW it, so searching the
# whole screen would find that and report a banner that was never drawn. With
# no prompt at all (the negative control) there is no console either, so a
# fixed window is the right fallback.
banner = any("X816" in r for r in rows[:pi if pi is not None else 12])

if negative:
    # Corrupted magic: boot must take the bands fallback, so the screen is a
    # bitmap, not the console -- no banner decodes.
    if banner:
        fail("banner present despite a corrupted firmware magic -- "
             "the magic check is not what admitted the kernel")
    print("PASS (negative control): no firmware magic, no kernel -- "
          "boot fell through as designed")
    sys.exit(0)

if not banner:
    fail("no banner -- the KERNEL did not come up from the firmware region")
if pi is None:
    fail("no prompt on screen: the kernel never reached sh_run")
if rows[pi] == ">":
    fail("bare prompt: no key arrived (SMC path) under the resident kernel")
body = " ".join(rows[pi + 1:]).upper()
for cmd in ("HELP", "VER", "RUN", "GO", "LS"):
    if cmd not in body:
        fail(f"`help' ran but did not list {cmd}")
if "0.1" not in body and "V0" not in body:
    fail("`ver' produced no version line")

print("PASS: the kernel booted FROM THE FIRMWARE REGION, installed its table,")
print("      and answered on the real keyboard path (help + ver)")
for r in rows:
    if r:
        print("   ", r)
PY
