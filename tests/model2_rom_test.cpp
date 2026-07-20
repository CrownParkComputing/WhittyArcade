#include "model2_rom.h"

#include <cassert>
#include <cstdint>
#include <cstdio>

namespace {
uint32_t read32le(const std::vector<uint8_t>& data, std::size_t offset) {
    return static_cast<uint32_t>(data[offset]) |
           (static_cast<uint32_t>(data[offset + 1]) << 8) |
           (static_cast<uint32_t>(data[offset + 2]) << 16) |
           (static_cast<uint32_t>(data[offset + 3]) << 24);
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: model2_rom_test <srallyc.zip>\n");
        return 2;
    }

    assert(model2_rom_loader::identify_set(argv[1]) ==
           model2_rom_set::sega_rally_revision_c);
    model2_rom_load_result result = model2_rom_loader::load(argv[1]);
    if (!result) {
        std::fputs(result.error.c_str(), stderr);
        return 1;
    }

    assert(result.roms.complete());
    assert(result.roms.has_billboard());
    assert(result.roms.main_cpu.size() == 0x200000);
    assert(result.roms.main_data.size() == 0x2400000);
    assert(result.roms.polygon_data.size() == 0x2000000);
    assert(result.roms.texture_data.size() == 0x1000000);
    assert(result.roms.samples.size() == 0x800000);
    assert(read32le(result.roms.main_cpu, 0) != 0xffffffffu);
    assert(read32le(result.roms.main_data, 0) != 0xffffffffu);

    std::printf("Loaded %s: i960=%zu data=%zu polygon=%zu texture=%zu "
                "samples=%zu billboard=%s\n",
                model2_rom_loader::set_display_name(result.set),
                result.roms.main_cpu.size(), result.roms.main_data.size(),
                result.roms.polygon_data.size(),
                result.roms.texture_data.size(), result.roms.samples.size(),
                result.roms.has_billboard() ? "yes" : "no");
    return 0;
}
