#include "sega/model2/model2_video.h"

#include "system24_tile_video.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace {
uint16_t word_at(const std::vector<uint8_t>& memory, std::size_t index) {
    const std::size_t offset = index * 2;
    if (offset + 1 >= memory.size()) return 0;
    return static_cast<uint16_t>(memory[offset]) |
           (static_cast<uint16_t>(memory[offset + 1]) << 8);
}

uint8_t model2_gamma(uint8_t value) {
    if (value <= 64) return 0;
    return static_cast<uint8_t>(
        std::min(255, (static_cast<int>(value) - 64) * 255 / 191));
}

uint32_t model2_color(uint16_t color,
                      const std::vector<uint8_t>& translation,
                      unsigned luma = 0x40) {
    luma &= 0xff;
    const unsigned red_index = luma + ((color & 0x1f) << 8);
    const unsigned green_index = 0x2000 + luma + (((color >> 5) & 0x1f) << 8);
    const unsigned blue_index = 0x4000 + luma + (((color >> 10) & 0x1f) << 8);
    const uint8_t red = model2_gamma(
        static_cast<uint8_t>(word_at(translation, red_index)));
    const uint8_t green = model2_gamma(
        static_cast<uint8_t>(word_at(translation, green_index)));
    const uint8_t blue = model2_gamma(
        static_cast<uint8_t>(word_at(translation, blue_index)));
    return (static_cast<uint32_t>(red) << 16) |
           (static_cast<uint32_t>(green) << 8) | blue;
}

uint32_t memory_rgba(uint32_t rgb) {
    return 0xff000000U | ((rgb & 0x0000ffU) << 16) |
           (rgb & 0x00ff00U) | ((rgb & 0xff0000U) >> 16);
}

struct screen_vertex {
    float x;
    float y;
    float z;
    float inverse_z;
    float u_over_z;
    float v_over_z;
};

float edge(const screen_vertex& a, const screen_vertex& b,
           float x, float y) {
    return (x - a.x) * (b.y - a.y) - (y - a.y) * (b.x - a.x);
}

uint32_t little_dword(const std::vector<uint8_t>& memory,
                      std::size_t index) {
    const std::size_t offset = index * 4;
    if (offset + 3 >= memory.size()) return 0;
    return static_cast<uint32_t>(memory[offset]) |
           (static_cast<uint32_t>(memory[offset + 1]) << 8) |
           (static_cast<uint32_t>(memory[offset + 2]) << 16) |
           (static_cast<uint32_t>(memory[offset + 3]) << 24);
}

uint32_t float_bits(float value) {
    uint32_t bits{};
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

int32_t model2_fast_log2(float value) {
    if (value < 0.0f) return 0;
    const uint32_t bits = float_bits(value) >> 16;
    const int32_t exponent = static_cast<int32_t>(bits >> 7) - 127;
    constexpr std::array<uint8_t, 128> fraction{{
        0, 2, 5, 8, 11, 14, 16, 19, 22, 25, 27, 30, 33, 35, 38, 40,
        43, 46, 48, 51, 53, 56, 58, 61, 63, 65, 68, 70, 73, 75, 77, 80,
        82, 84, 87, 89, 91, 93, 96, 98, 100, 102, 104, 106, 109, 111,
        113, 115, 117, 119, 121, 123, 125, 127, 129, 132, 134, 136,
        138, 140, 141, 143, 145, 147, 149, 151, 153, 155, 157, 159,
        161, 162, 164, 166, 168, 170, 172, 173, 175, 177, 179, 181,
        182, 184, 186, 188, 189, 191, 193, 194, 196, 198, 200, 201,
        203, 205, 206, 208, 209, 211, 213, 214, 216, 218, 219, 221,
        222, 224, 225, 227, 229, 230, 232, 233, 235, 236, 238, 239,
        241, 242, 244, 245, 247, 248, 250, 251, 253, 254,
    }};
    return exponent * 256 | fraction[bits & 127];
}

uint32_t packed_lerp(uint32_t first, uint32_t second, unsigned amount) {
    return (first + (((second - first) * amount) >> 8)) & 0x00ff00ffU;
}

uint8_t texture_texel_at(const model2_geometry_polygon& polygon,
                         const std::vector<uint8_t>& sheet0,
                         const std::vector<uint8_t>& sheet1,
                         int base_x, int base_y, int x, int y,
                         unsigned level, bool microtexture) {
    int sheet_x = base_x + x;
    int sheet_y = base_y + y;
    if (sheet_x >= 1024) {
        sheet_x -= 1024;
        sheet_y ^= 1024;
    }
    if (sheet_x < 0 || sheet_x >= 1024 ||
        sheet_y < 0 || sheet_y >= 2048)
        return 0x0f;
    const std::size_t packed_offset =
        static_cast<std::size_t>(sheet_y / 2) * 512 + sheet_x / 2;
    const unsigned base_sheet =
        (polygon.texture_header[2] & 0x1000) ? 1U : 0U;
    const unsigned sheet_index = microtexture ?
        (base_sheet ^ 1U) : (base_sheet ^ (level & 1U));
    const std::vector<uint8_t>& sheet = sheet_index ? sheet1 : sheet0;
    uint32_t word = little_dword(sheet, packed_offset >> 1);
    if (packed_offset & 1) word >>= 16;
    if ((y & 1) == 0) word >>= 8;
    if ((x & 1) == 0) word >>= 4;
    return static_cast<uint8_t>(word & 0x0f);
}

uint32_t filtered_texture_sample(
        const model2_geometry_polygon& polygon,
        const std::vector<uint8_t>& sheet0,
        const std::vector<uint8_t>& sheet1,
        int level, int32_t u, int32_t v, bool microtexture) {
    const uint16_t header0 = polygon.texture_header[0];
    const uint16_t header2 = polygon.texture_header[2];
    int texture_width{};
    int texture_height{};
    int texture_x{};
    int texture_y{};
    if (microtexture) {
        texture_width = texture_height = 128;
        texture_x = ((header2 >> 13) & 1) * 128;
        texture_y = ((header2 >> 14) & 3) * 128;
        const unsigned minimum_lod = (header0 >> 10) & 3;
        const unsigned coordinate_shift = 1U << minimum_lod;
        u = static_cast<int32_t>(static_cast<uint32_t>(u) <<
                                 coordinate_shift);
        v = static_cast<int32_t>(static_cast<uint32_t>(v) <<
                                 coordinate_shift);
        level = 0;
    } else {
        texture_width = (32 << (header0 & 7)) >> level;
        texture_height = (32 << ((header0 >> 3) & 7)) >> level;
        texture_x = ((32 * (header2 & 0x3f) - 2048) >> level) & 2047;
        texture_y = ((32 * ((header2 >> 6) & 0x1f) - 1024) >> level) &
                    1023;
        u >>= level;
        v >>= level;
    }

    const bool mirror_x = (header0 & 0x0100) != 0;
    const bool mirror_y = (header0 & 0x0200) != 0;
    const bool wrap_x = (header0 & 0x0040) != 0 && !mirror_x;
    const bool wrap_y = (header0 & 0x0080) != 0 && !mirror_y;
    if (mirror_x && (u & (texture_width << 8))) u = ~u;
    if (mirror_y && (v & (texture_height << 8))) v = ~v;

    u -= 0x80;
    v -= 0x80;
    unsigned u_fraction = static_cast<unsigned>(u) & 0xff;
    unsigned v_fraction = static_cast<unsigned>(v) & 0xff;
    int u0 = (u >> 8) & (texture_width - 1);
    int u1 = (u0 + 1) & (texture_width - 1);
    int v0 = (v >> 8) & (texture_height - 1);
    int v1 = (v0 + 1) & (texture_height - 1);
    if (!wrap_x && u1 == 0) {
        if (u_fraction >= 0x80) {
            u0 = u1;
            ++u1;
            u_fraction = 0;
        } else {
            u1 = u0;
            --u0;
            u_fraction = 0x100;
        }
    }
    if (!wrap_y && v1 == 0) {
        if (v_fraction >= 0x80) {
            v0 = 0;
            ++v1;
            v_fraction = 0;
        } else {
            v1 = v0;
            --v0;
            v_fraction = 0x100;
        }
    }

    const auto texel = [&](int x, int y) {
        return static_cast<uint32_t>(texture_texel_at(
            polygon, sheet0, sheet1, texture_x, texture_y, x, y,
            static_cast<unsigned>(level), microtexture)) << 4;
    };
    uint32_t tex00 = texel(u0, v0);
    uint32_t tex01 = texel(u1, v0);
    uint32_t tex10 = texel(u0, v1);
    uint32_t tex11 = texel(u1, v1);
    if (polygon.renderer == 3) {
        if (tex00 != 0xf0) tex00 |= 0x00800000;
        if (tex01 != 0xf0) tex01 |= 0x00800000;
        if (tex10 != 0xf0) tex10 |= 0x00800000;
        if (tex11 != 0xf0) tex11 |= 0x00800000;
        if (tex00 == 0x000000f0) tex00 = tex01 & 0xff;
        if (tex01 == 0x000000f0) tex01 = tex00 & 0xff;
        if (tex10 == 0x000000f0) tex10 = tex11 & 0xff;
        if (tex11 == 0x000000f0) tex11 = tex10 & 0xff;
    }
    uint32_t top = packed_lerp(tex00, tex01, u_fraction);
    uint32_t bottom = packed_lerp(tex10, tex11, u_fraction);
    if (polygon.renderer == 3) {
        if (top == 0x000000f0) top = bottom & 0xff;
        if (bottom == 0x000000f0) bottom = top & 0xff;
    }
    return packed_lerp(top, bottom, v_fraction);
}
} // namespace

model2_texture_sample model2_sample_texture(
        const model2_geometry_polygon& polygon,
        const std::vector<uint8_t>& sheet0,
        const std::vector<uint8_t>& sheet1,
        float texture_u, float texture_v, float camera_z) {
    const uint16_t header0 = polygon.texture_header[0];
    const int texture_width = 32 << (header0 & 7);
    const int texture_height = 32 << ((header0 >> 3) & 7);
    int maximum_level = 0;
    for (int dimension = std::min(texture_width, texture_height);
         dimension > 2; dimension >>= 1)
        ++maximum_level;

    const int32_t mml = -polygon.texture_lod + model2_fast_log2(camera_z);
    const int level = std::clamp(mml >> 7, 0, maximum_level);
    const int32_t u = static_cast<int32_t>(texture_u * 256.0f);
    const int32_t v = static_cast<int32_t>(texture_v * 256.0f);
    uint32_t sample = filtered_texture_sample(
        polygon, sheet0, sheet1, level, u, v, false);
    if (mml > 0 && level < maximum_level) {
        const uint32_t next = filtered_texture_sample(
            polygon, sheet0, sheet1, level + 1, u, v, false);
        sample = packed_lerp(sample, next,
                             static_cast<unsigned>(mml & 127) << 1);
    } else if ((header0 & 0x1000) != 0 && mml < 0) {
        const uint32_t micro = filtered_texture_sample(
            polygon, sheet0, sheet1, 0, u, v, true);
        const unsigned minimum_lod = (header0 >> 10) & 3;
        const unsigned amount = std::min(
            static_cast<unsigned>((-mml) >> minimum_lod), 127U);
        sample = packed_lerp(sample, micro, amount);
    }

    const bool covered = polygon.renderer != 3 || sample >= 0x00400000;
    return {static_cast<uint8_t>((sample & 0xff) >> 1), covered};
}

model2_video::model2_video()
    : m_rgb_pixels(static_cast<std::size_t>(width * height)),
      m_rgba_pixels(static_cast<std::size_t>(width * height)),
      m_base_rgb(static_cast<std::size_t>(width * height)),
      m_foreground_rgb(static_cast<std::size_t>(width * height)),
      m_base_rgba(static_cast<std::size_t>(width * height)),
      m_foreground_rgba(static_cast<std::size_t>(width * height)),
      m_depth(static_cast<std::size_t>(width * height)) {}

void model2_video::render_layers(
    const std::vector<uint8_t>& tile_ram,
    const std::vector<uint8_t>& character_ram,
    const std::vector<uint8_t>& palette_ram,
    const std::vector<uint8_t>& color_translation) {
    const auto palette_lookup =
        [&palette_ram, &color_translation](uint16_t entry, unsigned pen) {
            const unsigned index = ((entry >> 7) & 0xff) * 16 + pen;
            return model2_color(word_at(palette_ram, index),
                                color_translation);
        };
    const uint32_t background =
        model2_color(word_at(palette_ram, 0), color_translation);
    std::fill(m_base_rgb.begin(), m_base_rgb.end(), background);
    system24_draw_tile_pair(
        m_base_rgb.data(), width, height, tile_ram, character_ram,
        1, -1, true, palette_lookup);
    system24_draw_tile_pair(
        m_base_rgb.data(), width, height, tile_ram, character_ram,
        0, 0, false, palette_lookup);

    constexpr uint32_t transparent_marker = 0x01000000U;
    std::fill(m_foreground_rgb.begin(), m_foreground_rgb.end(),
              transparent_marker);
    system24_draw_tile_pair(
        m_foreground_rgb.data(), width, height, tile_ram, character_ram,
        1, 1, false, palette_lookup);
    system24_draw_tile_pair(
        m_foreground_rgb.data(), width, height, tile_ram, character_ram,
        0, 1, false, palette_lookup);

    for (std::size_t index = 0; index < m_base_rgb.size(); ++index) {
        m_base_rgba[index] = memory_rgba(m_base_rgb[index]);
        m_foreground_rgba[index] =
            m_foreground_rgb[index] == transparent_marker ? 0U :
            memory_rgba(m_foreground_rgb[index]);
    }
}

void model2_video::render(
    const std::vector<uint8_t>& tile_ram,
    const std::vector<uint8_t>& character_ram,
    const std::vector<uint8_t>& palette_ram,
    const std::vector<uint8_t>& color_translation,
    const std::vector<uint8_t>& framebuffer_a,
    const std::vector<uint8_t>& framebuffer_b,
    const std::vector<uint8_t>& texture_sheet_0,
    const std::vector<uint8_t>& texture_sheet_1,
    const std::vector<uint8_t>& luma_ram,
    const std::vector<model2_geometry_polygon>& polygons,
    uint32_t frame_number, uint32_t render_control,
    int16_t horizontal_offset, int16_t vertical_offset) {
    render_layers(tile_ram, character_ram, palette_ram, color_translation);
    m_rgb_pixels = m_base_rgb;

    std::fill(m_depth.begin(), m_depth.end(),
              std::numeric_limits<float>::infinity());
    if ((render_control & 1) == 0) {
        const auto draw_triangle = [&](const model2_geometry_polygon& polygon,
                                       const model2_geometry_vertex& vertex0,
                                       const model2_geometry_vertex& vertex1,
                                       const model2_geometry_vertex& vertex2,
                                       uint32_t color,
                                       const std::array<uint32_t, 128>&
                                           texture_colors) {
            const auto project = [&](const model2_geometry_vertex& vertex) {
                const float reciprocal = 1.0f /
                    (vertex.z + std::numeric_limits<float>::min());
                return screen_vertex{
                    static_cast<float>(horizontal_offset + polygon.center[0]) +
                        vertex.x * reciprocal,
                    static_cast<float>(384 - polygon.center[1] +
                                       vertical_offset) - vertex.y * reciprocal,
                    vertex.z, reciprocal,
                    vertex.u * reciprocal,
                    vertex.v * reciprocal};
            };
            const screen_vertex a = project(vertex0);
            const screen_vertex b = project(vertex1);
            const screen_vertex c = project(vertex2);
            if (!std::isfinite(a.x) || !std::isfinite(a.y) ||
                !std::isfinite(b.x) || !std::isfinite(b.y) ||
                !std::isfinite(c.x) || !std::isfinite(c.y))
                return;
            const float area = edge(a, b, c.x, c.y);
            if (std::fabs(area) < 0.0001f) return;
            const int viewport_left = polygon.viewport[0] + horizontal_offset;
            const int viewport_right = polygon.viewport[2] + horizontal_offset;
            const int viewport_top = 384 - polygon.viewport[3] + vertical_offset;
            const int viewport_bottom = 384 - polygon.viewport[1] + vertical_offset;
            const int left = std::max({0, std::min(viewport_left, viewport_right),
                static_cast<int>(std::floor(std::min({a.x, b.x, c.x})))});
            const int right = std::min({width - 1,
                std::max(viewport_left, viewport_right),
                static_cast<int>(std::ceil(std::max({a.x, b.x, c.x})))});
            const int top = std::max({0, std::min(viewport_top, viewport_bottom),
                static_cast<int>(std::floor(std::min({a.y, b.y, c.y})))});
            const int bottom = std::min({height - 1,
                std::max(viewport_top, viewport_bottom),
                static_cast<int>(std::ceil(std::max({a.y, b.y, c.y})))});
            if (left > right || top > bottom) return;
            for (int y = top; y <= bottom; ++y) {
                for (int x = left; x <= right; ++x) {
                    const float px = static_cast<float>(x) + 0.5f;
                    const float py = static_cast<float>(y) + 0.5f;
                    const float wa = edge(b, c, px, py) / area;
                    const float wb = edge(c, a, px, py) / area;
                    const float wc = edge(a, b, px, py) / area;
                    if (wa < 0.0f || wb < 0.0f || wc < 0.0f) continue;
                    const float z = wa * a.z + wb * b.z + wc * c.z;
                    const std::size_t pixel = static_cast<std::size_t>(y) *
                                              width + x;
                    // The board uses a one-bit fill buffer, not camera-Z.
                    // Polygons arrive here in hardware draw order, so the
                    // first visible fragment owns this pixel for the frame.
                    if (z < 0.0f || std::isfinite(m_depth[pixel])) continue;
                    if ((polygon.texture_header[0] & 0x8000) != 0 &&
                        ((x ^ y) & 1) == 0)
                        continue;
                    uint32_t pixel_color = color;
                    if (polygon.renderer & 2) {
                        const float inverse_z = wa * a.inverse_z +
                                                wb * b.inverse_z +
                                                wc * c.inverse_z;
                        if (inverse_z <= 0.0f) continue;
                        const float texture_u =
                            (wa * a.u_over_z + wb * b.u_over_z +
                             wc * c.u_over_z) / inverse_z;
                        const float texture_v =
                            (wa * a.v_over_z + wb * b.v_over_z +
                             wc * c.v_over_z) / inverse_z;
                        const float camera_z = 1.0f / inverse_z;
                        const model2_texture_sample texel =
                            model2_sample_texture(
                                polygon, texture_sheet_0, texture_sheet_1,
                                texture_u, texture_v, camera_z);
                        if (!texel.covered) continue;
                        pixel_color = texture_colors[texel.luma_index];
                    }
                    m_depth[pixel] = 0.0f;
                    m_rgb_pixels[pixel] = pixel_color;
                }
            }
        };
        for (std::size_t polygon_index :
             model2_polygon_draw_order(polygons)) {
            const model2_geometry_polygon& polygon =
                polygons[polygon_index];
            const model2_clipped_polygon clipped =
                model2_clip_polygon(polygon);
            if (clipped.vertex_count < 3) continue;
            const uint16_t palette_color =
                word_at(palette_ram, 0x1000 + polygon.color_base);
            const uint32_t color = model2_color(
                palette_color, color_translation, polygon.luma >> 2);
            std::array<uint32_t, 128> texture_colors{};
            const std::size_t luma_base =
                static_cast<std::size_t>(polygon.texture_header[1] & 0xff) << 7;
            for (unsigned texel = 0; texel < texture_colors.size(); ++texel) {
                const std::size_t luma_address = luma_base + texel;
                const unsigned texture_luma = luma_address < luma_ram.size() ?
                    luma_ram[luma_address] : 0x3f;
                const unsigned luma = std::min(
                    0x3fU, texture_luma * polygon.luma / 256U);
                texture_colors[texel] = model2_color(
                    palette_color, color_translation, luma);
            }
            for (unsigned vertex = 1; vertex + 1 < clipped.vertex_count;
                 ++vertex) {
                draw_triangle(polygon, clipped.vertices[0],
                              clipped.vertices[vertex],
                              clipped.vertices[vertex + 1], color,
                              texture_colors);
            }
        }
    }

    // In render-test mode the board exposes its alternating 512x512 direct
    // framebuffer instead of the polygon renderer output.
    if (render_control & 1) {
        const std::vector<uint8_t>& framebuffer =
            (frame_number & 1) ? framebuffer_b : framebuffer_a;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const std::size_t source =
                    static_cast<std::size_t>(y + 128) * 512 + x;
                m_rgb_pixels[static_cast<std::size_t>(y) * width + x] =
                    model2_color(word_at(framebuffer, source),
                                 color_translation);
            }
        }
    }

    // Category one is the foreground/HUD drawn after 3D composition.
    constexpr uint32_t transparent_marker = 0x01000000U;
    for (std::size_t index = 0; index < m_rgb_pixels.size(); ++index) {
        if (m_foreground_rgb[index] != transparent_marker)
            m_rgb_pixels[index] = m_foreground_rgb[index];
    }

    m_non_black_pixels = 0;
    m_frame_hash = 1469598103934665603ULL;
    for (std::size_t index = 0; index < m_rgb_pixels.size(); ++index) {
        const uint32_t rgb = m_rgb_pixels[index];
        if (rgb != 0) ++m_non_black_pixels;
        m_rgba_pixels[index] = memory_rgba(rgb);
        m_frame_hash ^= rgb;
        m_frame_hash *= 1099511628211ULL;
    }
}
