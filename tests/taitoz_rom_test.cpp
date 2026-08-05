// Taito Z ROM loader test.
//
// Without a ROM path this only checks the metadata contract, so it runs in
// CI. Given a real archive it checks every assembled region size, and if a
// directory of MAME region dumps is supplied it compares byte for byte --
// which is the only way to catch a wrong interleave, since a mis-assembled
// sprite region loads without complaint and simply draws garbage.
//
// Usage: taitoz_rom_test [contcirc.zip [mame_region_dump_dir]]
//   The dump directory is expected to hold rgn_maincpu.bin, rgn_sub.bin,
//   rgn_audiocpu.bin, rgn_tc0100scn.bin, rgn_sprites.bin, rgn_tc0150rod.bin,
//   rgn_spritemap.bin, rgn_ymsnd_adpcma.bin and rgn_ymsnd_adpcmb.bin.

#include "taito/taitoz/taitoz_rom.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f),
                                std::istreambuf_iterator<char>{});
}

int compared = 0;

void compare_region(const std::string& dir, const char* file,
                    const std::vector<uint8_t>& ours, const char* label) {
    const std::string path = dir + "/" + file;
    const std::vector<uint8_t> theirs = read_file(path);
    if (theirs.empty()) {
        std::printf("  %-11s no dump at %s, skipped\n", label, path.c_str());
        return;
    }
    assert(ours.size() == theirs.size());
    assert(std::memcmp(ours.data(), theirs.data(), ours.size()) == 0);
    std::printf("  %-11s %8zu bytes byte-exact vs MAME\n", label, ours.size());
    ++compared;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace taitoz;

    // Metadata contract, always checked.
    assert(std::string(taitoz_rom_loader::set_short_name(
               taitoz_rom_set::contcirc)) == "contcirc");
    assert(std::strlen(taitoz_rom_loader::set_display_name(
               taitoz_rom_set::contcirc)) != 0);
    assert(taitoz_rom_loader::set_short_name(taitoz_rom_set::unknown)[0] == 0);
    // A path that cannot exist must identify as nothing rather than
    // half-loading.
    assert(taitoz_rom_loader::identify_set("/definitely/missing/contcirc.zip") ==
           taitoz_rom_set::unknown);
    {
        const taitoz_rom_load_result bad =
            taitoz_rom_loader::load("/definitely/missing/contcirc.zip");
        assert(!bad);
        assert(!bad.error.empty());
    }
    // An empty set is never "complete".
    {
        taitoz_roms empty{};
        assert(!empty.complete());
    }

    if (argc < 2) {
        std::printf("taitoz_rom_test: metadata checks passed "
                    "(no ROM supplied)\n");
        return 0;
    }

    const std::string rom = argv[1];
    assert(taitoz_rom_loader::identify_set(rom) == taitoz_rom_set::contcirc);
    const taitoz_rom_load_result r = taitoz_rom_loader::load(rom);
    if (!r) {
        std::fprintf(stderr, "taitoz_rom_test: load failed: %s\n",
                     r.error.c_str());
        return 1;
    }
    assert(r.set == taitoz_rom_set::contcirc);
    assert(r.roms.main_cpu.size()   == kMainCpuBytes);
    assert(r.roms.sub_cpu.size()    == kSubCpuBytes);
    assert(r.roms.audio_cpu.size()  == kAudioCpuBytes);
    assert(r.roms.scn_gfx.size()    == kScnGfxBytes);
    assert(r.roms.sprite_gfx.size() == kSpriteGfxBytes);
    assert(r.roms.road_gfx.size()   == kRoadGfxBytes);
    assert(r.roms.sprite_map.size() == kSpriteMapBytes);
    assert(r.roms.adpcm_a.size()    == kAdpcmABytes);
    assert(r.roms.adpcm_b.size()    == kAdpcmBBytes);
    assert(r.roms.complete());

    // The 68000 reset vector lives at the very start of CPU A: a stack
    // pointer then a PC, both of which must land inside the board's map.
    // This is what catches an even/odd swap in the LOAD16_BYTE pair.
    const auto be32 = [](const std::vector<uint8_t>& v, std::size_t o) {
        return (uint32_t(v[o]) << 24) | (uint32_t(v[o + 1]) << 16) |
               (uint32_t(v[o + 2]) << 8) | uint32_t(v[o + 3]);
    };
    const uint32_t reset_pc = be32(r.roms.main_cpu, 4);
    assert(reset_pc < kMainCpuBytes);
    const uint32_t sub_pc = be32(r.roms.sub_cpu, 4);
    assert(sub_pc < kSubCpuBytes);
    std::printf("taitoz_rom_test: %s loaded; CPU A reset PC=0x%06x, "
                "CPU B reset PC=0x%06x\n",
                taitoz_rom_loader::set_display_name(r.set), reset_pc, sub_pc);

    if (argc < 3) {
        std::printf("taitoz_rom_test: region sizes OK "
                    "(no MAME dump dir supplied)\n");
        return 0;
    }

    const std::string dir = argv[2];
    std::printf("taitoz_rom_test: comparing against MAME dumps in %s\n",
                dir.c_str());
    compare_region(dir, "rgn_maincpu.bin",      r.roms.main_cpu,   "maincpu");
    compare_region(dir, "rgn_sub.bin",          r.roms.sub_cpu,    "sub");
    compare_region(dir, "rgn_audiocpu.bin",     r.roms.audio_cpu,  "audiocpu");
    compare_region(dir, "rgn_tc0100scn.bin",    r.roms.scn_gfx,    "tc0100scn");
    compare_region(dir, "rgn_sprites.bin",      r.roms.sprite_gfx, "sprites");
    compare_region(dir, "rgn_tc0150rod.bin",    r.roms.road_gfx,   "tc0150rod");
    compare_region(dir, "rgn_spritemap.bin",    r.roms.sprite_map, "spritemap");
    compare_region(dir, "rgn_ymsnd_adpcma.bin", r.roms.adpcm_a,    "adpcma");
    compare_region(dir, "rgn_ymsnd_adpcmb.bin", r.roms.adpcm_b,    "adpcmb");
    std::printf("taitoz_rom_test: %d region(s) matched MAME exactly\n",
                compared);
    return 0;
}
