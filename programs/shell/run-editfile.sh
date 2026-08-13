#!/usr/bin/env bash
# Resident editor FILE ROUND TRIP: open a file off the card into the editor's
# buffer, render it, save it back out under another name, and read it back.
#
#   ./run-editfile.sh              build and run
#   ./run-editfile.sh --negative   ask for a file that is not there, and require
#                                  the round-trip checks to FAIL
#
# What this covers, none of it stubbed: K_FS_OPEN / K_FS_READ through the real
# FAT32 image and the real SD block device, the line-break detection, the fill
# of mem.inc's doubly-linked 256-byte pages in the reserved editor region, the
# render of that buffer, the page walk back out, K_FS_WRITE, K_FS_CLOSE, and
# the return to the shell prompt.
#
# TWO EMULATOR RUNS, ONE CARD, and that is not incidental. Reading the result
# back in the SAME run does not work: -autokeys types on a 25 ms emulated
# clock and keeps typing while the editor holds the screen for its captured
# frame, so the SMC key FIFO overflows and the follow-up commands are dropped
# -- which looked exactly like a broken save (the third command sat half
# echoed at the prompt). Splitting the runs also buys a real check: the second
# machine BOOTS FROM SCRATCH and finds the file, so the directory entry and
# the cluster chain reached the image rather than living in a cache.
#
# THE CHECKS THAT MATTER, and why each is here rather than something easier:
#
#   * The text appears INSIDE THE EDITOR, in a captured frame, before the save.
#     A load that filled the pages but left the display pointers wrong would
#     still write a correct file.
#
#   * `type` on the SAVED file shows the original text. This is the one that
#     catches a page walk which reports the right byte count and moves the
#     wrong bytes -- everything checked on the editor's own screen passes in
#     that case.
#
#   * `ls` shows the saved file at EXACTLY 26 bytes, the size of the source.
#     A save that dropped the last page, doubled the final newline, or wrote
#     CRLF where the source had LF is a content check away from passing and a
#     size check away from failing.
#
#   * The negative control asks for /NOSUCH.TXT. The round-trip checks must
#     then fail AND the editor must report file-not-found rather than starting
#     on an empty buffer as though nothing had been asked of it.
#
# The card is a SCRATCH COPY of X816_core/boot/fat32.img -- this test WRITES.
# /HELLO.TXT there is "Hello from FAT32 on X816!\n", 26 bytes, which is also
# smaller than one cluster: the case fat32_read_far used to get silently wrong.
#
# Requires Pillow:  pip install pillow
set -eu

cd "$(dirname "$0")"
bash build.sh >/dev/null

. ../../runtime/calypsi.sh

NEG=0
if [ "${1:-}" = "--negative" ]; then
    NEG=1
    echo "negative control: asking for a file that is not on the card,"
    echo "expecting the round-trip checks to fail"
fi

EDIT_GIF=${EDIT_GIF:-$PWD/out-editfile-edit.gif}
BACK_GIF=${BACK_GIF:-$PWD/out-editfile-back.gif}
SCRATCH=${SCRATCH:-$PWD/out-editfile-card.img}
rm -f "$EDIT_GIF" "$BACK_GIF" "$SCRATCH"
trap 'rm -f "$EDIT_GIF" "$BACK_GIF" "$SCRATCH"' EXIT

cp "$CORE/boot/fat32.img" "$SCRATCH"

PYTHON=${PYTHON:-python}
if ! command -v "$PYTHON" >/dev/null 2>&1; then
    PYTHON=/c/Users/jyv/AppData/Local/Programs/Python/Python312/python.exe
fi
POWERSHELL=${POWERSHELL:-powershell.exe}
EMU_W=$(cygpath -m "$EMU/build/x16emu.exe")
BOOT_W=$(cygpath -m "$CORE/boot/boot.rom")
KERNEL_W=$(cygpath -m "$PWD/kernel.bin")
CARD_W=$(cygpath -m "$SCRATCH")
FONT_W=$(cygpath -m "$RT/font_cp437.s")
CAPTURE_W=$(cygpath -m "$PWD/run-edit-capture.ps1")

capture() {  # capture <gif> <keys> <seconds>
    "$POWERSHELL" -NoProfile -ExecutionPolicy Bypass -File "$CAPTURE_W" \
        -Emu "$EMU_W" -Boot "$BOOT_W" -Kernel "$KERNEL_W" \
        -Gif "$(cygpath -m "$1")" -Sdcard "$CARD_W" \
        -Keys "$2" -Seconds "$3" >/dev/null
}

SRC=/hello.txt
[ "$NEG" = "1" ] && SRC=/nosuch.txt

# Run 1: the editor loads SRC, holds it on screen, saves it as EDITOUT.TXT.
capture "$EDIT_GIF" "editfl $SRC" 30

# Run 2: a fresh machine, the same card. `type` uses a RELATIVE name against
# the kernel's one working directory -- the same cwd the editor resolved its
# absolute path against, which is the point of the editor not keeping one.
capture "$BACK_GIF" 'ls\ntype editout.txt' 30

"$PYTHON" - "$EDIT_GIF" "$BACK_GIF" "$FONT_W" "$NEG" <<'PY'
import os, re, sys, io
from collections import Counter
from PIL import Image, ImageFile
ImageFile.LOAD_TRUNCATED_IMAGES = True

edit_gif, back_gif, fontinc, neg = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4] == "1"
for g in (edit_gif, back_gif):
    if not os.path.exists(g):
        sys.exit("FAIL: emulator did not create %s" % g)

vals = []
for line in io.open(fontinc, encoding="utf-8"):
    m = re.match(r"\s*\.byte\s+(.*)$", line.split(";")[0])
    if m:
        vals += [int(x.strip().lstrip("$"), 16)
                 for x in m.group(1).split(",") if x.strip()]

glyph = {}
for c in range(0x20, 0x7F):
    glyph[tuple(vals[c * 8:(c + 1) * 8])] = chr(c)
# The prompt is CP437 $AF, the chevron from the boot mark, NOT '>'. The
# table above stops at $7E, so decode $AF as '>' and every "the prompt is
# back" assertion below keeps reading as what it means.
glyph[tuple(vals[0xAF * 8:0xB0 * 8])] = ">"


def frame_row(im, r):
    px = im.convert("RGB").load()
    out = ""
    for col in range(80):
        colors = []
        for y in range(8):
            for x in range(8):
                colors.append(px[col * 8 + x, r * 8 + y])
        # The editor paints a header and two footer rows in their own colours,
        # so the background cannot be assumed black the way it can at the shell
        # prompt: take the most common colour in the cell as background.
        #
        # THAT HEURISTIC IS WRONG FOR DENSE GLYPHS. 'B', 'R' and 'N' light more
        # than half the cell, so the most common colour is their INK and the
        # pattern comes out inverted -- which decoded 'B' as '?' and read "BIG"
        # as "?IG" while every check still looked plausible. So when a pattern
        # is not a glyph, try its complement before giving up.
        bg = Counter(colors).most_common(1)[0][0]
        bits = []
        for y in range(8):
            b = 0
            for x in range(8):
                if px[col * 8 + x, r * 8 + y] != bg:
                    b |= 0x80 >> x
            bits.append(b)
        ch = glyph.get(tuple(bits))
        if ch is None:
            ch = glyph.get(tuple((~b) & 0xFF for b in bits), "?")
        out += ch
    return out.rstrip()


def frame_count(im):
    # Do not cap this. A warp-mode run of this length records a few thousand
    # frames, and a scan that stopped at some round number and then read "the
    # last frame" read one from the middle of the run instead. Seeking is
    # cheap; the per-cell decode is what gets sampled, below.
    n = 0
    while True:
        try:
            im.seek(n)
            im.load()
            n += 1
        except (EOFError, OSError, IndexError):
            break
    return n


TEXT = "HELLO FROM FAT32 ON X816!"

# ---- run 1: the editor ----------------------------------------------------
im = Image.open(edit_gif)
n = frame_count(im)
if n == 0:
    sys.exit("FAIL: no decodable frame in the editor run")

# Every third frame is plenty for "did the text reach the text area": the
# editor holds the screen for a deliberate delay, tens of frames even in warp.
in_editor = False
for f in range(0, n, 3):
    im.seek(f)
    im.load()
    body = "\n".join(frame_row(im, r) for r in (2, 3, 4)).upper()
    if TEXT in body:
        in_editor = True
        break

im.seek(n - 1)
im.load()
edit_rows = [frame_row(im, r) for r in range(20)]
edit_screen = "\n".join(edit_rows).upper()

# ---- run 2: reading the result back --------------------------------------
im2 = Image.open(back_gif)
n2 = frame_count(im2)
if n2 == 0:
    sys.exit("FAIL: no decodable frame in the read-back run")
im2.seek(n2 - 1)
im2.load()
# 24 rows: the boot banner, two commands and their output. Widen this with any
# case added below -- a verdict below the window makes every check print "ok"
# while the run fails.
back_rows = [frame_row(im2, r) for r in range(24)]
back_screen = "\n".join(back_rows).upper()


def dump():
    print("\neditor run, final screen:")
    for i, r in enumerate(edit_rows):
        if r:
            print("  %2d: %s" % (i, r))
    print("\nread-back run, final screen:")
    for i, r in enumerate(back_rows):
        if r:
            print("  %2d: %s" % (i, r))


size_ok = re.search(r"EDITOUT\.TXT\s+26\b", back_screen) is not None
typed_ok = TEXT in back_screen
reported_ok = "EDITFL OK" in edit_screen

if neg:
    problems = []
    if in_editor or typed_ok:
        problems.append("the round-trip checks passed for a file that is not "
                        "on the card -- they are not testing what they claim")
    if size_ok:
        problems.append("EDITOUT.TXT was written at 26 bytes from a file that "
                        "does not exist")
    if "EDITFL FAIL 82" not in edit_screen:
        problems.append("the editor did not report file-not-found ($80 | "
                        "KERR_NOTFOUND = 82); a missing file must be an error, "
                        "not a silent empty buffer")
    if problems:
        print("FAIL (negative control):")
        for p in problems:
            print("   -", p)
        dump()
        sys.exit(1)
    print("PASS (negative control): a missing file was reported, and the "
          "round-trip checks correctly did not pass")
    sys.exit(0)

problems = []
if not in_editor:
    problems.append("the loaded text never appeared in the editor's text area")
if not reported_ok:
    problems.append("editfl did not report OK -- the load or the save set "
                    "file_io_err")
if not typed_ok:
    problems.append("`type` on the saved file did not show the source text: "
                    "the page walk wrote the wrong bytes")
if not size_ok:
    problems.append("`ls` did not show EDITOUT.TXT at 26 bytes: the save "
                    "changed the length of the file")

if problems:
    print("FAIL:")
    for p in problems:
        print("   -", p)
    dump()
    sys.exit(1)

print("PASS: the resident editor opened a file off the card, rendered it, "
      "saved it back byte for byte, and a fresh machine read it again")
dump()
PY
