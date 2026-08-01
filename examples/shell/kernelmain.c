/* The RESIDENT KERNEL's entry point.
 *
 * This is the shell promoted to what examples/shell/shell.c's header always
 * said it would become: linked by runtime/x816-kernel.scm into the firmware
 * region ($F0:0000, shipped as games/X816/boot2.rom), entered by boot/boot.s
 * through the firmware magic, and RESIDENT -- `run` erases $01:0000, not the
 * kernel, and the $00:FE00 table it installs here stays pointing at live
 * code. K_EXIT re-enters through the same path (a full restart: console up,
 * table re-stamped, prompt).
 *
 * The difference from the loadable shell's main: the resident kernel is the
 * one image that MUST install the jump table -- that is the point of it.
 */

#include "shell.h"
#include "console.h"
#include "kernel.h"

int
main(void)
{
    con_init();
    kern_install();    /* the table now survives every `run` */
    sh_run();          /* never returns */
    return 0;
}
