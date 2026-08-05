// Taito Z System video: TC0100SCN (BG0 / BG1 / TX layers), TC0150ROD
// (the pseudo-3D road) and zoomed 16x8 sprites, composited into the
// board's RGBA framebuffer through the TC0110PCR palette.
//
// Layer order is BG (one of the two is the opaque bottom layer), then the
// road, then TX, then sprites -- sprites carry a priority bit that lets
// them pass behind the road below the road's priority switch line.
//
// Behaviour derived from MAME's tc0100scn.cpp, tc0150rod.cpp and
// taito_z.cpp; written independently against those facts.

#include "taito/taitoz/taitoz_machine.h"

#include <cstring>

namespace taitoz {

namespace {

inline int r16(const uint8_t* p) { return (p[0] << 8) | p[1]; }

}  // namespace

// One byte per road pixel, unpacked once from the 2bpp road ROM. The two
// bitplanes of a road word are 8 bits apart.
void board::transcode_road_gfx() {
    if (!road_pix_.empty() || gfx_road_.empty()) return;
    const std::size_t words = gfx_road_.size() / 2;
    road_pix_.assign(words * 8, 0);
    for (std::size_t w = 0; w < words; ++w) {
        const int gw = (gfx_road_[w * 2 + 1] << 8) | gfx_road_[w * 2];
        uint8_t* p = road_pix_.data() + w * 8;
        for (int b = 0; b < 8; ++b)
            p[b] = static_cast<uint8_t>((((gw >> (15 - b)) & 1) << 1) |
                                        ((gw >> (7 - b)) & 1));
    }
}

void board::draw_road() {
    transcode_road_gfx();
    if (road_pix_.empty()) return;

    const int screen_width = kScreenW, H = kScreenH;
    const int type = 1, road_trans = 0, y_offs = -3;
    const int palette_offs = road_palbank_ << 6, x_offs = 0xa7;
    const auto rd_ram = [this](int idx) -> int {
        if (idx < 0 || idx >= 0x1000) return 0;
        return r16(road_.data() + idx * 2);
    };
    const auto road_pixel = [this](int tile, int x_index) -> uint16_t {
        const std::size_t idx =
            ((static_cast<unsigned>(tile) << 8) + (x_index >> 3)) & 0x3ffffu;
        return road_pix_[idx * 8 + (x_index & 7)];
    };

    uint16_t roada_line[512], roadb_line[512];
    const int road_ctrl = rd_ram(0xfff);
    road_priority_switch_line_ = (road_ctrl & 0x00ff) - y_offs;
    const int rA = y_offs * 4 + ((road_ctrl & 0x300) << 2);
    const int rB = y_offs * 4 + ((road_ctrl & 0xc00) << 0);

    std::fill(road_layer_.begin(), road_layer_.end(), uint16_t{0x8000});

    for (int y = 0; y < H; ++y) {
        int lnd = 0;
        const int ri = rA + y * 4, ri2 = rB + y * 4;
        for (int i = 0; i < screen_width; ++i) {
            roada_line[i] = 0x8000;
            roadb_line[i] = 0x8000;
        }
        uint8_t pr[6] = {1, 1, 2, 3, 3, 4};
        const uint16_t ra_cr = rd_ram(ri),  ra_cl = rd_ram(ri + 1),
                       ra_bc = rd_ram(ri + 2);
        const uint16_t rb_cr = rd_ram(ri2), rb_cl = rd_ram(ri2 + 1),
                       rb_bc = rd_ram(ri2 + 2);
        if (ra_bc & 0x2000) pr[2] += 2;
        if (rb_bc & 0x2000) pr[2] += 1;
        if (ra_cl & 0x2000) pr[3] -= 1;
        if (rb_cl & 0x2000) pr[3] -= 2;
        if (ra_cr & 0x2000) pr[4] -= 1;
        if (rb_cr & 0x2000) pr[4] -= 2;
        if (pr[4] == 0) pr[4]++;

        uint16_t* roada;
        uint16_t* roadb;
        int x_index, i, xoffset, paloffs, palloffs, palroffs, tile, colbank, rc;
        int le, re, begin, end, r_over, l_over, dtr, bgonly;
        uint16_t pixel, color, pri, pixpri;

        // ---- ROAD A ----
        palroffs = (ra_cr & 0x1000) >> 11;
        palloffs = (ra_cl & 0x1000) >> 11;
        xoffset  = ra_bc & 0x7ff;
        paloffs  = (ra_bc & 0x1800) >> 11;
        colbank  = (rd_ram(ri + 3) & 0xf000) >> 10;
        tile     = rd_ram(ri + 3) & 0x3ff;
        r_over = 0; l_over = 0;
        rc = 0x5ff - ((-xoffset + x_offs) & 0x7ff);
        le = rc - (ra_cl & 0x3ff);
        re = rc + 1 + (ra_cr & 0x3ff);
        if (ra_cl || ra_cr) lnd = 1;
        begin = le + 1; if (begin < 0) begin = 0;
        end = re; if (end > screen_width) end = screen_width;
        if (re < 0) { r_over = -re; re = 0; }
        if (le >= screen_width) { l_over = le - screen_width + 1; le = screen_width - 1; }
        bgonly = (rc > (screen_width - 2 + 512)) ? 1 : 0;

        color = static_cast<uint16_t>(((palette_offs + colbank + paloffs) << 4) + (type ? 1 : 4));
        pri = static_cast<uint16_t>(pr[2] << 12);
        x_index = (-xoffset + x_offs + begin) & 0x7ff;
        roada = roada_line + screen_width - 1 - begin;
        if (lnd && begin < end) {
            for (i = begin; i < end; ++i) {
                if (tile) {
                    pixel = road_pixel(tile, x_index);
                    if (pixel || !road_trans) {
                        if (type) pixel = (pixel - 1) & 3;
                        *roada-- = static_cast<uint16_t>((color + pixel) | pri);
                    } else {
                        *roada-- = 0xf000;
                    }
                } else {
                    roada--;
                }
                x_index = (x_index + 1) & 0x7ff;
            }
        }
        color = static_cast<uint16_t>(((palette_offs + colbank + palloffs) << 4) + (type ? 1 : 4));
        pri = static_cast<uint16_t>(pr[0] << 12);
        if (bgonly) {
            if (ra_cl & 0x8000) {
                roada = roada_line;
                for (i = 0; i < screen_width; ++i)
                    *roada++ = static_cast<uint16_t>(color + (type ? 3 : 0));
            }
        } else if (le >= 0 && le < screen_width) {
            x_index = (511 - l_over) & 0x7ff;
            roada = roada_line + screen_width - 1 - le;
            if (lnd) {
                for (i = le; i >= 0; --i) {
                    pixel = road_pixel(tile, x_index);
                    pixpri = (pixel == 0) ? 0 : pri;
                    if (pixel == 0 && !(ra_cl & 0x8000)) {
                        roada++;
                    } else {
                        if (type) pixel = (pixel - 1) & 3;
                        *roada++ = static_cast<uint16_t>((color + pixel) | pixpri);
                    }
                    x_index = (x_index - 1) & 0x7ff;
                }
            }
        }
        color = static_cast<uint16_t>(((palette_offs + colbank + palroffs) << 4) + (type ? 1 : 4));
        pri = static_cast<uint16_t>(pr[1] << 12);
        if (re < screen_width && re >= 0) {
            x_index = (512 + r_over) & 0x7ff;
            roada = roada_line + screen_width - 1 - re;
            if (lnd) {
                for (i = re; i < screen_width; ++i) {
                    pixel = road_pixel(tile, x_index);
                    pixpri = (pixel == 0) ? 0 : pri;
                    if (pixel == 0 && !(ra_cr & 0x8000)) {
                        roada--;
                    } else {
                        if (type) pixel = (pixel - 1) & 3;
                        *roada-- = static_cast<uint16_t>((color + pixel) | pixpri);
                    }
                    x_index = (x_index + 1) & 0x7ff;
                }
            }
        }

        // ---- ROAD B ----
        palroffs = (rb_cr & 0x1000) >> 11;
        palloffs = (rb_cl & 0x1000) >> 11;
        xoffset  = rb_bc & 0x7ff;
        paloffs  = (rb_bc & 0x1800) >> 11;
        colbank  = (rd_ram(ri2 + 3) & 0xf000) >> 10;
        tile     = rd_ram(ri2 + 3) & 0x3ff;
        r_over = 0; l_over = 0;
        rc = 0x5ff - ((-xoffset + x_offs) & 0x7ff);
        le = rc - (rb_cl & 0x3ff);
        re = rc + 1 + (rb_cr & 0x3ff);
        if ((rb_cl || rb_cr) && ((road_ctrl & 0x800) || (type == 2))) {
            dtr = 1; lnd = 1;
        } else {
            dtr = 0;
        }
        begin = le + 1; if (begin < 0) begin = 0;
        end = re; if (end > screen_width) end = screen_width;
        if (re < 0) { r_over = -re; re = 0; }
        if (le >= screen_width) { l_over = le - screen_width + 1; le = screen_width - 1; }
        bgonly = (rc > (screen_width - 2 + 512)) ? 1 : 0;

        color = static_cast<uint16_t>(((palette_offs + colbank + paloffs) << 4) + (type ? 1 : 4));
        pri = static_cast<uint16_t>(pr[5] << 12);
        x_index = (-xoffset + x_offs + begin) & 0x7ff;
        if (x_index > 0x3ff) {
            roadb = roadb_line + screen_width - 1 - begin;
            if (dtr && tile && begin < end) {
                for (i = begin; i < end; ++i) {
                    pixel = road_pixel(tile, x_index);
                    if (pixel || !road_trans) {
                        if (type) pixel = (pixel - 1) & 3;
                        *roadb-- = static_cast<uint16_t>((color + pixel) | pri);
                    } else {
                        *roadb-- = 0xf000;
                    }
                    x_index = (x_index + 1) & 0x7ff;
                }
            }
        }
        color = static_cast<uint16_t>(((palette_offs + colbank + palloffs) << 4) + (type ? 1 : 4));
        pri = static_cast<uint16_t>(pr[3] << 12);
        if (bgonly) {
            if ((rb_cl & 0x8000) && dtr) {
                roadb = roadb_line;
                for (i = 0; i < screen_width; ++i)
                    *roadb++ = static_cast<uint16_t>(color + (type ? 3 : 0));
            }
        } else if (le >= 0 && le < screen_width) {
            x_index = (511 - l_over) & 0x7ff;
            roadb = roadb_line + screen_width - 1 - le;
            if (dtr) {
                for (i = le; i >= 0; --i) {
                    pixel = road_pixel(tile, x_index);
                    pixpri = (pixel == 0) ? 0 : pri;
                    if (pixel == 0 && !(rb_cl & 0x8000)) {
                        roadb++;
                    } else {
                        if (type) pixel = (pixel - 1) & 3;
                        *roadb++ = static_cast<uint16_t>((color + pixel) | pixpri);
                    }
                    if (--x_index < 0) break;
                }
            }
        }
        color = static_cast<uint16_t>(((palette_offs + colbank + palroffs) << 4) + (type ? 1 : 4));
        pri = static_cast<uint16_t>(pr[4] << 12);
        if (re < screen_width && re >= 0) {
            x_index = (512 + r_over) & 0x7ff;
            roadb = roadb_line + screen_width - 1 - re;
            if (dtr) {
                for (i = re; i < screen_width; ++i) {
                    pixel = road_pixel(tile, x_index);
                    pixpri = (pixel == 0) ? 0 : pri;
                    if (pixel == 0 && !(rb_cr & 0x8000)) {
                        roadb--;
                    } else {
                        if (type) pixel = (pixel - 1) & 3;
                        *roadb-- = static_cast<uint16_t>((color + pixel) | pixpri);
                    }
                    if (++x_index > 0x3ff) break;
                }
            }
        }

        // Merge A and B by priority into this scanline of the road layer.
        if (lnd) {
            for (i = 0; i < screen_width; ++i) {
                const uint16_t va = roada_line[i], vb = roadb_line[i];
                uint16_t o;
                if (va == 0x8000)                    o = vb & 0x8fff;
                else if (vb == 0x8000)               o = va & 0x8fff;
                else if ((vb & 0x7000) > (va & 0x7000)) o = vb & 0x8fff;
                else                                 o = va & 0x8fff;
                road_layer_[static_cast<std::size_t>(y) * screen_width + i] = o;
            }
        }
    }
}

void board::draw_sprites(uint16_t* pens, int stride) {
    const int W = kScreenW, H = kScreenH, y_offs = 5, top_clip = 16;
    static const uint8_t primasks[2] = {0xf0, 0xfc};

    // 16x8 tiles, 4bpp. MAME's decode assigns planeoffset[0] to the most
    // significant bit, so byte pair p supplies bit (3-p) -- reversing this
    // silently recolours every sprite.
    const auto sprite_pixel = [this](unsigned code, int x, int y) -> int {
        const unsigned base = (code * 64u) & 0x1fffffu;
        int px = 0;
        for (int p = 0; p < 4; ++p) {
            const uint8_t b =
                gfx_spr_[(base + y * 8 + p * 2 + (x >> 3)) & 0x1fffffu];
            px |= ((b >> (7 - (x & 7))) & 1) << (3 - p);
        }
        return px;
    };
    const auto smap_word = [this](unsigned i) -> int {
        i &= 0x3ffffu;
        return gfx_smap_[i * 2] | (gfx_smap_[i * 2 + 1] << 8);
    };

    for (int offs = 0; offs < 0x700 / 2; offs += 4) {
        const auto sw = [this, offs](int n) {
            return (spr_[(offs + n) * 2] << 8) | spr_[(offs + n) * 2 + 1];
        };
        const int d0 = sw(0), d1 = sw(1), d2 = sw(2), d3 = sw(3);
        int zoomy = (d0 & 0xfe00) >> 9;
        int y = d0 & 0x1ff;
        const int tile = d1 & 0x7ff;
        const int priority = (d2 & 0x8000) >> 15;
        const int fx = (d2 & 0x4000) >> 14, fy = (d2 & 0x2000) >> 13;
        int x = d2 & 0x1ff;
        const int color = (d3 & 0xff00) >> 8;
        int zoomx = d3 & 0x7f;
        if (!tile) continue;

        const unsigned mo = static_cast<unsigned>(tile) << 7;
        zoomx += 1;
        zoomy += 1;
        y += y_offs + (128 - zoomy);
        if (x > 0x140) x -= 0x200;
        if (y > 0x140) y -= 0x200;
        const uint8_t pmask = primasks[priority];

        for (int ch = 0; ch < 128; ++ch) {
            const int k = ch % 8, j = ch / 8;
            const int spx = fx ? (7 - k) : k, spy = fy ? (15 - j) : j;
            const int code = smap_word(mo + spx + (spy << 3));
            if (code == 0xffff) continue;
            const int cx = x + ((k * zoomx) / 8), cy = y + ((j * zoomy) / 16);
            const int zx = x + (((k + 1) * zoomx) / 8) - cx;
            const int zy = y + (((j + 1) * zoomy) / 16) - cy;
            if (zx <= 0 || zy <= 0) continue;
            const int dx0 = (cx < 0) ? -cx : 0;
            const int dx1 = (cx + zx > W) ? (W - cx) : zx;
            if (dx0 >= dx1) continue;
            for (int dy = 0; dy < zy; ++dy) {
                const int oy = cy + dy;
                if (oy < top_clip || oy >= H) continue;
                int sy = (dy * 8) / zy;
                if (fy) sy = 7 - sy;
                uint16_t* o = pens + oy * stride + cx + dx0;
                uint8_t* pr = pri_layer_.data() + oy * W + cx + dx0;
                for (int dx = dx0; dx < dx1; ++dx) {
                    int sx = (dx * 16) / zx;
                    if (fx) sx = 15 - sx;
                    const int pix = sprite_pixel(static_cast<unsigned>(code), sx, sy);
                    if (!pix) continue;
                    if (((1u << (pr[dx - dx0] & 0x1f)) & pmask) == 0)
                        o[dx - dx0] = static_cast<uint16_t>(color * 16 + pix);
                    pr[dx - dx0] = 31;
                }
            }
        }
    }
}

void board::render_frame() {
    if (gfx_scn_.empty()) return;
    draw_road();
    std::fill(pri_layer_.begin(), pri_layer_.end(), uint8_t{0});

    uint16_t* pens = pen_buffer_.data();
    const int stride = kScreenW;

    // TC0100SCN scroll registers. The chip's built-in offsets for this
    // board put BG at +16 and TX at +23.
    const int bgsx = -static_cast<int16_t>(r16(scnctl_.data() + 0));
    const int bgsy = -static_cast<int16_t>(r16(scnctl_.data() + 6));
    const int fgsx = -static_cast<int16_t>(r16(scnctl_.data() + 2));
    const int fgsy = -static_cast<int16_t>(r16(scnctl_.data() + 8));
    const int txsx = -static_cast<int16_t>(r16(scnctl_.data() + 4));
    const int txsy = -static_cast<int16_t>(r16(scnctl_.data() + 10));
    const int ctrl6 = r16(scnctl_.data() + 12);
    const int dis = ctrl6 & 0xf7;      // layer disable: 1=BG0 2=BG1 4=TX
    const int bottom = (ctrl6 >> 3) & 1;   // which BG is the opaque layer
    const bool bg0_on = !(dis & 1), bg1_on = !(dis & 2), tx_on = !(dis & 4);

    const auto bg_pix = [this](unsigned code, int x, int y) -> int {
        const unsigned off = (code * 32u + y * 4u + (x >> 1)) & 0x7ffffu;
        const uint8_t b = gfx_scn_[off];
        return (x & 1) ? (b & 0xf) : (b >> 4);
    };
    // The TX layer's characters live in scroll RAM, not in a gfx ROM.
    const auto tx_pix = [this](unsigned code, int x, int y) -> int {
        const uint8_t* c = scn_.data() + 0x6000 + code * 16u + y * 2u;
        return ((c[0] >> (7 - x)) & 1) | (((c[1] >> (7 - x)) & 1) << 1);
    };

    for (int y = 0; y < kScreenH; ++y) {
        uint16_t* o = pens + y * stride;
        // BG1 is the pseudo-3D ground: per-row X scroll from 0xc400 and a
        // per-column Y warp from 0xe000, applied per pixel below.
        const int fg_rowbase =
            fgsx - static_cast<int16_t>(r16(scn_.data() + 0xc400 + (y & 0x1ff) * 2)) + 16;
        const int fg_srcy = fgsy + y;
        for (int x = 0; x < kScreenW; ++x) {
            int pen = 0;
            int b0px = 0, b0pen = 0, b1px = 0, b1pen = 0;
            {   // BG0: flat, no row scroll
                const int wx = (x + bgsx + 16) & 511, wy = (y + bgsy) & 511;
                const uint8_t* e = scn_.data() + ((wy >> 3) * 64 + (wx >> 3)) * 4;
                const int attr = r16(e), code = r16(e + 2);
                const int c = attr & 0xff;
                const int fxx = (attr >> 14) & 1, fyy = (attr >> 15) & 1;
                b0px = bg_pix(static_cast<unsigned>(code),
                              fxx ? 7 - (wx & 7) : (wx & 7),
                              fyy ? 7 - (wy & 7) : (wy & 7));
                b0pen = c * 16 + b0px;
            }
            {   // BG1: row scroll in X, column scroll in Y
                const int sx = (fg_rowbase + x) & 511;
                const int col = static_cast<int16_t>(
                    r16(scn_.data() + 0xe000 + ((sx >> 3) & 0x7f) * 2));
                const int sy = (fg_srcy - col) & 511;
                const uint8_t* e = scn_.data() + 0x8000 + ((sy >> 3) * 64 + (sx >> 3)) * 4;
                const int attr = r16(e), code = r16(e + 2);
                const int c = attr & 0xff;
                const int fxx = (attr >> 14) & 1, fyy = (attr >> 15) & 1;
                b1px = bg_pix(static_cast<unsigned>(code),
                              fxx ? 7 - (sx & 7) : (sx & 7),
                              fyy ? 7 - (sy & 7) : (sy & 7));
                b1pen = c * 16 + b1px;
            }
            // The bottom layer draws opaque (pen 0 included); the top one
            // is transparent on pen 0.
            if (bottom == 0) {
                if (bg0_on) pen = b0pen;
                if (bg1_on && b1px) pen = b1pen;
            } else {
                if (bg1_on) pen = b1pen;
                if (bg0_on && b0px) pen = b0pen;
            }
            {   // road
                const uint16_t rv = road_layer_[static_cast<std::size_t>(y) * kScreenW + x];
                if (!(rv & 0x8000)) {
                    pen = rv & 0xfff;
                    pri_layer_[static_cast<std::size_t>(y) * kScreenW + x] =
                        (y > road_priority_switch_line_) ? 2 : 1;
                }
            }
            if (tx_on) {   // TX, transparent
                const int wx = (x + txsx + 23) & 511, wy = (y + txsy) & 511;
                const int a = r16(scn_.data() + 0x4000 + ((wy >> 3) * 64 + (wx >> 3)) * 2);
                const int code = a & 0xff, c = (a >> 8) & 0x3f;
                const int fxx = (a >> 14) & 1, fyy = (a >> 15) & 1;
                const int px = tx_pix(static_cast<unsigned>(code),
                                      fxx ? 7 - (wx & 7) : (wx & 7),
                                      fyy ? 7 - (wy & 7) : (wy & 7));
                if (px) {
                    pen = c * 16 + px;
                    pri_layer_[static_cast<std::size_t>(y) * kScreenW + x] = 4;
                }
            }
            o[x] = static_cast<uint16_t>(pen);
        }
    }

    draw_sprites(pens, stride);

    // Resolve pens through the TC0110PCR palette (xRGB555) to RGBA8888.
    for (std::size_t i = 0; i < frame_.size(); ++i) {
        const uint16_t d = pal_[pens[i] & 0xfff];
        const int r5 = (d >> 10) & 0x1f, g5 = (d >> 5) & 0x1f, b5 = d & 0x1f;
        const uint32_t r = static_cast<uint32_t>((r5 * 255 + 15) / 31);
        const uint32_t g = static_cast<uint32_t>((g5 * 255 + 15) / 31);
        const uint32_t b = static_cast<uint32_t>((b5 * 255 + 15) / 31);
        frame_[i] = r | (g << 8) | (b << 16) | 0xff000000u;
    }
}

}  // namespace taitoz
