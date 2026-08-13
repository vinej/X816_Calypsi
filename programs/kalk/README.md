# kalk — a VisiCalc-style spreadsheet for X816

Ported from zserge's **kalk**, a small C spreadsheet, and filled out to
VisiCalc's own command and function set. It runs on the bare machine over the
resident kernel: no operating system, no C library beyond Calypsi's, and the
floating point is this repo's own software package.

A **256 × 1024** sheet — columns `A`..`IV`, rows 1..1024 — in 80 × 60 text.

```
 A1  Item                                                             READY
                                                     <- the entry line
          A          B          C          D
   1 Item      Qty        Price      Total
   2 Widget A         10       4.99      49.90
   3 Widget B         25       2.50      62.50
   4 Subtotal                           112.40
 ...
arrows move  type  " label  F2 edit  / menu  INS blank  ! calc  > goto  /Q quit
```

Row 0 is the status line — the cursor's cell name and, for a formula, its
**source** rather than its result. Row 1 is the entry line while typing, row 2
the column letters, rows 3–58 the sheet, and row 59 the help.

## Building and running it

Each part has its own test, and each builds and runs under the emulator on its
own. From this directory:

```sh
sh ../shell/build.sh      # once: the resident kernel the demos load over
./run-kalk.sh             # the spreadsheet, typed at
```

Every script takes `--negative`, which breaks the thing under test on purpose
to prove the test can see it fail. `run-kalk.sh` also takes `--csv`,
`--insert`, `--replicate`, `--titles`, `--move`, `--clear` and `--menu`, each
driving one command through the menu and reading the result off the screen,
and `--cwd`, which runs the program out of a subdirectory and then reads the
**card** back to see which directory `/SS` wrote into.

For a live keyboard rather than a scripted one, `~/x816-kalk-demo/build-card.sh`
writes a card with every demo on it and `./launch.sh` starts the emulator with
a window; type `kalk` at the prompt. `type readme.txt` on the card has a short
tour.

## Typing into it

**What a typed line means** is decided by its first character, so there is no
mode to be in:

| starts with | becomes |
|---|---|
| `+` `-` `(` `@` | a **formula** — the text is kept and the value computed |
| `0`–`9` `.` | a **number**, if it parses as one |
| `"` | a **label**, forced. The quote is syntax and is stripped |
| anything else | a **label** |

So `+A1*2` calculates and `Total` does not. The one ambiguity a person
actually hits is a label that starts with a digit — a year, a part number —
and the quote is the answer to it: type `"2024`.

Note that `-12.5` is a **formula**, not a number, because `-` begins one. It
shows and calculates identically; only the status line can tell.

**Keys**

| | |
|---|---|
| arrows, `Home`, `PgUp`, `PgDn` | move; the view scrolls by the least that brings the cell into view |
| `F2` | **edit** what is already in the cell, instead of replacing it |
| `Return` / `Tab` | commit and advance **down** / **right** |
| `INS` | blank the cell |
| `!` | recalculate |
| `>` | jump to a cell by name — `>b12`, or `>$B$4` |
| `/` | the command menu |
| `ESC` | abandon what is being typed, or back out of the menu. It does **not** quit |
| `/Q` | quit to the prompt — `/SQ` saves first |

Typing a column of figures is `11` `Return` `22` `Return` … — commit and
advance is one action, which is VisiCalc's behaviour and the reason it feels
like a spreadsheet rather than a form.

### Changing a cell

Typing **replaces**. Put the cursor on a cell, type, and whatever was there is
gone — which is what makes entering a sheet fast, and is the wrong thing
entirely when `@SUM(D2...D50)` needs one character altered.

`F2` is the way in. It loads the cell's **source** — the same text the status
line shows — onto the entry line, where `Backspace` can reach it, and `Return`
commits it like anything else. `ESC` abandons the edit and leaves the cell as
it was.

A label comes back with its quote in front (`"2024`, not `2024`), because the
entry line is read by the same rule that reads typing: handed back bare, a
label of `2024` would commit as a **number**. A number comes back with all
nine digits it actually holds, not the six the column shows.

`ESC` at the sheet does nothing but say so. Leaving is `/Q`, or `/SQ` to save
on the way out — the one key everybody presses to mean "never mind" should not
also throw away an afternoon's sheet.

## The command menu

`/` then a letter. `ESC` backs out of any of them, and so does any key the
menu does not recognise, rather than the key being swallowed.

| | |
|---|---|
| `/B` `/C` | blank the cell / clear the whole sheet |
| `/F` *code* | this cell's format |
| `/GF` *code* | every cell's format — the one used where a cell has none |
| `/GC` *n* | column width, 4 to 20, then `Return` |
| `/IR` `/IC` | insert a row here / a column here |
| `/DR` `/DC` | delete this row / this column |
| `/M` | drag this row or column with the arrow keys; `ESC` when done |
| `/R` | replicate: the cell or range to copy **from**, then the cell or range to copy **to** |
| `/TV` `/TH` `/TB` `/TN` | lock the columns left of the cursor / the rows above it / both / neither |
| `/SL` `/SS` `/SQ` | load / save / save and quit a **CSV** file |
| `/Q` | quit |

**Insert and delete rewrite the formulas.** Everything after the line moves,
and every reference in every formula moves with it, so `+B4` still names the
cell it named. The `$` is *ignored* here — `$B$4` means "B4, and do not adjust
when this formula is **copied**", but if the cell B4 itself moves down then
what `$B$4` names has moved too, and following it is what keeps the reference
true. A reference into a deleted row is left naming that position, which now
holds whatever moved up into it.

**Replicate is where the `$` means something**, and the only place it does. A
relative reference moves by the same offset the cell did, so `+A1` copied one
column right becomes `+B1`; an anchored one does not, because `$` is precisely
the user saying "not this one". So `+A1*$D$1` replicated down a column keeps
multiplying by the rate in D1 — which is what makes a column of totals worth
writing once.

A **single cell** as the target means "put the block here": `A1...A3` onto `B1`
fills B1, B2 and B3. A **range** means "fill this with the block", so one
formula onto `B2...B4` fills all three.

**A file name is relative to where kalk was started.** `/SS BOOK.CSV` run from
`/KALK` writes `/KALK/BOOK.CSV`, and `/SL BOOK.CSV` reads it back from there;
`..` and an absolute `/SHEETS/BOOK.CSV` both work if you want somewhere else.
The prompt's `cd` is what sets this, and the program is handed the directory it
was launched from — it used to come up at the card's root regardless, so a
sheet saved from `/KALK` landed beside `KERNEL.BIN` and `/SL` could not find it
again.

The entry line reports what happened — `saved`, `loaded`, or a reason it did
not: `NO CARD -- is one in the slot?` is a different problem from `no such
file, or it cannot be created`, and the message stays until the next thing is
typed.

**Locked titles** freeze rows or columns in place while the rest scrolls under
them. They are counts rather than a mode, so `/TN` is simply zero, and rows
are capped at half the sheet area — locking more than there is room to scroll
in leaves a sheet whose arrow keys appear dead.

## Formats

`/F` *code* for one cell, `/GF` *code* for the sheet. Numbers are right
aligned and labels left aligned unless a format says otherwise.

| code | |
|---|---|
| `G` | general — six significant digits, trailing zeros dropped, exponent form only when the value will not write out plainly |
| `I` | integer, **truncated** toward zero, not rounded |
| `$` | two decimal places |
| `%` | the value × 100, two decimals, then `%` |
| `*` | a bar of `*`, left aligned. It counts 0, 1, 2 … while each is *less than* the value, so 2.5 draws **three** stars rather than two |
| `L` `R` | left / right aligned |
| `D` | use the sheet's format — which is what undoes a `/F` on one cell |

`$` is the key you press, **not** a character that appears: kalk's original is
`snprintf(t, "%.2f", val)` and there is no currency symbol. It reads like a
bug and is not one.

A value too wide for its column is **truncated**, which is what the original
does. VisiCalc filled the field with `>` instead.

## Formulas

Ordinary precedence — `1+2*3` is 7, where VisiCalc evaluated strictly left to
right and got 9.

```
expr    := term (('+' | '-') term)*
term    := primary (('*' | '/') primary)*
primary := number | ref | '(' expr ')' | ('-'|'+') primary | '@' fn
```

References carry VisiCalc's dollars — `A1`, `$A$1`, `$A1`, `A$1` — and a range
is written with **three** dots: `A1...B5`, either corner first.

**Labels spill.** A label wider than its column runs into the columns to its
right for as far as they are empty, and the first occupied neighbour cuts it
off exactly where the text would have collided. Numbers never spill; they are
truncated to their column, because a number that ran on would be read as a
different number.

### Functions

| | |
|---|---|
| over a range | `@SUM` `@MIN` `@MAX` `@COUNT` `@AVERAGE` (or `@AVG`) |
| | `@NPV(rate, range)` — each flow discounted one more period than the last |
| | `@LOOKUP(value, range)` — the last key at or below `value`, answered from the column beside it (or the row below, for a horizontal range) |
| over a value | `@ABS` `@INT` `@SQRT` `@EXP` `@LN` `@LOG10` |
| | `@SIN` `@COS` `@TAN` `@ASIN` `@ACOS` `@ATAN` — radians |
| on their own | `@PI` `@ERROR` `@NA` — written bare, as VisiCalc does, though empty parentheses are accepted |

Empty cells are zero, and so are labels — a `@SUM` over a range with gaps is
the ordinary case, not an error. `@COUNT` and `@AVERAGE` count only cells that
hold values, so an average divides by how many there are rather than by the
size of the range.

#### Examples

A formula has to *start* like one. `@SUM(A1...A9)` does, because `@` is one of
the four opening characters; `A1*2` does **not** and lands as the label
`A1*2`, which is why a formula that begins with a reference is written `+A1*2`.

| typed | what it answers |
|---|---|
| `+B2*C2` | quantity times price, in the row's own cells |
| `+D4-D5` | one cell less another |
| `+A1*$D$1` | A1 times the rate in D1 — the `$` is what survives `/R` |
| `@SUM(D2...D9)` | eight cells added; empty ones and labels count as zero |
| `@SUM(A1...C3)` | a **rectangle**, not a row — nine cells |
| `@COUNT(D2...D9)` | how many of them actually hold a value |
| `@AVERAGE(D2...D9)` | their mean, divided by that count and not by 8 |
| `@AVG(D2...D9)` | the same function, VisiCalc's shorter spelling |
| `@MIN(B2...B9)` `@MAX(B2...B9)` | the smallest / largest of them |
| `@ABS(C3-C2)` | the gap between two cells, sign discarded |
| `@INT(12.9)` | `12` — truncated toward zero, so `@INT(-12.9)` is `-12` |
| `@SQRT(A1)` | the root; a negative A1 is `ERROR`, not a guess |
| `@LN(A1)` `@LOG10(A1)` `@EXP(A1)` | natural log, base 10, and *e*ˣ |
| `@SIN(@PI/6)` | `0.5` — the angle is in **radians** |
| `@ATAN(B2/B1)` | likewise radians out |
| `@NPV(0.1,C2...C6)` | five cash flows discounted at 10%, C2 by one period, C3 by two, and so on |
| `@LOOKUP(B4,A2...A9)` | finds the last key at or below B4 in A2..A9 and answers from the column **beside** it — B2..B9 |
| `@NA` | "no value here yet", and every formula reading this cell says `NA` too |

A worked sheet, typed straight down:

| cell | typed | shows |
|---|---|---|
| A1..D1 | `Item`, `Qty`, `Price`, `Total` | labels, since none starts with `+-(@` or a digit |
| A2, A3 | `Widget A`, `Widget B` | labels |
| B2, B3 | `10`, `25` | numbers |
| C2, C3 | `4.99`, `2.50` | numbers |
| D2 | `+B2*C2` | `49.9` |
| D3 | `+B3*C3` | `62.5` — or type D2 once, then `/R`, `D2`, `D3` |
| D4 | `@SUM(D2...D3)` | `112.4` |
| D5 | `+D4*0.15` | the tax on it |
| D6 | `+D4+D5` | the total |

A heading that is all digits is the one case needing the quote: `2024` typed
into E1 is a **number**, and `"2024` is the year.

`/F$` on each of D2..D6 gives them two decimal places — or `/GF$` for the
whole sheet at once — and `/GC12` widens the columns enough to hold them.
`F2` on D4 puts `@SUM(D2...D3)` back on the entry line when the sheet grows a
row and the range has to reach further.

`@ERROR` and `@NA` **propagate**: a formula that reads an `@NA` cell is itself
`@NA`, one that reads an `@ERROR` cell is `@ERROR`, and `@ERROR` wins when a
formula meets both. Domain errors are caught before the arithmetic sees them —
`@LN` of zero or less, `@SQRT` of a negative, `@ASIN` outside −1..1, division
by zero — and show as `ERROR` rather than the zero the float package would
otherwise answer.

**Recalculation is a fixed point, not an order.** A formula may read a cell
whose own formula has not run yet, so every formula is evaluated repeatedly
until nothing changes. The pass limit — ten — is what makes a circular
reference terminate: after it the sheet keeps whatever it last computed rather
than hanging, and the user can recover by editing one of the two cells.

## Files

| | |
|---|---|
| `kalk.c` | the edit loop: keys in, sheet out, and the command menu |
| `cell.c` `cell.h` | the sheet — 256 × 1024 cells of 16 bytes, 4 MiB, plus the text arena |
| `expr.c` `expr.h` | the formula parser and evaluator |
| `fmt.c` `fmt.h` | a value into a column: `%ld`, `%g` and `%.2f` without a printf |
| `view.c` `view.h` | where everything is drawn, the scroll, the locked titles, and the render cache |
| `sheet.c` `sheet.h` | what a typed line means, CSV both ways, insert/delete/move/replicate |
| `*test.c` `run-*.sh` | one test per part, each with a negative control |

## Worth knowing about the inside

**The render cache is what makes it usable.** Every sheet row's finished
80-character line is kept in the BRAM banks `runtime/x816-kalk.scm` reserves —
1,024 rows of 80 bytes, in two halves because Calypsi refuses a single object
over 64 KB. A repaint that would reformat a screenful of floats writes the
characters straight out instead.

It is keyed per **row**, not per cell, and that was measured rather than
guessed: composing a line means walking right from every label to see how far
it may spill, so a per-cell cache would remove the formatting but not the
lookahead. `run-bench.sh` splits a repaint into parts that sum and prints them.

A cached repaint is **75 ms**; a cold one is 1.9 s. A vertical scroll is all
hits, because a cached line says nothing about where on screen it goes — a
sideways scroll throws the whole cache away, which is why holding `Right` is
slower than holding `Down`.

**Nothing ever walks the whole grid.** A quarter of a million cells is
affordable only because every sweep is bounded by a watermark and by a
per-row map: a row nobody has written to is skipped without reading a cell of
it. Inserting a row across a screenful of data is 474 ms; across a sheet
holding three cells, 42 ms.

**CSV holds sources, not results** — a formula's text, not the number it
worked out, which is the difference between saving a spreadsheet and saving a
table. Loading goes through exactly the rule typing does, so a file reads back
as the sheet that wrote it. A quoted field loads as a **label**, and the writer
quotes any label that would otherwise read back as a number or a formula;
ordinary CSV re-guesses the type and turns a label `2024` into a number.

## Not here yet

Mouse selection, and the `/` menu's more obscure corners. Column widths are
global (`/GC`) — the per-column machinery exists in the view layer but the
reference command set has no key for it.
