#!/usr/bin/env bash
# Kernel FILESYSTEM conformance: everything through $00:FE00, on a real card.
#
# Two independent verdicts, and both have to agree:
#
#   on screen   kfstest.c walks the fifteen filesystem entries and paints green
#               only if every one behaved -- including the refusals
#   on the host pyfatfs opens the card afterwards and checks what is actually
#               there, because a program that verifies its own writes with its
#               own reads proves only that the two agree with each other
#
# The host side checks that /KEEP.TXT exists with the right length and exact
# contents, and that /KT is GONE -- the test creates it, fills it, renames
# inside it and removes it, so a leftover /KT means a delete silently failed
# while reporting success.
#
#   ./run-kfs.sh              build and run
#   ./run-kfs.sh --negative   corrupt the expectation, to prove it can fail
#
# Requires: pip install pillow pyfatfs
set -u

# The toolchain, the memory map and the -O0 rule come from one place --
# runtime/calypsi.sh -- so this script cannot drift from the build that ships.
# It also sets EMU, CORE, RT and X16LIB, and cc816 refuses -O1+ silently.
. "$(dirname "$0")/../../runtime/calypsi.sh"
cd "$(dirname "$0")"
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
WOUT=$(cygpath -m "$OUT" 2>/dev/null || echo "$OUT")

cd "$(dirname "$0")"

cc816 kfstest.c "$OUT/t.o"       || exit 1
cc816 $RT/fat32.c "$OUT/fat32.o"   || exit 1
cc816 $RT/kfs.c "$OUT/kfs.o"     || exit 1
cc816 $RT/goshell.c "$OUT/gosh.o"    || exit 1
cc816 $RT/console.c "$OUT/console.o" || exit 1
cc816 $RT/font8x8.c "$OUT/font.o"    || exit 1
as816 $RT/x816hdr.s "$OUT/hdr.o"    || exit 1
as816 $RT/smc.s "$OUT/smc.o"    || exit 1
as816 $RT/exec.s "$OUT/exec.o"   || exit 1
as816 $RT/font_cp437.s "$OUT/fontcp.o" || exit 1
as816 $RT/kerntab.s "$OUT/tab.o"    || exit 1
# kerntab.s's generated table names the four interrupt/clock entries (IRQ_SET,
# TIME_GET, TIME_SET, IRQ_FRAMES), so every image that links the table now
# links kirq.s behind it.
as816 $RT/kirq.s      "$OUT/kirq.o"   -I "$RT" || exit 1
cc816 $RT/kexec.c "$OUT/kexec.o"   || exit 1
cc816 $RT/kmem.c  "$OUT/kmem.o"    || exit 1
as816 $RT/kcall.s "$OUT/kcall.o"  || exit 1

ln816 "$OUT/KFSTEST" "$OUT/hdr.o" "$OUT/t.o" "$OUT/fat32.o" "$OUT/kfs.o" "$OUT/kexec.o" "$OUT/kmem.o" "$OUT/gosh.o" "$OUT/console.o" "$OUT/font.o" "$OUT/smc.o" "$OUT/exec.o" "$OUT/fontcp.o" "$OUT/tab.o" "$OUT/kcall.o" "$OUT/kirq.o" || exit 1
cp "$OUT/KFSTEST.raw" "$OUT/kfstest.bin" || exit 1

# A SCRATCH copy: the test mutates it, and a conformance image that changes
# every run is no longer a fixed reference.
cp "$CORE/boot/fat32.img" "$OUT/scratch.img" || exit 1

SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout -s KILL 60 \
    "$EMU/build/x16emu.exe" -boot "$(cygpath -m "$CORE/boot/boot.rom")" \
    -sdcard "$WOUT/scratch.img" \
    -load "010000,$WOUT/kfstest.bin" \
    -warp -gif "$WOUT/out.gif" >/dev/null 2>&1

NEG=0
[ "${1:-}" = "--negative" ] && NEG=1 && echo "negative control: expecting FAIL"

python - "$WOUT/out.gif" "$WOUT/scratch.img" "$NEG" <<'PY'
import sys, collections
from PIL import Image, ImageFile
ImageFile.LOAD_TRUNCATED_IMAGES = True

gif, img, neg = sys.argv[1], sys.argv[2], sys.argv[3] == '1'

WHICH = {
    (0x00, 0xCC, 0x55): (0, 'green'),
    (0x88, 0x00, 0x00): (1, 'FS_MKDIR'),
    (0xEE, 0xEE, 0x77): (2, 'FS_OPEN write / FS_WRITE / FS_CLOSE'),
    (0x00, 0x00, 0xAA): (3, 'FS_SIZE / FS_READ'),
    (0xCC, 0x44, 0xCC): (4, 'DIR_OPEN / DIR_NEXT / DIR_CLOSE'),
    (0xAA, 0xFF, 0xEE): (5, 'FS_CHDIR / FS_GETCWD / relative open'),
    (0xDD, 0x88, 0x85): (6, 'FS_SEEK'),
    (0x66, 0x44, 0x00): (7, 'FS_RENAME'),
    (0xFF, 0x77, 0x77): (8, 'FS_DELETE / FS_RMDIR / refusals'),
}

im = Image.open(gif)
n = 0
while True:
    try:
        im.seek(n); im.load(); n += 1
    except EOFError:
        break
    except Exception:
        break
im.seek(n - 1)
colour = collections.Counter(im.convert('RGB').get_flattened_data()).most_common(1)[0][0]

code, what = WHICH.get(colour, (99, 'unrecognised colour %r' % (colour,)))
if code != 0:
    print('FAIL: on screen, test %d -- %s' % (code, what))
    sys.exit(0 if neg else 1)

# ---- the independent half ------------------------------------------------
# The screen said pass. Now ask a DIFFERENT FAT32 implementation what is
# actually on the card, because the reader that verified those writes is the
# same code that performed them.
from pyfatfs.PyFatFS import PyFatFS

want = bytes((ord('A') + (i % 26)) for i in range(40))
if neg:
    want = bytes(40)          # deliberately wrong, to prove this can fail

fs = PyFatFS(img)
try:
    names = set(fs.listdir('/'))
    if 'KEEP.TXT' not in names:
        print('FAIL: pyfatfs cannot see /KEEP.TXT -- the kernel reported a '
              'write that did not reach the card')
        sys.exit(0 if neg else 1)
    if 'KT' in names:
        print('FAIL: /KT survived -- FS_RMDIR reported success and removed '
              'nothing')
        sys.exit(0 if neg else 1)
    got = fs.open('/KEEP.TXT', 'rb').read()
    if got != want:
        print('FAIL: /KEEP.TXT is %d bytes and does not match: %r' %
              (len(got), got[:16]))
        sys.exit(0 if neg else 1)
finally:
    fs.close()

if neg:
    print('FAIL: the negative control passed, so the check proves nothing')
    sys.exit(1)
print('PASS: fifteen kernel filesystem entries, and pyfatfs agrees with the card')
PY
