// Super System 22 sprite-list decoding.
#pragma once

#include "namco/system22/system22_types.h"

#include <cstddef>
#include <cstdint>
#include <vector>

enum class system22_sprite_profile : uint8_t {
    standard,
    dirt_dash,
};

class system22_sprite_decoder {
public:
    // Decode the C374 list and both VICS lists into renderer-neutral quads.
    // Sprite graphics remain in ROM and are selected by sprite_tile.
    static std::vector<polygon_object> decode(
        const uint8_t* sprite_ram, std::size_t sprite_ram_size,
        const uint8_t* vics_data, std::size_t vics_data_size,
        const uint8_t* vics_control, std::size_t vics_control_size,
        std::size_t sprite_rom_size,
        system22_sprite_profile profile = system22_sprite_profile::standard);
};
