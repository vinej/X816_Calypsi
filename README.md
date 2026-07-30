# X816_Calypsi

Calypsi C support for the **X816** — a flat 16 MB, native-mode-only 65C816
MiSTer core.

| | |
|---|---|
| Core | <https://github.com/vinej/X816_Core> |
| Emulator | <https://github.com/vinej/X816_Emulator> |
| Assembly library | <https://github.com/vinej/X816_Library> |
| Toolchain | [Calypsi](https://www.calypsi.cc/) — see the core's `doc/TOOLCHAIN.md` |

## Why this is a separate repository

**X816_Library is the assembly library and stays that way** — ACME source of
truth plus the `acme2*.py` converters that generate the ca65, 64tass, MADS,
vasm, dasm and KickAssembler trees. Someone writing assembly never has to know
Calypsi exists.

Everything Calypsi-specific lives here instead, and the dependency runs one
way: this repo *reads* X816_Library's ACME tree and generates from it.

## Contents

```
tools/acme2calypsi.py   ACME -> Calypsi as65816 converter
src/                    generated assembly (gitignored -- regenerate it)
```

Planned, not yet written: C headers, `__simple_call` entry stubs presenting a
C-idiomatic API, the `x816-plain.scm` linker script and `cstartup.s`.

To check a conversion, assemble the whole tree through the root include with
every module selected:

```sh
{ grep -rhoE "^#ifn?def +X16_USE_[A-Z_0-9]+" src/ | awk '{print "#define "$2" 1"}' | sort -u
  echo '#include "x16.s"'; echo '#include "x16_code.s"'; } > all.s
as65816 --core=65816 -I src all.s -o all.o
```

## Generating

```sh
git clone https://github.com/vinej/X816_Library
python tools/acme2calypsi.py X816_Library/src_acme src
```

75 modules convert. `src/` is gitignored on purpose — it is a build product,
and checking it in would invite hand-edits that the next regeneration silently
discards. That is the same rule X816_Library applies to its own generated
trees.

## The dialect gap

Calypsi is a C-toolchain assembler rather than a 6502-community one, so it sits
further from ACME than the other six targets. Every rule below was checked
against the Calypsi 65816 guide 5.18 **and** against the `.s` sources that ship
in the toolchain's own `src/lib/lowlevel/` — none of it is inferred.

| ACME | Calypsi | Note |
|---|---|---|
| `$1F` | `0x1F` | **`$` is rejected outright** — "invalid operand field" |
| `%1010` | `0b1010` | |
| `!byte` / `!word` | `.byte` / `.word` | |
| `!text "s"` | `.ascii "s"` | `.asciz` for NUL-terminated |
| `!fill n, v` | `.space n, v` | |
| `!source "f"` | `#include "f"` | the C preprocessor is available; `__CALYPSI_ASSEMBLER__` is predefined |
| `!ifdef X { .. }` | `#ifdef X .. #endif` | preprocessor, not an assembler directive |
| `NAME = expr` | `NAME:  .equ  expr` | |
| `!macro n .a { .. }` | `n .macro a .. .endm` | parameters referenced as `\a` |
| `+name args` | `name args` | |
| `asl` (accumulator) | `asl a` | Calypsi wants the explicit operand |
| bare column-0 label | `label:` | |
| `!zone` / `!addr` braces | dropped | a brace stack tracks what each `}` closes |

Every emitted module also carries a `.rtmodel` header. That is not cosmetic —
without it `ln65816` refuses the object.

Two of those caused real bugs while writing the converter and are worth
knowing:

* **A label is a bare identifier at column 0**, not merely a bare identifier.
  Testing the stripped line turned accumulator-mode `asl` into a label `asl:`.
* **`$` really is fatal**, not merely unidiomatic — verified by feeding
  `lda #$41` to `as65816` and watching it reject the line.

## Three findings worth keeping

**A missing `.section` loses all your code, silently.** `as65816` does not
require one and does not warn. Without it the instructions go into `.rodata`
and the labels are not even entered in the symbol table — a two-instruction
file assembles with `rc=0` and produces an object with no code and no `foo`
symbol. The tree once assembled with *zero diagnostics* and contained nothing
but constants. **Zero diagnostics is not evidence that a conversion worked**;
the object has to be inspected.

**`dp:` is a correctness fix, not an optimisation.** ACME picks direct-page
addressing for any operand under `$100`. `as65816` does the same for a
literal (`lda 0x2A` → 2 bytes) but *not* for a symbol: with `zp: .equ 0x2A`,
`lda zp` assembles to the 3-byte absolute form. Since x16lib touches its
zero-page pointers constantly, that inflated every routine and broke the build
outright — five branches, in `audio/zsm.s`, `gfx/bitmap8h.s`, `ui/filepick.s`,
`util/double.s` and `util/tscrunch.s`, overflowed the 8-bit range they fit in
comfortably under ACME. The converter now resolves every equate in the tree to
a number (a fixpoint pass, since the definitions chain) and emits `dp:` on the
435 symbols that land below `$100`. `lda dp:X16_P0` is `a5 22`, exactly what
ACME emits; indirect forms like `lda (X16_P0),y` are already shortest and are
left alone.

**One `!if` in the tree generates code** rather than asserting, in
`vera_addrsel`. `as65816` has no assembler-level conditional at all — every
plausible spelling (`.if .ifeq .ifne .cond .iif .ifdef .else .endif .error
.assert` …) comes back "unknown instruction" — and the C preprocessor runs too
early to see either `.equ` symbols or macro arguments. Dropping it would have
emitted a macro that loads a constant and does nothing with it, which
assembles clean. It is handled by a `PATCHES` entry that pastes the port
literal into the macro *name* (`vera_addrsel\port`), which `as65816` supports
and which is byte-for-byte what ACME emits. Note there must be **no space**
before the backslash: `sel \port` is parsed as the macro calling itself and
hangs the assembler. Any *other* code-generating `!if` stops the conversion
with an error rather than being dropped silently.

## Status

The whole tree converts and assembles. 75 modules; with every `X16_USE_*` gate
enabled the lot assembles through the root include with **zero diagnostics**
into a ~280 KB object.

**Nothing is hand-ported — `SKIP` is empty.** The other six targets each need
three hand-written modules (`x16.asm`, `core/macros.asm`, `util/math.asm`);
this one needs none. `util/math.asm`'s `!for`-computed sine and arctangent
tables are *evaluated* with the same formula on the same IEEE doubles and
emitted as literal bytes, rather than transcribed by hand.

Not yet done: linking (`x816-plain.scm`), and the C wrapper on top — headers,
`__simple_call` entry stubs, and `cstartup.s`.

## Licence

The converter is MIT. Calypsi itself is closed-source and free for hobby use
only — see the core's `doc/TOOLCHAIN.md` for the terms, which bind the project
rather than just the individual.
