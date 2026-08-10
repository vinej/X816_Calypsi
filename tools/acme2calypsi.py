#!/usr/bin/env python3
"""acme2calypsi.py -- mechanical ACME -> Calypsi as65816 conversion.

X816_Library's src_acme/ is the single source of truth; this generates the
Calypsi dialect from it, the same way that repo's own acme2*.py generate the
ca65, 64tass and MADS trees.

    python tools/acme2calypsi.py <X816_Library>/src_acme src

Calypsi's dialect is further from ACME than the other three targets, because it
is a C-toolchain assembler rather than a 6502-community one. Every rule below
was checked against the Calypsi 65816 guide 5.18 and against the .s sources
that ship in the toolchain's src/lib/lowlevel/ -- not inferred:

  numbers     $1F -> 0x1F, %1010 -> 0b1010.  as65816 REJECTS '$' outright
              ("invalid operand field"), so every literal must be rewritten.
              None of the other converters need this.
  data        !byte -> .byte, !word -> .word, !text -> .byte (NOT .ascii --
              .ascii takes a bare string, !text takes a mixed list)
  fill        !fill n, v -> .space n, v
  include     !source "f.asm" -> #include "f.s"   (the C preprocessor is
              available; __CALYPSI_ASSEMBLER__ is predefined)
  conditional !ifdef X { .. } -> #ifdef X .. #endif
  guards      !ifdef G !eof -> #ifndef G .. #endif around the whole file
  symbols     NAME = expr  ->  NAME:  .equ  expr
  macros      !macro n .a { .. } -> n .macro a .. .endm, with .a -> \\a
  calls       +name args -> name args
  labels      bare column-0 label -> label:; `label !dir` splits onto two lines
  byte ops    #<x -> #.byte0 (x), #>x -> #.byte1 (x), ^(x) -> .byte2 (x)
  shifts      >>> (logical) -> >>   (exact here; all operands non-negative)
  blocks      !zone / !addr braces are dropped; a brace stack tracks which
              construct each '}' closes
  !for        UNROLLED by evaluating the expression in Python (see expand_for)
  !if         assertions dropped; the one code-generating case is in PATCHES
  direct page dp: is added wherever ACME would have picked direct-page
              addressing (see collect_zp / direct_page) -- required for
              correctness, not just size
  anon labels ACME's column-0 '+' / '-' runs become generated unique names
              (see resolve_anon) -- 50 definitions, 79 references
  code/data   data directives, and the labels naming them, are moved into a
              `data` section (see split_sections) so they land in bank $00
  code refs   references to LABELS get Calypsi's .word0, so the linker takes
              the low 16 bits of a bank-$01 address (see word0_refs)

Every emitted file also gets a .rtmodel header and a `.section code`. Neither
is cosmetic: without .rtmodel ln65816 refuses the object, and without .section
the assembler silently drops all the code (see the SECTION comment below).

STATUS: the whole tree converts and assembles. 78 modules, and with every
X16_USE_* gate enabled the lot assembles through the root include with ZERO
diagnostics into a ~280 KB object. Nothing is hand-ported: SKIP is empty,
where the other three targets each need three hand-written modules.

It also LINKS. A program using the library links to a loadable image with
runtime/x816-lib.scm, and a link that references one entry point in each of
the 66 modules produces a 40,797-byte image placed exactly per X816_Core
doc/MEMORY_MAP.md -- library variables and tables in bank $00 with the direct
page and stack, code at $01:0000+ in SDRAM.

Not done yet: the C wrapper on top.
"""
import math
import re
import sys
from pathlib import Path

# Nothing is hand-maintained: !for is unrolled by evaluating it (see
# expand_for), the macro layer converts once single-line !if is dropped, and
# the root include is just !source lines. Every other acme2*.py target needs
# three hand-ported modules; this one needs none.
SKIP = set()

RTMODEL = (
    '              .rtmodel version, "1"\n'
    '              .rtmodel core, "65816"\n'
    '              .rtmodel codeModel, "large"\n'
    '              .rtmodel dataModel, "small"\n'
    '              .rtmodel huge, "0"\n'
)

# WITHOUT THIS THE TREE ASSEMBLES CLEAN AND PRODUCES NOTHING.
#
# as65816 does not require a .section, and does not warn when one is missing --
# it quietly drops the instructions into .rodata and does not even record the
# labels in the symbol table. Verified directly: a two-instruction file with no
# .section assembles with rc=0 and yields an object whose only sections are
# .rodata and debug info, with no `foo` symbol. Zero diagnostics is therefore
# NOT evidence that the conversion worked; the object has to be inspected.
#
# One `code` section per module matches ACME, which likewise interleaves the
# routines with their state (`d_ac !fill 8, 0`). That means the section is
# written at run time, so the linker script must place it in RAM -- on X816
# everything is RAM, so there is nothing to reconcile.
SECTION = '              .section code\n'

# THE ONE THING THIS TARGET DISAGREES WITH THE ACME TREE ABOUT.
#
# ACME assembles code and data into a single image, so a module's
# constants are wherever that image was loaded -- on X816, bank $01,
# reached with DBR $00, which is not where a plain read looks. The ACME
# source therefore copies them out of the code bank before use, and says
# so at util/double.asm's +dconst.
#
# Here a linker splits the two: x816-lib.scm puts data in bank $00, keeps
# DBR $00, and has cstartup copy the initial values down at startup. The
# label already names a bank $00 address, reading it through DBR is
# exactly right, and copying it out of the code bank would be the bug --
# it would read bank $01 at a bank $00 address and find nothing to do
# with the constant.
#
# So every generated module says which shape it is, at the top, in
# writing. It is emitted rather than left to a command-line -D because
# forgetting produces no diagnostic at all: the arithmetic simply comes
# out wrong, and mostly comes out zero, which compares equal to other
# zeros and looks like a pass.
# A #define, not an .equ: the ACME source spells this !ifdef, which lands
# here as the C preprocessor's #ifdef, and that tests preprocessor macros
# rather than assembler symbols. An .equ would assemble happily and never
# be seen by the conditional it exists for.
#
# Nothing follows the 1 on that line. A ';' comment is the ASSEMBLER's,
# not the preprocessor's, so anything after the replacement text becomes
# part of the replacement text -- harmless while the symbol is only ever
# tested by #ifdef, and a trap the first time somebody expands it.
LINKED_DATA = ('; a linker put data in bank $00 -- see acme2calypsi.py\n'
               '#define X16_LINKED_DATA 1\n')

# ---------------------------------------------------------------------------
# numeric literals
# ---------------------------------------------------------------------------
# Only outside strings and comments. '$' also appears as ACME's program-counter
# symbol, but x16lib does not use it that way, so a plain literal match is safe.
HEX = re.compile(r'\$([0-9A-Fa-f]+)')
BIN = re.compile(r'%([01]+)\b')


def split_code_comment(line):
    """Return (code, comment) splitting at the first ';' outside a string."""
    inq = False
    for i, c in enumerate(line):
        if c == '"':
            inq = not inq
        elif c == ';' and not inq:
            return line[:i], line[i:]
    return line, ""


def numbers(code):
    """Rewrite $hex and %binary outside string literals."""
    out, buf, inq = "", "", False
    for c in code:
        if c == '"':
            if inq:                       # closing quote: flush verbatim
                out += buf + c
                buf, inq = "", False
            else:
                out += BIN.sub(r'0b\1', HEX.sub(r'0x\1', buf))
                buf, inq = "", True
                out += c
            continue
        buf += c
    if inq:                               # unterminated: leave alone
        return out + buf
    return out + BIN.sub(r'0b\1', HEX.sub(r'0x\1', buf))


# ---------------------------------------------------------------------------
# line rules
# ---------------------------------------------------------------------------
# A label is a bare identifier at COLUMN 0. Testing the stripped line instead
# would match any bare mnemonic -- accumulator-mode `asl` became `asl:`.
LABEL = re.compile(r'^([A-Za-z_][A-Za-z0-9_]*)\s*$')
# ACME writes accumulator mode bare; Calypsi wants an explicit operand, as its
# own library sources do (`asl     a`).
ACCUM = re.compile(r'^(\s+)(asl|lsr|rol|ror|inc|dec)\s*$', re.I)
EQUATE = re.compile(r'^([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.+)$')
MACRO_DEF = re.compile(r'^!macro\s+([A-Za-z_][A-Za-z0-9_]*)\s*(.*?)\s*\{\s*$')
MACRO_CALL = re.compile(r'^(\s*)\+([A-Za-z_][A-Za-z0-9_]*)\s*(.*)$')
IFDEF = re.compile(r'^!(ifdef|ifndef)\s+([A-Za-z_][A-Za-z0-9_]*)\s*\{\s*$')
ZONE = re.compile(r'^!zone\b.*\{\s*$')
ADDR = re.compile(r'^!addr\b.*\{\s*$')
SOURCE = re.compile(r'^!source\s+"([^"]+)"')
FILL = re.compile(r'^(\s*)!fill\s+(.+)$')


def split_statements(line):
    """Split ACME's ' : ' statement separator, outside quotes and comments."""
    out, cur, i, inq = [], "", 0, False
    while i < len(line):
        c = line[i]
        if c == '"':
            inq = not inq
        if c == ';' and not inq:
            cur += line[i:]
            break
        if not inq and line[i:i + 3] == ' : ':
            out.append(cur)
            cur = ""
            i += 3
            continue
        cur += c
        i += 1
    out.append(cur)
    if len(out) == 1:
        return [line]
    indent = re.match(r'\s*', out[0]).group(0)
    return [out[0]] + [indent + p.strip() for p in out[1:]]


# ACME's unary low/high byte operators. Calypsi spells these .byte0/.byte1 --
# they are link-time relocation operators, so they must sit at the top level of
# the expression, and as65816 will not parse a bare sign after one: `.byte0 -32`
# is rejected, `.byte0 (-32)` is accepted. So the whole operand is parenthesised
# rather than only the cases that need it. Only the '#<' / '#>' immediate forms
# occur in this tree, which conveniently cannot collide with '>>' or with a
# comparison, and an immediate operand always runs to the end of the statement.
LOWBYTE = re.compile(r'#<\s*(.+)$')
HIGHBYTE = re.compile(r'#>\s*(.+)$')
# ACME's third unary byte operator is '^' -- the BANK byte, not exclusive-or
# (ACME spells that XOR). It always appears as '^(' in this tree; every other
# '^' is inside a comment writing e^x or 2^n. Unlike '<' and '>' it is used
# mid-expression, e.g. `#((^(.addr)) & VERA_ADDR_H_BANK) | ((.inc) << 4)`, so
# only the operator itself is rewritten and the existing parentheses are kept.
BANKBYTE = re.compile(r'\^(\()')

# ACME distinguishes '>>' (arithmetic, sign-propagating) from '>>>' (logical).
# Calypsi has only '>>'. All three uses in this tree shift a non-negative
# address constant, where the two are identical, so the mapping is exact rather
# than approximate -- worth stating, since it would NOT be for a negative
# operand.
LSHIFT3 = re.compile(r'>>>')

# `!addr NAME = expr` without a block -- ACME's "this symbol is an address"
# hint on a single equate. It carries no meaning for Calypsi, so the prefix is
# simply dropped and the equate converts as usual.
ADDR_EQU = re.compile(r'^!addr\s+(.+)$')

# ACME lets a column-0 label share its line with a directive -- `d_ac !fill 8, 0`
# -- and 752 lines in this tree do. Calypsi is happy with `label:` on its own
# line, so the two are split. Only a '!' continuation is split: a column-0
# identifier followed by a bare mnemonic would be ambiguous with an instruction,
# and this tree has none.
LABEL_DIR = re.compile(r'^([A-Za-z_][A-Za-z0-9_]*)[ \t]+(![A-Za-z].*)$')

# `!if expr { !error "..." }` -- two build-time assertions in core/const_zp.asm.
# as65816 has NO assembler-level conditional (.if/.error are unknown
# instructions; verified), and the C preprocessor cannot see .equ symbols, so
# `#if X16_ZP < 0x22` would silently read X16_ZP as 0 and fire spuriously. The
# assertion is therefore dropped and recorded as a comment rather than
# mistranslated into something that reports the opposite of the truth.
IF_BLOCK = re.compile(r'^!if\b(.*)\{\s*$')
# Calypsi takes the target from --core=65816 on the command line.
CPU = re.compile(r'^!cpu\b')

# ACME's include guard: "if this symbol already exists, stop reading the file".
# There is no early-out in the C preprocessor, so it becomes the C idiom --
# #ifndef at the top, #endif appended at the bottom. Getting this wrong is not
# subtle: without it every constants file is included five times over and every
# symbol is a duplicate definition.
GUARD = re.compile(r'^!ifdef\s+([A-Za-z_]\w*)\s+!eof\s*$')


# ---------------------------------------------------------------------------
# !if -- assertions are dropped, code-generating ones are patched
# ---------------------------------------------------------------------------
# as65816 has NO assembler-level conditional. Verified by probing every
# plausible spelling -- .if .ifeq .ifne .cond .iif .ifdef .ifndef .else .endif
# .endc .exitm .error .fail .assert -- against as65816 5.18: every one comes
# back "unknown instruction". Conditionals exist only in the C preprocessor,
# which runs before the assembler and so cannot see .equ symbols OR macro
# arguments. So an ACME !if has to be resolved here, at conversion time.
#
# Six of the seven !if blocks in the tree are `!if cond { !error "..." }`
# build-time assertions with no run-time effect; those are dropped and left as
# a comment. The seventh GENERATES CODE:
#
#     !macro vera_addrsel .port {
#         lda #VERA_CTRL_ADDRSEL
#         !if .port = 0 { trb VERA_CTRL } else { tsb VERA_CTRL }
#     }
#
# Dropping that would silently emit a macro that loads a constant and does
# nothing with it -- the worst kind of failure, because it assembles clean.
# It is patched instead. VERA_CTRL_ADDRSEL is bit 0 (%00000001, const_vera.asm
# line 126), so clearing the bit and OR-ing the port number in is exactly
# equivalent for the port in {0,1} that the macro's contract allows, with the
# same "clobbers A and flags" contract and one instruction more.
#
# The replacement pastes the port literal into the macro NAME. as65816 does
# support that -- `sel\port` with `sel 0` expands `sel0`, and `sel 2` fails with
# "unknown instruction: sel2", which proves the argument is substituted before
# the name is looked up. There must be NO SPACE before the backslash: `sel
# \port` is parsed as the macro `sel` called with argument `\port`, which
# recurses into itself and hangs the assembler rather than reporting anything.
#
# Pasting is used in preference to the obvious branch-free rewrite
# (lda/and/ora/sta, which is exactly equivalent since ADDRSEL is bit 0) because
# that is 10 bytes against the original 5, and the 5 extra bytes per call
# pushed a `bcs` in audio/zsm.s past the 8-bit branch range. This version is
# byte-for-byte what ACME emits.
#
# These patches apply to the GENERATED text, not the ACME source, because what
# they are fixing is the output of a construct the converter cannot express.
PATCHES = {
    "core/macros.asm": [(
        """vera_addrsel  .macro  port
    lda #VERA_CTRL_ADDRSEL
; dropped ACME assertion -- as65816 has no assembler-level conditional:
;     !if .port = 0 { ... }
              .endm""",
        r"""; PATCHED by acme2calypsi.py -- see PATCHES in tools/acme2calypsi.py.
vera_addrsel0 .macro
    lda #VERA_CTRL_ADDRSEL
    trb VERA_CTRL           ; clear bit 0
              .endm

vera_addrsel1 .macro
    lda #VERA_CTRL_ADDRSEL
    tsb VERA_CTRL           ; set bit 0
              .endm

vera_addrsel  .macro  port
    vera_addrsel\port
              .endm""",
    )],
}

# The !if conditions that a PATCHES entry is known to take care of. Any other
# code-generating !if stops the conversion; see check_if_blocks.
PATCHED_IF = {
    "core/macros.asm": {".port = 0"},
}

ERROR_ONLY = re.compile(r'^\s*!error\b')


def apply_patches(text, rel):
    for old, new in PATCHES.get(rel, []):
        if old not in text:
            raise SystemExit(
                "%s: patch no longer applies -- the generated output changed.\n"
                "Re-check the construct and update PATCHES." % rel)
        text = text.replace(old, new)
    return text


def check_if_blocks(text, rel):
    """Refuse to silently drop an !if that generates code.

    Dropping an assertion is safe; dropping a code-generating conditional
    produces an object that assembles clean and behaves wrongly. Anything whose
    body is not purely !error stops the conversion so a PATCHES entry gets
    written instead.
    """
    allowed = PATCHED_IF.get(rel, set())
    lines, depth, patched = text.split("\n"), 0, False
    for n, line in enumerate(lines, 1):
        s = line.strip()
        if depth:
            if s == "}":
                depth -= 1
                continue
            if s and not s.startswith(";") and not ERROR_ONLY.match(s) \
                    and not patched:
                raise SystemExit(
                    "%s:%d: !if block generates code, and as65816 has no "
                    "assembler conditional:\n    %s\nAdd a PATCHES entry."
                    % (rel, n, s))
        else:
            m = IF_BLOCK.match(s)
            if m:
                depth += 1
                patched = m.group(1).strip() in allowed


# ---------------------------------------------------------------------------
# ACME anonymous labels
# ---------------------------------------------------------------------------
# ACME lets a label be written as a run of '+' or '-' at column 0, referenced
# by the same run: `beq +` jumps to the next '+' forward, `bra -` to the most
# recent '-' backward, `++` and `--` to the second one out, and so on. There
# are 50 definitions and 79 references in this tree. Calypsi has nothing
# equivalent, so each definition becomes a generated unique label.
#
# These went unnoticed for a long time because they ASSEMBLE: as65816 accepts
# `+   jsr foo` without complaint. The failure only showed up at link time,
# because a definition sharing its line with an instruction hid that
# instruction from the operand rules -- the jsr never got its .word0 and the
# linker rejected the 24-bit address.
ANON_DEF = re.compile(r'^([+-]+)([ \t]+.*)?$')
ANON_REF = re.compile(r'^(\s+[A-Za-z]{3}\s+)([+-]+)(\s*(?:;.*)?)$')
ANON_NAME = re.compile(r'_anon\d+$')


def resolve_anon(text, stem):
    """Turn ACME's '+' / '-' anonymous labels into generated unique names."""
    lines = text.split("\n")
    defs = []                              # (line index, token, name)
    for i, line in enumerate(lines):
        code, _ = split_code_comment(line)
        m = ANON_DEF.match(code.rstrip())
        if m:
            defs.append((i, m.group(1), "%s_anon%d" % (stem, len(defs))))
    if not defs:
        return text

    out = []
    for i, line in enumerate(lines):
        code, comment = split_code_comment(line)
        m = ANON_DEF.match(code.rstrip())
        if m:
            name = next(n for j, _, n in defs if j == i)
            out.append(name)               # column 0: the label rule sees it
            rest = (m.group(2) or "").rstrip()
            if rest or comment:
                out.append((rest or "    ") + comment)
            continue
        m = ANON_REF.match(code)
        if m:
            tok = m.group(2)
            if tok[0] == '+':              # nearest matching token forward
                cand = [n for j, t, n in defs if j > i and t == tok]
            else:                          # nearest matching token backward
                cand = [n for j, t, n in defs if j < i and t == tok][-1:]
            if cand:
                out.append(m.group(1) + cand[0] + m.group(3) + comment)
                continue
        out.append(line)
    return "\n".join(out)


def expand_braces(text):
    """Split ACME's single-line `!dir X { body }` into open/body/close lines.

    Over a hundred lines in this tree fold a whole block onto one line --
    include guards (`!ifdef X { !source "y.asm" }`) and the assertions inside
    macro bodies. Splitting them here means the block machinery in convert()
    only ever has to deal with the multi-line form.

    Restricted to lines starting with '!' at column 0, which is what every real
    case looks like; that also makes it impossible to mistake a brace inside a
    string literal for a block delimiter.
    """
    out = []
    for line in text.split("\n"):
        out.extend(_expand_line(line))
    return "\n".join(out)


def _expand_line(line):
    code, comment = split_code_comment(line)
    body = code.strip()
    if not body.startswith("!") or "{" not in body or not body.endswith("}"):
        return [line]
    indent = re.match(r'\s*', code).group(0)
    head, inner = body.split("{", 1)
    inner = inner[:inner.rfind("}")].strip()
    opener = indent + head.rstrip() + " {"
    if comment:
        opener += "  " + comment
    return [opener] + _expand_line(indent + "    " + inner) + [indent + "}"]


# ---------------------------------------------------------------------------
# !for -- evaluated, not translated
# ---------------------------------------------------------------------------
# util/math.asm builds its sine and arctangent tables with ACME's assemble-time
# floating point:
#
#     !for i, 0, 255 {
#         !byte int(sin(float(i) * 3.14159265358979 / 128.0) * 127.0 + 128.5) - 128
#     }
#
# Calypsi's assembler has no float and no !for, so the loop is UNROLLED HERE by
# evaluating the same expression in Python and emitting literal bytes. That is
# the whole reason util/math.asm no longer needs hand-porting: transcribing 289
# table entries by hand would be a source of silent, hard-to-spot arithmetic
# drift, whereas this is the identical formula on identical IEEE doubles.
#
# ACME's int() truncates toward zero, which is exactly Python's int() on a
# float -- including for negatives, which this table relies on.
FOR_OPEN = re.compile(r'^!for\s+([A-Za-z_]\w*)\s*,\s*(.+?)\s*,\s*(.+?)\s*\{\s*$')
FOR_BODY = re.compile(r'^\s*!(byte|word)\s+(.+?)\s*$')

ACME_FUNCS = {
    "int": int, "float": float,
    "sin": math.sin, "cos": math.cos, "tan": math.tan,
    "arcsin": math.asin, "arccos": math.acos, "arctan": math.atan,
    "sqrt": math.sqrt,
}


def expand_for(text):
    """Unroll `!for v, lo, hi { !byte expr }` into literal data lines."""
    lines = text.split("\n")
    out, i = [], 0
    while i < len(lines):
        m = FOR_OPEN.match(lines[i].strip())
        if not m:
            out.append(lines[i])
            i += 1
            continue
        var, lo, hi = m.group(1), m.group(2), m.group(3)
        body, i = [], i + 1
        while i < len(lines) and lines[i].strip() != "}":
            body.append(lines[i])
            i += 1
        i += 1                                   # step over the '}'
        py = lambda e: HEX.sub(r'0x\1', e)       # ACME $1F -> Python 0x1F
        lo_v = int(eval(py(lo), {"__builtins__": {}}, dict(ACME_FUNCS)))
        hi_v = int(eval(py(hi), {"__builtins__": {}}, dict(ACME_FUNCS)))
        out.append("; !for %s, %s, %s -- unrolled by acme2calypsi.py" % (var, lo, hi))
        for b in body:
            bm = FOR_BODY.match(b)
            if not bm:
                out.append(b)
                continue
            width = 0xFF if bm.group(1) == "byte" else 0xFFFF
            vals = []
            for n in range(lo_v, hi_v + 1):
                env = dict(ACME_FUNCS)
                env[var] = n
                vals.append(int(eval(py(bm.group(2)), {"__builtins__": {}}, env))
                            & width)
            # ACME truncates a signed result to the data width; the mask above
            # does the same, so the emitted bytes are bit-identical.
            fmt = "0x%02X" if width == 0xFF else "0x%04X"
            per = 8
            # Emitted as ACME `!byte`, not Calypsi `.byte`: expand_for runs
            # BEFORE the line rules, and the zone-local rewrite (`.name` ->
            # `stem_name`) would otherwise turn `.byte` into `math_byte`. Going
            # back through the normal !byte -> .byte rule keeps one code path.
            for k in range(0, len(vals), per):
                out.append("              !%s   %s"
                           % (bm.group(1),
                              ", ".join(fmt % v for v in vals[k:k + per])))
    return "\n".join(out)


# ---------------------------------------------------------------------------
# direct page
# ---------------------------------------------------------------------------
# ACME chooses direct-page addressing for any operand whose VALUE is under
# $100. as65816 does the same for a literal -- `lda 0x2A` assembles to 2 bytes
# -- but NOT for a symbol: `zp: .equ 0x2A` followed by `lda zp` assembles to
# the 3-byte absolute form. The guide's answer is the `dp:` prefix (there are
# `abs:` and `long:` too), and `lda dp:zp` is 2 bytes again.
#
# This is not a micro-optimisation. x16lib reaches for its zero-page pointers
# constantly, so one extra byte per access inflates every routine, and it broke
# the build outright: five branches -- in audio/zsm.s, gfx/bitmap8h.s,
# ui/filepick.s, util/double.s and util/tscrunch.s -- overflowed the 8-bit
# range that they fit comfortably within under ACME.
#
# Sizes below were measured, not assumed, using the assembler's own range
# error as a ruler (put the instruction before `.space 126` / `bra back` and
# read the reported overflow):
#     nop 1   lda 0x2A 2   lda zp 3   lda dp:zp 2   lda (zp),y 2
# Indirect forms are already at their shortest and `dp:` is rejected there, so
# only the direct forms are prefixed.
DP_LIMIT = 0x100

# Mnemonics with a direct-page addressing mode. Restricting by mnemonic keeps
# `dp:` off instructions that have no such mode, where it is an error.
DP_CAPABLE = {
    "lda", "sta", "ldx", "ldy", "stx", "sty", "stz",
    "adc", "sbc", "and", "ora", "eor", "cmp", "cpx", "cpy",
    "inc", "dec", "asl", "lsr", "rol", "ror", "bit", "trb", "tsb",
}
DP_INSN = re.compile(r'^(\s+)([A-Za-z]{3})(\s+)([A-Za-z_]\w*)(.*)$')
EQU_LINE = re.compile(r'^(?:!addr\s+)?([A-Za-z_]\w*)\s*=\s*([^;]+?)\s*$')


def collect_zp(src):
    """Resolve every ACME equate in the tree; return those under $100.

    A fixpoint loop, because the definitions chain -- X16_P0 = X16_ZP + 0,
    d_ptr = X16_TPTR0, and so on -- and the files are not in dependency order.
    Anything that will not evaluate (it references a label, or ACME's '*') is
    simply left out, which is the safe direction: a missing dp: costs a byte,
    a wrong one would change the addressing mode.
    """
    raw = {}
    for f in sorted(src.rglob("*.asm")):
        text = f.read_text(encoding="utf-8", errors="replace")
        # Brace-expand first. The root of the whole zero-page map is written
        # `!ifndef X16_ZP { X16_ZP = $22 }`, all on one line, so a scan that
        # required column 0 missed it -- and with X16_ZP unresolved the entire
        # X16_P0..X16_T7 chain that depends on it stayed unresolved too. That
        # is why `sta X16_P0` was still assembling as 3-byte absolute.
        # Indentation is not a useful filter here: after expansion the real
        # definitions are indented, and the only indented equates in this tree
        # are the X16_USE_* gates, which are never used as operands.
        for line in expand_braces(text).split("\n"):
            code, _ = split_code_comment(line)
            m = EQU_LINE.match(code.strip())
            if m:
                raw.setdefault(m.group(1), m.group(2))
    vals = {}
    for _ in range(16):                      # converges in ~4; 16 is slack
        progress = False
        for name, expr in raw.items():
            if name in vals:
                continue
            py = BIN.sub(r'0b\1', HEX.sub(r'0x\1', expr))
            try:
                vals[name] = int(eval(py, {"__builtins__": {}}, dict(vals)))
                progress = True
            except Exception:
                pass
        if not progress:
            break
    # Second return value is every equate NAME, resolved or not. That is the
    # set of constants, so anything outside it is a label -- which is what
    # word0_refs keys off.
    # Only RESOLVED equates count as constants. An equate that aliases a
    # label -- gfx/shapes.asm has SHP_PSET = gfx2h_pset and friends -- does
    # not evaluate, and must still be relocated, so it is deliberately left
    # out of this set and picks up .word0 like any other label reference.
    return ({n for n, v in vals.items() if 0 <= v < DP_LIMIT}, set(vals))


def direct_page(code, zp):
    """Prefix `dp:` on a direct-page operand, matching what ACME would emit."""
    m = DP_INSN.match(code)
    if not m or m.group(2).lower() not in DP_CAPABLE or m.group(4) not in zp:
        return code
    return "%s%s%sdp:%s%s" % (m.group(1), m.group(2), m.group(3),
                              m.group(4), m.group(5))


# ---------------------------------------------------------------------------
# code / data split, and bank-relative code references
# ---------------------------------------------------------------------------
# x16lib is 16-bit-bank code in three ways at once: it reaches its data with
# 16-bit absolute addressing (through DBR), calls itself with 16-bit jsr
# (through PBR), and touches I/O at $9F00-$9FFF with 16-bit absolute addressing
# as well (through DBR again). On X816 those cannot all hold with code and data
# in the same SDRAM bank, because the I/O page is in bank $00 and 114 of the
# library's I/O accesses use instructions the 65816 has no absolute-long form
# for (stz 45, trb 31, tsb 19, stx/sty/bit/ldx 15) -- so `long:` cannot rescue
# them and DBR must be $00.
#
# The split that does work, and that matches X816's documented memory map:
#
#   code        $01:0000+  SDRAM      16-bit jsr, bank supplied by PBR = $01
#   data        bank $00              16-bit absolute, DBR = $00
#   I/O page    bank $00              16-bit absolute, DBR = $00 -- unchanged
#
# Data is why this is cheap: the library's entire mutable and table storage
# across all 75 modules is 3,831 bytes, and a real program enables only a few
# modules. Bank $00 is where the map already puts "direct page, stack, OS
# variables".
#
# Code references then need the low 16 bits of a bank-$01 address, which is
# Calypsi's `.word0` relocation operator. It is applied to any symbol that is
# NOT a known equate -- i.e. to labels rather than constants. Applying it to a
# bank-$00 symbol would be harmless anyway (the low 16 bits of a bank-$00
# address are the address), so the direction of any misclassification is safe.
DATA_DIR = re.compile(r'^\s+\.(?:byte|word|space|ascii|asciz)\b')
BARE_LABEL = re.compile(r'^[A-Za-z_]\w*:\s*$')
# A data directive the source marks as an inline opcode -- see split_sections.
INLINE_OPCODE = re.compile(r';.*\bINLINE OPCODE\b')
# The phb/phk/plb triple specifically -- the one that POINTS DBR at the image.
# The lone $ab that closes it matches INLINE_OPCODE but not this, which is what
# lets image_tables() tell an opening marker from a closing one.
DBR_TO_PBR = re.compile(r'0x8b\s*,\s*0x4b\s*,\s*0xab')
# A symbol used as an absolute operand: `lda .word0 (fp_consts),x`.
IMAGE_OPERAND = re.compile(r'\.word0\s*\(\s*([A-Za-z_]\w*)\s*\)')

NEUTRAL = re.compile(r'^\s*(;|#|$)|\.(equ|rtmodel|section|macro|endm)\b')
MACRO_OPEN = re.compile(r'^\S+\s+\.macro\b')
MACRO_CLOSE = re.compile(r'^\s+\.endm\b')

# Applies to any absolute reference to a LABEL, not just to calls. A broad
# link test -- referencing one entry point in each of the 66 modules -- showed
# that restricting this to jsr/jmp was not enough: the library also patches
# itself, e.g. `lda gfx4l_init` / `lda gfx4l_init+1` to read a routine's
# address, and blit routines index code labels directly.
#
# These mnemonics must NOT be touched. Branches are PC-relative, so a .word0
# would corrupt them, and jml/jsl/per already carry a full 24-bit or
# PC-relative target.
NO_WORD0 = {"bcc", "bcs", "beq", "bne", "bmi", "bpl", "bvc", "bvs",
            "bra", "brl", "jml", "jsl", "per"}

# An instruction whose operand starts with a bare identifier. Immediate (#),
# indirect ( and [, and anything already prefixed (dp:, abs:, long:, .word0)
# are excluded by requiring the operand to begin with an identifier character.
INSN_REF = re.compile(
    r'^(\s+)([A-Za-z]{3})(\s+)([A-Za-z_]\w*)((?:\s*[-+]\s*[^,;]+)?)'
    r'((?:\s*,\s*[XxYy])?)(\s*(?:;.*)?)$')
# A .word holding a label -- jump tables, and pointers to routines.
WORD_REF = re.compile(
    r'^(\s+\.word\s+)([A-Za-z_]\w*)((?:\s*[-+]\s*[^,;]+)?)(\s*(?:;.*)?)$')


def word0_refs(code, equates):
    """Take the low 16 bits of a label reference; leave constants alone.

    Safe in the direction that matters: `.word0` of a bank-$00 symbol is that
    symbol's own address, so applying it to a data reference that did not need
    it changes nothing. Only equates are skipped, because those are constants
    -- KERNAL entry points, I/O registers -- and are not relocated at all.
    """
    m = INSN_REF.match(code)
    # `a` is the accumulator operand that the ACCUM rule adds, not a symbol.
    if (m and m.group(2).lower() not in NO_WORD0
            and m.group(4).lower() != "a"
            and m.group(4) not in equates):
        return "%s%s%s.word0 (%s%s)%s%s" % (
            m.group(1), m.group(2), m.group(3), m.group(4),
            m.group(5).strip(), m.group(6), m.group(7))
    m = WORD_REF.match(code)
    if m and m.group(2) not in equates:
        return "%s.word0 (%s%s)%s" % (m.group(1), m.group(2),
                                      m.group(3).strip(), m.group(4))
    return code


def image_tables(lines):
    """Symbols that are read with DBR pointed at the PROGRAM BANK.

    An `!byte $8b,$4b,$ab` marked INLINE OPCODE is phb/phk/plb: it points DBR
    at the image's own bank for exactly one load, and the matching `$ab` puts
    it back. Whatever that load names therefore has to BE in the image -- it is
    the entire reason the sequence is written.

    So a table between those two markers is not library state. It is part of
    the program, and split_sections must leave it in `code` rather than sweep
    it into bank $00 with the variables.

    THIS IS INFERRED, NOT DECLARED, and deliberately so. The alternative was a
    second marker in the ACME sources naming each table, which is a marker that
    can be forgotten -- and forgetting it is silent: the load still assembles,
    still runs, and reads whatever the image happens to hold at a bank $00
    symbol's offset. Reading the answer out of the sequence that creates the
    requirement means the two cannot drift.

    Only symbol operands count. `lda (fpstr),y` between the same markers is an
    indirect through a pointer the CALLER supplies, which is that caller's
    problem and is documented as such in util/float.asm.
    """
    syms, banked = set(), False
    for line in lines:
        if INLINE_OPCODE.search(line):
            banked = bool(DBR_TO_PBR.search(line))   # opening triple, or the
            continue                                 # closing plb that ends it
        if banked:
            syms.update(IMAGE_OPERAND.findall(line))
    return syms


def split_sections(lines):
    """Move data directives, and the labels that name them, into `data`.

    The converter emits one `.section code` per module and everything lands in
    it -- which is what ACME does, but it would put the library's state in
    bank $01 where a 16-bit data reference cannot reach it. This runs over the
    finished lines and switches section around each run of data.

    A label is classified by what FOLLOWS it, skipping blanks, comments and
    preprocessor lines, so that `keymap:` goes with its table rather than with
    whatever code happens to precede it.

    EXCEPT for the tables image_tables() finds, which stay in `code`. Sweeping
    those into `data` splits one mechanism across two banks: the phb/phk/plb
    stays behind as code (it is marked INLINE OPCODE) while the bytes it exists
    to read move to bank $00, so the load points DBR at the image and then
    reads a bank $00 offset out of it. That is not a crash. util/float.asm's
    constant table was converted this way and every transcendental, every
    scaled conversion and f_to_str returned confident nonsense; audio/notes.asm
    and audio/ym.asm carry the same shape.

    Macro bodies are left completely alone: a `.section` inside a macro would
    be re-executed at every expansion, switching the caller's section.
    """
    keep_in_code = image_tables(lines)
    n = len(lines)
    cls = [None] * n                       # 'data', 'code' or None (neutral)
    in_macro = False
    for i, line in enumerate(lines):
        if MACRO_OPEN.match(line):
            in_macro = True
        if in_macro:
            if MACRO_CLOSE.match(line):
                in_macro = False
            continue
        if NEUTRAL.match(line) or BARE_LABEL.match(line):
            continue
        # A `!byte` marked INLINE OPCODE is an INSTRUCTION, not data: it is how
        # the ACME sources spell a 65816 opcode that ACME has no mnemonic for
        # under !cpu 65c02 -- phb/phk/plb, which point DBR at the program's own
        # bank for one load. Swept into `data` with the tables, those bytes
        # would be assembled somewhere else entirely and the load left reading
        # bank $00. It does not fault; it reads the wrong memory. See
        # X816_Library audio/notes.asm and audio/ym.asm, and the README's
        # "The data bank" for why those reads have to be banked at all.
        if INLINE_OPCODE.search(line):
            cls[i] = 'code'
            continue
        cls[i] = 'data' if DATA_DIR.match(line) else 'code'

    # A label takes the class of the next classified line.
    for i in range(n - 1, -1, -1):
        if BARE_LABEL.match(lines[i]):
            nxt = next((cls[j] for j in range(i + 1, n) if cls[j]), None)
            cls[i] = nxt

    # ...unless it names a table read out of the image, in which case the label
    # and the run of data under it stay put. The run ends at the first thing
    # that is not data -- an instruction, or the next label, which starts a
    # table of its own and gets judged on its own name.
    for i, line in enumerate(lines):
        m = BARE_LABEL.match(line)
        if not m or line.rstrip()[:-1] not in keep_in_code:
            continue
        cls[i] = 'code'
        for j in range(i + 1, n):
            if BARE_LABEL.match(lines[j]) or cls[j] == 'code':
                break
            if cls[j] == 'data':
                cls[j] = 'code'

    # `data` is a reserved section TYPE in Calypsi, so a section that is also
    # named `data` has to be spelt `data,data` -- as65816 rejects the bare form
    # with "section type by itself not allowed". `code` needs no such spelling.
    SPELL = {'code': 'code', 'data': 'data,data'}
    out, cur = [], 'code'                  # the module header opened `code`
    for i, line in enumerate(lines):
        if cls[i] and cls[i] != cur:
            cur = cls[i]
            out.append("              .section %s" % SPELL[cur])
        out.append(line)
    return out


DATA_LINE = re.compile(r'^(\s*\.(?:byte|word)\s+)(.*)$')


def data_byteops(code):
    """`.byte <10000, <1000` -> `.byte .byte0 (10000), .byte0 (1000)`.

    ACME's `<`/`>` also appear without a leading `#` inside data lists. Scoping
    the rewrite to .byte/.word lines is what makes it safe: elsewhere in this
    tree `<` and `>` are comparisons (inside !if, and !macro bodies), and a
    blind substitution would corrupt them.
    """
    m = DATA_LINE.match(code)
    if not m or '"' in m.group(2):     # a comma inside a string must not split
        return code
    items = []
    for item in m.group(2).split(','):
        t = item.strip()
        if t[:1] in '<>' and t[1:2] not in ('', '<', '>', '='):
            op = ".byte0" if t[0] == '<' else ".byte1"
            t = "%s (%s)" % (op, t[1:].strip())
        items.append(t)
    return m.group(1) + ", ".join(items)


def convert(text, stem, rel="<input>", zp=frozenset(), equates=frozenset()):
    out = ["; Generated by tools/acme2calypsi.py from X816_Library/src_acme.",
           "; Do not edit -- change the ACME source and regenerate.",
           "",
           LINKED_DATA.rstrip("\n"),
           RTMODEL.rstrip("\n"),
           SECTION.rstrip("\n"),
           ""]
    blocks = []                            # what each '}' closes
    guarded = False                        # an include guard needs a trailing #endif
    scope = stem                           # enclosing global label, for @locals
    lines = []
    prepared = resolve_anon(expand_braces(expand_for(text)), stem)
    check_if_blocks(prepared, rel)
    for physical in prepared.split("\n"):
        lines.extend(split_statements(physical))
    for raw in lines:
        line = raw.rstrip()
        code, comment = split_code_comment(line)
        s = code.strip()

        # ---- dropped outright ----------------------------------------------
        if CPU.match(s):
            continue
        m = GUARD.match(s)
        if m:
            out.append("#ifndef %s" % m.group(1))
            guarded = True
            continue

        # ---- block openers -------------------------------------------------
        m = IFDEF.match(s)
        if m:
            out.append("#%s %s" % (m.group(1), m.group(2)))
            blocks.append("cond")
            continue
        if ZONE.match(s) or ADDR.match(s):
            blocks.append("drop")
            continue
        m = ADDR_EQU.match(s)               # `!addr NAME = expr`, no block
        if m:
            code = re.match(r'\s*', code).group(0) + m.group(1)
            s = code.strip()
        m = IF_BLOCK.match(s)
        if m:
            out.append("; dropped ACME assertion -- as65816 has no assembler-"
                       "level conditional:")
            out.append(";     !if%s{ ... }" % m.group(1))
            blocks.append("skip")
            continue
        if blocks and blocks[-1] == "skip" and s != "}":
            continue
        m = MACRO_DEF.match(s)
        if m:
            params = [p.strip().lstrip('.') for p in m.group(2).split(',') if p.strip()]
            out.append("%-13s .macro  %s" % (m.group(1), ", ".join(params)))
            blocks.append("macro")
            continue

        # ---- block closer --------------------------------------------------
        if s == "}":
            kind = blocks.pop() if blocks else "drop"
            if kind == "cond":
                out.append("#endif")
            elif kind == "macro":
                out.append("              .endm")
            continue

        # ---- ACME local labels -> flat unique globals -----------------------
        # Two forms, both DEFINED at column 0 and referenced indented:
        #   @name   cheap local, scoped to the enclosing global label
        #   .name   zone local; each file here is one zone, so the stem scopes it
        # Calypsi has neither, so both are promoted to flat names. This has to
        # run BEFORE '!byte' becomes '.byte', or the .name rewrite would eat
        # the directive it just created.
        # A column-0 label is a global whether or not a directive shares its
        # line, and ACME resets @cheap locals at EVERY global. Missing the
        # `bmx_t !byte 0` form let two different scopes both claim @ask, which
        # surfaced as `duplicate symbol: bmx_load_hires_ask`.
        m = re.match(r'^([A-Za-z_][A-Za-z0-9_]*)(\s*$|[ 	]+!)', code)
        if m and not ANON_NAME.search(m.group(1)):
            # An anonymous label does NOT open a new @cheap-local scope in
            # ACME, so the labels generated for '+' / '-' must not either --
            # letting them through re-scoped every following @local and left
            # references pointing at names like bitmap4h_anon0_aligned.
            scope = m.group(1)                 # a column-0 global label
        if blocks and blocks[-1] == "macro":
            # Inside a macro body the same '.name' spelling means a PARAMETER,
            # which Calypsi writes '\name'. Both rewrites have to happen here,
            # while directives are still spelt '!byte' -- doing the parameter
            # substitution after '!byte' became '.byte' turned the directive
            # itself into '\byte'.
            code = re.sub(r'(?<![\w.])\.([A-Za-z_][A-Za-z0-9_]*)', r'\\\1', code)
        else:
            code = re.sub(r'(?<![\w.])\.([A-Za-z_][A-Za-z0-9_]*)',
                          lambda mm: "%s_%s" % (stem, mm.group(1)), code)
        code = re.sub(r'@([A-Za-z_][A-Za-z0-9_]*)',
                      lambda mm: "%s_%s" % (scope, mm.group(1)), code)
        s = code.strip()

        # ---- label sharing a line with a directive -------------------------
        m = LABEL_DIR.match(code)
        if m:
            out.append(m.group(1) + ":")
            code = "              " + m.group(2)
            s = code.strip()

        # ---- single-line forms --------------------------------------------
        m = SOURCE.match(s)
        if m:
            # The generated tree is .s, so the include has to follow.
            out.append('#include "%s"' % re.sub(r'\.asm$', '.s', m.group(1)))
            continue
        if s in ("!eof", "!end"):
            out.append("; !eof -- ACME end-of-file; nothing to emit")
            continue
        m = FILL.match(code)
        if m:
            out.append(numbers("%s.space %s" % (m.group(1), m.group(2))) + comment)
            continue

        code = re.sub(r'^(\s*)!byte\b', r'\1.byte', code)
        code = re.sub(r'^(\s*)!word\b', r'\1.word', code)
        # A standalone !error inside an !ifdef gate (the parked-module gates
        # in x16_code.asm) must stay an error on this path too. cpp's #error
        # is verified to fire in as65816. Distinct from the !if-block
        # assertions, which are resolved at conversion time and dropped.
        code = re.sub(r'^(\s*)!error\b', r'\1#error', code)
            # .byte, not .ascii: ACME's !text takes a MIXED list -- `!text "atz", $00`
        # -- and Calypsi's .ascii accepts a bare string only ("string operand
        # expected here"). .byte takes both, so it covers every case.
        code = re.sub(r'^(\s*)!text\b', r'\1.byte', code)

        m = MACRO_CALL.match(code)
        if m:
            code = "%s%s %s" % (m.group(1), m.group(2), m.group(3))

        m = ACCUM.match(code)
        if m:
            code = "%s%s a" % (m.group(1), m.group(2))

        s = code.strip()
        m = EQUATE.match(s)
        if m and not s.startswith('!'):
            if re.match(r'X16_USE_\w+$', m.group(1)):
                # Gate symbols must live in the preprocessor domain. The
                # derivation chains in x16_code.asm are #ifdef A -> B = 1 ->
                # #ifdef B, and cpp cannot see a .equ, so as .equ the chain
                # stops after one link and whole modules silently drop out
                # (KERNEL.md 11.5). Gates are never used as operands, so no
                # assembler symbol is lost by defining them here instead.
                code = "#define %s %s" % (m.group(1), m.group(2).strip())
            else:
                code = "%-13s .equ    %s" % (m.group(1) + ":", m.group(2))
        elif LABEL.match(code):        # column 0 only -- see LABEL above
            code = s + ":"

        code = LSHIFT3.sub('>>', code)
        code = LOWBYTE.sub(lambda mm: "#.byte0 (%s)" % mm.group(1).strip(), code)
        code = HIGHBYTE.sub(lambda mm: "#.byte1 (%s)" % mm.group(1).strip(), code)
        code = BANKBYTE.sub('.byte2 \\1', code)
        code = data_byteops(code)
        code = direct_page(code, zp)
        code = word0_refs(code, equates)
        out.append(numbers(code) + comment)

    if guarded:
        out.append("#endif")               # closes the include guard
    return "\n".join(split_sections(out)) + "\n"


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__.strip().split("\n\n")[1].strip())
    src, dst = Path(sys.argv[1]), Path(sys.argv[2])
    if not src.is_dir():
        raise SystemExit(f"no such source tree: {src}")

    zp, equates = collect_zp(src)
    print(f"{len(zp)} symbols resolve below ${DP_LIMIT:X} -- these get dp:")

    n = 0
    for f in sorted(src.rglob("*.asm")):
        rel = f.relative_to(src).as_posix()
        if rel in SKIP:
            print(f"skip  {rel} (hand-maintained)")
            continue
        outp = dst / rel.replace(".asm", ".s")
        outp.parent.mkdir(parents=True, exist_ok=True)
        text = f.read_text(encoding="utf-8", errors="replace")
        outp.write_text(apply_patches(convert(text, f.stem, rel, zp, equates), rel),
                        encoding="utf-8", newline="\n")
        print(f"conv  {rel}")
        n += 1
    print(f"\n{n} modules -> {dst}")


if __name__ == "__main__":
    main()
