#!/usr/bin/env bash
# What does executing from SDRAM cost? One workload, run twice: in place in
# bank $01 (SDRAM) and from a copy in bank $00 (BRAM). Same bytes, same data.
#
#     ./run-bankbench.sh
#
# IN THE EMULATOR THIS MEASURES NOTHING AND WILL REPORT 1.00x. Its memory is
# uniform; only the board has two kinds. The run here is a correctness check
# -- that the copy is position independent and both runs produce the same
# answer -- not a measurement. BANKBENCH.BIN on the card is the measurement.
#
# The number matters beyond "fast programs": doc/VERA_MEMORY_REVIEW.md 1.3
# found the gaming bottleneck is FILL RATE, one `sta` per pixel, which is a
# CPU-throughput limit -- so this multiplier applies to it directly. And 3
# deferred VERA2 partly on "CPU-from-SDRAM contention", which moving code to
# BRAM would remove.
#
# Requires Pillow:  pip install pillow
set -u

. "$(dirname "$0")/../../runtime/calypsi.sh"
cd "$(dirname "$0")"
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
WOUT=$(cygpath -m "$OUT" 2>/dev/null || echo "$OUT")

as816 bankbench.s      "$OUT/t.o"       -I "$X16LIB" -I "$RT" || exit 1
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
as816 $RT/kirq.s      "$OUT/kirq.o"   -I "$RT" || exit 1

ln816 "$OUT/BANKBENCH" "$OUT/hdr.o" "$OUT/t.o" "$OUT/kmem.o" "$OUT/kfs.o" \
      "$OUT/fat32.o" "$OUT/kexec.o" "$OUT/gosh.o" "$OUT/console.o" \
      "$OUT/font.o" "$OUT/smc.o" "$OUT/exec.o" "$OUT/fontcp.o" \
      "$OUT/tab.o" "$OUT/kirq.o" || exit 1
cp "$OUT/BANKBENCH.raw" "$OUT/bankbench.bin" || exit 1

SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout -s KILL 300 \
    "$EMU/build/x16emu.exe" -boot "$(cygpath -m "$CORE/boot/boot.rom")" \
    -load "010000,$WOUT/bankbench.bin" \
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
# The six results are printed UNLABELLED in a fixed order -- bankbench.s's
# header says why the labels were given up on. The order is the contract.
# The third and sixth rows are MVN with its instruction in the bank-$01 CODE
# section -- i.e. in SDRAM. That is NOT a reference the library should match:
# MVN re-fetches its own three instruction bytes for every byte it moves, and
# the library's stub lives in bank $00 BRAM, so on hardware the library is 2.5x
# FASTER than this row. Named for what they actually measure. doc/AUDIT.md 6.2.
ORDER = ['SDRAM (bank $01, in place)', 'BRAM  (bank $00, copied)']
# Four values, in order: time of run 1, its bank, time of run 2, its bank.
nums = re.findall(r'^\s*([0-9A-F]{4})\s*$', blob, re.M)
if len(nums) < 4:
    print('could not read four values off the screen; got %r' % nums)
    print('--- screen ---')
    print(blob)
    sys.exit(1)
sd, b1, br, b2 = int(nums[0], 16), nums[1], int(nums[2], 16), nums[3]

# The banks are the correctness check, and they matter MORE than the times.
# Equal times prove nothing on their own -- a copy that jumped back into bank
# $01 would produce them too, and would go on doing so on hardware, where it
# would read as "SDRAM costs nothing".
if b1 != '0001' or b2 != '0000':
    print('FAIL: the two runs executed in banks %s and %s, wanted 0001 and '
          '0000. The bank-$00 copy is not running where it should, so any '
          'timing below is meaningless.' % (b1, b2))
    sys.exit(1)
print('banks: run 1 in $01 (SDRAM), run 2 in $00 (BRAM) -- as intended')
print()

print('bankbench: the same loop fetched from each memory')
print()
print('    %-28s %6d ms' % (ORDER[0], sd))
print('    %-28s %6d ms' % (ORDER[1], br))
print()
if br:
    print('    BRAM is %.2fx faster' % (sd / float(br)))
if abs(sd - br) <= max(2, sd // 50):
    print()
    print('    (equal, as expected in the emulator -- uniform memory.')
    print('     Run BANKBENCH.BIN on the board for the real figure.)')
PY
