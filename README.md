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

## Generating

```sh
git clone https://github.com/vinej/X816_Library
python tools/acme2calypsi.py X816_Library/src_acme src
```

72 modules convert. `src/` is gitignored on purpose — it is a build product,
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

## Status

First cut. The converter runs over all 72 modules and a spot-checked module
assembles with **no syntax errors** — the only diagnostics are undefined
symbols from `core/const_zp.asm`, which is expected when assembling a module
standalone rather than through the root include.

Not yet done: assembling the whole tree, the three hand-ported modules, and the
C wrapper.

`SKIP` lists the modules that use ACME-only features and need hand-porting,
exactly as they do for the other six targets:

* `x16.asm` — the root include, hand-written per target
* `core/macros.asm` — the macro layer
* `util/math.asm` — `!for`-computed sine and arctangent tables

## Licence

The converter is MIT. Calypsi itself is closed-source and free for hobby use
only — see the core's `doc/TOOLCHAIN.md` for the terms, which bind the project
rather than just the individual.
