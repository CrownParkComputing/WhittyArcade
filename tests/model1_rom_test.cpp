#include "model1_rom.h"

#include <cassert>
#include <algorithm>
#include <cstdint>
#include <cstdio>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: model1_rom_test <model1.zip>\n");
        return 2;
    }

    const model1_rom_set identified = model1_rom_loader::identify_set(argv[1]);
    assert(identified != model1_rom_set::unknown);
    model1_rom_load_result result = model1_rom_loader::load(argv[1]);
    if (!result) {
        std::fputs(result.error.c_str(), stderr);
        return 1;
    }

    assert(result.set == identified);
    assert(result.roms.complete());
    assert(result.roms.main_cpu[0x200000] != 0xff);
    assert(result.roms.main_cpu[0x200001] != 0xff);
    assert(result.roms.main_cpu[0xfc0000] != 0xff);
    assert(result.roms.main_cpu[0xfe0000] != 0xff);
    if (result.set == model1_rom_set::star_wars_arcade) {
        assert(result.roms.main_cpu[0] != 0xff);
        assert(result.roms.main_cpu[0xf80000] != 0xff);
        assert(result.roms.dsb_cpu.size() == 0x20000);
        assert(result.roms.dsb_mpeg.size() == 0x800000);
    } else {
        assert(result.roms.main_cpu[0x1000000] != 0xff);
    }
    assert(result.roms.polygon_data.size() == 0x1000000);
    assert(result.roms.copro_data.size() == 0x200000);
    assert(result.roms.lookup_tables.size() == 0x0e0000);
    assert(result.roms.legacy_tgp.size() == 0x2000);
    if (result.set == model1_rom_set::virtua_formula) {
        assert(result.roms.io_cpu.size() == 0x10000);
        assert(result.roms.communication_cpu.size() == 0x20000);
    } else if (result.set == model1_rom_set::virtua_fighter) {
        const auto populated = [](const std::vector<uint8_t>& bytes,
                                  std::size_t begin, std::size_t end) {
            return std::any_of(bytes.begin() + begin, bytes.begin() + end,
                               [](uint8_t value) { return value != 0; });
        };
        assert(populated(result.roms.sound_cpu, 0, 0x20000));
        assert(populated(result.roms.sound_cpu, 0x20000, 0x40000));
        assert(populated(result.roms.samples_1, 0x200000, 0x400000));
        assert(populated(result.roms.samples_2, 0x200000, 0x400000));
    }

    std::printf("Loaded %s: V60=%zu polygon=%zu copro=%zu lookup=%zu\n",
                model1_rom_loader::set_display_name(result.set),
                result.roms.main_cpu.size(), result.roms.polygon_data.size(),
                result.roms.copro_data.size(), result.roms.lookup_tables.size());
    return 0;
}
