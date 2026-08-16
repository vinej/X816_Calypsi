#!/usr/bin/env bash
# The desktop, exercised seven ways, on a card the test builds itself.
#
#   ./run-desktop.sh              X at the prompt opens the desktop
#   ./run-desktop.sh --exit       E comes back to the console
#   ./run-desktop.sh --boot       -desktop opens it with nothing typed
#   ./run-desktop.sh --negative   without -desktop it does NOT open
#   ./run-desktop.sh --resume     a launched program comes back TO THE DESKTOP
#   ./run-desktop.sh --cwd        a tile launch lands in the PROGRAM'S directory
#   ./run-desktop.sh --browse     the file browser starts a .BIN it navigates to
#   ./run-desktop.sh --all        all seven
#
# SHOW=1 prints the decoded screen even on a pass; CHECKMODE=show turns any
# mode into a dump with no assertion, which is how you work out what a new key
# sequence really does before deciding what to assert about it.
#
# --cwd IS CHECKED ON THE CARD, NOT ON THE SCREEN, because kalk's /SS says
# "saved" whichever directory it wrote to -- writing to the wrong one is a
# perfectly successful write. The card is the only place the two answers differ.
#
# WHY --negative EXISTS. --boot types nothing and asserts the desktop is on
# screen, which is also what you would see if the prompt launched the desktop
# unconditionally and SYSCTL[3] were never read. The two runs differ by the
# -desktop flag and by nothing else, so the pair measures the bit rather than
# the launcher. Without the control, --boot is the kind of green this project
# has already shipped three times.
#
# The emulator models the OSD switch as -desktop -- see X816_Emulator's
# src/memory.c, where SYSCTL[3] is read-only for the same reason it is in the
# RTL: it is a switch on the front of the machine, not a guest register.
#
# Requires Pillow and pyfatfs:  pip install pillow pyfatfs
set -u

. "$(dirname "$0")/../../runtime/calypsi.sh"
cd "$(dirname "$0")"

CORE=${CORE_DIR:-$(cd ../../../X816_core && pwd)}
EMU=${EMU_DIR:-$(cd ../../../X816_Emulator && pwd)}

MODES="launch"
case "${1:-}" in
    --exit)     MODES="exit" ;;
    --boot)     MODES="boot" ;;
    --negative) MODES="negative" ;;
    --resume)   MODES="resume" ;;
    --cwd)      MODES="cwd" ;;
    --browse)   MODES="browse" ;;
    --all)      MODES="launch exit boot negative resume cwd browse" ;;
    "")         ;;
    *)          echo "unknown option $1" >&2; exit 2 ;;
esac

[ -f ../shell/kernel.bin ] || {
    echo "../shell/kernel.bin missing -- run: sh ../shell/build.sh"; exit 1; }
[ -f x.bin ] || {
    echo "x.bin missing -- run: sh ../shell/build.sh"; exit 1; }

OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
WOUT=$(cygpath -m "$OUT" 2>/dev/null || echo "$OUT")

cp ../shell/kernel.bin "$OUT/kernel.bin" || exit 1

# --resume and --cwd need one real program to launch. kalk is the one with an
# unambiguous quit (/Q) and a cwd-observable side effect (/SS writes where it
# was run), so both tests ride on it.
KALK=""
case " $MODES " in
    *" resume "*|*" cwd "*|*" browse "*)
        [ -f ../kalk/kalk.bin ] || {
            echo "../kalk/kalk.bin missing -- run: sh ../kalk/build.sh"; exit 1; }
        KALK=../kalk/kalk.bin ;;
esac
python mkcard.py "$WOUT/card.img" x.bin $KALK || exit 1

# THE PADDING IS NOT OPTIONAL. -autokeys starts typing 500 ms after reset and
# waits for nothing, while the prompt is still coming up; the SMC FIFO holds 16
# events and silently drops the rest. Backspace is the safe filler -- the
# prompt ignores it on an empty line and the desktop ignores it outside the
# browser. This is the same trap documented at length in kalk's run-kalk.sh.
PAD=$(printf '\b%.0s' $(seq 1 120))
DRAIN=$(printf '\b%.0s' $(seq 1 200))
# Loading kalk off the card is ~40 KB of SD reads and takes far longer than the
# desktop's own start; the modes that launch it need a much longer wait than
# the ones that only draw a screen.
LONG=$(printf '\b%.0s' $(seq 1 400))
# BACKSPACE IS NOT SAFE INSIDE THE BROWSER. There it means "up a directory",
# so padding with it walks back to the root and resets the selection -- the
# filler silently undoes the very navigation the test is trying to perform.
# The first --browse run "reproduced" the reported bug and was measuring only
# this. Space is the filler that both screens genuinely ignore.
SPAD=$(printf ' %.0s' $(seq 1 200))
SLONG=$(printf ' %.0s' $(seq 1 400))

status=0
for mode in $MODES; do
    KEYS=""
    FLAG=""
    LIMIT=45
    case "$mode" in
        launch)   KEYS="${PAD}x\n${DRAIN}" ;;
        exit)     KEYS="${PAD}x\n${DRAIN}e${DRAIN}" ;;
        boot)     KEYS="${DRAIN}${DRAIN}"; FLAG="-desktop" ;;
        negative) KEYS="${DRAIN}${DRAIN}" ;;
        # KALK is tile 0 and tile 0 is selected at entry, so a bare Return
        # opens it without any arrow keys to get lost.
        resume)   KEYS="${LONG}\n${LONG}/q${LONG}"; FLAG="-desktop"; LIMIT=120 ;;
        cwd)      KEYS="${LONG}\n${LONG}/ssbook.csv\n${LONG}"
                  FLAG="-desktop"; LIMIT=120 ;;
        # B opens the browser at "/", which lists DESKTOP then KALK in the
        # order mkcard.py created them. Down to KALK, Return to enter it,
        # Return again on the single KALK.BIN inside.
        browse)   KEYS="${LONG}b${SPAD}\\d${SPAD}\n${SPAD}\n${SLONG}"
                  FLAG="-desktop"; LIMIT=120 ;;
    esac

    printf '%-9s ' "$mode"
    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout $LIMIT \
        "$EMU/build/x16emu.exe" -boot "$(cygpath -m "$CORE/boot/boot.rom")" \
        -sdcard "$WOUT/card.img" \
        -load "F00000,$WOUT/kernel.bin" \
        $FLAG \
        -autokeys "$KEYS" \
        -warp -gif "$WOUT/$mode.gif" >/dev/null 2>&1

    if [ "$mode" = cwd ]; then
        python checkcard.py "$WOUT/card.img" || status=1
    else
        python check.py "$WOUT/$mode.gif" "$RT/font_cp437.s" "${CHECKMODE:-$mode}" || status=1
    fi
done

exit $status
