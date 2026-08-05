// Headless attract-frame dumper for the classic boards, so launcher title
// icons can be seeded without playing each game on a device (the Model 1/2
// boot tests already dump frames; this covers the boards whose machines
// render straight into a framebuffer).
//
//   title_frame_dump <set> <rom> <frames> <out.ppm>
//
// Sets: galaxian, mooncrst, uniwars, phoenix, galaga, pacmania. The PPM
// byte order matches the machines' framebuffer convention (R in the low
// byte), the same as gng_machine_test's dump.

#include "arcade_types.h"
#include "namco/galaxian/galaxian_machine.h"
#include "namco/galaga/galaga_machine.h"
#include "namco/system1/system1_machine.h"
#include "namco/namco_rom.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>

namespace {

bool write_ppm(const char* path, const uint32_t* pixels, int width,
               int height) {
    std::ofstream image(path, std::ios::binary);
    if (!image) return false;
    image << "P6\n" << width << ' ' << height << "\n255\n";
    for (int index = 0; index < width * height; ++index) {
        const uint32_t pixel = pixels[index];
        const char rgb[3]{static_cast<char>(pixel),
                          static_cast<char>(pixel >> 8),
                          static_cast<char>(pixel >> 16)};
        image.write(rgb, sizeof(rgb));
    }
    return image.good();
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 5) {
        std::fprintf(stderr,
                     "usage: %s <set> <rom> <frames> <out.ppm>\n"
                     "sets: galaxian mooncrst uniwars phoenix galaga "
                     "pacmania\n",
                     argv[0]);
        return 2;
    }
    const std::string set = argv[1];
    const std::string rom = argv[2];
    const int frames = std::atoi(argv[3]);
    const char* out = argv[4];

    if (set == "galaga" || set == "pacmania" || set == "galaga88") {
        const namco::load_result loaded = namco::rom_loader::load(rom);
        if (!loaded) {
            std::fprintf(stderr, "%s\n", loaded.error.c_str());
            return 1;
        }
        if (set == "galaga") {
            namco::galaga_machine machine;
            if (!machine.initialize(loaded.galaga)) return 1;
            for (int frame = 0; frame < frames; ++frame)
                machine.run_frame();
            return write_ppm(out, machine.framebuffer(),
                             namco::galaga_machine::width,
                             namco::galaga_machine::height) ? 0 : 1;
        }
        namco::system1_machine machine;
        // Both Namco System 1 games share this machine; only the ROM set
        // handed to initialize() differs.
        if (!(set == "galaga88" ? machine.initialize(loaded.galaga88)
                                : machine.initialize(loaded.pacmania)))
            return 1;
        input_state idle{};
        machine.set_input(idle, 0xff);
        for (int frame = 0; frame < frames; ++frame) machine.run_frame();
        return write_ppm(out, machine.framebuffer(),
                         namco::system1_machine::width,
                         namco::system1_machine::height) ? 0 : 1;
    }

    std::unique_ptr<galaxian_board_interface> board;
    if (set == "galaxian") board = make_galaxian_board_interface();
    else if (set == "mooncrst") board = make_mooncrst_board_interface();
    else if (set == "uniwars") board = make_uniwars_board_interface();
    else if (set == "warofbug") board = make_warofbug_board_interface();
    else if (set == "phoenix") board = make_phoenix_board_interface();
    if (!board) {
        std::fprintf(stderr, "unknown set %s\n", set.c_str());
        return 2;
    }
    galaxian_machine machine(std::move(board));
    if (!machine.load_roms(rom)) {
        std::fprintf(stderr, "could not load %s\n", rom.c_str());
        return 1;
    }
    machine.reset();
    for (int frame = 0; frame < frames; ++frame) machine.run_frame();
    return write_ppm(out, machine.frame_buffer(), machine.screen_width(),
                     machine.screen_height()) ? 0 : 1;
}
