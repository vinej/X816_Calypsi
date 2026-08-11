#!/usr/bin/env bash
# How fast can this machine move memory? Three implementations of mem_copy and
# three of mem_fill, timed against the kernel's millisecond clock.
#
#     ./run-membench.sh
#
# It began as a MEASUREMENT, to settle whether rewriting x16lib's block-move
# primitives was worth doing. It was (96.1 -> 7.0 cycles/byte for copy), the
# rewrite landed, and the table's job changed with it: LIBCOPY and LIBFILL are
# now the library's own MVN path, and MVNCOPY/MVNFILL are a raw reference
# implementation of the same instruction. THE TWO SHOULD MATCH.
#
# So this is now a REGRESSION GUARD, and read it that way: if a library row
# drifts back toward the W16 row, something has put a loop back in the path --
# most likely the I/O-page carve-out catching a case it should not, since that
# is the one branch that still runs a byte at a time.
#
# THE TWO MVN ROWS ARE NOT A TARGET. They run MVN with its instruction in the
# bank-$01 code section, i.e. in SDRAM, and MVN re-fetches its own three
# instruction bytes for every byte moved. The library's stub is in bank $00
# BRAM, so on real hardware the library is 2.5x FASTER than those rows -- 12.1
# against 30.0 cycles/byte, measured on a DE10-Nano. In the emulator, whose
# memory is uniform, all four MVN figures come out identical and the effect is
# invisible. doc/AUDIT.md 6.2 has the arithmetic.
#
# It can exist at all only because TIME_GET gives a program a timebase --
# before the $9F90 counter there was nothing on this machine to measure with.
#
# What is being measured is INSTRUCTION cost: the emulator's memory is uniform,
# so SDRAM wait states are not in these numbers. On hardware every figure grows
# and the ordering should not change, since all six variants touch the same
# bytes the same number of times. membench.bin is not shipped on the card; if
# the hardware figure ever matters, add it there the way IRQTEST is.
set -u

. "$(dirname "$0")/../../runtime/calypsi.sh"
cd "$(dirname "$0")"
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
WOUT=$(cygpath -m "$OUT" 2>/dev/null || echo "$OUT")

as816 membench.s      "$OUT/t.o"       -I "$X16LIB" -I "$RT" || exit 1
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
as816 $RT/kirq.s      "$OUT/kirq.o"   -I "$RT" || exit 1

ln816 "$OUT/MEMBENCH" "$OUT/hdr.o" "$OUT/t.o" "$OUT/kmem.o" "$OUT/kfs.o" \
      "$OUT/fat32.o" "$OUT/kexec.o" "$OUT/gosh.o" "$OUT/console.o" "$OUT/ccur.o" \
      "$OUT/font.o" "$OUT/smc.o" "$OUT/exec.o" "$OUT/fontcp.o" \
      "$OUT/tab.o" "$OUT/kirq.o" || exit 1
cp "$OUT/MEMBENCH.raw" "$OUT/membench.bin" || exit 1

SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout -s KILL 300 \
    "$EMU/build/x16emu.exe" -boot "$(cygpath -m "$CORE/boot/boot.rom")" \
    -load "010000,$WOUT/membench.bin" \
    -warp -gif "$WOUT/out.gif" >/dev/null 2>&1

# The screen is read back by matching 8x8 cells against the font the console
# draws with -- the same trick run-fwboot.sh uses to read the shell's banner.
python - "$WOUT/out.gif" "$RT/font_cp437.s" <<'PY'
import sys, re, io
from PIL import Image, ImageFile
ImageFile.LOAD_TRUNCATED_IMAGES = True

gif, fontinc = sys.argv[1], sys.argv[2]

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
    except Exception:
        break
if n == 0:
    sys.exit('no decodable frame -- did the emulator run?')
im.seek(n - 1)
px = im.convert('RGB').load()
w, h = im.size

bg = px[0, 0]
text = []
for row in range(h // 8):
    line = ''
    for col in range(w // 8):
        bits = []
        for y in range(8):
            b = 0
            for x in range(8):
                if px[col * 8 + x, row * 8 + y] != bg:
                    b |= 0x80 >> x
            bits.append(b)
        t = tuple(bits)
        line += ' ' if not any(t) else glyph.get(t, '?')
    text.append(line.rstrip())

blob = '\n'.join(text)
# The six results are printed UNLABELLED in a fixed order -- membench.s's
# header says why the labels were given up on. The order is the contract.
# The third and sixth rows are MVN with its instruction in the bank-$01 CODE
# section -- i.e. in SDRAM. That is NOT a reference the library should match:
# MVN re-fetches its own three instruction bytes for every byte it moves, and
# the library's stub lives in bank $00 BRAM, so on hardware the library is 2.5x
# FASTER than this row. Named for what they actually measure. doc/AUDIT.md 6.2.
ORDER  = ['LIBCOPY', 'W16COPY', 'MVNCOPY', 'LIBFILL', 'W16FILL', 'MVNFILL']
LABEL  = {'LIBCOPY': 'library (MVN, stub in BRAM)',
          'W16COPY': '16-bit word loop  (SDRAM)',
          'MVNCOPY': 'MVN, stub in SDRAM',
          'LIBFILL': 'library (MVN, stub in BRAM)',
          'W16FILL': '16-bit word loop  (SDRAM)',
          'MVNFILL': 'MVN fill, stub in SDRAM'}
nums = re.findall(r'^\s*([0-9A-F]{4})\s*$', blob, re.M)
if len(nums) < 6:
    print('could not read six results off the screen; got %r' % nums)
    print('--- screen ---')
    print(blob)
    sys.exit(1)
found = dict(zip(ORDER, nums[:6]))

ITER, LEN, MHZ = 4, 0x8000, 8
total = ITER * LEN

def show(title, keys):
    print('  %s' % title)
    for k in keys:
        ms = int(found[k], 16)
        cyc = ms * MHZ * 1000.0 / total
        print('    %-28s %6d ms   %5.1f cycles/byte'
              % (LABEL[k], ms, cyc))

print('membench: %d x %d KB, 65816 at %d MHz (instruction cost; no SDRAM waits)'
      % (ITER, LEN // 1024, MHZ))
print()
show('copy', ['LIBCOPY', 'W16COPY', 'MVNCOPY'])
print()
show('fill', ['LIBFILL', 'W16FILL', 'MVNFILL'])
PY
