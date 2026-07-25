// Xbox 360 extracted-title discovery for the Xbox 360 board.
//
// A title is identified by the execution-info title ID in its default.xex, not
// by the name of the directory it was extracted into, and it counts as complete
// only when the data files that title actually reads sit beside it. The
// per-title facts live in one table in the implementation, so a second game is a
// table row rather than a new code path.
#pragma once

#include <cstdint>
#include <string>

enum class xbox360_rom_set : std::uint8_t {
    unknown,
    robotron_2084,
    geometry_wars,
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
    static constexpr std::uint32_t geometry_wars_title_id = 0x584107ed;

    // Accepts either an extracted game directory or its default.xex. The XEX
    // execution-info title ID is used for identification, not its filename.
    static xbox360_rom_set identify_set(const std::string& path);
    static xbox360_rom_info inspect(const std::string& path,
                                    bool require_game_data = true);
    static const char* set_short_name(xbox360_rom_set set);
    static const char* set_display_name(xbox360_rom_set set);
};
