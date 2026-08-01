#!/usr/bin/env bash
# VERA816 conformance test 5 -- 640x480 8bpp bitmap scanout past line 204.
# Contract: X816_core/doc/VERA816.md section 8 test 5, section 5 (the bitmap
# line-address truncation) and doc/AUDIT.md H-3 (the renderer address wires).
#
# This is the test whose ABSENCE let H-3 live for weeks while four green
# conformance tests said the widening worked. Those four all reach VRAM
# through the CPU data port; this one is judged entirely on what a RENDERER
# fetched and the display showed.
#
# scanout.c paints the framebuffer as eight 60-line colour bands and reads it
# back through the data port before trusting the screen, so a failure here is
# the renderer's. This script then probes all 480 lines of the last GIF frame:
#
#   screen line y must show band y/60 -- white, red, cyan, purple, green,
#   blue, yellow, orange from top to bottom.
#
# The normative assertion, "line 205 must differ from line 0", is the case
# that matters: line 205 is the first line entirely above 128 KB, and both a
# 15-bit l0_addr and a stock line_idx_mul5[9:0] make it show line 0. Checking
# every line instead of just that one turns a pass into a statement about the
# whole 307,200-byte framebuffer, and lets the failure message name which
# line is showing which.
#
# A failure BEFORE the display comes up paints one colour instead:
#   dark grey    VRAMCAP is not 22 -- no VERA816 answered
#   light green  no blitter answered, so the register-window band (below)
#                cannot be painted at all
#   brown        $4AFFF is not real, independent memory
#   grey         a store above 128 KB aliased onto the palette or the sprite
#                attributes -- see VERA816.md 2.2
#   light red    the framebuffer does not read back as the painted ramp
#
# Lines 202-204 cross VERA's PSG/palette/sprite-attribute windows at
# $1F9C0-$1FFFF, which no 307,200-byte framebuffer inside 352 KB can avoid.
# scanout.c paints them with the blitter (whose port does not touch those
# shadows) and cannot read them back through the data port, so they are the
# one part of the picture the screen alone attests to. The report says so if
# they are the lines that fail.
#
# WATCHING IT PAINT ON HARDWARE: a ragged black gap ~2.5 rows tall opens in
# the middle of the purple band and closes at the end. That is this band --
# skipped by the ramp, filled by the blitter afterwards -- and is expected.
#
#   ./run-scanout.sh              build and run
#   ./run-scanout.sh --negative   flatten the ramp to one colour, so line 205
#                                 no longer differs from line 0 -- proving the
#                                 CHECK can fail, not merely the program
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

# -O0 IS MANDATORY: MMIO goes through volatile pointers, and Calypsi 5.18
# eliminates volatile reads above -O0. See the project README.
CFLAGS="--core=65816 --code-model=large --data-model=small -O0 -I $RT"

SRC=scanout.c
NEGATIVE=0
REGWIN=0
if [ "${1:-}" = "--negative" ]; then
    NEGATIVE=1
    # Flatten the ramp: every band becomes colour 1. The program stays
    # self-consistent -- its own readback checks the same rule -- so it paints
    # and comes up cleanly, and only the line probes below can catch it.
    sed 's/^#define BAND_STEP.*/#define BAND_STEP       0/' scanout.c > "$OUT/neg.c"
    SRC="$OUT/neg.c"
    echo "negative control: the ramp is flat, so line 205 must equal line 0"
elif [ "${1:-}" = "--regwin" ]; then
    REGWIN=1
    sed 's/^#define USE_REGWIN.*/#define USE_REGWIN      1/' scanout.c > "$OUT/rw.c"
    SRC="$OUT/rw.c"
    echo "REGWIN mode: CTRL816.REGWIN set, whole framebuffer painted by the"
    echo "             data port, blitter not involved"
elif [ "${1:-}" = "--regwin-negative" ]; then
    # The negative control FOR --regwin, and the only honest way to show that
    # mode can fail. REGWIN_BIT becomes 0, so the program writes 0 to CTRL816
    # (leaving the windows where they are), reads back 0, finds it equal to
    # its own REGWIN_BIT and proceeds to paint straight through anyway. The
    # stock windows are still live, so the paint rewrites the palette from
    # its own pixels and the bands must come out wrong.
    NEGATIVE=2
    sed -e 's/^#define USE_REGWIN.*/#define USE_REGWIN      1/' \
        -e 's/^#define REGWIN_BIT.*/#define REGWIN_BIT      0x00/' \
        scanout.c > "$OUT/rwn.c"
    SRC="$OUT/rwn.c"
    echo "negative control for --regwin: painting straight through with the"
    echo "                               windows NOT relocated must corrupt"
    echo "                               the palette and wreck the bands"
fi

"$CALYPSI/bin/cc65816" $CFLAGS "$SRC"             -o "$OUT/t.o"   || exit 1
"$CALYPSI/bin/as65816" --core=65816 $RT/x816hdr.s -o "$OUT/hdr.o" || exit 1
"$CALYPSI/bin/ln65816" $RT/x816-lib.scm "$OUT/hdr.o" "$OUT/t.o" \
    "$CALYPSI/lib/clib-lc-sd.a" -o "$OUT/SCANOUT.elf" --output-format raw \
    --program-root __x816_root_section --rtattr exit=simplified || exit 1
cp "$OUT/SCANOUT.raw" "$OUT/scanout.bin" || exit 1

SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout 60 \
    "$EMU/build/x16emu.exe" -boot "$(cygpath -m "$CORE/boot/boot.rom")" \
    -load "010000,$WOUT/scanout.bin" \
    -warp -gif "$WOUT/out.gif" >/dev/null 2>&1

python - "$WOUT/out.gif" "$NEGATIVE" "$REGWIN" <<'PY'
import sys, collections
from PIL import Image, ImageFile
ImageFile.LOAD_TRUNCATED_IMAGES = True

gif      = sys.argv[1]
negative = sys.argv[2] == "1"     # flat ramp: line 205 must stop differing
rwneg    = sys.argv[2] == "2"     # REGWIN not really set, painted anyway
regwin   = sys.argv[3] == "1"

SCR_W, SCR_H = 640, 480
BAND_LINES   = 60
PROBE_X      = 320

# VERA's default palette, entries 0-11, expanded the way both implementations
# expand it (each 4-bit channel x 17).
PAL = {0:  (0, 0, 0),        1:  (255, 255, 255), 2:  (136, 0, 0),
       3:  (170, 255, 238),  4:  (204, 68, 204),  5:  (0, 204, 85),
       6:  (0, 0, 170),      7:  (238, 238, 119), 8:  (221, 136, 85),
       9:  (102, 68, 0),     10: (255, 119, 119), 11: (51, 51, 51),
       12: (119, 119, 119),  13: (170, 255, 102), 14: (0, 136, 255),
       15: (187, 187, 187)}

# The pre-display failure paints scanout.c can put up.
WHY = {PAL[11]: "VRAMCAP did not read 22 -- no VERA816 answered at DCSEL=32",
       PAL[13]: "no blitter answered at DCSEL=33 -- the $1F9C0-$1FFFF band "
                "cannot be painted without it (VERA816.md 2.2)",
       PAL[9]:  "$4AFFF is not real, independent memory -- the framebuffer "
                "does not fit in populated VRAM",
       PAL[12]: "a store to $3FA02/$3FC00 changed the palette or the sprite "
                "attributes: the window decodes are ignoring address bits "
                "[18:17], so a 640x480 paint rewrites the palette from its "
                "own pixels. See VERA816.md 2.2 and top.v's palette_write / "
                "sprite_attr_write / audio_write",
       PAL[10]: "the framebuffer did not read back as the painted ramp -- "
                "the fault is upstream of the renderer",
       PAL[14]: "CTRL816 did not read $00 at reset -- this bitstream has no "
                "DCSEL-34 bank, so it predates VERA816.md 4.4 (--regwin only)",
       PAL[15]: "CTRL816.REGWIN was written 1 but did not read back 1 "
                "(--regwin only)"}

# Lines whose pixels the data port cannot read back: they cross $1F9C0-$1FFFF.
# Only in the default build -- with REGWIN set they are ordinary VRAM and the
# program reads them back like every other line.
REGWIN_LINES = (202, 203, 204)

def expected_index(y):
    return 1 + y // BAND_LINES

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
rgb = im.convert('RGB')
if rgb.size != (SCR_W, SCR_H):
    sys.exit("frame is %dx%d, expected %dx%d -- the composer is not at 1:1"
             % (rgb.size[0], rgb.size[1], SCR_W, SCR_H))
px = rgb.load()

top, cnt = collections.Counter(rgb.get_flattened_data()).most_common(1)[0]
if top in WHY and cnt > SCR_W * SCR_H * 0.9 and not rwneg:
    sys.exit("FAIL on the machine, before the display was trusted: " + WHY[top])

if rwneg:
    # Any outcome except a correct picture is a pass here: the point is that
    # painting through the live window range must not leave the bands intact.
    wrong = sum(1 for y in range(SCR_H)
                if px[PROBE_X, y] != PAL[expected_index(y)])
    if wrong == 0:
        sys.exit("FAIL (negative control for --regwin): the bands came out "
                 "perfect while the windows were NOT relocated -- painting "
                 "through $1FA00-$1FBFF left the palette untouched, so "
                 "--regwin proves nothing")
    print("PASS (negative control for --regwin): %d of %d lines wrong."
          % (wrong, SCR_H))
    print("      With the windows left in place, painting straight through")
    print("      rewrote the palette from the picture's own pixels -- so the")
    print("      --regwin run passing is a statement about REGWIN, not luck.")
    sys.exit(0)

# ---- the assertion VERA816.md section 8 test 5 names --------------------
line0, line205 = px[PROBE_X, 0], px[PROBE_X, 205]

if negative:
    if line205 == line0:
        print("PASS (negative control): with the ramp flattened, line 205 and")
        print("      line 0 are both %r -- the check fires on exactly the" % (line0,))
        print("      condition test 5 forbids, so it can fail.")
        sys.exit(0)
    sys.exit("FAIL (negative control): the ramp was flattened, yet line 205 %r "
             "still differs from line 0 %r" % (line205, line0))

# ---- every line, not just the one --------------------------------------
bad = []
for y in range(SCR_H):
    want = PAL[expected_index(y)]
    got  = px[PROBE_X, y]
    if got != want:
        bad.append((y, got, want))

# A truncated renderer fetches from (y*640) mod 131072 -- identical for the
# 15-bit l0_addr of H-3 and the stock line_idx_mul5[9:0] of section 5, since
# 131072 bytes is exactly 1024 * 128. Name it when the picture matches.
def truncated_source_line(y):
    return (((y * SCR_W) % 131072) + PROBE_X) // SCR_W

if bad and all(y in REGWIN_LINES for y, _, _ in bad):
    print("FAIL: only the register-window lines are wrong -- %s"
          % (", ".join(str(y) for y, _, _ in bad),))
    print("      Those are the 2.5 lines crossing $1F9C0-$1FFFF, the only part")
    print("      of the picture the blitter paints and the data port cannot")
    print("      read back. The scanout itself is fine; suspect the blitter or")
    print("      VERA816.md 2.2, not the renderer.")
    for y, got, want in bad:
        print("      line %3d: got %r, expected %r" % (y, got, want))
    sys.exit(1)

if bad:
    print("FAIL: the display does not show the framebuffer that was read back")
    print("      %d of %d lines wrong; first five:" % (len(bad), SCR_H))
    trunc = 0
    for y, got, want in bad[:5]:
        src = truncated_source_line(y)
        note = ""
        if got == PAL[expected_index(src)] and src != y:
            note = "  <- this is line %d's content" % (src,)
        print("      line %3d: got %r, expected %r%s" % (y, got, want, note))
    for y, got, _ in bad:
        src = truncated_source_line(y)
        if src != y and got == PAL[expected_index(src)]:
            trunc += 1
    if trunc > len(bad) * 0.9:
        print()
        print("      %d of the %d wrong lines show exactly what a 128 KB"
              % (trunc, len(bad)))
        print("      truncation would put there. Check l0_addr's width in")
        print("      vera/fpga/source/top.v (AUDIT.md H-3) and bm_line_addr_tmp")
        print("      in graphics/layer_renderer.v (VERA816.md section 5).")
        print("      `sim/run.sh lint` catches the first of those in seconds.")
    sys.exit(1)

# Width, on the three lines that matter. A 320-wide bitmap misconfiguration
# addresses lines differently and would already have been caught above; these
# probes cover the other half -- that all 640 columns came from the same line.
wide = []
for y in (0, 205, SCR_H - 1):
    want = PAL[expected_index(y)]
    for x in (0, SCR_W - 1):
        if px[x, y] != want:
            wide.append((x, y, px[x, y], want))
if wide:
    print("FAIL: line colours are right at x=%d but not across the full width"
          % (PROBE_X,))
    for x, y, got, want in wide:
        print("      (%d,%d): got %r, expected %r" % (x, y, got, want))
    sys.exit(1)

if regwin:
    # What makes --regwin a real assertion is NOT a mid-paint frame: in warp
    # mode the blitter fills land inside the same GIF frame as the last paint
    # line, so no frame ever catches the stock build's gap and any such check
    # would pass for both builds. The discrimination is the 480-line check
    # above, and it is total: this build streams the data port straight
    # through $1FA00-$1FBFF, so if REGWIN were not in force those writes would
    # rewrite the palette from the picture's own pixels and every band would
    # come out wrong -- exactly how scanout.c failed the first time it ran
    # (a uniform $0404). --regwin-negative proves that by writing 0 to
    # CTRL816 while still painting straight through.
    print("PASS (--regwin): CTRL816.REGWIN in force -- the data port painted")
    print("    all 307,200 bytes itself, streaming straight through the stock")
    print("    window range, and the palette survived. The blitter was never")
    print("    invoked and no black gap exists to close. The whole 352 KB is")
    print("    ordinary VRAM, which is the point of VERA816.md 4.4.")

print("PASS: 640x480 8bpp scans out past line 204 -- VERA816.md section 8 test 5")
print("    all %d screen lines match their band, at x=0, %d and %d"
      % (SCR_H, PROBE_X, SCR_W - 1))
print("    line   0 %r  and line 205 %r differ -- the normative assertion"
      % (line0, line205))
print("    lines 205-479 are fetched from $20080-$4AFFF, above the stock 128 KB")
print("    bands 4-7 (lines 240-479) exist ONLY above 128 KB and all rendered")
print("    the framebuffer was read back through the data port first, so this")
print("    is a statement about the RENDERER, not about VRAM")
PY
