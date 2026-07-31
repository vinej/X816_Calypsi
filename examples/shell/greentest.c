/* greentest -- the smallest thing that can prove `run` works.
 *
 * Paints the screen green straight through VERA and stops. No console, no
 * font, no keyboard, no FAT32: if this comes up green then the shell staged
 * the file, the relocator moved it into bank $01 and the entry point was
 * reached, and ANY remaining fault is in the program that was run rather than
 * in run itself.
 *
 * Deliberately not reusing console.c. Sharing code with the thing under test is
 * how you end up proving that two broken halves agree.
 */

#define VERA_ADDR_L     (*(volatile unsigned char *)0x9F20)
#define VERA_ADDR_M     (*(volatile unsigned char *)0x9F21)
#define VERA_ADDR_H     (*(volatile unsigned char *)0x9F22)
#define VERA_DATA0      (*(volatile unsigned char *)0x9F23)
#define VERA_CTRL       (*(volatile unsigned char *)0x9F25)
#define VERA_DC_VIDEO   (*(volatile unsigned char *)0x9F29)
#define VERA_DC_HSCALE  (*(volatile unsigned char *)0x9F2A)
#define VERA_DC_VSCALE  (*(volatile unsigned char *)0x9F2B)
#define VERA_L0_CONFIG  (*(volatile unsigned char *)0x9F2D)
#define VERA_L0_TILEB   (*(volatile unsigned char *)0x9F2F)

int
main(void)
{
    unsigned int x, y;

    VERA_CTRL      = 0;
    VERA_DC_VIDEO  = 0x11;      /* VGA + layer 0 */
    VERA_DC_HSCALE = 0x40;      /* 320x240 */
    VERA_DC_VSCALE = 0x40;
    VERA_L0_CONFIG = 0x07;      /* 8bpp bitmap */
    VERA_L0_TILEB  = 0;

    VERA_CTRL   = 0;
    VERA_ADDR_L = 0;
    VERA_ADDR_M = 0;
    VERA_ADDR_H = 0x10;         /* auto-increment 1 */

    for (y = 0; y < 240; y++)
        for (x = 0; x < 320; x++)
            VERA_DATA0 = 0x05;  /* green, VERA default palette */

    for (;;)
        ;
}
