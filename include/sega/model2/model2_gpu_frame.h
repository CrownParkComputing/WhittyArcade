// Immutable Sega Model 2 frame packet consumed by the graphics worker.
#pragma once

#include "sega/model2/model2_geometry.h"

#include <cstdint>
#include <vector>

struct model2_gpu_frame {
    static constexpr int width = 496;
    static constexpr int height = 384;

    std::vector<model2_geometry_polygon> polygons;
    std::vector<uint32_t> base_rgba;
    std::vector<uint32_t> foreground_rgba;
    std::vector<uint8_t> texture_sheet_0;
    std::vector<uint8_t> texture_sheet_1;
    std::vector<uint8_t> luma;
    std::vector<uint8_t> palette;
    std::vector<uint8_t> color_translation;
    uint64_t texture_generation{};
    uint64_t color_generation{};
    int16_t horizontal_offset{};
    int16_t vertical_offset{};
};
