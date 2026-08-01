/* ==========================================================================
 * goshell.h -- the way back to the prompt.
 *
 * Every conformance test ends by parking on its result colour. Without this
 * that park is permanent: `run` loaded the test over the shell at $01:0000, so
 * reading the next result means power-cycling the board. ESC loads the shell
 * again and hands over the way `run` does.
 *
 * con_init() MUST have run first. Not for the screen -- it is what copies the
 * exec relocator into bank $00, and without it the handover jumps into
 * whatever bank $00 happened to contain. A test that skipped con_init because
 * it painted its own graphics mode found this the hard way.
 *
 * Put goshell_on_esc() where the test used to spin forever. It never returns
 * in the ordinary case; if the card cannot be read it keeps waiting instead,
 * leaving the failure on screen rather than clearing the diagnosis because the
 * recovery also failed.
 * ========================================================================== */

#ifndef X816_GOSHELL_H
#define X816_GOSHELL_H

#include <stdbool.h>

/* Wait for ESC, then reload the shell. Does not return while the card is
   readable. */
void goshell_on_esc(void);

/* Reload the shell now. Returns false -- and only false -- if the card, the
   image or its "X816" magic would not cooperate; on success it does not
   return at all. */
bool goshell(void);

#endif /* X816_GOSHELL_H */
