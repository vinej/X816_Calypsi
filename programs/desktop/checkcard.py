"""Where did the tile-launched program think it was?

    python checkcard.py <card.img>

The desktop opened /KALK/KALK.BIN and kalk was told to save BOOK.CSV by a bare
name. A bare name resolves against the working directory, so the file lands in
/KALK if the desktop handed the program its own directory and at the root if it
did not.

BOTH HALVES ARE ASSERTED. "It is in /KALK" alone would also pass if kalk
resolved every bare name against its own image directory regardless of cwd, and
"it is not at the root" alone passes on a run where kalk never started. The
pair only passes when the file went to one specific place.
"""
import sys

from pyfatfs.PyFatFS import PyFatFS

fs = PyFatFS(sys.argv[1])


def has(path):
    try:
        fs.getinfo(path)
        return True
    except Exception:
        return False


in_kalk = has("/KALK/BOOK.CSV")
at_root = has("/BOOK.CSV")
fs.close()

if in_kalk and not at_root:
    print("PASS (cwd)")
    sys.exit(0)

print("CARD: /KALK/BOOK.CSV=%s  /BOOK.CSV=%s" % (in_kalk, at_root))
if at_root:
    print("FAIL: the tile launch left the working directory at the root --")
    print("      a program that loads its own files by bare name (Forth's")
    print("      BASE, SuperBasic's LOAD) would not find them")
elif not in_kalk:
    print("FAIL: BOOK.CSV was not written at all -- kalk did not start, or")
    print("      the typing missed it. This is not a cwd verdict either way.")
sys.exit(1)
