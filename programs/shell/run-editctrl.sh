#!/usr/bin/env bash
# The editor's Ctrl SHORTCUT TABLE really is the default shortcut table.
#
#   ./run-editctrl.sh              build and run
#   ./run-editctrl.sh --negative   never enter the editor, and require the
#                                  check to fail
#
# keyboard_init copies the `keyval` constant into keyboard_ctrl_keyval, and
# every Ctrl command -- Ctrl+O to save, Ctrl+X to quit -- is dispatched by
# comparing the key against that copy. The two ends live in DIFFERENT BANKS on
# X816: `keyval` is a constant inside the editor blob, which is linked into the
# firmware bank, while keyboard_ctrl_keyval is VARS, i.e. bank $00. Absolute
# addressing follows DBR and the editor runs with DBR=$00, so without a
# code-bank bracket on the read the copy moved 28 bytes of bank $00 into the
# table and the editor came up with no working commands at all -- silently, and
# with a chance of a stray byte matching and running the WRONG command.
#
# WHY A MEMORY DUMP AND NOT A KEYPRESS: -autokeys has no Ctrl, so the shortcuts
# cannot be typed. The table itself is the mechanism, so the table is what this
# reads -- through the shell's `dump`, after the editor has run its setup and
# returned. Both ends of the check are DERIVED, not transcribed: the address
# comes from build/x816-edit.sym and the expected bytes from the `keyval` table
# in keyboard.inc, so a shortcut added upstream cannot leave this asserting on
# a stale list.
#
# THE PADDING IS LOAD-BEARING. editsmk holds the screen for a deliberate delay
# so a frame can be captured, and -autokeys keeps typing through it on a 25 ms
# emulated clock into a 16-entry SMC FIFO -- so anything queued during that
# window is dropped. The run therefore types a screenful of spaces to be eaten
# and only then the command whose output matters.
#
# Requires Pillow:  pip install pillow
set -eu

cd "$(dirname "$0")"
bash build.sh >/dev/null

. ../../runtime/calypsi.sh

NEG=0
if [ "${1:-}" = "--negative" ]; then
    NEG=1
    echo "negative control: dumping the table without ever entering the editor,"
    echo "so keyboard_init never runs and the check must fail"
fi

EDIT_REPO=${EDIT_REPO:-$PWD/../../../X816_Edit}

# The address of the copy, from the map the build just produced.
ADDR=$(sed -n 's/^al \([0-9A-Fa-f]*\) \.keyboard_ctrl_keyval$/\1/p' \
       "$EDIT_REPO/build/x816-edit.sym" | head -1)
if [ -z "$ADDR" ]; then
    echo "FAIL: keyboard_ctrl_keyval not found in x816-edit.sym" >&2
    exit 1
fi
ADDR=$(printf '%x' "0x$ADDR")

OUT_GIF=${OUT_GIF:-$PWD/out-editctrl.gif}
rm -f "$OUT_GIF"
if [ "${KEEP_GIF:-0}" = "1" ]; then
    trap - EXIT
else
    trap 'rm -f "$OUT_GIF"' EXIT
fi

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

PAD=$(printf '%.0s ' $(seq 1 70))
if [ "$NEG" = "1" ]; then
    KEYS="dump $ADDR 1c"
else
    KEYS="editsmk\\n${PAD}\\ndump $ADDR 1c"
fi

"$POWERSHELL" -NoProfile -ExecutionPolicy Bypass -File "$CAPTURE_W" \
    -Emu "$EMU_W" -Boot "$BOOT_W" -Kernel "$KERNEL_W" -Gif "$GIF_W" \
    -Keys "$KEYS" -Seconds 30 >/dev/null

"$PYTHON" - "$OUT_GIF" "$FONT_W" "$EDIT_REPO/keyboard.inc" "$ADDR" "$NEG" <<'PY'
import os, re, sys, io
from collections import Counter
from PIL import Image, ImageFile
ImageFile.LOAD_TRUNCATED_IMAGES = True

gif, fontinc, kbdinc, addr, neg = (sys.argv[1], sys.argv[2], sys.argv[3],
                                   sys.argv[4], sys.argv[5] == "1")
if not os.path.exists(gif):
    sys.exit("FAIL: emulator did not create GIF")

# ---- expected bytes: the default half of keyboard.inc's `keyval` table -------
src = io.open(kbdinc, encoding="utf-8", errors="replace").read().splitlines()
expected = []
state = 0
for line in src:
    s = line.split(";")[0].strip()
    if state == 0:
        # The file defines keyval twice, under .ifndef/.else alt_shortcuts. The
        # build takes the first, so stop reading at the .else.
        if s.startswith("keyval:"):
            state = 1
        continue
    if s.startswith(".else") or s.startswith(".endif"):
        break
    m = re.match(r"\.byt\s+(.*)$", s)
    if not m:
        if s:
            break
        continue
    for v in m.group(1).split(","):
        v = v.strip()
        if v.startswith("$"):
            expected.append(int(v[1:], 16))
if len(expected) != 28:
    sys.exit("FAIL: read %d shortcut bytes from keyboard.inc, expected 28"
             % len(expected))

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
        ch = glyph.get(tuple(bits))
        if ch is None:
            ch = glyph.get(tuple((~b) & 0xFF for b in bits), "?")
        out += ch
    return out.rstrip()


def frame_count(im):
    n = 0
    while True:
        try:
            im.seek(n)
            im.load()
            n += 1
        except (EOFError, OSError, IndexError):
            break
    return n


im = Image.open(gif)
n = frame_count(im)
if n == 0:
    sys.exit("FAIL: no decodable frame")
im.seek(n - 1)
im.load()
rows = [frame_row(im, r) for r in range(60)]

# `dump` prints "BB:AAAA XX XX ... |ascii" -- bank, colon, offset. Collect the
# hex bytes of every dump line, keyed by address.
base = int(addr, 16)
got = {}
for r in rows:
    m = re.match(r"^([0-9A-Fa-f]{2}):([0-9A-Fa-f]{4})((?: [0-9A-Fa-f]{2})+)", r)
    if not m:
        continue
    a = (int(m.group(1), 16) << 16) | int(m.group(2), 16)
    for i, b in enumerate(m.group(3).split()):
        got[a + i] = int(b, 16)
actual = [got.get(base + i) for i in range(28)]

ok = actual == expected


def dump():
    print("    expected: %s" % " ".join("%02X" % b for b in expected))
    print("    actual:   %s" % " ".join("--" if b is None else "%02X" % b
                                        for b in actual))
    print("    final screen:")
    for i, r in enumerate(rows):
        if r:
            print("      %2d: %s" % (i, r))


if neg:
    if ok:
        print("FAIL (negative control): the shortcut table was already correct "
              "at $%s without the editor ever running -- the check is not "
              "testing keyboard_init's copy" % addr)
        dump()
        sys.exit(1)
    if not any(b is not None for b in actual):
        print("FAIL (negative control): nothing was dumped at all, so this run "
              "says nothing about the check")
        dump()
        sys.exit(1)
    print("PASS (negative control): with the editor never entered the table is "
          "not there, and the check correctly did not pass")
    sys.exit(0)

if not ok:
    print("FAIL: keyboard_ctrl_keyval at $%s is not the default shortcut "
          "table -- every Ctrl command is dispatched against these bytes" % addr)
    dump()
    sys.exit(1)

print("PASS: keyboard_init copied all 28 default shortcuts into "
      "keyboard_ctrl_keyval at $%s, across the bank boundary" % addr)
dump()
PY
