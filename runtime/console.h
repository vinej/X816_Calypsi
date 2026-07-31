/* ==========================================================================
 * console.h -- the X816 console: VERA text out, SMC keyboard in.
 *
 * This is the kernel's console layer, specified in X816_Core doc/KERNEL.md
 * §5.1. It owns the text screen when no program has taken it, which is what
 * puts it kernel-side under §2.1 test 1 -- two copies would fight over the
 * screen and the keyboard queue.
 *
 * 80x60 at 640x480, 1bpp tile mode with per-cell attributes: the same VERA
 * setup already proven on hardware by X816_Core boot/hello.s, rather than a
 * fresh one.
 *
 * BUILD AT -O0. Every one of these functions reaches VERA and the VIA through
 * volatile registers, and Calypsi 5.18 eliminates volatile reads at -O1 and
 * above -- see the README.
 * ========================================================================== */

#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdint.h>
#include <stdbool.h>

#define CON_COLS 80
#define CON_ROWS 60

/* Sets up VERA, uploads the font and clears the screen. Call once. */
void con_init(void);

void con_cls(void);

/* Printable range is $20-$5F -- the font has no lower case. Anything outside
 * it prints as a space rather than as garbage, except for the control codes
 * below, which are acted on:
 *     \n  newline, scrolling at the bottom
 *     \r  carriage return, column 0
 *     \b  backspace, non-destructive at column 0
 */
void con_putc(char c);
void con_puts(const char *s);

void con_gotoxy(uint8_t x, uint8_t y);
uint8_t con_getx(void);
uint8_t con_gety(void);

/* Non-blocking: returns 0 if no key is waiting. */
char con_getkey(void);

/* Blocking. */
char con_getc(void);

#endif /* CONSOLE_H */
