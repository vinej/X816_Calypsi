#!/usr/bin/env bash
# Keyboard conformance: build shell.bin, TYPE AT IT, and check what came back.
#
# This is the test that did not exist, and its absence is the whole reason a
# dead keyboard had to be found on a DE10-Nano by hand. Everything else in the
# console had a conformance run; con_getkey() had none, because testing it
# seemed to need a person at a keyboard. It does not: X816_Emulator now takes
#
#     -autokeys <text>
#
# which feeds the SAME SMC key FIFO a real keypress feeds, so this drives the
# real path -- bit-banged I2C, GETKEY, release-flag filtering, keymap, echo --
# with no window and no user.
#
# What it proves, in order, and each one failed at some point:
#   1. the I2C transaction is well formed, so the SMC ACKs and serves a byte
#      (a miscompiled shift sent 3-4 bits per byte and every read NACKed)
#   2. bit 7 releases are filtered out, so each key registers once
#   3. the keycode decodes through keymap[] to the right character
#   4. sh_readline echoes it, so what was typed appears on screen
#   5. Enter dispatches the line, so `help' actually lists the commands
#
#   ./run-kbd.sh                 build and run
#   ./run-kbd.sh --negative      type a command that does not exist, to prove
#                                this can fail rather than always printing PASS
#
# Requires Pillow:  pip install pillow
set -u

CALYPSI=${CALYPSI:-../../Calypsi/calypsi-65816-5.18}
EMU=${EMU:-/c/quartus/projects/X816_Emulator}
CORE=${CORE:-/c/quartus/projects/X816_core}
RT=../../runtime
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
WOUT=$(cygpath -m "$OUT" 2>/dev/null || echo "$OUT")

CFLAGS="--core=65816 --code-model=large --data-model=small -O0 -I $RT"

TYPE='help\n'
WANT_ROW3='HELP'
if [ "${1:-}" = "--negative" ]; then
    TYPE='zzz\n'
    WANT_ROW3='ZZZ'
    echo "negative control: typing an unknown command, expecting the ? line"
fi

"$CALYPSI/bin/cc65816" $CFLAGS shell.c          -o "$OUT/main.o"    || exit 1
"$CALYPSI/bin/cc65816" $CFLAGS $RT/shell.c      -o "$OUT/shell.o"   || exit 1
"$CALYPSI/bin/cc65816" $CFLAGS $RT/console.c    -o "$OUT/console.o" || exit 1
"$CALYPSI/bin/cc65816" $CFLAGS $RT/font8x8.c    -o "$OUT/font.o"    || exit 1
"$CALYPSI/bin/as65816" --core=65816 $RT/x816hdr.s -o "$OUT/hdr.o"   || exit 1
"$CALYPSI/bin/as65816" --core=65816 $RT/smc.s     -o "$OUT/smc.o"   || exit 1
"$CALYPSI/bin/ln65816" $RT/x816-lib.scm "$OUT/hdr.o" "$OUT/main.o" \
    "$OUT/shell.o" "$OUT/console.o" "$OUT/font.o" "$OUT/smc.o" \
    "$CALYPSI/lib/clib-lc-sd.a" -o "$OUT/SHELL.elf" --output-format raw \
    --program-root __x816_root_section --rtattr exit=simplified || exit 1
cp "$OUT/SHELL.raw" "$OUT/shell.bin" || exit 1

SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout 90 \
    "$EMU/build/x16emu.exe" -boot "$(cygpath -m "$CORE/boot/boot.rom")" \
    -load "010000,$WOUT/shell.bin" -autokeys "$TYPE" \
    -warp -gif "$WOUT/out.gif" >/dev/null 2>&1

python - "$WOUT/out.gif" "$CORE/boot/font8x8.inc" "$WANT_ROW3" <<'PY'
import sys, re, io
from PIL import Image, ImageFile
ImageFile.LOAD_TRUNCATED_IMAGES = True

gif, fontinc, want_typed = sys.argv[1], sys.argv[2], sys.argv[3]

vals = []
for line in io.open(fontinc, encoding='utf-8'):
    m = re.match(r'\s*\.byte\s+(.*)$', line.split(';')[0])
    if m:
        vals += [int(x.strip().lstrip('$'), 16)
                 for x in m.group(1).split(',') if x.strip()]
glyph = {tuple(vals[i:i+8]): chr(0x20 + i // 8) for i in range(0, len(vals), 8)}

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

rows = [row_text(r) for r in range(12)]

def fail(msg):
    print("FAIL:", msg)
    for i, r in enumerate(rows):
        print(f"  {i}: {r!r}")
    sys.exit(1)

if rows[0] != "X816":
    fail("no banner -- the shell did not even start")

# Row 1 is the prompt plus whatever was echoed as it was typed. If the I2C
# transaction is malformed the SMC NACKs, con_getkey() returns 0 for ever, and
# this row stays at just "> " -- which is precisely how the bug presented.
if rows[1] == ">":
    fail("the prompt is bare: nothing was echoed, so no key ever arrived. "
         "That is the SMC/I2C path, not the shell.")
if not rows[1].startswith("> " + want_typed):
    fail(f"row 1 is {rows[1]!r}, expected the prompt then {want_typed!r} "
         "-- keys arrived but decoded to the wrong characters")

# Enter must have dispatched the line, which means output BELOW the input row.
if not any(rows[2:]):
    fail("nothing below the input line: Enter never dispatched the command")

if want_typed == "HELP":
    body = " ".join(rows[2:])
    for cmd in ("HELP", "VER", "CLS", "DUMP", "PEEK", "POKE", "FILL", "MOVE"):
        if cmd not in body:
            fail(f"`help' ran but did not list {cmd}")
    print("PASS: typed HELP, it echoed, Enter dispatched it, "
          "and all eight commands were listed")
else:
    if "?" not in " ".join(rows[2:]):
        fail("an unknown command did not produce the ? line")
    print("PASS (negative control): the unknown command echoed and was refused")

for r in rows:
    if r:
        print("   ", r)
PY
