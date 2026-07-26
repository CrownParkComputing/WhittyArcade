// Shinobi (Sega System 16-B) ROM loader.
//
// This matches the MAME `shinobi` (English, single-cabinet) parent set
// only -- the user instruction was "just one English romset, not all".
// Loader accepts either an extracted directory or a ZIP archive, and
// uses MAME 0.172-era CRC-32 to distinguish the parent from alt sets.
//
// File layout (one or both of):
//   - Extracted directory: shader-style file basenames from the MAME
//     `shinobi` set. We accept either the MAME basenames (mpr-1XXXX.aN)
//     or a single 256 KiB program image named `shinobi_main.bin`.
//
// On success result.set is system16b_rom_set::shinobi_us, result.roms is
// populated, and result.error is empty. On failure result.error describes
// the first missing or invalid file.

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace system16b {

// Region sizes fit the largest supported set (Aurail); smaller games fill
// the front of each buffer and leave the tail clear, and every consumer
// masks with its own game's window, so growth here changes nothing for
// them.
constexpr std::size_t kProgramRomBytes = 0x80000;    // 512 KiB
constexpr std::size_t kGfxFileBytes    = 0x40000;    // per tile plane
constexpr std::size_t kSpriteGfxBytes  = 0x200000;   // assembled region

// Sound CPU program (Z80): `epr-11361.a10` is 32 KiB and occupies the
// complete 0x0000-0x7fff program window. Truncating this to 8 KiB mirrors
// the reset/initialisation code four times and prevents the sound driver
// from ever reaching its sequencer.
constexpr std::size_t kSoundProgRomBytes = 0x08000;
// Sound data bank region (banked through the Z80 0x8000-0xdfff window by
// the uPD7759 control port). Sized for the largest supported layout: the
// bank offset formula reaches 0x3c000, and Alien Syndrome scatters three
// 32 KiB sample ROMs across it. Shinobi's single 128 KiB ROM is mirrored
// into the upper half so its historical modulo addressing is unchanged.
constexpr std::size_t kSoundDataRomBytes = 0x40000;

enum class system16b_rom_set : uint8_t {
    unknown,
    shinobi_us,
    alien_syndrome,
    aurail,
    riot_city,
    golden_axe,
};

struct system16b_roms {
    std::array<uint8_t, kProgramRomBytes>   program{};
    std::array<uint8_t, kGfxFileBytes>      tile_plane0{};
    std::array<uint8_t, kGfxFileBytes>      tile_plane1{};
    std::array<uint8_t, kGfxFileBytes>      tile_plane2{};
    std::array<uint8_t, kSpriteGfxBytes>    sprite_gfx{};
    std::array<uint8_t, kSoundProgRomBytes> sound_prog{};
    std::array<uint8_t, kSoundDataRomBytes> sound_data{};
    // i8751 protection MCU program (empty when the set has none).
    std::vector<uint8_t> mcu{};

    bool complete() const;
};

struct system16b_rom_load_result {
    system16b_rom_set set{system16b_rom_set::unknown};
    system16b_roms   roms{};
    std::string    error{};

    explicit operator bool() const { return error.empty() && roms.complete(); }
};

class system16b_rom_loader {
public:
    static system16b_rom_set identify_set(const std::string& path);
    static const char*      set_short_name(system16b_rom_set set);
    static const char*      set_display_name(system16b_rom_set set);
    static system16b_rom_load_result load(const std::string& path);
};

}  // namespace system16b
