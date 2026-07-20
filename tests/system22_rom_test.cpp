#include "system22_rom.h"

#include <cassert>
#include <cstdio>
#include <filesystem>

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: system22_rom_test <game.zip>\n");
        return 2;
    }
    const std::string path = argv[1];
    const ridge_racer_rom_set set = rom_loader::identify_set(path);
    assert(set != ridge_racer_rom_set::unknown);
    assert(rom_loader::is_working_set(set));

    const ridge_racer_roms roms = rom_loader::load_ridge_racer(
        path, fs::path(path).parent_path().string());
    assert(roms.has_complete_program());
    assert(!roms.mcu_rom.empty());
    assert(!roms.texture_rom.empty());
    assert(!roms.tilemap_rom.empty());
    assert(!roms.point_rom.empty());
    assert(!roms.c352_samples.empty());
    assert(!roms.gamma_proms.empty());
    assert(roms.has_c71_firmware());
    assert(roms.has_c74_firmware());

    std::printf("Loaded %s: program=%zu texture=%zu point=%zu samples=%zu\n",
                rom_loader::set_display_name(set), roms.maincpu_rom.size(),
                roms.texture_rom.size(), roms.point_rom.size(),
                roms.c352_samples.size());
    return 0;
}
