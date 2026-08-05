#include "namco/namco_rom.h"

#include <cassert>
#include <string>

int main(int argc, char** argv) {
    if (argc < 3) return 0;
    const namco::load_result galaga = namco::rom_loader::load(argv[1]);
    assert(galaga);
    assert(galaga.set == namco::rom_set::galaga);
    assert(galaga.galaga.main[0] != galaga.galaga.main[1] ||
           galaga.galaga.main[1] != galaga.galaga.main[2]);

    const namco::load_result pacmania = namco::rom_loader::load(argv[2]);
    assert(pacmania);
    assert(pacmania.set == namco::rom_set::pacmania);
    assert(pacmania.pacmania.program6.size() == 0x20000);
    assert(pacmania.pacmania.sprite1.size() == 0x20000);

    // Galaga '88 is the other Namco System 1 set. It must not be mistaken
    // for Pac-Mania, and its five main-ROM slots, six voice chips and six
    // sprite chips must all arrive at their documented sizes -- the CUS117
    // slot layout depends on every one of them being present.
    if (argc < 4) return 0;
    const namco::load_result galaga88 = namco::rom_loader::load(argv[3]);
    assert(galaga88);
    assert(galaga88.set == namco::rom_set::galaga88);
    assert(std::string(namco::rom_loader::set_short_name(galaga88.set)) ==
           "galaga88");
    assert(galaga88.galaga88.program0.size() == 0x10000);
    assert(galaga88.galaga88.program5.size() == 0x10000);
    assert(galaga88.galaga88.program7.size() == 0x10000);
    assert(galaga88.galaga88.mask.size() == 0x20000);
    for (const auto& voice : galaga88.galaga88.voices)
        assert(voice.size() == 0x10000);
    for (const auto& chars : galaga88.galaga88.chars)
        assert(chars.size() == 0x20000);
    for (const auto& sprite : galaga88.galaga88.sprites)
        assert(sprite.size() == 0x20000);
    // The two System 1 games share only the HD63701 MCU chip, so their
    // program ROMs must be distinct.
    assert(galaga88.galaga88.mcu == pacmania.pacmania.mcu);
    assert(galaga88.galaga88.program7 != pacmania.pacmania.program7);
    return 0;
}
