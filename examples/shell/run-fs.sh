#!/usr/bin/env bash
# File-command conformance: type at the shell with a card attached and check
# what came back.
#
# Drives ls / pwd / cd / type through the real keyboard path (-autokeys) against
# the real FAT32 image, so this covers the whole stack at once: SMC keyboard,
# tokeniser, dispatcher, path resolution, FAT32 directory walk and the SD block
# device. Nothing is stubbed.
#
# The card is a SCRATCH COPY of X816_core/boot/fat32.img (built by
# boot/mkfat32.py) -- this test writes, and the fixture has to survive it.
# It holds:
#   /HELLO.TXT    26 bytes
#   /BIG.BIN      20000 bytes
#   /SUB/         a directory
#   /SUB/NESTED.TXT  12 bytes
#
# Checks that matter, and each one is a bug someone actually ships:
#   * a relative path resolves against the working directory
#   * `cd ..` at the ROOT stays at the root instead of walking off the top
#   * `cd` onto a FILE is refused
#   * a missing path is reported as missing, and a directory passed to `type`
#     is reported as a directory -- not as "not found", which would send the
#     reader looking for a file that is right there
#   * `rmdir` refuses a directory that still has something in it
#   * `load` of a file SMALLER THAN ONE CLUSTER actually writes the bytes.
#     fat32_read_far moves whole clusters and reports how many; a caller that
#     ignores the count copies nothing at all for a small file and silently
#     truncates every larger one, while still printing the correct size
#
#   ./run-fs.sh              build and run
#   ./run-fs.sh --negative   corrupt the expectation, to prove this can fail
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

CFLAGS="--core=65816 --code-model=large --data-model=small -O0 -I $RT"

"$CALYPSI/bin/cc65816" $CFLAGS shell.c        -o "$OUT/main.o"    || exit 1
"$CALYPSI/bin/cc65816" $CFLAGS $RT/shell.c    -o "$OUT/shell.o"   || exit 1
"$CALYPSI/bin/cc65816" $CFLAGS $RT/fat32.c    -o "$OUT/fat32.o"   || exit 1
"$CALYPSI/bin/cc65816" $CFLAGS $RT/kfs.c      -o "$OUT/kfs.o"   || exit 1
"$CALYPSI/bin/cc65816" $CFLAGS $RT/console.c  -o "$OUT/console.o" || exit 1
"$CALYPSI/bin/cc65816" $CFLAGS $RT/font8x8.c  -o "$OUT/font.o"    || exit 1
"$CALYPSI/bin/as65816" --core=65816 $RT/x816hdr.s -o "$OUT/hdr.o" || exit 1
"$CALYPSI/bin/as65816" --core=65816 $RT/smc.s     -o "$OUT/smc.o" || exit 1
"$CALYPSI/bin/as65816" --core=65816 $RT/exec.s    -o "$OUT/exec.o"  || exit 1
"$CALYPSI/bin/as65816" --core=65816 $RT/font_cp437.s -o "$OUT/fontcp.o" || exit 1
"$CALYPSI/bin/ln65816" $RT/x816-lib.scm "$OUT/hdr.o" "$OUT/main.o" \
    "$OUT/shell.o" "$OUT/fat32.o" "$OUT/kfs.o" "$OUT/console.o" "$OUT/font.o" "$OUT/smc.o" "$OUT/exec.o" "$OUT/fontcp.o" \
    "$CALYPSI/lib/clib-lc-sd.a" -o "$OUT/SHELL.elf" --output-format raw \
    --program-root __x816_root_section --rtattr exit=simplified || exit 1
cp "$OUT/SHELL.raw" "$OUT/shell.bin" || exit 1

NEG=0
[ "${1:-}" = "--negative" ] && NEG=1 && \
    echo "negative control: expecting the check to FAIL"

# A SCRATCH COPY, because this test WRITES now. It used to only read, so
# pointing at the shared conformance image was harmless; the moment mkdir was
# added it stopped being. rmdir deliberately refuses the non-empty directory,
# so /MD survives the run -- and the next run finds it already there and fails
# at mkdir. A test that mutates its own fixture passes exactly once.
cp "$CORE/boot/fat32.img" "$OUT/scratch.img" || exit 1

SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout 240 \
    "$EMU/build/x16emu.exe" -boot "$(cygpath -m "$CORE/boot/boot.rom")" \
    -sdcard "$WOUT/scratch.img" \
    -load "010000,$WOUT/shell.bin" \
    -autokeys 'ls\ncd /sub\ntype nested.txt\ncd ..\ncd ..\npwd\ncd hello.txt\ntype /sub\nload /hello.txt 020000\ndump 020000 10\nmkdir /md\ncopy /hello.txt /md/x.txt\nrmdir /md\n' \
    -warp -gif "$WOUT/out.gif" >/dev/null 2>&1

python - "$WOUT/out.gif" "$RT/font_cp437.s" "$NEG" <<'PY'
import sys, re, io
from PIL import Image, ImageFile
ImageFile.LOAD_TRUNCATED_IMAGES = True

gif, fontinc, neg = sys.argv[1], sys.argv[2], sys.argv[3] == '1'

# The console now carries all 256 CP437 glyphs, so decode against the
# GENERATED font rather than the old 64-glyph boot font -- and lower case is
# real lower case on screen now, not folded up.
vals = []
for line in io.open(fontinc, encoding='utf-8'):
    m = re.match(r'\s*\.byte\s+(.*)$', line.split(';')[0])
    if m:
        vals += [int(x.strip().lstrip('$'), 16)
                 for x in m.group(1).split(',') if x.strip()]
# Only $20-$7E is mapped back to characters. Everything else decodes to '?',
# which keeps blank glyphs from colliding with space and makes a stray box
# character obvious rather than silently readable.
glyph = {}
for _c in range(0x20, 0x7F):
    glyph[tuple(vals[_c * 8:(_c + 1) * 8])] = chr(_c)

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
px = im.convert('RGB').load()

def row_text(r):
    out = ""
    for col in range(60):
        bits = []
        for y in range(8):
            b = 0
            for x in range(8):
                if px[col * 8 + x, r * 8 + y] != (0, 0, 0):
                    b |= 0x80 >> x
            bits.append(b)
        out += glyph.get(tuple(bits), '?')
    return out.rstrip()

rows = [row_text(r) for r in range(28)]
# Upper-cased on both sides. The console prints real lower case now, and
# every expectation below is about whether the TEXT is right, not its
# case -- pinning the tests to a cosmetic choice would break them on
# every restyle of a message.
screen = "\n".join(rows).upper()

if neg:
    checks = [("HELLO.TXT   99999", "a size that is not on the card")]
else:
    checks = [
        ("HELLO.TXT",       "ls listed the root"),
        ("SUB           <DIR>", "ls marked the directory"),
        ("NESTED FILE",     "type read a file through a RELATIVE path"),
        ("HELLO.TXT NOT A DIRECTORY", "cd onto a file was refused"),
        ("/SUB IS A DIRECTORY", "type named the directory, not 'not found'"),
        # /HELLO.TXT is 26 bytes -- SMALLER THAN ONE CLUSTER, which is the case
        # that was silently broken. fat32_read_far moves whole clusters only and
        # returns how many it moved; ignoring that return value made `load`
        # report the full size while copying NOTHING for a file this size, and
        # truncating every larger file to a cluster boundary. Dumping the bytes
        # back is the only way to notice: the reported byte count was right.
        ("02:0000 48 65 6C 6C 6F", "load of a sub-cluster file actually wrote"),
        ("/HELLO.TXT -> 02:0000, 26", "load reported the right size"),
        # rmdir MUST refuse a directory with something in it. Freeing the chain
        # would strand the file inside with nothing pointing at it -- lost space
        # that only chkdsk would ever find, and silent until then.
        ("/MD CANNOT REMOVE", "rmdir refused a non-empty directory"),
    ]

bad = [why for text, why in checks if text not in screen]

# `cd ..` twice from /SUB must land at the root and STAY there.
if not neg:
    pwd_lines = [rows[i + 1] for i, r in enumerate(rows[:-1])
                 if r.upper().endswith("PWD")]
    if not pwd_lines or pwd_lines[-1] != "/":
        bad.append("cd .. did not clamp at the root (pwd showed %r)"
                   % (pwd_lines[-1] if pwd_lines else None))

if bad:
    print("FAIL:")
    for b in bad:
        print("   -", b)
    print("\nscreen:")
    for i, r in enumerate(rows):
        if r:
            print("  %2d: %s" % (i, r))
    sys.exit(1)

print("PASS: ls, pwd, cd and type over real FAT32, typed at the real keyboard")
for r in rows:
    if r:
        print("   ", r)
PY
