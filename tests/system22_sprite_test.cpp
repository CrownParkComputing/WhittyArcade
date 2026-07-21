#include "system22_sprites.h"

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
    return 0;
}
