#include "namco/system22/system22_sprites.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

void put32(std::vector<uint8_t>& data, std::size_t offset, uint32_t value) {
    data[offset] = static_cast<uint8_t>(value >> 24);
    data[offset + 1] = static_cast<uint8_t>(value >> 16);
    data[offset + 2] = static_cast<uint8_t>(value >> 8);
    data[offset + 3] = static_cast<uint8_t>(value);
}

} // namespace

int main() {
    std::vector<uint8_t> sprite_ram(0x30000);
    std::vector<uint8_t> vics_data(0x10000);
    std::vector<uint8_t> vics_control(0x80);

    // One enabled, normal-resolution C374 sprite. Hardware origin offsets
    // are 0x2d/0x2a, so raw 145/92 places it at 100/50.
    put32(sprite_ram, 0x00000, 0x00040000);
    put32(sprite_ram, 0x00004, 0x00000000);
    put32(sprite_ram, 0x00008, 0x00000000);
    put32(sprite_ram, 0x0000c, 0x00000000);
    put32(sprite_ram, 0x00200, (45u << 16) | 684u);
    put32(sprite_ram, 0x00204, (42u << 16) | 521u);

    put32(sprite_ram, 0x04000, (145u << 16) | 92u);
    put32(sprite_ram, 0x04004, (32u << 16) | 32u);
    put32(sprite_ram, 0x04008, 0x00ff0011);
    put32(sprite_ram, 0x0400c, 0x00020000);
    put32(sprite_ram, 0x20000, 0x00000123);
    put32(sprite_ram, 0x20004, 0x000500fe);

    const auto sprites = system22_sprite_decoder::decode(
        sprite_ram.data(), sprite_ram.size(),
        vics_data.data(), vics_data.size(),
        vics_control.data(), vics_control.size(), 0x1000000);
    const auto expect = [](bool condition, const char* message) {
        if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
        return condition;
    };
    if (!expect(sprites.size() == 1,
                "one C374 entry must decode to one tile"))
        return 1;
    const polygon_object& sprite = sprites.front();
    if (!expect(sprite.sprite && sprite.sprite_tile == 2 &&
                sprite.zsort == 0x123 && sprite.color == 5 &&
                sprite.cmode == 1,
                "tile, priority, palette and text priority must decode"))
        return 1;
    if (!expect(sprite.clip_left == 0 && sprite.clip_right == 639 &&
                sprite.clip_top == 0 && sprite.clip_bottom == 479,
                "hardware clipping window must map to the native screen"))
        return 1;
    if (!expect(sprite.vertices[0].x == 100 &&
                sprite.vertices[0].y == 50 &&
                sprite.vertices[2].x == 132 &&
                sprite.vertices[2].y == 82 &&
                sprite.vertices[0].u == 0 && sprite.vertices[2].u == 31,
                "origin, size and raw 32-pixel UVs must decode"))
        return 1;

    // Dirt Dash ignores the nominal first-list size register. Its list count
    // is stored in the low byte of VICS data at the bank selected by bit 14 of
    // the source register, and it does not submit the second VICS list.
    std::fill(sprite_ram.begin(), sprite_ram.end(), 0);
    std::fill(vics_data.begin(), vics_data.end(), 0);
    std::fill(vics_control.begin(), vics_control.end(), 0);
    put32(sprite_ram, 0x00000, 0x00050000); // Main C374 list disabled.
    put32(sprite_ram, 0x00200, (45u << 16) | 684u);
    put32(sprite_ram, 0x00204, (42u << 16) | 521u);
    put32(vics_control, 0x40, 0);           // Deliberately no normal count.
    put32(vics_control, 0x48, 0x00001004);
    put32(vics_control, 0x58, 0x00002000);
    put32(vics_data, 0x0000, 1);            // Two sprites: stored count + 1.
    for (std::size_t index = 0; index < 2; ++index) {
        const std::size_t source = 0x1004 + index * 16;
        const std::size_t attribute = 0x2000 + index * 8;
        put32(vics_data, source, ((145u + index * 32) << 16) | 92u);
        put32(vics_data, source + 4, (32u << 16) | 32u);
        put32(vics_data, source + 8, 0x00ff0011);
        put32(vics_data, source + 12,
              (static_cast<uint32_t>(index + 2) << 16));
        put32(vics_data, attribute, static_cast<uint32_t>(0x100 + index));
        put32(vics_data, attribute + 4, 0x000500fe);
    }
    const auto dirt_sprites = system22_sprite_decoder::decode(
        sprite_ram.data(), sprite_ram.size(),
        vics_data.data(), vics_data.size(),
        vics_control.data(), vics_control.size(), 0x1000000,
        system22_sprite_profile::dirt_dash);
    if (!expect(dirt_sprites.size() == 2 &&
                dirt_sprites[0].sprite_tile == 2 &&
                dirt_sprites[1].sprite_tile == 3,
                "Dirt Dash VICS header count must select its complete list"))
        return 1;

    if (!expect(system22_direct_texture_coordinate(0xabc5, false) == 0x0bc5 &&
                system22_direct_texture_coordinate(0xabc5, true) == 0x0abc,
                "direct-polygon UV packing must follow the selected board"))
        return 1;

    polygon_object base_shadow{};
    base_shadow.color = 0x42;
    for (poly_vertex& vertex : base_shadow.vertices)
        vertex = {0.0f, 0.0f, 1.0f, 11, 15, 0};
    if (!expect(system22_temporal_shadow_material(base_shadow, false) &&
                !system22_temporal_shadow_material(base_shadow, true),
                "Victory Lap shadow material must match only base System 22"))
        return 1;
    base_shadow.vertices[0].bri = 1;
    if (!expect(!system22_temporal_shadow_material(base_shadow, false),
                "nearby base materials must not enter the shadow cache"))
        return 1;

    polygon_object dirt_shadow{};
    dirt_shadow.color = 0x01;
    dirt_shadow.cmode = 5;
    dirt_shadow.texturebank = 2;
    for (poly_vertex& vertex : dirt_shadow.vertices)
        vertex = {0.0f, 0.0f, 1.0f, 0, 0, 0x40};
    if (!expect(system22_temporal_shadow_material(dirt_shadow, true) &&
                !system22_temporal_shadow_material(dirt_shadow, false),
                "Dirt Dash shadow material must match only Super System 22"))
        return 1;
    dirt_shadow.direct = true;
    if (!expect(!system22_temporal_shadow_material(dirt_shadow, true),
                "direct polygons must never enter the shadow cache"))
        return 1;
    return 0;
}
