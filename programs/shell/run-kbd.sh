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
#   6. SHIFT works: ':' is Shift-';' and cannot be typed any other way
#
#   ./run-kbd.sh                 build and run
#   ./run-kbd.sh --negative      type a command that does not exist, to prove
#                                this can fail rather than always printing PASS
#
# Requires Pillow:  pip install pillow
set -u

# The toolchain, the memory map and the -O0 rule come from one place --
# runtime/calypsi.sh -- so this script cannot drift from the build that ships.
# It also sets EMU, CORE, RT and X16LIB, and cc816 refuses -O1+ silently.
. "$(dirname "$0")/../../runtime/calypsi.sh"
cd "$(dirname "$0")"
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
WOUT=$(cygpath -m "$OUT" 2>/dev/null || echo "$OUT")

TYPE='help\npeek 00:0400\n'
WANT_ROW3='HELP'
if [ "${1:-}" = "--negative" ]; then
    TYPE='zzz\n'
    WANT_ROW3='ZZZ'
    echo "negative control: typing an unknown command, expecting the ? line"
fi

cc816 shell.c "$OUT/main.o"    || exit 1
cc816 $RT/shell.c "$OUT/shell.o"   || exit 1
cc816 $RT/fat32.c "$OUT/fat32.o"   || exit 1
cc816 $RT/kfs.c "$OUT/kfs.o"   || exit 1
cc816 $RT/kmem.c "$OUT/kmem.o"   || exit 1
cc816 $RT/console.c "$OUT/console.o" || exit 1
cc816 $RT/font8x8.c "$OUT/font.o"    || exit 1
as816 $RT/x816hdr.s "$OUT/hdr.o"   || exit 1
as816 $RT/smc.s "$OUT/smc.o"   || exit 1
as816 $RT/exec.s "$OUT/exec.o"  || exit 1
as816 $RT/font_cp437.s "$OUT/fontcp.o" || exit 1
# The console cursor. console.c calls ccur_suspend/ccur_resume around every
# scroll, so it is not optional for anything that links console.o -- leaving
# it out is an undefined-symbol link error, which is how this script stopped
# building when the cursor landed.
as816 $RT/ccursor.s "$OUT/ccursor.o" || exit 1
ln816 "$OUT/SHELL" "$OUT/hdr.o" "$OUT/main.o" "$OUT/shell.o" "$OUT/fat32.o" "$OUT/kmem.o" "$OUT/kfs.o" "$OUT/console.o" "$OUT/font.o" "$OUT/smc.o" "$OUT/exec.o" "$OUT/fontcp.o" "$OUT/ccursor.o" || exit 1
cp "$OUT/SHELL.raw" "$OUT/shell.bin" || exit 1

SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout 90 \
    "$EMU/build/x16emu.exe" -boot "$(cygpath -m "$CORE/boot/boot.rom")" \
    -load "010000,$WOUT/shell.bin" -autokeys "$TYPE" \
    -warp -gif "$WOUT/out.gif" >/dev/null 2>&1

python - "$WOUT/out.gif" "$RT/font_cp437.s" "$WANT_ROW3" <<'PY'
import sys, re, io
from PIL import Image, ImageFile
ImageFile.LOAD_TRUNCATED_IMAGES = True

gif, fontinc, want_typed = sys.argv[1], sys.argv[2], sys.argv[3]

# The console now carries all 256 CP437 glyphs, so decode against the
# GENERATED font rather than the old 64-glyph boot font -- and lower case is
# real lower case on screen now, not folded up.
vals = []
for line in io.open(fontinc, encoding='utf-8'):
    m = re.match(r'\s*\.byte\s+(.*)$', line.split(';')[0])
    if m:
        vals += [int(x.strip().lstrip('$'), 16)
                 for x in m.group(1).split(',') if x.strip()]
# Only $20-$7E is mapped back to characters. Everything else decodes to '?',
# which keeps blank glyphs from colliding with space and makes a stray box
# character obvious rather than silently readable.
glyph = {}
for _c in range(0x20, 0x7F):
    glyph[tuple(vals[_c * 8:(_c + 1) * 8])] = chr(_c)

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

# The prompt row, found rather than assumed -- the banner above it is art whose
# height and layout may change. If the I2C transaction is malformed the SMC
# NACKs, con_getkey() returns 0 for ever, and this row stays at just "> " --
# which is precisely how the bug presented.
pi = next((i for i, r in enumerate(rows) if r.startswith(">")), None)
if pi is None:
    fail("no prompt on screen at all: the shell never reached sh_run")

# The banner is the chevron logo (block glyphs, which do not decode as text)
# with the wordmark under it, so "X816" is somewhere ABOVE THE PROMPT rather
# than on a fixed row. Above the prompt is the whole point: `ver' prints
# "X816 shell 0.1" BELOW it, so a search over the whole screen would find that
# and report a banner on a machine that never drew one.
if not any("X816" in r for r in rows[:pi]):
    fail("no banner -- the shell did not even start")
if rows[pi] == ">":
    fail("the prompt is bare: nothing was echoed, so no key ever arrived. "
         "That is the SMC/I2C path, not the shell.")
if not rows[pi].upper().startswith("> " + want_typed):
    fail(f"the prompt row is {rows[pi]!r}, expected the prompt then "
         f"{want_typed!r} -- keys arrived but decoded to the wrong characters")

# Enter must have dispatched the line, which means output BELOW the input row.
if not any(rows[pi + 1:]):
    fail("nothing below the input line: Enter never dispatched the command")

if want_typed == "HELP":
    # Upper-cased on both sides. The shell prints its command table in lower
    # case now that the console has a real lower case, and this check is about
    # whether each command is THERE -- pinning it to a cosmetic choice would
    # make the test fail every time someone restyles the help text.
    body = " ".join(rows[pi + 1:]).upper()
    for cmd in ("HELP", "VER", "CLS", "DUMP", "PEEK", "POKE", "FILL", "MOVE",
                "LS", "DIR", "CD", "PWD", "TYPE", "RUN", "LOAD", "SAVE",
                "COPY", "DEL", "RENAME", "MKDIR", "RMDIR"):
        if cmd not in body:
            fail(f"`help' ran but did not list {cmd}")
    # SHIFT. ':' is Shift-';' and there is no other way to type it, so seeing it
    # echoed proves the modifier is tracked on BOTH edges. A shift whose key-up
    # is discarded sticks down for ever and every later character arrives
    # shifted instead -- which is silent, and looks like a keymap fault.
    if "PEEK 00:0400" not in " ".join(rows).upper():
        fail("a shifted character (:) did not reach the shell")

    print("PASS: typed HELP, it echoed, Enter dispatched it, "
          "and every command was listed")
else:
    # BELOW THE PROMPT, not below row 1. The banner is drawn with block glyphs
    # that this decoder does not have in its table, so it renders them as '?' --
    # scanning from row 2 would find the LOGO and pass without the shell having
    # refused anything at all.
    if "?" not in " ".join(rows[pi + 1:]):
        fail("an unknown command did not produce the ? line")
    print("PASS (negative control): the unknown command echoed and was refused")

for r in rows:
    if r:
        print("   ", r)
PY
