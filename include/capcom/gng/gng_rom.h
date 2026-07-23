// Capcom Ghosts'n Goblins ROM loading and parent-set identification.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace gng {

enum class rom_set : uint8_t { unknown, world_set_1 };

struct roms {
    std::array<uint8_t, 0x18000> main{};
    std::array<uint8_t, 0x08000> sound{};
    std::array<uint8_t, 0x04000> chars{};
    std::array<uint8_t, 0x18000> tiles{};
    std::array<uint8_t, 0x20000> sprites{};
    std::array<uint8_t, 0x00200> proms{};
    bool valid{false};

    bool complete() const noexcept { return valid; }
};

struct rom_load_result {
    rom_set set{rom_set::unknown};
    roms data{};
    std::string error{};
    explicit operator bool() const noexcept {
        return error.empty() && data.complete();
    }
};

class rom_loader {
public:
    static rom_set identify_set(const std::string& path);
    static const char* set_short_name(rom_set set) noexcept;
    static const char* set_display_name(rom_set set) noexcept;
    static rom_load_result load(const std::string& path);
};

} // namespace gng
