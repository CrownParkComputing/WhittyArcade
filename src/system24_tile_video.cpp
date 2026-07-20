#include "system24_tile_video.h"

#include <cstddef>

namespace {
uint16_t word_at(const std::vector<uint8_t>& memory, std::size_t offset) {
    const std::size_t byte = offset * 2;
    if (byte + 1 >= memory.size()) return 0;
    return static_cast<uint16_t>(memory[byte]) |
           (static_cast<uint16_t>(memory[byte + 1]) << 8);
}
} // namespace

void system24_draw_tile_pair(
    uint32_t* pixels, int width, int height,
    const std::vector<uint8_t>& tile_ram,
    const std::vector<uint8_t>& character_ram,
    int pair, int category, bool opaque,
    const system24_palette_lookup& palette_lookup) {
    if (!pixels || width <= 0 || height <= 0 || pair < 0 || pair > 1 ||
        category < -1 || category > 1 || character_ram.size() < 32 ||
        tile_ram.size() < 0x10000 || !palette_lookup)
        return;

    const int base_page = pair * 2;
    const uint16_t pair_hscroll = word_at(tile_ram, 0x5000 + base_page);
    const uint16_t pair_vscroll = word_at(tile_ram, 0x5004 + base_page);
    const unsigned mode = (pair_vscroll >> 13) & 3;
    const std::size_t mask_base = pair ? 0x6800 : 0x6000;
    if (mode != 0 && (pair_vscroll & 0x8000)) return;

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int page = base_page;
            uint16_t hscroll_word = pair_hscroll;
            uint16_t vscroll_word = pair_vscroll;
            if (mode == 0) {
                const uint16_t mask =
                    word_at(tile_ram, mask_base + y * 4 + x / 128);
                const bool selected =
                    (mask & (uint16_t{0x8000} >> ((x & 127) >> 3))) != 0;
                if (selected) ++page;
                hscroll_word = word_at(tile_ram, 0x5000 + page);
                vscroll_word = word_at(tile_ram, 0x5004 + page);
                if (vscroll_word & 0x8000) continue;
                if (hscroll_word & 0x8000)
                    hscroll_word =
                        word_at(tile_ram, 0x4000 + 0x200 * page + y);
            } else if (mode == 1) {
                if (((-static_cast<int>(pair_vscroll)) & 0x200) == 0)
                    page ^= 1;
                if (y >= ((-static_cast<int>(pair_vscroll)) & 0x1ff))
                    page ^= 1;
                if (pair_hscroll & 0x8000)
                    hscroll_word =
                        word_at(tile_ram, 0x4000 + 0x200 * base_page + y);
            } else {
                if (pair_hscroll & 0x8000)
                    hscroll_word =
                        word_at(tile_ram, 0x4000 + 0x200 * base_page + y);
                if ((hscroll_word & 0x200) == 0) page ^= 1;
                if (x >= (hscroll_word & 0x1ff)) page ^= 1;
            }

            const int scroll_x = (-static_cast<int>(hscroll_word)) & 0x1ff;
            const int scroll_y = ((vscroll_word & 0x1ff) + y) & 0x1ff;
            const int source_x = (scroll_x + x) & 0x1ff;
            const std::size_t tile_offset =
                static_cast<std::size_t>(page) * 0x1000 +
                static_cast<std::size_t>(scroll_y >> 3) * 64 +
                (source_x >> 3);
            const uint16_t entry = word_at(tile_ram, tile_offset);
            if (category >= 0 && ((entry >> 15) & 1) != category)
                continue;

            const std::size_t character = entry & 0x3fff;
            const std::size_t row_offset = character * 32 +
                static_cast<std::size_t>(scroll_y & 7) * 4;
            if (row_offset + 3 >= character_ram.size()) continue;

            const unsigned pixel = source_x & 7;
            const std::size_t byte_offset = row_offset + ((pixel >> 1) ^ 1);
            const uint8_t packed = character_ram[byte_offset];
            const unsigned pen = (pixel & 1) ? packed & 0x0f : packed >> 4;
            if (!opaque && pen == 0) continue;
            pixels[static_cast<std::size_t>(y) * width + x] =
                palette_lookup(entry, pen);
        }
    }
}
