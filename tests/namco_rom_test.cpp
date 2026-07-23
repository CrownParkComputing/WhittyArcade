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
    return 0;
}
