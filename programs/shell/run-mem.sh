#!/usr/bin/env bash
# MEM_TOP / MEM_RELEASE conformance: the releasable kernel writable-data region.
#
#     ./run-mem.sh [--negative]
#
# Boots the RESIDENT kernel and drives the new `mem` command through the real
# keyboard path, reading the answers back off the screen. What it proves:
#
#   * the ceiling MEM_TOP reports at boot is $BF:FFFF -- the region reserved
#   * `mem release` moves it to $DF:FFFF and says the region is released
#   * a SECOND release does not move it again and does not report reserved
#   * the free-byte count grows by exactly 2 MB across the release, which is the
#     property that says MEM_ALLOC actually got the space rather than just the
#     printed ceiling changing
#
# WHY THE BYTE COUNT AND NOT ONLY THE CEILING. The bug this design exists to
# prevent is a boundary that two parties disagree about. Printing the ceiling
# proves what MEM_TOP thinks; the delta in kmem_free_bytes() proves the
# ALLOCATOR agrees. A release that updated the reported ceiling without moving
# the allocator's limit would pass a ceiling-only check and hand out nothing.
#
# THE NEGATIVE CONTROL drives `mem` twice with NO release in between and
# requires the ceiling NOT to move. Without it a test that only ever saw the
# ceiling change could be passing because `mem` prints $DF:FFFF unconditionally.
#
# Requires Pillow:  pip install pillow
set -u

. "$(dirname "$0")/../../runtime/calypsi.sh"
cd "$(dirname "$0")"
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
WOUT=$(cygpath -m "$OUT" 2>/dev/null || echo "$OUT")

[ -f kernel.bin ] || { echo "kernel.bin missing -- run: sh build.sh"; exit 1; }
cp kernel.bin "$OUT/kernel.bin"

NEGATIVE=0
KEYS='mem\nmem release\nmem\n'
if [ "${1:-}" = "--negative" ]; then
    NEGATIVE=1
    KEYS='mem\nmem\nmem\n'
    echo "negative control: three plain 'mem' calls, expecting the ceiling to stay put"
fi

SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout 90 \
    "$EMU/build/x16emu.exe" -boot "$(cygpath -m "$CORE/boot/boot.rom")" \
    -load "F00000,$WOUT/kernel.bin" -autokeys "$KEYS" \
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

# The console is 60 rows; read them all. A fixed window that is too short is a
# trap this project has already paid for once -- added cases scrolled the
# verdict off the bottom and every case still printed "ok" while the run failed.
rows = [row_text(r) for r in range(60)]

def fail(msg):
    print("FAIL:", msg)
    for i, r in enumerate(rows):
        if r:
            print("  %2d: %r" % (i, r))
    sys.exit(1)

tops  = [r for r in rows if r.startswith("top ")]
heaps = [r for r in rows if r.startswith("heap ")]

if len(tops) < 2:
    fail("expected at least two 'top' lines, got %d" % len(tops))
if len(heaps) < 2:
    fail("expected at least two 'heap' lines, got %d" % len(heaps))

def ceiling(line):
    m = re.search(r'([0-9A-F]{2}):([0-9A-F]{4})', line)
    if not m:
        fail("no bank:offset in %r" % line)
    return (int(m.group(1), 16) << 16) | int(m.group(2), 16)

def freebytes(line):
    m = re.search(r'heap\s+(\d+) free,\s*(\d+) live', line)
    if not m:
        fail("could not parse %r" % line)
    return int(m.group(1)), int(m.group(2))

first, last = ceiling(tops[0]), ceiling(tops[-1])
f_first, live_first = freebytes(heaps[0])
f_last,  live_last  = freebytes(heaps[-1])

RESERVED = 0xBFFFFF
RELEASED = 0xDFFFFF
TWO_MB   = 0x200000

if negative:
    if first != RESERVED:
        fail("boot ceiling is $%06X, expected $%06X" % (first, RESERVED))
    if last != first:
        fail("the ceiling MOVED ($%06X -> $%06X) with no release -- "
             "'mem' is not reporting the live boundary" % (first, last))
    if f_last != f_first:
        fail("free bytes changed (%d -> %d) with no release" % (f_first, f_last))
    print("PASS (negative control): no release, ceiling stayed $%06X" % first)
    sys.exit(0)

if first != RESERVED:
    fail("boot ceiling is $%06X, expected $%06X -- the region is not reserved "
         "at boot, which is the safe default the whole design rests on"
         % (first, RESERVED))
if "reserved" not in tops[0]:
    fail("boot 'top' line does not say reserved: %r" % tops[0])
if last != RELEASED:
    fail("ceiling after release is $%06X, expected $%06X" % (last, RELEASED))
if "released" not in tops[-1]:
    fail("post-release 'top' line does not say released: %r" % tops[-1])

got = f_last - f_first
if got != TWO_MB:
    fail("free bytes grew by %d across the release, expected %d (%s) -- the "
         "reported ceiling moved but the ALLOCATOR did not get the space"
         % (got, TWO_MB, hex(TWO_MB)))
if live_first != 0 or live_last != 0:
    fail("blocks live changed: %d -> %d; nothing here allocates"
         % (live_first, live_last))

# The third `mem` is the idempotence check: it must still read released, and the
# ceiling must not have climbed a second time.
if len(tops) >= 3 and ceiling(tops[2]) != RELEASED:
    fail("a second release moved the ceiling again: $%06X" % ceiling(tops[2]))

print("PASS: boot $%06X reserved -> release -> $%06X released, "
      "heap +%d bytes, %d blocks live"
      % (first, last, got, live_last))
PY
