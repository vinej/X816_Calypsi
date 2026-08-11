#!/usr/bin/env bash
# x16lib's system/irq.asm and system/clock.asm, over the kernel.
#
# programs/kernel/run-irq.sh already proved IRQ_SET, the dispatcher and both
# clocks AT THE ABI. This covers the LIBRARY layer on top, and one property
# that only exists there: the 8-bit/16-bit crossing running BACKWARDS. Every
# other call in this tree is 8-bit library code calling a 16-bit kernel; an
# interrupt handler is the kernel calling into the library, and the
# trampolines in system/x816kernel.asm have to sep down, run 65C02 code, rep
# back and rtl. Get that wrong and the dispatcher's own stack pulls take the
# wrong number of bytes -- silently, and not where the mistake is.
#
#   ./run-libirq.sh              build and run
#   ./run-libirq.sh --negative   break the LIBRARY, not the test, and require
#                                the test to notice
#
# THE NEGATIVE CONTROL BREAKS THE CODE UNDER TEST. It patches the LINE
# trampoline in system/x816kernel.s so it never calls irq_on_line -- the
# kernel still dispatches, the trampoline still returns cleanly, and the
# library handler simply never runs. Test 5 must catch that. If it did not,
# it would be testing "the kernel called something" rather than "the crossing
# reaches the library", which is the only reason this file exists.
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
    # A patched COPY OF THE WHOLE LIBRARY. Not merely a patched file on an
    # include path -- x16_code.s pulls its siblings by relative name, so a
    # half-patched tree would quietly assemble the original.
    cp -r "$X16LIB" "$OUT/patch" || exit 1
    sed 's/^    jsr \.word0 (irq_on_line)$/    nop/'         "$X16LIB/system/x816kernel.s" > "$OUT/patch/system/x816kernel.s"
    # And a SPECIFIC check that the patch landed. A run whose "negative
    # control" silently did nothing paints green and proves the opposite of
    # what it claims -- run-libmem.sh's first version did exactly that.
    if grep -q "jsr .word0 (irq_on_line)" "$OUT/patch/system/x816kernel.s"; then
        echo "negative control: the patch did not apply -- refusing to run" >&2
        exit 1
    fi
    INC="$OUT/patch"
    echo "negative control: the LINE trampoline will not reach the library,"
    echo "                  so test 5 (CYAN) must fail"
fi

as816 libirq.s        "$OUT/t.o"       -I "$INC" || exit 1
cc816 $RT/kmem.c      "$OUT/kmem.o"    || exit 1
cc816 $RT/kfs.c       "$OUT/kfs.o"     || exit 1
cc816 $RT/fat32.c     "$OUT/fat32.o"   || exit 1
cc816 $RT/kexec.c     "$OUT/kexec.o"   || exit 1
cc816 $RT/goshell.c   "$OUT/gosh.o"    || exit 1
cc816 $RT/console.c   "$OUT/console.o" || exit 1
as816 $RT/ccursor.s    "$OUT/ccur.o"     || exit 1
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

ln816 "$OUT/LIBIRQ" "$OUT/hdr.o" "$OUT/t.o" "$OUT/kmem.o" "$OUT/kfs.o" \
      "$OUT/fat32.o" "$OUT/kexec.o" "$OUT/gosh.o" "$OUT/console.o" "$OUT/ccur.o" \
      "$OUT/font.o" "$OUT/smc.o" "$OUT/exec.o" "$OUT/fontcp.o" \
      "$OUT/tab.o" "$OUT/kirq.o" || exit 1
cp "$OUT/LIBIRQ.raw" "$OUT/libirq.bin" || exit 1

SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout -s KILL 90 \
    "$EMU/build/x16emu.exe" -boot "$(cygpath -m "$CORE/boot/boot.rom")" \
    -load "010000,$WOUT/libirq.bin" \
    -warp -gif "$WOUT/out.gif" >/dev/null 2>&1

python - "$WOUT/out.gif" "$NEG" <<'PY'
import sys, collections
from PIL import Image, ImageFile
ImageFile.LOAD_TRUNCATED_IMAGES = True

gif, neg = sys.argv[1], sys.argv[2] == '1'

WHICH = {
    (0x00, 0xCC, 0x55): (0, 'green -- every check passed'),
    (0x88, 0x00, 0x00): (1, 'irq_frames advances: the kernel count reaches the library'),
    (0xEE, 0xEE, 0x77): (2, 'clock_get_ms advances and agrees with the frame count'),
    (0x00, 0x00, 0xAA): (3, 'clock_mark / clock_elapsed measure a known interval'),
    (0xCC, 0x44, 0xCC): (4, 'clock_delay waits about as long as it was asked to'),
    (0xAA, 0xFF, 0xEE): (5, 'a raster handler runs through the trampoline, and stops when removed'),
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
        print('PASS (negative control): with the trampoline cut, the library '
              'handler never runs and test 5 caught it')
        sys.exit(0)
    print('FAIL: with the trampoline cut the suite reported %r, not test 5 '
          '-- test 5 is not testing what it says' % (what,))
    sys.exit(1)
if code != 0:
    print('FAIL: test %d -- %s' % (code, what))
    sys.exit(1)
print('PASS: x16lib irq and clock over the kernel -- frames, milliseconds, '
      'and a raster handler reached through the 8-bit/16-bit trampoline '
      'that stops when its slot is cleared')
PY
