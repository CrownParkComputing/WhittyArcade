// Galaxian-family ROM loading. ZIP archives and extracted directories are
// both accepted; matching is case-insensitive and by basename (mirrors the
// source_reader convention in model1_rom.cpp / model2_rom.cpp).
//
// The loader currently covers Phoenix, Moon Cresta and UniWar S. The wider
// Galaxian family shares the Z80 + tile pipeline but memory maps and I/O
// behaviour still belong in explicit board profiles.
#pragma once

#include <array>
#include <cstdint>
#include <string>

enum class galaxian_rom_set : uint8_t {
    unknown,
    phoenix,
    mooncrst,
    uniwars,
};

struct galaxian_roms {
    // Common 0x4000 program image. Both Phoenix and Moon Cresta lay out
    // eight 0x800 ROMs; the loader is responsible for assembling them in
    // the right order and applying any per-set post-decode.
    std::array<uint8_t, 0x4000> program{};

    // Phoenix: 2x 0x1000-byte video RAM pages. Moon Cresta does not use
    // paged video RAM and these are left zero-initialised.
    std::array<std::array<uint8_t, 0x1000>, 2> video_pages{};

    // Phoenix background and foreground tile graphics (2x 0x800 each,
    // concatenated to 0x1000 in load order).
    std::array<uint8_t, 0x1000> background_graphics{};
    std::array<uint8_t, 0x1000> foreground_graphics{};

    // Phoenix: two 256-byte palette PROMs, 4 bits per gun.
    std::array<uint8_t, 0x0200> palette_prom{};

    // Moon Cresta / UniWar S: 32-byte palette PROM. The loader populates the
    // complete array with the set-specific PROM.
    std::array<uint8_t, 32> mooncrst_palette_prom{};

    // Moon Cresta / UniWar S: 4 x 0x800 character ROMs concatenated to 0x2000.
    std::array<uint8_t, 0x2000> char_rom{};

    // True iff the populated fields match the set the load was performed
    // for. Used by galaxian_rom_load_result::operator bool to gate the
    // frontend's session construction.
    bool complete() const;
};

struct galaxian_rom_load_result {
    galaxian_rom_set set{galaxian_rom_set::unknown};
    galaxian_roms roms;
    std::string error;

    explicit operator bool() const { return error.empty() && roms.complete(); }
};

class galaxian_rom_loader {
public:
    // ROM-header probe. At least two of the program's distinctive files
    // must be present for the set to be identified. Returns
    // galaxian_rom_set::unknown for a missing path, an empty archive, or a
    // non-Galaxian-family archive.
    static galaxian_rom_set identify_set(const std::string& path);

    // Canonical MAME short name. This is the stable key shared by ROM
    // import, persistence and the board catalog.
    static const char* set_short_name(galaxian_rom_set set);

    // Display name for the picker / log. Always non-null; "Unsupported
    // Galaxian-family ROM set" is returned for galaxian_rom_set::unknown.
    static const char* set_display_name(galaxian_rom_set set);

    // Loads the archive or directory into a result. On success, result.set
    // is non-unknown, result.roms.complete() is true, and result.error is
    // empty. On failure, result.error describes the first missing or
    // invalid file.
    static galaxian_rom_load_result load(const std::string& path);
};
