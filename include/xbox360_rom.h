// Xbox 360 extracted-title discovery for the ReXGlue-backed board.
#pragma once

#include <cstdint>
#include <string>

enum class xbox360_rom_set : std::uint8_t {
    unknown,
    robotron_2084,
};

struct xbox360_rom_info {
    xbox360_rom_set set{xbox360_rom_set::unknown};
    std::string game_root;
    std::string xex_path;
    std::uint32_t title_id{};
    std::string error;

    explicit operator bool() const {
        return error.empty() && set != xbox360_rom_set::unknown &&
               !game_root.empty() && !xex_path.empty();
    }
};

class xbox360_rom_loader {
public:
    static constexpr std::uint32_t robotron_title_id = 0x584107e0;

    // Accepts either an extracted game directory or its default.xex. The XEX
    // execution-info title ID is used for identification, not its filename.
    static xbox360_rom_set identify_set(const std::string& path);
    static xbox360_rom_info inspect(const std::string& path,
                                    bool require_game_data = true);
    static const char* set_short_name(xbox360_rom_set set);
    static const char* set_display_name(xbox360_rom_set set);
};
