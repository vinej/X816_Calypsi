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
runtime/x816hdr.s       the 8-byte boot header every image starts with
runtime/x816-plain.scm  linker map for a C program
runtime/x816-lib.scm    linker map for a program using the assembly library
examples/               a C program, an assembly demo, and the runtime test
```

Planned, not yet written: C headers and `__simple_call` entry stubs presenting
a C-idiomatic API.

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
| `!text "s", $00` | `.byte "s", 0x00` | **not `.ascii`** — that takes a bare string, `!text` takes a mixed list |
| `!fill n, v` | `.space n, v` | |
| `!source "f.asm"` | `#include "f.s"` | the C preprocessor is available; `__CALYPSI_ASSEMBLER__` is predefined |
| `!ifdef G !eof` | `#ifndef G .. #endif` | ACME's early-out include guard becomes the C idiom wrapping the file |
| `!addr NAME = expr` | `NAME:  .equ  expr` | the `!addr` hint has no meaning here |
| `label !fill 8, 0` | `label:` + `.space 8, 0` | 752 lines share a label with a directive |
| `#<x` / `#>x` | `#.byte0 (x)` / `#.byte1 (x)` | always parenthesised: `.byte0 -32` is rejected |
| `^(x)` (bank byte) | `.byte2 (x)` | `^` is ACME's bank byte, **not** exclusive-or |
| `>>>` | `>>` | exact here — every operand is non-negative |
| ` : ` separator | separate lines | |
| `!for i, a, b { .. }` | literal `.byte` rows | **evaluated in Python**, not translated |
| `!if c { !error .. }` | dropped, kept as a comment | as65816 has no assembler conditional |
| `lda X16_P0` | `lda dp:X16_P0` | see below — required for correctness |
| `!ifdef X { .. }` | `#ifdef X .. #endif` | preprocessor, not an assembler directive |
| `NAME = expr` | `NAME:  .equ  expr` | |
| `!macro n .a { .. }` | `n .macro a .. .endm` | parameters referenced as `\a` |
| `+name args` | `name args` | |
| `asl` (accumulator) | `asl a` | Calypsi wants the explicit operand |
| bare column-0 label | `label:` | |
| `!zone` / `!addr` braces | dropped | a brace stack tracks what each `}` closes |
| `+` / `-` anonymous labels | generated unique names | 50 definitions, 79 references |
| `!byte`/`!word`/`!fill` and their labels | moved into a `data` section | so they land in bank `$00` — spelt `data,data`, since `data` is a reserved *type* |
| `jsr routine`, `lda routine+1` | `.word0 (routine)` | low 16 bits of a bank-`$01` address |

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

It also **links**. `runtime/x816-lib.scm` places a library program exactly per
X816_Core `doc/MEMORY_MAP.md`:

| | Lives in | Reached by |
|---|---|---|
| library + user code | `$01:0000`+ SDRAM | 16-bit `jsr`, bank from PBR=`$01` |
| library variables and tables | bank `$00` | 16-bit absolute, DBR=`$00` |
| I/O `$9F00-$9FFF` | bank `$00` | 16-bit absolute, DBR=`$00` — unchanged |

`examples/asm-lib` builds a 909-byte image in one link — no stub, no copy
step. A link referencing one entry point in each of the 66 modules produces a
40,797-byte image with bank `$00` topping out at `$00:280C` and code at
`$01:9F5C`, both well inside the map.

Calypsi's own `cstartup` runs first and does exactly what X816 needs — native
mode, stack, direct page, DBR=`$00`, and copying the library's initialised
tables out of the image into bank `$00` — so your entry point is `main()`.

**Why the split is forced, not stylistic.** x16lib is 16-bit-bank code in three
ways at once: data through DBR, internal `jsr` through PBR, and I/O at
`$9F00-$9FFF` through DBR again. Keeping code and data together in an SDRAM
bank would need DBR to be that bank, and then I/O is unreachable — 114 of the
library's I/O accesses use `stz`/`trb`/`tsb`/`stx`/`sty`/`bit`/`ldx`, which
have no absolute-long form, so `long:` cannot rescue them. Putting data and I/O
both in bank `$00` costs 3,831 bytes for the *entire* library, and a program
enables only a few modules.

## It runs

`examples/asm-lib/libtest.s` is a runtime conformance test, in the same spirit
as the core's `boot/vramtest.s`: **green screen = pass**, and a distinct colour
per failing test. It checks the four things the converter had to get right that
a clean link cannot prove:

| Test | Checks | Fails as |
|---|---|---|
| 1 | the sine table reached bank `$00` and `cstartup`'s `data_init_table` walk copied it out of the image — an EOR checksum over all 256 bytes | red |
| 2 | calls into the library resolve through `.word0` | yellow |
| 3 | `dp:` reaches the library's zero-page pointers (`lerp8` must be *exact* at both endpoints) | blue |
| 4 | the patched `vera_addrsel` does at run time what ACME's `!if` generated | magenta |

The checksum is an EOR, not a sum, deliberately: a full sine period sums to
zero either way, so a sum would pass over a table of zeroes.

```sh
cd examples/asm-lib
./run-emu.sh              # -> final frame: GREEN (0, 204, 85) at 100%
./run-emu.sh --negative   # -> final frame: RED ...  (proves it can fail)
```

Both are verified. The emulator has no usable headless mode here — `-testbench`
hooks a PC value X816 never reaches, and memory is dumped only when the PC hits
`$FFFF` — so the script reads the screen colour out of a `-gif` capture with
SDL's dummy video driver. **Not yet run on real hardware**; `LIBTEST.raw` loads
at `$01:0000` like any other image.

Not yet done: the C wrapper — headers and `__simple_call` entry stubs.

## Licence

The converter is MIT. Calypsi itself is closed-source and free for hobby use
only — see the core's `doc/TOOLCHAIN.md` for the terms, which bind the project
rather than just the individual.
