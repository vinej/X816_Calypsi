#!/usr/bin/env bash
# FAT32 WRITE conformance: X816 writes, pyfatfs checks.
#
# The direction is the point. A writer verified with our own reader proves only
# that the two agree with each other; both could be wrong in the same way. So
# X816 writes into a scratch image and an INDEPENDENT implementation reads it
# back and compares. The reader was brought up the same way round, against an
# image pyfatfs wrote.
#
# On top of the on-screen result this checks, from the host:
#   * the files exist with the right names and sizes
#   * their CONTENTS match byte for byte
#   * a truncated file is actually short, not merely reported short
#   * an unlinked file is gone
#   * the pre-existing files are untouched -- writing must not disturb them
#
#   ./run-write.sh              build and run
#   ./run-write.sh --negative   corrupt the expectation, to prove it can fail
#
# Requires: pip install pillow pyfatfs
set -u

CALYPSI=${CALYPSI:-../../Calypsi/calypsi-65816-5.18}
EMU=${EMU:-/c/quartus/projects/X816_Emulator}
CORE=${CORE:-/c/quartus/projects/X816_core}
RT=../../runtime
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
WOUT=$(cygpath -m "$OUT" 2>/dev/null || echo "$OUT")

CFLAGS="--core=65816 --code-model=large --data-model=small -O0 -I $RT"

"$CALYPSI/bin/cc65816" $CFLAGS fwtest.c    -o "$OUT/t.o"     || exit 1
"$CALYPSI/bin/cc65816" $CFLAGS $RT/fat32.c -o "$OUT/fat32.o" || exit 1
"$CALYPSI/bin/as65816" --core=65816 $RT/x816hdr.s -o "$OUT/hdr.o" || exit 1
"$CALYPSI/bin/ln65816" $RT/x816-lib.scm "$OUT/hdr.o" "$OUT/t.o" "$OUT/fat32.o" \
    "$CALYPSI/lib/clib-lc-sd.a" -o "$OUT/FWTEST.elf" --output-format raw \
    --program-root __x816_root_section --rtattr exit=simplified || exit 1
cp "$OUT/FWTEST.raw" "$OUT/fwtest.bin" || exit 1

# A SCRATCH copy: the test mutates it, and a conformance image that changes
# every run is no longer a fixed reference.
cp "$CORE/boot/fat32.img" "$OUT/scratch.img" || exit 1

SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout 240 \
    "$EMU/build/x16emu.exe" -boot "$(cygpath -m "$CORE/boot/boot.rom")" \
    -sdcard "$WOUT/scratch.img" \
    -load "010000,$WOUT/fwtest.bin" \
    -warp -gif "$WOUT/out.gif" >/dev/null 2>&1

NEG=0
[ "${1:-}" = "--negative" ] && NEG=1 && echo "negative control: expecting FAIL"

python - "$WOUT/out.gif" "$WOUT/scratch.img" "$NEG" <<'PY'
import sys, collections
from PIL import Image, ImageFile
ImageFile.LOAD_TRUNCATED_IMAGES = True

gif, img, neg = sys.argv[1], sys.argv[2], sys.argv[3] == '1'

# ---- 1. what the machine itself reported --------------------------------
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
top, cnt = collections.Counter(rgb.get_flattened_data()).most_common(1)[0]

WHY = {(0, 204, 85):     None,                 # green: all passed
       (136, 0, 0):      "test 1 -- create + write + close",
       (238, 238, 119):  "test 2 -- a file spanning clusters",
       (0, 0, 170):      "test 3 -- truncate",
       (204, 68, 204):   "test 4 -- read back through our own reader",
       (170, 255, 238):  "test 5 -- unlink",
       (255, 255, 255):  "mount failed"}

if top not in WHY:
    sys.exit("unrecognised screen colour %r -- the test did not finish" % (top,))
if WHY[top]:
    sys.exit("FAIL on the machine: " + WHY[top])

# ---- 2. what an INDEPENDENT implementation sees -------------------------
from pyfatfs.PyFatFS import PyFatFS

def pattern(i):
    return (i * 7 + 13) & 0xFF

fs = PyFatFS(img)
bad = []

def read(path):
    with fs.open(path, "rb") as h:
        return h.read()

want_short = b"SHORT\n" if not neg else b"THIS IS NOT WHAT WAS WRITTEN\n"

try:
    got = read("/WROTE.TXT")
    if got != want_short:
        bad.append("/WROTE.TXT is %r, expected %r -- truncate left the old tail?"
                   % (got, want_short))
except Exception as e:
    bad.append("/WROTE.TXT unreadable by pyfatfs: %s" % e)

try:
    got = read("/BIGW.BIN")
    if len(got) != 3000:
        bad.append("/BIGW.BIN is %d bytes, expected 3000" % len(got))
    else:
        for i, b in enumerate(got):
            if b != pattern(i):
                bad.append("/BIGW.BIN differs at byte %d: %02X, expected %02X"
                           % (i, b, pattern(i)))
                break
except Exception as e:
    bad.append("/BIGW.BIN unreadable by pyfatfs: %s" % e)

if fs.exists("/GONE.TXT"):
    bad.append("/GONE.TXT still exists after unlink")

# Pre-existing files must be untouched: allocating clusters for new data must
# not wander into someone else's chain.
try:
    if read("/HELLO.TXT") != b"Hello from FAT32 on X816!\n":
        bad.append("/HELLO.TXT was damaged by writing")
    if len(read("/BIG.BIN")) != 20000:
        bad.append("/BIG.BIN was damaged by writing")
except Exception as e:
    bad.append("pre-existing files unreadable after writing: %s" % e)

fs.close()

if bad:
    print("FAIL: pyfatfs disagrees with what X816 wrote")
    for b in bad:
        print("   -", b)
    sys.exit(1)

print("PASS: X816 wrote it, pyfatfs read it back, and they agree")
print("    /WROTE.TXT truncated correctly")
print("    /BIGW.BIN  3000 bytes across a cluster chain, byte-exact")
print("    /GONE.TXT  unlinked")
print("    pre-existing files untouched")
PY
