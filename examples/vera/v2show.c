/* v2show -- enable VERA2 4bpp over a framebuffer somebody else already
 * filled (run-v2.sh -loads v2pattern.bin at $E0:0000). Touches NO
 * framebuffer memory itself, so a wrong picture with THIS program is the
 * emulator model's fault, and a wrong picture with v2demo but a right one
 * here is the drawing code's fault. That split is the whole point. */
#include <stdint.h>
#include "x816_contract.h"
#define V2_CTRL   (*(volatile uint8_t *)X816_VERA2_BASE)
#define V2_ID     (*(volatile uint8_t *)X816_VERA2_ID)
#define V2_DISPL  (*(volatile uint8_t *)X816_VERA2_DISPL)
#define V2_DISPM  (*(volatile uint8_t *)X816_VERA2_DISPM)
#define V2_DISPH  (*(volatile uint8_t *)X816_VERA2_DISPH)
#define V2_PALADR (*(volatile uint8_t *)X816_VERA2_PALADR)
#define V2_PALLO  (*(volatile uint8_t *)X816_VERA2_PALLO)
#define V2_PALHI  (*(volatile uint8_t *)X816_VERA2_PALHI)
static uint8_t pal_r[16] = {0,15, 8, 0, 0, 0,15,15, 4, 8,15, 0, 8,15, 6,12};
static uint8_t pal_g[16] = {0,15, 8, 0,10,10, 0,10, 4, 0, 6,15,12, 4, 6, 8};
static uint8_t pal_b[16] = {0,15, 8,12, 0,12, 0, 0, 4, 8, 6,12, 0, 8,15, 0};
int main(void) {
    uint16_t i;
    if (V2_ID != X816_VERA2_ID_VALUE) { for(;;){} }
    V2_PALADR = 0;
    for (i = 0; i < 16u; i++) {
        V2_PALLO = (uint8_t)((pal_g[i] << 4) | pal_b[i]);
        V2_PALHI = pal_r[i];
    }
    V2_DISPL = 0; V2_DISPM = 0; V2_DISPH = 0;
    V2_CTRL = (uint8_t)((X816_VERA2_MODE_4BPP << 1) | 1u);
    for (;;) {}
}
