/* galaga88_render.h -- portable Namco System-1 video renderer for Galaga '88.
 * Renders the C123 6-playfield tilemaps + CUS39/35/48 sprites with the C116
 * 8192-pen 8-bit-RGB palette into a native 288x224 framebuffer, then exposes
 * it for the host (PPM dump / MAME diff) and the Amiga RTG presenter (which
 * rotates ROT270 -> 224x288). Verified against MAME namco/namcos1_v.cpp,
 * namco_c116.cpp, namco_c123tmap.cpp, namcos1_sprite.cpp.
 *
 * No Amiga deps. Draws from the live machine state in galaga88_machine.c.
 */
#ifndef GALAGA88_RENDER_H
#define GALAGA88_RENDER_H
#include <stdint.h>

#define G88_FB_W 288            /* native Namco System-1 horizontal draw width */
#define G88_FB_H 224            /* native vertical draw height                 */

/* Palette: 8192 pens, 8-bit RGB each (C116). Built fresh each frame from
 * g88_paletteram() so the attract fade/flash cycles are captured. */
extern uint8_t g88_pal_r[8192], g88_pal_g[8192], g88_pal_b[8192];

/* Render one frame into the 288x224 RGB framebuffer `rgb` (row-major,
 * rgb[y*G88_FB_W+x] = (r,g,b) packed as 0xRRGGBB). Reads videoram/spriteram/
 * control/paletteram/gfx ROMs from galaga88_machine.c. Mirrors MAME
 * screen_update: black fill -> C116 clip window -> 8 priority tilemap passes
 * -> sprites (with per-tile C123 mask + priority bitmap). */
void g88_render(uint32_t *rgb);

/* Render one frame into a rotated 224x288 8-bit CLUT-index framebuffer `out`.
 * On return `clut` holds the 256-entry RGB palette (256*3 bytes) and
 * `pen_map` maps every C116 pen (8192 entries) to the 8-bit index chosen.
 * This is the fast path for the Amiga 8-bit RTG presenter. */
void g88_render8(uint8_t *out, uint8_t *clut, uint16_t *pen_map);

#endif