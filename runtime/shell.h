/* ==========================================================================
 * shell.h -- the X816 boot prompt.
 *
 * Specified in X816_Core doc/SHELL.md. A fixed command set over the console
 * and (later) the filesystem, not a language: the prompt's job is to load
 * programs and move around a card, which is a command set, and building it as
 * one is smaller and sooner.
 *
 * The command TABLE is the extension point (doc/SHELL.md §4). A scripting
 * layer added later reuses it verbatim and inherits every command; anything
 * special-cased in the parser is a command that layer would not have. So
 * nothing is special-cased.
 *
 * BUILD AT -O0 -- it reaches memory and device registers through volatile
 * pointers, and Calypsi 5.18 eliminates volatile reads above -O0.
 * ========================================================================== */

#ifndef SHELL_H
#define SHELL_H

#include <stdint.h>
#include <stdbool.h>

#define SH_MAX_LINE 80          /* one console row */
#define SH_MAX_ARGS 8

/* A command handler. argv[0] is the command name, so argc is always >= 1.
   Returns 0 on success; non-zero is reported as an error by the dispatcher. */
typedef uint8_t (*sh_handler)(uint8_t argc, char **argv);

/* The names and help text are INLINE ARRAYS, not pointers to literals, and
 * the table is not const.
 *
 * A string literal lands in Calypsi's `cdata`, which the linker places with
 * the code in bank $01, and the compiler builds even a __far pointer's address
 * from a 16-bit immediate (`ldx ##_StringLiteral_...`) -- so a literal simply
 * cannot be addressed from bank $00, and the link fails outright. Inline
 * arrays in a non-const table go to `data`, whose initialiser rides in the
 * image and is copied into bank $00 at startup.
 *
 * It costs a few hundred bytes of bank $00 and is scaffolding: once the kernel
 * lives in the firmware region, `cdata` is simply part of the image and
 * directly reachable, and this can go back to pointers -- but shell.c is
 * compiled ONCE and linked into both images (build.sh), so undoing it means a
 * second object with -DKERNEL_RESIDENT, the way kerntab.s/kirq.s/ccursor.s
 * already do it. Worth knowing before the next thing needs bank $00.
 *
 * THESE TWO WIDTHS ARE A BANK-$00 BUDGET, NOT A GUESS. Every row costs
 * SH_NAME_MAX + SH_HELP_MAX + 3 bytes of `data`, which x816-kernel.scm places
 * in KernRAM -- the kernel's 4 KB claim (doc/KERNEL.md 3.1). Adding `mem`
 * overflowed it, and the rule for an overflow is to shrink the offender rather
 * than grow the claim, so the widths came down to the longest string actually
 * present plus its NUL: `rename` at 6, `save file addr len` at 18. Two help
 * texts were shortened to reach it ("load a program and go", "enter image at
 * $01:0000"). If a new command needs a wider one, that is a decision about the
 * claim, not about this line. */
#define SH_NAME_MAX 7
#define SH_HELP_MAX 19

#ifdef KERNEL_RESIDENT
typedef struct {
    const char __far *name;
    const char __far *help;
    uint8_t     min_args;       /* not counting argv[0] */
    uint8_t     max_args;
    sh_handler  fn;
} sh_command;
#else
typedef struct {
    char        name[SH_NAME_MAX];
    char        help[SH_HELP_MAX];
    uint8_t     min_args;       /* not counting argv[0] */
    uint8_t     max_args;
    sh_handler  fn;
} sh_command;
#endif

/* Split `line` in place into argv. Returns the count, 0 for a blank line.
   Repeated and trailing spaces collapse; more than SH_MAX_ARGS words is an
   error, reported as SH_TOO_MANY_ARGS rather than silently truncated. */
#define SH_TOO_MANY_ARGS 0xFF
uint8_t sh_tokenise(char *line, char **argv);

/* Look up and run one line. Reports its own errors to the console. */
void sh_exec(char *line);

/* Read a line from the console, with backspace, into `buf`. */
void sh_readline(char *buf, uint8_t size);

/* The prompt. Never returns. */
void sh_run(void);

/* Exposed for the conformance test, which drives dispatch without a keyboard. */
extern sh_command sh_commands[];
uint8_t sh_command_count(void);

/* Hex helpers, shared by the memory commands and the test. `sh_parse_hex`
   returns false on any non-hex character, so a typo is an error rather than a
   silently truncated address. */
bool sh_parse_hex(const char *s, uint32_t *out);
void sh_put_hex8(uint8_t v);
void sh_put_hex16(uint16_t v);
void sh_put_hex24(uint32_t v);

#endif /* SHELL_H */
