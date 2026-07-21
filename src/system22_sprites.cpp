#include "system22_sprites.h"

#include <algorithm>

namespace {

uint16_t read_be16(const uint8_t* data, std::size_t size,
                   std::size_t offset) {
    if (!data || offset + 1 >= size) return 0;
    return static_cast<uint16_t>(
        (static_cast<uint16_t>(data[offset]) << 8) | data[offset + 1]);
}

uint32_t read_be32(const uint8_t* data, std::size_t size,
                   std::size_t offset) {
    if (!data || offset + 3 >= size) return 0;
    return (static_cast<uint32_t>(data[offset]) << 24) |
           (static_cast<uint32_t>(data[offset + 1]) << 16) |
           (static_cast<uint32_t>(data[offset + 2]) << 8) |
           data[offset + 3];
}

int signed16(uint16_t value) {
    return static_cast<int16_t>(value);
}

void append_group(std::vector<polygon_object>& result,
                  const uint8_t* source, std::size_t source_size,
                  std::size_t source_offset,
                  std::size_t attribute_offset,
                  int sprite_count, int delta_x, int delta_y,
                  bool y_low_resolution,
                  const uint8_t* sprite_ram, std::size_t sprite_ram_size,
                  std::size_t sprite_elements) {
    if (!source || !sprite_ram || sprite_count <= 0 ||
        sprite_elements == 0)
        return;

    for (int sprite = 0; sprite < sprite_count; ++sprite) {
        if (result.size() >=
            static_cast<std::size_t>(SYSTEM22_MAX_POLYGONS_PER_FRAME))
            return;
        const std::size_t src = source_offset +
            static_cast<std::size_t>(sprite) * 16;
        const std::size_t attr = attribute_offset +
            static_cast<std::size_t>(sprite) * 8;
        if (src + 15 >= source_size || attr + 7 >= source_size) break;

        const uint32_t word0 = read_be32(source, source_size, src);
        const uint32_t word1 = read_be32(source, source_size, src + 4);
        const uint32_t word2 = read_be32(source, source_size, src + 8);
        const uint32_t word3 = read_be32(source, source_size, src + 12);
        const uint32_t attr0 = read_be32(source, source_size, attr);
        const uint32_t attr1 = read_be32(source, source_size, attr + 4);

        int x = static_cast<int>(word0 >> 16) - delta_x;
        int y = static_cast<int>(word0 & 0xffffu) - delta_y;
        int size_x = static_cast<int>(word1 >> 16);
        int size_y = static_cast<int>(word1 & 0xffffu);
        const bool flip_y = (word2 & 0x08u) != 0;
        const bool flip_x = (word2 & 0x80u) != 0;
        int rows = static_cast<int>(word2 & 7u);
        int columns = static_cast<int>((word2 >> 4) & 7u);
        if (rows == 0) rows = 8;
        if (columns == 0) columns = 8;
        const uint8_t link_type = static_cast<uint8_t>(word2 >> 16);
        const uint32_t tile_base = word3 >> 16;
        const uint8_t color = static_cast<uint8_t>(attr1 >> 16);
        const uint8_t depth_cue = static_cast<uint8_t>(attr1);

        if ((word2 & 0x200u) != 0) x -= size_x * columns - 1;
        if ((word2 & 0x100u) != 0) y -= size_y * rows - 1;
        if (flip_y) {
            y += size_y * rows - 1;
            size_y = -size_y;
        }
        if (flip_x) {
            x += size_x * columns - 1;
            size_x = -size_x;
        }
        if (y_low_resolution) {
            size_y *= 2;
            y *= 2;
        }
        if (size_x == 0 || size_y == 0) continue;

        const int clip = static_cast<int>((word2 >> 23) & 0x0eu);
        const uint32_t clip_x = read_be32(
            sprite_ram, sprite_ram_size,
            static_cast<std::size_t>(0x80 | clip) * 4);
        const uint32_t clip_y = read_be32(
            sprite_ram, sprite_ram_size,
            static_cast<std::size_t>(0x81 | clip) * 4);
        const float clip_left = static_cast<float>(std::max(
            0, signed16(static_cast<uint16_t>(clip_x >> 16)) - delta_x));
        const float clip_right = static_cast<float>(std::min(
            639, signed16(static_cast<uint16_t>(clip_x)) - delta_x));
        const float clip_top = static_cast<float>(std::max(
            0, signed16(static_cast<uint16_t>(clip_y >> 16)) - delta_y));
        const float clip_bottom = static_cast<float>(std::min(
            479, signed16(static_cast<uint16_t>(clip_y)) - delta_y));
        if (clip_left > clip_right || clip_top > clip_bottom) continue;

        int tile_offset = 0;
        for (int row = 0; row < rows; ++row) {
            for (int column = 0; column < columns; ++column, ++tile_offset) {
                if (result.size() >=
                    static_cast<std::size_t>(SYSTEM22_MAX_POLYGONS_PER_FRAME))
                    return;
                uint32_t tile = tile_base;
                if (link_type == 0xff) {
                    tile += static_cast<uint32_t>(tile_offset);
                } else {
                    const std::size_t link_word =
                        static_cast<std::size_t>(tile_offset +
                                                 link_type * 4);
                    tile += read_be16(sprite_ram, sprite_ram_size,
                                      0x800 + link_word * 2);
                }
                tile %= static_cast<uint32_t>(sprite_elements);

                const float first_x = static_cast<float>(
                    x + column * size_x);
                const float second_x = first_x + size_x;
                const float first_y = static_cast<float>(
                    y + row * size_y);
                const float second_y = first_y + size_y;
                const float left = std::min(first_x, second_x);
                const float right = std::max(first_x, second_x);
                const float top = std::min(first_y, second_y);
                const float bottom = std::max(first_y, second_y);

                polygon_object quad{};
                quad.zsort = attr0 & 0x00ffffffu;
                quad.color = color;
                quad.cmode = depth_cue == 0xfe ? 1u : 0u;
                quad.sprite = true;
                quad.sprite_tile = tile;
                quad.sprite_alpha = static_cast<uint8_t>(word3 >> 8);
                quad.direct = true;
                quad.clip_left = clip_left;
                quad.clip_right = clip_right;
                quad.clip_top = clip_top;
                quad.clip_bottom = clip_bottom;

                const int u_left = flip_x ? 31 : 0;
                const int u_right = flip_x ? 0 : 31;
                const int v_top = flip_y ? 31 : 0;
                const int v_bottom = flip_y ? 0 : 31;
                quad.vertices[0] = {left, top, 1.0f,
                                    u_left, v_top, 64};
                quad.vertices[1] = {right, top, 1.0f,
                                    u_right, v_top, 64};
                quad.vertices[2] = {right, bottom, 1.0f,
                                    u_right, v_bottom, 64};
                quad.vertices[3] = {left, bottom, 1.0f,
                                    u_left, v_bottom, 64};
                result.push_back(quad);
            }
        }
    }
}

} // namespace

std::vector<polygon_object> system22_sprite_decoder::decode(
        const uint8_t* sprite_ram, std::size_t sprite_ram_size,
        const uint8_t* vics_data, std::size_t vics_data_size,
        const uint8_t* vics_control, std::size_t vics_control_size,
        std::size_t sprite_rom_size, system22_sprite_profile profile) {
    std::vector<polygon_object> result;
    if (!sprite_ram || sprite_ram_size < 0x30000 || !vics_data ||
        vics_data_size < 0x10000 || !vics_control ||
        vics_control_size < 0x80 || sprite_rom_size < 0x400)
        return result;

    const std::size_t sprite_elements = sprite_rom_size / 0x400;
    const uint32_t control0 = read_be32(sprite_ram, sprite_ram_size, 0);
    const uint32_t control1 = read_be32(sprite_ram, sprite_ram_size, 4);
    const uint32_t control2 = read_be32(sprite_ram, sprite_ram_size, 8);
    const uint32_t control3 = read_be32(sprite_ram, sprite_ram_size, 12);
    const int delta_x = static_cast<int>(control1 & 0xffffu) +
                        static_cast<int>(control2 & 0xffffu) + 0x2d;
    const bool main_y_low = ((~control0) & 0x00040000u) != 0;
    const int delta_y = static_cast<int>(control3 >> 16) +
                        (0x2a >> (main_y_low ? 1 : 0));

    const int base = static_cast<int>(control0 & 0xffffu);
    const int main_count = static_cast<int>(control1 >> 16) - base + 1;
    if (((~control0) & 0x00010000u) != 0 &&
        main_count > 0 && main_count < 0x400) {
        append_group(result, sprite_ram, sprite_ram_size,
                     0x4000 + static_cast<std::size_t>(base) * 16,
                     0x20000 + static_cast<std::size_t>(base) * 8,
                     main_count, delta_x, delta_y, main_y_low,
                     sprite_ram, sprite_ram_size, sprite_elements);
    }

    const uint32_t vics30 = read_be32(vics_control, vics_control_size, 0x30);
    const bool vics_on = ((~vics30) & 0x01000000u) != 0;
    const bool vics_y_low = ((~vics30) & 0x04000000u) != 0;
    const auto append_vics = [&](std::size_t size_register,
                                 std::size_t source_register,
                                 std::size_t attribute_register) {
        int count = static_cast<int>(
            (read_be32(vics_control, vics_control_size, size_register) >> 4) &
            0x1ffu);
        const uint32_t source_control = read_be32(
            vics_control, vics_control_size, source_register);
        if (profile == system22_sprite_profile::dirt_dash &&
            size_register == 0x40) {
            // Dirt Dash stores the first VICS list length in the low byte of a
            // data header selected by address bit 14, rather than in reg 0x40.
            const std::size_t count_offset = source_control & 0x4000u;
            count = static_cast<int>(
                (read_be32(vics_data, vics_data_size, count_offset) & 0xffu) + 1);
        }
        const std::size_t source_offset = source_control & 0xffffu;
        const std::size_t attribute_offset = read_be32(
            vics_control, vics_control_size, attribute_register) & 0xffffu;
        if (vics_on && count > 0)
            append_group(result, vics_data, vics_data_size,
                         source_offset, attribute_offset, count,
                         delta_x, delta_y, vics_y_low,
                         sprite_ram, sprite_ram_size, sprite_elements);
    };
    append_vics(0x40, 0x48, 0x58);
    if (profile != system22_sprite_profile::dirt_dash)
        append_vics(0x60, 0x68, 0x78);
    return result;
}
