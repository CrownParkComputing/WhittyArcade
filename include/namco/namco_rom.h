// ROM loading for Galaga and Pac-Mania's Namco System 1 parent sets.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace namco {

enum class rom_set : uint8_t { unknown, galaga, pacmania };

struct galaga_roms {
    std::array<uint8_t, 0x4000> main{};
    std::array<uint8_t, 0x1000> sub{};
    std::array<uint8_t, 0x1000> sound{};
    std::array<uint8_t, 0x1000> chars{};
    std::array<uint8_t, 0x2000> sprites{};
    std::array<uint8_t, 0x20> palette{};
    std::array<uint8_t, 0x100> char_lookup{};
    std::array<uint8_t, 0x100> sprite_lookup{};
    std::array<uint8_t, 0x100> waveform{};
    bool valid{};
};

struct pacmania_roms {
    std::vector<uint8_t> program6, program7;
    std::vector<uint8_t> sound0, sound1, mcu, voice;
    std::array<std::vector<uint8_t>, 4> chars;
    std::vector<uint8_t> mask, sprite0, sprite1;
    bool valid{};
};

struct load_result {
    rom_set set{rom_set::unknown};
    galaga_roms galaga;
    pacmania_roms pacmania;
    std::string error;
    explicit operator bool() const noexcept {
        return error.empty() &&
            ((set == rom_set::galaga && galaga.valid) ||
             (set == rom_set::pacmania && pacmania.valid));
    }
};

class rom_loader {
public:
    static rom_set identify_set(const std::string& path);
    static const char* set_short_name(rom_set set) noexcept;
    static const char* set_display_name(rom_set set) noexcept;
    static load_result load(const std::string& path);
};

} // namespace namco
