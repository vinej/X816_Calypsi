# ============================================================================
# calypsi.sh -- the one X816 build recipe. SOURCE this file; do not run it.
#
#     . "$(dirname "$0")/../../runtime/calypsi.sh"
#
# WHY THIS EXISTS
# ---------------
# doc/AUDIT.md M-4: "-O0 is load-bearing and enforced only by copy-paste."
# Calypsi 5.18 ELIMINATES VOLATILE READS at -O1 and above, and the console,
# the shell, FAT32 and the SD block device all reach hardware through volatile
# pointers. The compile line carrying that -O0 was duplicated across a build
# script, ten run scripts and six Makefiles, so a new recipe that forgot it
# produced a CLEAN-LINKING BROKEN BINARY -- which is the worst kind, because
# nothing fails until the FAT32 reader walks a cluster number it never
# actually fetched.
#
# So the recipe lives here once, and cc816 REFUSES to compile without -O0
# unless a caller says explicitly that its code touches no memory-mapped
# register (calypsi_no_mmio). Forgetting is now an error message rather than a
# silent miscompile. That is the enforcement M-4 asked for; the durable fix
# (assembly SD accessors, then -O2 elsewhere) is still the plan and is still
# separate.
#
# WHAT IT SETS
# ------------
#   CALYPSI   toolchain root          RT       X816_Calypsi/runtime
#   REPO      X816_Calypsi            X16LIB   X816_Calypsi/src (the converted
#   EMU       X816_Emulator           CORE     X816_core        x16 asm library)
#   LIB       the C library           LDSCRIPT the ln65816 memory map
#   CFLAGS / ASFLAGS
#
# All of them are overridable from the environment (CALYPSI=... ./run-x.sh),
# and RT/REPO/X16LIB are derived from THIS FILE's own location, so a caller no
# longer has to be run from its own directory for ../../ to resolve. The
# library directory is X16LIB and not SRC because half the run scripts already
# use SRC for "the source file under test" and would silently shadow it.
#
# WHAT IT PROVIDES
# ----------------
#   cc816 <src> <obj> [flags...]     compile           (guards -O0)
#   as816 <src> <obj> [flags...]     assemble
#   ln816 <stem>  <objects...>       link -> <stem>.elf and <stem>.raw
#   calypsi_optimise -O2 "<reason>"  opt out of -O0, with the reason recorded
#   calypsi_banner                   one line naming the toolchain actually used
# ============================================================================

# Where this file is. BASH_SOURCE under bash, $0 under a plain `sh` that
# sourced it by path -- every consumer here has a bash shebang, and the
# fallback keeps `sh -c '. calypsi.sh'` from silently resolving to the wrong
# tree.
_calypsi_self=${BASH_SOURCE[0]:-$0}
RT=$(cd "$(dirname "$_calypsi_self")" && pwd)
REPO=$(cd "$RT/.." && pwd)
unset _calypsi_self

CALYPSI=${CALYPSI:-$REPO/Calypsi/calypsi-65816-5.18}
X16LIB=${X16LIB:-$REPO/src}
EMU=${EMU:-/c/quartus/projects/X816_Emulator}
CORE=${CORE:-/c/quartus/projects/X816_core}

LIB=${LIB:-$CALYPSI/lib/clib-lc-sd.a}
LDSCRIPT=${LDSCRIPT:-$RT/x816-lib.scm}
# Extra ln65816 flags, e.g. --list-file. Empty by default and declared here so
# `set -u` callers do not trip over it.
LNFLAGS=${LNFLAGS:-}

# The machine model. Large code / small data is not a preference: `far` code
# and a bank-0 data world are what the memory map in the linker scripts
# assumes, and changing either here silently changes what the scripts place.
CALYPSI_MODEL=${CALYPSI_MODEL:---core=65816 --code-model=large --data-model=small}

CFLAGS=${CFLAGS:-$CALYPSI_MODEL -O0 -I $RT}
ASFLAGS=${ASFLAGS:---core=65816}

# ---- the -O0 guard ---------------------------------------------------------
# Set by calypsi_no_mmio only. A caller that just edits CFLAGS by hand still
# trips the guard, which is the point.
CALYPSI_ALLOW_OPT=${CALYPSI_ALLOW_OPT:-0}

calypsi_optimise () {           # calypsi_optimise -O2 "why this is safe"
    # The escape hatch, and it takes a REASON on purpose. The hazard is
    # precise -- an elided volatile READ -- so code that only ever WRITES
    # registers is genuinely safe above -O0, and the x16lib programs are
    # exactly that. Anything that reads a register back (the console's
    # cursor, FAT32's SD status, the SMC's I2C byte) is not, whatever it
    # looks like. Writing the reason down is what keeps the next caller from
    # copying the flag instead of the argument.
    [ -n "${2:-}" ] || {
        echo "calypsi.sh: calypsi_optimise needs a reason" >&2
        return 1
    }
    CFLAGS="$CALYPSI_MODEL $1 -I $RT"
    CALYPSI_ALLOW_OPT=1
    echo "calypsi: $1 -- $2"
}

_calypsi_check_opt () {
    case " $CFLAGS " in
        *" -O0 "*) return 0 ;;
    esac
    [ "$CALYPSI_ALLOW_OPT" = 1 ] && return 0
    echo "calypsi.sh: refusing to compile without -O0." >&2
    echo "  CFLAGS = $CFLAGS" >&2
    echo "  Calypsi 5.18 eliminates volatile READS above -O0, so anything that" >&2
    echo "  reads a hardware register back miscompiles SILENTLY. If this code" >&2
    echo "  provably never does, say so: calypsi_optimise -O2 \"<reason>\"." >&2
    return 1
}

_calypsi_tool () {
    local tool="$CALYPSI/bin/$1"
    if [ -x "$tool" ]; then
        printf '%s\n' "$tool"
        return 0
    fi
    if [ -x "$tool.exe" ]; then
        printf '%s\n' "$tool.exe"
        return 0
    fi
    printf '%s\n' "$tool"
}

# ---- the three tools -------------------------------------------------------
cc816 () {                      # cc816 <src> <obj> [extra cc flags...]
    local src=$1 obj=$2
    shift 2
    _calypsi_check_opt || return 1
    "$(_calypsi_tool cc65816)" $CFLAGS "$@" "$src" -o "$obj"
}

as816 () {                      # as816 <src> <obj> [extra as flags...]
    local src=$1 obj=$2
    shift 2
    "$(_calypsi_tool as65816)" $ASFLAGS "$@" "$src" -o "$obj"
}

# ln65816 always names the raw image <ELF stem>.raw whatever -o says, so the
# stem is the argument and the caller copies <stem>.raw where it wants it.
# --program-root and --rtattr are not optional: the first keeps the x816hdr
# section (the magic and the entry jmp) from being dropped as unreferenced,
# the second picks the exit path that does not drag in the full C runtime
# teardown.
ln816 () {                      # ln816 <stem> <objects...>
    local stem=$1
    shift
    rm -f "$stem.raw"
    "$(_calypsi_tool ln65816)" "$LDSCRIPT" "$@" "$LIB" \
        -o "$stem.elf" --output-format raw \
        --program-root __x816_root_section --rtattr exit=simplified $LNFLAGS
}

calypsi_banner () {
    echo "calypsi: $(basename "$CALYPSI")  map: $(basename "$LDSCRIPT")  $CFLAGS"
}
