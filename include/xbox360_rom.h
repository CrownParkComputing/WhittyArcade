// Xbox 360 title discovery for the Xbox 360 board.
//
// A title is identified by its execution-info title ID, not by the name of the
// file or directory it arrived in. That ID is read from the default.xex of an
// extracted title, or from the metadata of a signed STFS package - a package is
// named after the SHA-1 of its content, so its filename says nothing at all and
// its header is the only thing that can be trusted.
//
// A title counts as complete only when everything it reads is there: for an
// extracted title that means the data files sitting beside the XEX, and for a
// package it means the package itself being whole, because the data is inside
// it. The per-title facts live in one table in the implementation, so a second
// game is a table row rather than a new code path.
//
// A title can have been dumped both ways, and then both copies play. Which one
// is offered is the title's own registered preference rather than whichever the
// filesystem listed first - see set_preferred_shape.
#pragma once

#include <cstdint>
#include <string>

enum class xbox360_rom_set : std::uint8_t {
    unknown,
    robotron_2084,
    geometry_wars,
    geometry_wars_2,
    space_giraffe,
};

// How a title's content sits on disk, which is a property of how it was dumped
// rather than of the game. It decides what "complete" means and, because the
// native runtime takes a package as its only argument, how the title is
// launched.
enum class xbox360_content_shape : std::uint8_t {
    // A default.xex with the title's data files beside it.
    extracted,
    // One signed STFS package (LIVE, CON or PIRS) holding the executable and
    // every data file inside it.
    package,
};

struct xbox360_rom_info {
    xbox360_rom_set set{xbox360_rom_set::unknown};
    // The shape the content at the inspected path actually has, not the shape
    // the title prefers: a title dumped both ways is played from whichever copy
    // was selected, and that decides how it is launched.
    xbox360_content_shape shape{xbox360_content_shape::extracted};
    // The directory the content lives in: the one holding default.xex for an
    // extracted title, the one holding the package for a packaged one.
    std::string game_root;
    // Set for an extracted title only.
    std::string xex_path;
    // Set for a packaged title only.
    std::string package_path;
    std::uint32_t title_id{};
    std::string error;

    bool packaged() const {
        return shape == xbox360_content_shape::package;
    }

    explicit operator bool() const {
        if (!error.empty() || set == xbox360_rom_set::unknown) return false;
        if (packaged()) return !package_path.empty();
        return !game_root.empty() && !xex_path.empty();
    }
};

class xbox360_rom_loader {
public:
    static constexpr std::uint32_t robotron_title_id = 0x584107e0;
    static constexpr std::uint32_t geometry_wars_title_id = 0x584107ed;
    static constexpr std::uint32_t geometry_wars_2_title_id = 0x584108ff;
    static constexpr std::uint32_t space_giraffe_title_id = 0x5841080c;

    // Accepts an extracted game directory, its default.xex, an STFS package, or
    // a directory holding one. Identification always comes from the content's
    // own header rather than from its filename.
    static xbox360_rom_set identify_set(const std::string& path);
    static xbox360_rom_info inspect(const std::string& path,
                                    bool require_game_data = true);
    static const char* set_short_name(xbox360_rom_set set);
    static const char* set_display_name(xbox360_rom_set set);
    // Which shape the set is played from when a machine holds more than one,
    // and the shape a caller should ask for when nothing is there to inspect.
    static xbox360_content_shape set_preferred_shape(xbox360_rom_set set);
    // Whether the set plays from this shape at all. Most titles were dumped one
    // way and play only from that one; a title dumped both ways plays from
    // either, and refusing the copy that happens to be on the machine would be
    // refusing a game that is present and complete.
    static bool set_plays_shape(xbox360_rom_set set,
                                xbox360_content_shape shape);
};
