/* galaga88_render.c -- portable Namco System-1 video renderer for Galaga '88.
 * Mirrors MAME namcos1_v.cpp screen_update + namco_c116/c123tmap/namcos1_sprite.
 *
 * MAME screen (namcos1.cpp set_raw): 384x264 bitmap, visible area
 *   x[73..360] y[16..239] (= 288x224). The C116 clip regs (L/R/T/B) tighten
 *   within that; for galaga88 they match the full visible area.
 *
 * Pipeline:
 *   1. build 8192-pen 8-bit-RGB palette from g88_paletteram() (C116).
 *   2. fill the 384x264 native bitmap black; clip = C116 window intersect vis.
 *   3. priority bitmap = 0 over clip.
 *   4. for priority P=0..7: draw each enabled C123 playfield whose
 *      (control[0x10+i]&7)==P, prival=P, with per-tile 1bpp mask.
 *   5. draw 127 sprites (index 126 first) with priority bitmap gating.
 *   6. crop the visible 288x224 (x[73..360],y[16..239]) and apply the
 *      parent set's ROT270 orientation -> 224x288.
 *
 * Tilemap effective scroll: src_x=(bx-left+scrollx)%W,
 *   src_y=(by-top+scrolly)%H. When the sprite controller asserts the global
 *   flip, the complete C123 raster (including PF4/PF5 text) is mirrored before
 *   the parent set's ROT270 output orientation is applied.
 *
 * Sprite 32x32 4bpp: pixel = nibble of sprrom[code*512 + 256*(y>=16) +
 *   8*(y&15) + 128*(x>=16) + (x&15)/2], high if (x&15) even, low if odd;
 *   pen=(color<<4)|pixel, color=src[12]>>1, transparent pixel=0x0F.
 */
#include "namcos1_render.h"
#include "namcos1_machine.h"
#include <string.h>

uint8_t g88_pal_r[8192], g88_pal_g[8192], g88_pal_b[8192];

#define NAT_W 384
#define NAT_H 264
#define VIS_X0 73
#define VIS_Y0 16
#define VIS_W 288            /* G88_FB_W */
#define VIS_H 224            /* G88_FB_H */

struct pf_geom { int w, h; unsigned base_byte; int left, top; };
static const struct pf_geom PF[6] = {
    /* C123 set_scrolldx(73-(44+{4,2,1,0})) and
     * set_scrolldy(16-24), matching the System 1 device configuration. */
    { 64, 64, 0x0000, 25, -8 },   /* PF0 */
    { 64, 64, 0x2000, 27, -8 },   /* PF1 */
    { 64, 64, 0x4000, 28, -8 },   /* PF2 */
    { 64, 32, 0x6000, 29, -8 },   /* PF3 (half height) */
    { 36, 28, 0x7010, 73, 16 },   /* PF4 fixed */
    { 36, 28, 0x7810, 73, 16 },   /* PF5 fixed */
};

static int      clip_minx, clip_maxx, clip_miny, clip_maxy;
static uint8_t  pri[NAT_W * NAT_H];
static uint16_t penfb[NAT_W * NAT_H];

static inline int wrap_coord(int v, int limit)
{
    v %= limit;
    return (v < 0) ? v + limit : v;
}

static void build_palette(void)
{
    const uint8_t *p = g88_paletteram();
    for (int page = 0; page < 4; page++) {
        const uint8_t *R = p + page * 0x2000 + 0x000;
        const uint8_t *G = p + page * 0x2000 + 0x800;
        const uint8_t *B = p + page * 0x2000 + 0x1000;
        for (int i = 0; i < 0x800; i++) {
            int pen = page * 0x800 + i;
            g88_pal_r[pen] = R[i];
            g88_pal_g[pen] = G[i];
            g88_pal_b[pen] = B[i];
        }
    }
}

static void draw_tilemap(int i, int prival)
{
    const struct pf_geom *g = &PF[i];
    const uint8_t *vram = g88_videoram();
    const uint8_t *chr   = g88_chrrom();
    const uint8_t *mask  = g88_maskrom();
    const uint8_t *ctrl  = g88_control();
    int flip = g88_flip();

    int scrollx = ((ctrl[i * 4 + 0] & 0xff) << 8) | (ctrl[i * 4 + 1] & 0xff);
    int scrolly = ((ctrl[i * 4 + 2] & 0xff) << 8) | (ctrl[i * 4 + 3] & 0xff);
    if (i >= 4) {
        scrollx = 0;
        scrolly = 0;
    }
    int paloff = ((ctrl[0x18 + i] & 7) << 8) + 0x800;

    int W = g->w * 8, H = g->h * 8;
    int left = g->left, top = g->top;
    for (int by = clip_miny; by <= clip_maxy; by++) {
        int source_y = by - top + scrolly;
        /* MAME's flipped effective-scroll mapping expressed directly as the
         * original tilemap source coordinate. */
        int sy = wrap_coord(flip ? H - 1 - source_y : source_y, H);
        int ty = sy >> 3, py = sy & 7;
        int rowbase = g->base_byte + ty * g->w * 2;
        for (int bx = clip_minx; bx <= clip_maxx; bx++) {
            int source_x = bx - left + scrollx;
            int sx = wrap_coord(flip ? W - 1 - source_x : source_x, W);
            int tx = sx >> 3, px = sx & 7;
            int wo = rowbase + tx * 2;
            int code = ((vram[wo] << 8) | vram[wo + 1]) & 0x3fff;
            uint8_t mb = mask[code * 8 + py];
            if (!(mb & (0x80 >> px))) continue;
            uint8_t gfx = chr[code * 64 + py * 8 + px];
            int di = by * NAT_W + bx;
            penfb[di] = paloff + gfx;
            pri[di]   = (uint8_t)prival;
        }
    }
}

static const int sprite_size[4] = { 16, 8, 32, 4 };

static void draw_sprites(void)
{
    const uint8_t *sp = g88_spriteram();
    const uint8_t *rom = g88_sprrom();
    int flip = g88_flip();
    int xoffs = sp[0x7f5] + ((sp[0x7f4] & 1) << 8);
    int yoffs = sp[0x7f7];

    for (int idx = 126; idx >= 0; idx--) {
        const uint8_t *s = sp + idx * 0x10;
        uint8_t attr1 = s[10], attr2 = s[14];
        int color8 = s[12];
        int flipx = (attr1 >> 5) & 1, flipy = attr2 & 1;
        int sizex = sprite_size[(attr1 & 0xc0) >> 6];
        int sizey = sprite_size[(attr2 & 0x06) >> 1];
        int tx = (attr1 & 0x18) & ~(sizex - 1);
        int ty = (attr2 & 0x18) & ~(sizey - 1);
        int sx = s[13] + ((color8 & 1) << 8);
        int sy = -s[15] - sizey;
        int sprite = (s[11] & 0xff) + ((attr1 & 7) << 8);
        int P = (attr2 & 0xe0) >> 5;
        int color = color8 >> 1;
        int shadow = (color == 0x7f);

        sx += xoffs;
        sy -= yoffs;
        if (flip) { sx = -sx - sizex; sy = -sy - sizey; flipx ^= 1; flipy ^= 1; }
        sy++;
        int dx0 = sx & 0x1ff;
        int dy0 = ((sy + 16) & 0xff) - 16;

        for (int yy = 0; yy < sizey; yy++) {
            int by = dy0 + yy;
            if (by < clip_miny || by > clip_maxy) continue;
            int gy = flipy ? (ty + sizey - 1 - yy) : (ty + yy);
            for (int xx = 0; xx < sizex; xx++) {
                int bx = dx0 + xx;
                if (bx < clip_minx || bx > clip_maxx) continue;
                int gx = flipx ? (tx + sizex - 1 - xx) : (tx + xx);
                int bo = sprite * 512 + 256 * (gy >= 16) + 8 * (gy & 15)
                         + 128 * (gx >= 16) + (gx & 15) / 2;
                uint8_t byte = rom[bo & 0xfffff];
                int pix = (gx & 1) ? (byte & 0x0f) : (byte >> 4);
                if (pix == 0x0f) continue;
                int di = by * NAT_W + bx;
                if (pri[di] > P) continue;
                if (shadow) {
                    int p = penfb[di];
                    if (p >= 0x800 && p <= 0xfff) penfb[di] = p + 0x800;
                } else {
                    penfb[di] = (color << 4) | pix;
                    pri[di]   = 31;
                }
            }
        }
    }
}

/* Shared render pass: fills penfb[] with C116 pens. */
static void render_pass(void)
{
    build_palette();

    int minx = (int)g88_c116_reg(0) - 1;
    int maxx = (int)g88_c116_reg(1) - 2;
    int miny = (int)g88_c116_reg(2) - 0x11;
    int maxy = (int)g88_c116_reg(3) - 0x12;
    if (minx < VIS_X0) minx = VIS_X0;
    if (miny < VIS_Y0) miny = VIS_Y0;
    if (maxx > VIS_X0 + VIS_W - 1) maxx = VIS_X0 + VIS_W - 1;
    if (maxy > VIS_Y0 + VIS_H - 1) maxy = VIS_Y0 + VIS_H - 1;
    clip_minx = minx; clip_maxx = maxx; clip_miny = miny; clip_maxy = maxy;

    for (int i = 0; i < NAT_W * NAT_H; i++) { penfb[i] = 0; pri[i] = 0; }

    if (clip_minx <= clip_maxx && clip_miny <= clip_maxy) {
        const uint8_t *ctrl = g88_control();
        for (int P = 0; P < 8; P++)
            for (int i = 0; i < 6; i++) {
                if (ctrl[0x10 + i] & 0x08) continue;
                if ((ctrl[0x10 + i] & 7) != P) continue;
                draw_tilemap(i, P);
            }
        draw_sprites();
    }
}

void g88_render(uint32_t *rgb)
{
    render_pass();

    for (int ny = VIS_Y0; ny < VIS_Y0 + VIS_H; ny++) {
        for (int nx = VIS_X0; nx < VIS_X0 + VIS_W; nx++) {
            int pen = penfb[ny * NAT_W + nx];
            uint32_t c = ((uint32_t)g88_pal_r[pen] << 16)
                       | ((uint32_t)g88_pal_g[pen] << 8)
                       | (uint32_t)g88_pal_b[pen];
            int dx = ny - VIS_Y0;
            int dy = VIS_W - 1 - (nx - VIS_X0);
            rgb[dy * VIS_H + dx] = c;
        }
    }
}

static uint8_t rgb332_index(int r, int g, int b)
{
    return (uint8_t)(((r & 0xe0)     ) |
                     ((g & 0xe0) >> 3) |
                     ((b & 0xc0) >> 6));
}

static void build_rgb332_clut(uint8_t *clut)
{
    for (int i = 0; i < 256; i++) {
        int r = (i >> 5) & 7;
        int g = (i >> 2) & 7;
        int b = i & 3;
        clut[i * 3 + 0] = (uint8_t)((r * 255 + 3) / 7);
        clut[i * 3 + 1] = (uint8_t)((g * 255 + 3) / 7);
        clut[i * 3 + 2] = (uint8_t)((b * 255 + 1) / 3);
    }
}

/* Fast 8-bit RTG path. Use a stable RGB332 CLUT so the hardware palette does
 * not change every frame; dynamic C116 colours are quantized per visible pixel. */
void g88_render8(uint8_t *out, uint8_t *clut, uint16_t *pen_map)
{
    render_pass();
    build_rgb332_clut(clut);
    (void)pen_map;

    /* Emit rotated 224x288 8-bit framebuffer. */
    for (int ny = VIS_Y0; ny < VIS_Y0 + VIS_H; ny++) {
        for (int nx = VIS_X0; nx < VIS_X0 + VIS_W; nx++) {
            int pen = penfb[ny * NAT_W + nx];
            int dx = ny - VIS_Y0;
            int dy = VIS_W - 1 - (nx - VIS_X0);
            out[dy * VIS_H + dx] = rgb332_index(g88_pal_r[pen], g88_pal_g[pen], g88_pal_b[pen]);
        }
    }
}
