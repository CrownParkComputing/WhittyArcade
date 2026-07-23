// Framework-free Sega Model 2 video composition boundary.
#pragma once

#include "sega/model2/model2_geometry.h"

#include <cstddef>
#include <cstdint>
#include <vector>

struct model2_texture_sample {
    uint8_t luma_index{};
    bool covered{true};
};

// Sample the Model 2 packed 4-bpp texture hierarchy.  The result is the
// seven-bit translator-table index after bilinear/trilinear filtering and
// the one-bit coverage decision used by translucent polygons.
model2_texture_sample model2_sample_texture(
    const model2_geometry_polygon& polygon,
    const std::vector<uint8_t>& sheet0,
    const std::vector<uint8_t>& sheet1,
    float texture_u, float texture_v, float camera_z);

class model2_video {
public:
    static constexpr int width = 496;
    static constexpr int height = 384;

    model2_video();
    void render_layers(const std::vector<uint8_t>& tile_ram,
                       const std::vector<uint8_t>& character_ram,
                       const std::vector<uint8_t>& palette_ram,
                       const std::vector<uint8_t>& color_translation);
    void render(const std::vector<uint8_t>& tile_ram,
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
                int16_t horizontal_offset, int16_t vertical_offset);

    const uint32_t* rgba_pixels() const { return m_rgba_pixels.data(); }
    const uint32_t* base_rgba_pixels() const { return m_base_rgba.data(); }
    const uint32_t* foreground_rgba_pixels() const {
        return m_foreground_rgba.data();
    }
    std::size_t non_black_pixels() const { return m_non_black_pixels; }
    uint64_t frame_hash() const { return m_frame_hash; }

private:
    std::vector<uint32_t> m_rgb_pixels;
    std::vector<uint32_t> m_rgba_pixels;
    std::vector<uint32_t> m_base_rgb;
    std::vector<uint32_t> m_foreground_rgb;
    std::vector<uint32_t> m_base_rgba;
    std::vector<uint32_t> m_foreground_rgba;
    std::vector<float> m_depth;
    std::size_t m_non_black_pixels{};
    uint64_t m_frame_hash{};
};
