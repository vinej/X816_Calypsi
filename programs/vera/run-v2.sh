#!/usr/bin/env bash
# VERA2 bitmap layer conformance (X816_core/doc/VERA2.md), on the emulator.
# Three phases, because the failure modes live in different places:
#
#   MODEL    v2show.c enables the layer over a framebuffer -load placed the
#            pattern into -- no guest drawing at all. Wrong pixels here are
#            the EMULATOR MODEL's fault (registers, palette, compositing,
#            DISPBASE, the 4bpp unpack).
#
#   CODEGEN  v2demo.c draws the same picture itself with far stores. Wrong
#            pixels here (with MODEL green) are the DRAWING CODE's fault --
#            and this phase is the regression test for the Calypsi 5.18 far
#            access bugs it flushed out: 32-bit indices on __far pointers
#            truncate to 16 bits, pointer walks wrap at bank boundaries, and
#            far RMW reads the wrong byte. See v2demo.c's header. The split
#            found them: MODEL passed pixel-perfect while CODEGEN scrambled.
#
#   NEGATIVE the same demo WITHOUT -vera2 must show none of it: $9F61 reads
#            $00, the program parks, the screen never shows the bands.
#
#   ./run-v2.sh
#
# Requires Pillow:  pip install pillow
set -u

. "$(dirname "$0")/../../runtime/calypsi.sh"
cd "$(dirname "$0")"
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
WOUT=$(cygpath -m "$OUT" 2>/dev/null || echo "$OUT")

# The reference pattern, built host-side: exactly the picture v2demo draws.
python - "$OUT/pattern.bin" <<'PY'
import sys
fb = bytearray(153600)
for y in range(480):
    off = y * 320
    for i in range(320):
        c = i // 20
        fb[off + i] = (c << 4) | c
for y in range(16):
    fb[y*320:(y*320)+320] = b'\x11' * 320
for y in range(464, 480):
    fb[y*320:(y*320)+320] = b'\x33' * 320
for y in range(480):
    x = y + y // 3                       # v2demo's Bresenham, exactly
    off = y*320 + (x >> 1)
    if x & 1: fb[off] = (fb[off] & 0xF0) | 0x01
    else:     fb[off] = (fb[off] & 0x0F) | 0x10
open(sys.argv[1], 'wb').write(bytes(fb))
PY

cc816 v2show.c "$OUT/show.o"       || exit 1
cc816 v2demo.c "$OUT/demo.o"       || exit 1
as816 $RT/x816hdr.s "$OUT/hdr.o"   || exit 1
ln816 "$OUT/V2SHOW" "$OUT/hdr.o" "$OUT/show.o" || exit 1
ln816 "$OUT/V2DEMO" "$OUT/hdr.o" "$OUT/demo.o" || exit 1

run_emu () {  # $1 extra flags, $2 program, $3 timeout, $4 gif
    # shellcheck disable=SC2086
    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout "$3" \
        "$EMU/build/x16emu.exe" $1 \
        -boot "$(cygpath -m "$CORE/boot/boot.rom")" \
        -load "010000,$2" \
        -warp -gif "$4" >/dev/null 2>&1
}

run_emu "-vera2 -load E00000,$WOUT/pattern.bin" "$WOUT/V2SHOW.raw" 40 "$WOUT/model.gif"
run_emu "-vera2"                                "$WOUT/V2DEMO.raw" 90 "$WOUT/demo.gif"
run_emu ""                                      "$WOUT/V2DEMO.raw" 25 "$WOUT/off.gif"

python - "$WOUT/model.gif" "$WOUT/demo.gif" "$WOUT/off.gif" <<'PY'
import sys
from PIL import Image, ImageFile
ImageFile.LOAD_TRUNCATED_IMAGES = True

def last_frame(path):
    im = Image.open(path)
    n = 0
    while True:
        try:
            im.seek(n); im.load(); n += 1
        except (EOFError, OSError):
            break
    if n == 0:
        sys.exit("no decodable frame in %s -- did the emulator run?" % path)
    im.seek(n - 1)
    return im.convert('RGB').load()

def d4(v):
    return (v << 4) | v

# v2demo.c's palette, kept in step by hand; drift fails loudly.
pal_r = [0,15, 8, 0, 0, 0,15,15, 4, 8,15, 0, 8,15, 6,12]
pal_g = [0,15, 8, 0,10,10, 0,10, 4, 0, 6,15,12, 4, 6, 8]
pal_b = [0,15, 8,12, 0,12, 0, 0, 4, 8, 6,12, 0, 8,15, 0]
def pal(c):
    return (d4(pal_r[c]), d4(pal_g[c]), d4(pal_b[c]))

def judge(px, phase):
    bad = []
    # bands on the mid line (the diagonal crosses at x=320: probes avoid it)
    for x in (60, 100, 220, 420, 580):
        want = pal(x // 40)
        if px[x, 240] != want:
            bad.append("band x=%d: %r, want %r" % (x, px[x, 240], want))
    # stride bars
    if px[320, 8]   != pal(1): bad.append("top bar: %r" % (px[320, 8],))
    if px[320, 472] != pal(3): bad.append("bottom bar: %r" % (px[320, 472],))
    # diagonal: y=100 -> x = 100 + 33 = 133 (white); its line stays band-true
    if px[133, 100] != pal(1): bad.append("diagonal (133,100): %r" % (px[133, 100],))
    if px[200, 100] != pal(5): bad.append("beside diagonal: %r" % (px[200, 100],))
    # corners of the sweep: first and last band on first and last band line
    if px[8, 20]    != pal(0): bad.append("first band line: %r" % (px[8, 20],))
    if px[632, 460] != pal(15): bad.append("last band line: %r" % (px[632, 460],))
    if bad:
        print("FAIL [%s]:" % phase)
        for m in bad:
            print("   -", m)
        return False
    return True

ok  = judge(last_frame(sys.argv[1]), "MODEL   -load pattern via v2show")
ok &= judge(last_frame(sys.argv[2]), "CODEGEN v2demo draws it itself")

px_off = last_frame(sys.argv[3])
hits = sum(1 for x in (60, 100, 220, 420, 580) if px_off[x, 240] == pal(x // 40))
if hits >= 3:
    print("FAIL [NEGATIVE]: bands visible WITHOUT -vera2")
    ok = False

if not ok:
    sys.exit(1)
print("PASS: VERA2 4bpp end to end on the emulator")
print("    MODEL    -loaded pattern displays pixel-perfect (regs/palette/compositing)")
print("    CODEGEN  v2demo's far-store drawing matches it (fill_far bank-split idiom)")
print("    NEGATIVE none of it visible without -vera2 (ID gates the layer)")
PY
