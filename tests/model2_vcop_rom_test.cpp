#include "sega/model2/model2_rom.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: model2_vcop_rom_test <vcop.zip>\n");
        return 2;
    }

    assert(model2_rom_loader::identify_set(argv[1]) ==
           model2_rom_set::virtua_cop);
    model2_rom_load_result result = model2_rom_loader::load(argv[1]);
    if (!result) {
        std::fputs(result.error.c_str(), stderr);
        return 1;
    }

    // Some vcop data/polygon ROMs begin with a run of 0xff padding, so scan
    // the loaded window rather than assuming the first dword is content.
    const auto has_content = [](const std::vector<uint8_t>& region,
                                std::size_t limit) {
        for (std::size_t index = 0; index < limit && index < region.size();
             ++index)
            if (region[index] != 0xff) return true;
        return false;
    };

    // Original Model 2 memory-image sizes (MAME ROM_START(vcop)).
    assert(result.roms.complete());
    assert(result.roms.main_cpu.size() == 0x200000);
    assert(result.roms.main_data.size() == 0x2000000);
    assert(result.roms.polygon_data.size() == 0x1000000);
    assert(result.roms.texture_data.size() == 0x1000000);
    assert(result.roms.copro_data.size() == 0x800000);
    assert(result.roms.copro_tgp_tables.size() == 0x40000);
    assert(result.roms.other_data.size() == 0x80000);

    // Sega Model 1 sound board: 68000 program + two MultiPCM buses. The SCSP
    // `samples` region and the Model 2A video board are absent for this set.
    assert(result.roms.sound_cpu.size() == 0xc0000);
    assert(result.roms.multipcm_samples_1.size() == 0x400000);
    assert(result.roms.multipcm_samples_2.size() == 0x400000);
    assert(result.roms.samples.empty());
    assert(result.roms.video_tables.empty());
    assert(!result.roms.has_billboard());

    // Cabinet I/O board firmware (Sega model1io2, TMPZ84C015).
    assert(result.roms.io_cpu.size() == 0x10000);
    assert(has_content(result.roms.io_cpu, 0x10000));

    // Regions actually populated with ROM data, not left as fill.
    assert(has_content(result.roms.main_cpu, 0x200000));
    assert(has_content(result.roms.main_data, 0x880000));
    assert(has_content(result.roms.polygon_data, 0x400000));
    assert(has_content(result.roms.sound_cpu, 0xc0000));
    // MultiPCM buses are zero-filled where empty; confirm real samples landed.
    assert(result.roms.multipcm_samples_2[0] != 0x00 ||
           result.roms.multipcm_samples_2[0x200000] != 0x00);

    std::printf("Loaded %s: i960=%zu data=%zu polygon=%zu texture=%zu "
                "sound=%zu pcm1=%zu pcm2=%zu\n",
                model2_rom_loader::set_display_name(result.set),
                result.roms.main_cpu.size(), result.roms.main_data.size(),
                result.roms.polygon_data.size(),
                result.roms.texture_data.size(), result.roms.sound_cpu.size(),
                result.roms.multipcm_samples_1.size(),
                result.roms.multipcm_samples_2.size());
    return 0;
}
