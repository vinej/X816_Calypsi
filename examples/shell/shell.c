/* The X816 boot prompt, as a loadable program.
 *
 * Everything is in runtime/shell.c; this is only an entry point. When the
 * firmware region exists the same code becomes the kernel's prompt and this
 * file goes away.
 *
 * This is the first thing here that reads the keyboard in anger --
 * con_getkey() is untouched by every conformance test so far, because a test
 * cannot press a key.
 */

#include "shell.h"
#include "console.h"

int
main(void)
{
    con_init();
    sh_run();          /* never returns */
    return 0;
}
