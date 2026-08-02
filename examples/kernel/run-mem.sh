#!/usr/bin/env bash
# Kernel MEMORY conformance: MEM_ALLOC / MEM_FREE through $00:FE00.
#
# X816_Core doc/KERNEL.md section 8 test 7. Seven checks, each with its own
# failure colour, and every one of them writes through the address it was
# given -- an allocator that returns plausible, disjoint, page-aligned
# addresses backed by nothing passes every arithmetic test there is.
#
# No card is involved: this is the one kernel subsystem with no device under
# it, so the emulator's SDRAM is the real thing rather than a model of it.
# That also means the verdict is single -- there is no independent host-side
# check the way run-kfs.sh has pyfatfs. What stands in for it is that the
# tests check the ALLOCATOR'S OWN accounting (kmem_live, kmem_free_bytes)
# against what the ABI handed back, so a bug would have to lie consistently
# in two places.
#
#   ./run-mem.sh              build and run
#   ./run-mem.sh --negative   break one expectation, to prove this can fail
#
# The negative control breaks test 3's "a freed block comes back at the same
# address" into "comes back somewhere else", because that is the check most
# likely to be silently satisfied: a first-fit allocator and an appending one
# both return valid, disjoint addresses, and only that equality tells them
# apart. If the negative run still painted green, the whole file would be
# proving nothing.
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

SRC=memtest.c
NEG=0
if [ "${1:-}" = "--negative" ]; then
    NEG=1
    sed 's/if (again != b)/if (again == b)/' memtest.c > "$OUT/neg.c"
    SRC="$OUT/neg.c"
    echo "negative control: expecting BLUE (test 3, exact reuse of a freed block)"
fi

cc816 "$SRC"           "$OUT/t.o"       || exit 1
cc816 $RT/kmem.c       "$OUT/kmem.o"    || exit 1
cc816 $RT/kfs.c        "$OUT/kfs.o"     || exit 1
cc816 $RT/fat32.c      "$OUT/fat32.o"   || exit 1
cc816 $RT/kexec.c      "$OUT/kexec.o"   || exit 1
cc816 $RT/goshell.c    "$OUT/gosh.o"    || exit 1
cc816 $RT/console.c    "$OUT/console.o" || exit 1
cc816 $RT/font8x8.c    "$OUT/font.o"    || exit 1
as816 $RT/x816hdr.s    "$OUT/hdr.o"     || exit 1
as816 $RT/smc.s        "$OUT/smc.o"     || exit 1
as816 $RT/exec.s       "$OUT/exec.o"    || exit 1
as816 $RT/font_cp437.s "$OUT/fontcp.o"  || exit 1
as816 $RT/kerntab.s    "$OUT/tab.o"     || exit 1
as816 $RT/kcall.s      "$OUT/kcall.o"   || exit 1

ln816 "$OUT/MEMTEST" "$OUT/hdr.o" "$OUT/t.o" "$OUT/kmem.o" "$OUT/kfs.o" \
      "$OUT/fat32.o" "$OUT/kexec.o" "$OUT/gosh.o" "$OUT/console.o" \
      "$OUT/font.o" "$OUT/smc.o" "$OUT/exec.o" "$OUT/fontcp.o" \
      "$OUT/tab.o" "$OUT/kcall.o" || exit 1
cp "$OUT/MEMTEST.raw" "$OUT/memtest.bin" || exit 1

SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout -s KILL 90 \
    "$EMU/build/x16emu.exe" -boot "$(cygpath -m "$CORE/boot/boot.rom")" \
    -load "010000,$WOUT/memtest.bin" \
    -warp -gif "$WOUT/out.gif" >/dev/null 2>&1

python - "$WOUT/out.gif" "$NEG" <<'PY'
import sys, collections
from PIL import Image, ImageFile
ImageFile.LOAD_TRUNCATED_IMAGES = True

gif, neg = sys.argv[1], sys.argv[2] == '1'

WHICH = {
    (0x00, 0xCC, 0x55): (0, 'green -- every check passed'),
    (0x88, 0x00, 0x00): (1, 'a first allocation: aligned, in the arena, backed'),
    (0xEE, 0xEE, 0x77): (2, 'three live blocks pairwise disjoint (section 8 test 7)'),
    (0x00, 0x00, 0xAA): (3, 'a freed block is reused at exactly its own address'),
    (0xCC, 0x44, 0xCC): (4, 'the refusals: zero, oversized, unaligned, double free'),
    (0xAA, 0xFF, 0xEE): (5, 'a refused allocation left the heap unchanged'),
    (0x00, 0x88, 0x00): (6, 'the table fills, refuses with NOSPACE, and recovers'),
    (0x66, 0x44, 0x00): (7, 'neighbouring blocks do not bleed into each other'),
}

im = Image.open(gif)
n = 0
while True:
    try:
        im.seek(n); im.load(); n += 1
    except Exception:
        break
if n == 0:
    sys.exit('no decodable frame -- did the emulator run?')
im.seek(n - 1)
rgb = im.convert('RGB')
(top, cnt), = collections.Counter(rgb.get_flattened_data()).most_common(1)
frac = cnt / (rgb.width * rgb.height)

if frac < 0.99:
    print('FAIL: the screen is not one colour (%.0f%% is %r) -- the test did '
          'not reach its verdict' % (frac * 100, top))
    sys.exit(1)

code, what = WHICH.get(top, (99, 'unrecognised colour %r' % (top,)))
if code != 0:
    print('FAIL: test %d -- %s' % (code, what))
    sys.exit(0 if neg else 1)
if neg:
    print('FAIL: the negative control painted GREEN, so test 3 proves nothing')
    sys.exit(1)
print('PASS: MEM_ALLOC/MEM_FREE -- seven checks, every block written and read '
      'back through the address the kernel returned')
PY
