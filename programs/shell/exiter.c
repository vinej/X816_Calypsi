/* exiter.c -- a program whose whole job is to hand the machine back.
 *
 * It exists for run-cwd.sh. Nothing else in the tree calls K_EXIT yet, so
 * there was no way to drive the one path the working-directory carry-over is
 * about: launch a program from a subdirectory, let it exit, and see where the
 * prompt comes back.
 *
 * Deliberately silent, and deliberately without con_init(). The kernel's
 * console is already up when this starts, and K_EXIT restarts the kernel --
 * which clears the screen. Anything printed here would be erased a moment
 * later, so the evidence the test reads is the CLEARED screen itself: a fresh
 * banner at row 0 is what proves the exit happened at all.
 *
 * The spin after the call is not dead code. If K_EXIT is not available -- a
 * machine with no resident kernel, where the entry answers KERR_NOSYS rather
 * than jumping -- it returns, and stopping here leaves the test to time out
 * with the pre-exit screen still on it. That reads as "it did not exit",
 * which is exactly what happened.
 */

#include "kernel.h"

int
main(void)
{
    kern_c = 0;                 /* exit status; v1 accepts and discards it */
    kern_call(K_EXIT);          /* does not return when a kernel is resident */
    for (;;)
        ;
    return 0;
}
