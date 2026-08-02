#!/usr/bin/env bash
# The reshaped x16lib MEMORY API, end to end.
#
# libmem.s calls mem_alloc, mem_peek/poke, mem_fill, mem_copy, mem_crc and
# mem_free -- the library. Two of those cross into the kernel through
# system/x816kernel.asm; the rest are library code walking 24-bit pointers by
# hand, because there is no KERNAL here to do it. The whole stack is what is
# under test. examples/kernel/run-mem.sh already covers MEM_ALLOC/MEM_FREE at
# the ABI on their own.
#
# No card, so unlike run-libfs.sh there is no second, independent verdict from
# the host. What stands in for one is that the checks do not read back only
# what they wrote: mem_crc is run against "123456789", whose CRC-16/IBM-3740
# check value is the published $29B1 -- an oracle this tree did not produce.
#
#   ./run-libmem.sh              build and run
#   ./run-libmem.sh --negative   break the LIBRARY, not the test, and require
#                                the test to notice
#
# THE NEGATIVE CONTROL BREAKS THE CODE UNDER TEST. Most --negative flags in
# this tree corrupt an expectation, which proves the harness can report a
# failure. This one patches storage/mem.s so that .overlaps_up always answers
# "no overlap", which makes mem_copy run forwards through a range that
# overlaps upwards -- the classic smear, 1,2,3,4,1,2,3,4 instead of 1..8. If
# test 5 still passed after that, it would not be testing the direction logic
# at all, and the direction logic is the only reason mem_copy is more than a
# byte loop.
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

INC="$X16LIB"
NEG=0
if [ "${1:-}" = "--negative" ]; then
    NEG=1
    # A patched COPY OF THE WHOLE LIBRARY. Not merely a patched mem.s on an
    # earlier -I path: the C preprocessor resolves #include "storage/mem.s"
    # relative to the INCLUDING FILE first, so x16_code.s pulls in the real
    # one whatever -I says. The first version of this control did that, and
    # reported a pass while changing nothing.
    cp -r "$X16LIB" "$OUT/patch" || exit 1
    sed 's/^mem_ov_yes:/mem_ov_yes:\n    clc\n    rts/' \
        "$X16LIB/storage/mem.s" > "$OUT/patch/storage/mem.s"
    # And a SPECIFIC check that the patch landed. `grep clc` is not one --
    # mem.s has several -- so that guard could not have failed either.
    if ! grep -A1 "^mem_ov_yes:" "$OUT/patch/storage/mem.s" | grep -q "clc"; then
        echo "negative control: the patch did not apply -- refusing to run" >&2
        exit 1
    fi
    INC="$OUT/patch"
    echo "negative control: mem_copy will never choose the backward direction,"
    echo "                  so test 5 (CYAN) must fail"
fi

as816 libmem.s        "$OUT/t.o"       -I "$INC" || exit 1
cc816 $RT/kmem.c      "$OUT/kmem.o"    || exit 1
cc816 $RT/kfs.c       "$OUT/kfs.o"     || exit 1
cc816 $RT/fat32.c     "$OUT/fat32.o"   || exit 1
cc816 $RT/kexec.c     "$OUT/kexec.o"   || exit 1
cc816 $RT/goshell.c   "$OUT/gosh.o"    || exit 1
cc816 $RT/console.c   "$OUT/console.o" || exit 1
cc816 $RT/font8x8.c   "$OUT/font.o"    || exit 1
as816 $RT/x816hdr.s   "$OUT/hdr.o"     || exit 1
as816 $RT/smc.s       "$OUT/smc.o"     || exit 1
as816 $RT/exec.s      "$OUT/exec.o"    || exit 1
as816 $RT/font_cp437.s "$OUT/fontcp.o" || exit 1
as816 $RT/kerntab.s   "$OUT/tab.o"     || exit 1
# kerntab.s's generated table names the four interrupt/clock entries (IRQ_SET,
# TIME_GET, TIME_SET, IRQ_FRAMES), so every image that links the table now
# links kirq.s behind it.
as816 $RT/kirq.s      "$OUT/kirq.o"   -I "$RT" || exit 1

ln816 "$OUT/LIBMEM" "$OUT/hdr.o" "$OUT/t.o" "$OUT/kmem.o" "$OUT/kfs.o" \
      "$OUT/fat32.o" "$OUT/kexec.o" "$OUT/gosh.o" "$OUT/console.o" \
      "$OUT/font.o" "$OUT/smc.o" "$OUT/exec.o" "$OUT/fontcp.o" \
      "$OUT/tab.o" "$OUT/kirq.o" || exit 1
cp "$OUT/LIBMEM.raw" "$OUT/libmem.bin" || exit 1

SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout -s KILL 90 \
    "$EMU/build/x16emu.exe" -boot "$(cygpath -m "$CORE/boot/boot.rom")" \
    -load "010000,$WOUT/libmem.bin" \
    -warp -gif "$WOUT/out.gif" >/dev/null 2>&1

python - "$WOUT/out.gif" "$NEG" <<'PY'
import sys, collections
from PIL import Image, ImageFile
ImageFile.LOAD_TRUNCATED_IMAGES = True

gif, neg = sys.argv[1], sys.argv[2] == '1'

WHICH = {
    (0x00, 0xCC, 0x55): (0, 'green -- every check passed'),
    (0x88, 0x00, 0x00): (1, 'mem_alloc: in the arena and page-aligned'),
    (0xEE, 0xEE, 0x77): (2, 'mem_poke / mem_peek round-trip at 24 bits'),
    (0x00, 0x00, 0xAA): (3, 'mem_fill at the first, middle and last byte'),
    (0xCC, 0x44, 0xCC): (4, 'a second block is distinct, and mem_copy fills it'),
    (0xAA, 0xFF, 0xEE): (5, 'mem_copy with the ranges overlapping upwards'),
    (0x00, 0x88, 0x00): (6, 'mem_crc against the published $29B1'),
    (0x66, 0x44, 0x00): (7, 'mem_free, and a double free refused'),
    (0x77, 0x77, 0x77): (8, 'a fill that CROSSES A BANK BOUNDARY'),
    (0xFF, 0x77, 0x77): (9, 'a copy whose source and target cross at different points'),
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
if neg:
    if code == 5:
        print('PASS (negative control): a forward-only mem_copy smears an '
              'overlapping range, and test 5 caught it')
        sys.exit(0)
    print('FAIL: with the direction logic disabled the suite reported %r, '
          'not test 5 -- test 5 is not testing what it says' % (what,))
    sys.exit(1)
if code != 0:
    print('FAIL: test %d -- %s' % (code, what))
    sys.exit(1)
print('PASS: the x16lib memory API over the kernel allocator -- alloc, '
      'peek/poke, fill, copy (including overlap), CRC and free')
PY
