#!/usr/bin/env bash
# Kernel INTERRUPT conformance: IRQ_SET, the dispatcher, and the two clocks.
#
# X816_Core doc/KERNEL.md section 8 test 8. Nine checks, one failure colour
# each -- see irqtest.c's header for what each of them establishes.
#
#   ./run-irq.sh              build and run
#   ./run-irq.sh --negative   break the dispatcher, to prove this can fail
#
# THE NEGATIVE CONTROL PATCHES THE CODE UNDER TEST, NOT THE TEST.
#
# doc/AUDIT.md and the libmem conversion both paid for this lesson: a negative
# control that edits the test's own expectation only proves the test can be
# made to fail, which nobody doubted. So this one edits runtime/kirq.s and
# makes the VSYNC path dispatch through the SPURIOUS slot instead of the VSYNC
# slot. The installed handler then never runs and check 3 must go BLUE.
#
# That particular break was chosen because it is SAFE and TARGETED. Safe: the
# spurious slot is empty, so the dispatcher takes its no-handler path rather
# than jumping somewhere random, and the machine stays up to paint a verdict.
# Targeted: the frame counter still advances, so checks 1 and 2 stay green --
# which is itself worth seeing, because a negative control that turns
# everything red would not show that check 3 is the one doing the work.
#
# Requires Pillow:  pip install pillow
set -u

# The toolchain, the memory map and the -O0 rule come from one place --
# runtime/calypsi.sh -- so this script cannot drift from the build that ships.
. "$(dirname "$0")/../../runtime/calypsi.sh"
cd "$(dirname "$0")"
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
WOUT=$(cygpath -m "$OUT" 2>/dev/null || echo "$OUT")

KIRQ=$RT/kirq.s
NEG=0
if [ "${1:-}" = "--negative" ]; then
    NEG=1
    sed 's/ldx     ##KIRQ_VSYNC \* KIRQ_SLOT_SIZE/ldx     ##KIRQ_SPURIOUS * KIRQ_SLOT_SIZE/' \
        "$RT/kirq.s" > "$OUT/kirq_neg.s"
    if ! grep -q 'KIRQ_SPURIOUS \* KIRQ_SLOT_SIZE' "$OUT/kirq_neg.s"; then
        echo "negative control: the patch did not apply -- kirq.s moved."      >&2
        echo "  Refusing to run: an unpatched 'negative' run paints green and" >&2
        echo "  proves nothing, which is exactly the trap this guard exists"   >&2
        echo "  for (doc/AUDIT.md, and run-libmem.sh's first version)."        >&2
        exit 1
    fi
    KIRQ="$OUT/kirq_neg.s"
    echo "negative control: expecting BLUE (check 3, an installed handler runs)"
fi

cc816 irqtest.c        "$OUT/t.o"       || exit 1
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
as816 "$KIRQ"          "$OUT/kirq.o"   -I "$RT" || exit 1
as816 irqhelp.s        "$OUT/irqhelp.o" -I "$RT" || exit 1

ln816 "$OUT/IRQTEST" "$OUT/hdr.o" "$OUT/t.o" "$OUT/kmem.o" "$OUT/kfs.o" \
      "$OUT/fat32.o" "$OUT/kexec.o" "$OUT/gosh.o" "$OUT/console.o" \
      "$OUT/font.o" "$OUT/smc.o" "$OUT/exec.o" "$OUT/fontcp.o" \
      "$OUT/tab.o" "$OUT/kcall.o" "$OUT/kirq.o" "$OUT/irqhelp.o" || exit 1
cp "$OUT/IRQTEST.raw" "$OUT/irqtest.bin" || exit 1

SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout -s KILL 120 \
    "$EMU/build/x16emu.exe" -boot "$(cygpath -m "$CORE/boot/boot.rom")" \
    -load "010000,$WOUT/irqtest.bin" \
    -warp -gif "$WOUT/out.gif" >/dev/null 2>&1

python - "$WOUT/out.gif" "$NEG" <<'PY'
import sys, collections
from PIL import Image, ImageFile
ImageFile.LOAD_TRUNCATED_IMAGES = True

gif, neg = sys.argv[1], sys.argv[2] == '1'

# The VERA default palette, doubled from 4 bits per channel to 8 -- the same
# indices irqtest.c paints. Keep these in step with the paint() calls there.
WHICH = {
    (0x00, 0xCC, 0x55): (0, 'green -- every check passed'),
    (0x88, 0x00, 0x00): (1, 'kirq_install installed the vectors and trampolines'),
    (0xEE, 0xEE, 0x77): (2, 'IRQ_FRAMES advances: VSYNC dispatched and acknowledged'),
    (0x00, 0x00, 0xAA): (3, 'a handler installed through IRQ_SET actually runs'),
    (0xCC, 0x44, 0xCC): (4, 'IRQ_SET reports the previous handler; clearing stops it'),
    (0xAA, 0xFF, 0xEE): (5, 'BRK dispatches to its slot and execution resumes'),
    (0xDD, 0x88, 0x55): (6, 'TIME_GET advances and agrees with the frame counter'),
    (0x66, 0x44, 0x00): (7, 'TIME_SET moves the epoch and the clock keeps running'),
    (0x77, 0x77, 0x77): (8, 'IRQ_SET refuses a slot that does not exist'),
    (0xFF, 0x77, 0x77): (9, 'the stuck-source defence disables an unhandled AFLOW'),
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
          'not reach its verdict. An interrupt source left unacknowledged '
          'livelocks the machine, and that is what this looks like.'
          % (frac * 100, top))
    sys.exit(1)

code, what = WHICH.get(top, (99, 'unrecognised colour %r' % (top,)))
if code != 0:
    print('FAIL: check %d -- %s' % (code, what))
    if neg and code != 3:
        print('  ...but the negative control was supposed to break check 3, '
              'not check %d. The patch hit something else.' % code)
        sys.exit(1)
    sys.exit(0 if neg else 1)
if neg:
    print('FAIL: the negative control painted GREEN, so check 3 proves nothing')
    sys.exit(1)
print('PASS: IRQ_SET, the dispatcher and both clocks -- nine checks, including '
      'a handler that stops when its slot is cleared and a millisecond counter '
      'cross-checked against VSYNC')
PY
