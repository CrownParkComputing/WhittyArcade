// Taito Z System ROM loading.
//
// The Z System is a dual-68000 board: CPU A and CPU B share work RAM and
// drive a TC0100SCN tilemap generator, a TC0150ROD road generator, a
// TC0110PCR palette chip and zoomed sprites, with a Z80 + YM2610 sound
// board behind a TC0140SYT mailbox.
//
// Every region below was derived by dumping what MAME actually assembles
// and reproducing it byte for byte -- `-listxml` collapses the interleave
// forms these ROMs use and cannot be trusted for them. The sprite region
// in particular is ROM_LOAD64_WORD_SWAP: four chips land at byte offsets
// 0, 2, 4 and 6 of each 8-byte group with the two bytes of each source
// word swapped. Getting that wrong loads silently and draws garbage.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace taitoz {

// Assembled region sizes, matching MAME's memory regions exactly.
constexpr std::size_t kMainCpuBytes   = 0x040000;
constexpr std::size_t kSubCpuBytes    = 0x040000;
constexpr std::size_t kAudioCpuBytes  = 0x010000;
constexpr std::size_t kScnGfxBytes    = 0x080000;   // TC0100SCN tiles
constexpr std::size_t kSpriteGfxBytes = 0x200000;
constexpr std::size_t kRoadGfxBytes   = 0x080000;   // TC0150ROD
constexpr std::size_t kSpriteMapBytes = 0x080000;
constexpr std::size_t kAdpcmABytes    = 0x100000;
constexpr std::size_t kAdpcmBBytes    = 0x080000;

enum class taitoz_rom_set : uint8_t {
    unknown,
    contcirc,   // Continental Circus
};

struct taitoz_roms {
    std::vector<uint8_t> main_cpu;
    std::vector<uint8_t> sub_cpu;
    std::vector<uint8_t> audio_cpu;
    std::vector<uint8_t> scn_gfx;
    std::vector<uint8_t> sprite_gfx;
    std::vector<uint8_t> road_gfx;
    std::vector<uint8_t> sprite_map;
    std::vector<uint8_t> adpcm_a;
    std::vector<uint8_t> adpcm_b;

    bool complete() const;
};

struct taitoz_rom_load_result {
    taitoz_rom_set set{taitoz_rom_set::unknown};
    taitoz_roms roms{};
    std::string error{};

    explicit operator bool() const {
        return error.empty() && roms.complete();
    }
};

class taitoz_rom_loader {
public:
    static taitoz_rom_set identify_set(const std::string& path);
    static const char* set_short_name(taitoz_rom_set set);
    static const char* set_display_name(taitoz_rom_set set);
    static taitoz_rom_load_result load(const std::string& path);
};

}  // namespace taitoz
