// Shared Sega System 24 four-page RAM tile generator used by Model 1/2.
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

using system24_palette_lookup =
    std::function<uint32_t(uint16_t tile_entry, unsigned pen)>;

void system24_draw_tile_pair(
    uint32_t* pixels, int width, int height,
    const std::vector<uint8_t>& tile_ram,
    const std::vector<uint8_t>& character_ram,
    int pair, int category, bool opaque,
    const system24_palette_lookup& palette_lookup);
