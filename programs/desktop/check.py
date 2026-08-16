"""Decode the last GIF frame of a desktop run and rule on it.

    python check.py <out.gif> <font_cp437.s> <mode>

THE BINARIZATION IS PER CELL, NOT AGAINST BLACK. The desktop draws its tiles
on eight different background colours and inverts the selected one, so the
"foreground is anything that is not black" rule every other harness here uses
would decode a tile's label as a solid run of unknowns -- and the one glyph
that mattered would be the one that failed to read. Each 8x8 cell is split
into its most common colour (the background, whatever it happens to be) and
everything else, which reads normal and reverse video identically and does not
care what the palette is.

Unknown glyphs become '?' rather than raising: box-drawing characters are all
over this screen on purpose and none of the assertions are about them.
"""
import io
import os
import re
import sys

from PIL import Image, ImageFile

ImageFile.LOAD_TRUNCATED_IMAGES = True

SCREEN_W, SCREEN_H = 80, 60

gif, fontinc, mode = sys.argv[1], sys.argv[2], sys.argv[3]

# The font the core actually draws with, read from the assembler source, so a
# font change cannot leave this decoder quietly matching yesterday's shapes.
vals = []
for line in io.open(fontinc, encoding="utf-8"):
    m = re.match(r"\s*\.byte\s+(.*)$", line.split(";")[0])
    if m:
        vals += [int(x.strip().lstrip("$"), 16)
                 for x in m.group(1).split(",") if x.strip()]

glyph = {}
for code in range(0x20, 0x7F):
    glyph[tuple(vals[code * 8:(code + 1) * 8])] = chr(code)

im = Image.open(gif)
frames = 0
while True:
    try:
        im.seek(frames)
        im.load()
        frames += 1
    except (EOFError, OSError, IndexError):
        break
if frames == 0:
    sys.exit("no decodable frame -- did the emulator run at all?")
im.seek(frames - 1)
px = im.convert("RGB").load()


def cell(col, row):
    counts = {}
    grid = []
    for y in range(8):
        line = []
        for x in range(8):
            p = px[col * 8 + x, row * 8 + y]
            counts[p] = counts.get(p, 0) + 1
            line.append(p)
        grid.append(line)

    def bits_for(bg):
        return tuple(sum(0x80 >> x for x in range(8) if grid[y][x] != bg)
                     for y in range(8))

    # TRY BOTH POLARITIES, most common colour first.
    #
    # "The majority colour is the background" is wrong for any glyph that fills
    # more than half its cell -- R, B, N, M and W all do -- and it fails
    # SILENTLY, decoding them as an unknown rather than as the wrong letter.
    # That cost a debugging round here: the screen looked like FO?TH and
    # ?ASIC and the first suspicion was the font, not the decoder.
    order = sorted(counts, key=counts.get, reverse=True)
    for bg in order:
        hit = glyph.get(bits_for(bg))
        if hit is not None:
            return hit
    return "?"


screen = ["".join(cell(c, r) for c in range(SCREEN_W)) for r in range(SCREEN_H)]
text = "\n".join(screen)


def show():
    for r, line in enumerate(screen):
        if line.strip():
            print("%2d |%s|" % (r, line.rstrip()))


DESKTOP = "X816 DESKTOP"
desktop_up = any(DESKTOP in line for line in screen)
# The prompt's chevron banner paints X816 at row 8, columns 3-6. sh_banner says
# these four glyphs may move, so look for them anywhere rather than at 3,8.
console_up = any("X816" in line and DESKTOP not in line for line in screen)

fails = []

if mode in ("launch", "boot"):
    if not desktop_up:
        fails.append("expected the desktop; '%s' is not on screen" % DESKTOP)
    # The tiles are the difference between a title and a working launcher.
    for tile in ("KALK", "FORTH", "BASIC", "FILES", "EXIT"):
        if tile not in text:
            fails.append("tile %s is missing" % tile)

elif mode == "browse":
    # B, down to KALK, Return into it, Return on KALK.BIN. Reaching kalk at all
    # is the assertion: the browser used to list FAT's "." and ".." above the
    # real entries, so this same sequence walked into "/KALK/." and started
    # nothing.
    if desktop_up:
        fails.append("still on the tile grid; the browser never launched")
    elif "READY" not in text:
        fails.append("the browser did not reach KALK.BIN -- screen is neither "
                     "the desktop nor kalk")
    if "[DIR] ." in text:
        fails.append('"." or ".." is listed in the browser again')

elif mode == "resume":
    # The desktop is a loadable program, so starting one overwrites it. What
    # comes back is the resident prompt, and the only thing that puts the
    # desktop back is the "XDSK" byte the desktop left in the carry block. If
    # that is not read, this lands on the console -- which is a working
    # machine, and looks like a working machine, and is wrong.
    if console_up and not desktop_up:
        fails.append("kalk exited to the CONSOLE; the desktop did not resume")
    elif not desktop_up:
        fails.append("after leaving kalk, neither the desktop nor the console "
                     "is on screen")
    if "KALK" not in text:
        fails.append("the desktop resumed but its tiles are missing")

elif mode == "exit":
    if desktop_up:
        fails.append("E did not leave the desktop; '%s' is still up" % DESKTOP)
    if not console_up:
        fails.append("E left the desktop but the console banner never came back")

elif mode == "negative":
    # The control for `boot`: same card, same typing (none), no -desktop.
    if desktop_up:
        fails.append("the desktop came up WITHOUT -desktop -- the boot test "
                     "would pass no matter what SYSCTL[3] said")
    if not console_up:
        fails.append("neither the desktop nor the console banner is on screen")

elif mode == "show":
    # No assertion: print the screen and pass. For working out what a new
    # sequence actually does before deciding what to assert about it.
    print("SCREEN:")
    show()
    print("desktop_up=%s console_up=%s" % (desktop_up, console_up))

else:
    sys.exit("unknown mode %r" % mode)

if fails:
    print("SCREEN:")
    show()
    print()
    for f in fails:
        print("FAIL: %s" % f)
    sys.exit(1)

# SHOW=1 prints the screen on a PASS too. A green here means five strings were
# found on a 80x60 grid; it does not mean the screen looks right, and the one
# way to know that is to read it. Use it after changing anything about the
# drawing.
if os.environ.get("SHOW") == "1":
    print("SCREEN:")
    show()

print("PASS (%s)" % mode)
