"""Build the smallest card that can answer the question.

    python mkcard.py <card.img> <x.bin> [kalk.bin]

/DESKTOP/X.BIN is what the prompt reaches for. The other eight tiles are left
UNRESOLVABLE on purpose: a tile whose path does not exist draws exactly like
one whose path does, and the launch assertions are about the desktop coming up,
not about what it can start.

kalk.bin is the exception, placed at /KALK/KALK.BIN when it is given. The
--resume test needs one real program to leave and come back from, and kalk is
the one with an unambiguous quit key (/Q) that types cleanly through -autokeys.
"""
import sys

from pyfatfs.PyFat import PyFat
from pyfatfs.PyFatFS import PyFatFS

img, prog = sys.argv[1], sys.argv[2]
kalk = sys.argv[3] if len(sys.argv) > 3 else None

with open(img, "wb") as f:
    f.truncate(64 * 1024 * 1024)

fat = PyFat()
fat.mkfs(img, fat_type=PyFat.FAT_TYPE_FAT32, sector_size=512, label="X816DESK")
fat.close()

fs = PyFatFS(img)
fs.makedir("/DESKTOP")
with fs.open("/DESKTOP/X.BIN", "wb") as g:
    g.write(open(prog, "rb").read())

if kalk:
    fs.makedir("/KALK")
    with fs.open("/KALK/KALK.BIN", "wb") as g:
        g.write(open(kalk, "rb").read())

fs.close()
