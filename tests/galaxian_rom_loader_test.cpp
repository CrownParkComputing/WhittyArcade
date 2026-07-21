// galaxian_rom_loader_test - Galaxian-family ROM loading smoke test
//
// With no arguments: assert all known set names and return 0. With three
// arguments (phoenix_path mooncrst_path uniwars_path):
// assert identify_set, load + complete(), and the per-set field
// population; return 1 on any mismatch.

#include "galaxian_rom.h"

#include <array>
#include <cassert>
#include <cstdio>

namespace {
template <typename Range>
bool any_nonzero(const Range& range) {
    for (auto b : range) {
        if (b != 0) return true;
    }
    return false;
}
}  // namespace

int main(int argc, char** argv) {
    assert(galaxian_rom_loader::set_display_name(
               galaxian_rom_set::unknown) != nullptr);
    assert(galaxian_rom_loader::set_display_name(
               galaxian_rom_set::phoenix) != nullptr);
    assert(galaxian_rom_loader::set_display_name(
               galaxian_rom_set::mooncrst) != nullptr);
    assert(galaxian_rom_loader::set_display_name(
               galaxian_rom_set::uniwars) != nullptr);
    assert(std::string(galaxian_rom_loader::set_display_name(
               galaxian_rom_set::unknown))
               .find("Unsupported") != std::string::npos);

    if (argc < 4) {
        std::puts(
            "Galaxian rom loader: enum + display name only "
            "(no ROM integration path)");
        return 0;
    }

    const std::string phoenix_path = argv[1];
    const std::string mooncrst_path = argv[2];
    const std::string uniwars_path = argv[3];
    const std::string fake_path = "/nonexistent/galaxian/garbage.zip";

    // Unknown / missing paths.
    assert(galaxian_rom_loader::identify_set(phoenix_path) ==
           galaxian_rom_set::phoenix);
    assert(galaxian_rom_loader::identify_set(mooncrst_path) ==
           galaxian_rom_set::mooncrst);
    assert(galaxian_rom_loader::identify_set(uniwars_path) ==
           galaxian_rom_set::uniwars);
    assert(galaxian_rom_loader::identify_set(fake_path) ==
           galaxian_rom_set::unknown);
    assert(galaxian_rom_loader::identify_set("") ==
           galaxian_rom_set::unknown);
    {
        const galaxian_rom_load_result bad =
            galaxian_rom_loader::load(fake_path);
        assert(bad.set == galaxian_rom_set::unknown);
        assert(!bad);
        assert(!bad.error.empty());
    }

    // Phoenix load.
    {
        const galaxian_rom_load_result result =
            galaxian_rom_loader::load(phoenix_path);
        if (!result) {
            std::fputs(result.error.c_str(), stderr);
            return 1;
        }
        assert(result.set == galaxian_rom_set::phoenix);
        assert(result.roms.complete());
        assert(any_nonzero(result.roms.program));
        assert(any_nonzero(result.roms.background_graphics));
        assert(any_nonzero(result.roms.foreground_graphics));
        assert(any_nonzero(result.roms.palette_prom));
        std::printf("Loaded %s: program=%zu background=%zu foreground=%zu "
                    "palette=%zu\n",
                    galaxian_rom_loader::set_display_name(result.set),
                    result.roms.program.size(),
                    result.roms.background_graphics.size(),
                    result.roms.foreground_graphics.size(),
                    result.roms.palette_prom.size());
    }

    // Moon Cresta load.
    {
        const galaxian_rom_load_result result =
            galaxian_rom_loader::load(mooncrst_path);
        if (!result) {
            std::fputs(result.error.c_str(), stderr);
            return 1;
        }
        assert(result.set == galaxian_rom_set::mooncrst);
        assert(result.roms.complete());
        assert(any_nonzero(result.roms.program));
        assert(any_nonzero(result.roms.char_rom));
        assert(any_nonzero(result.roms.mooncrst_palette_prom));
        std::printf("Loaded %s: program=%zu char_rom=%zu palette=%zu\n",
                    galaxian_rom_loader::set_display_name(result.set),
                    result.roms.program.size(),
                    result.roms.char_rom.size(),
                    result.roms.mooncrst_palette_prom.size());
    }

    // UniWar S load: same region sizes as the Galaxian tile pipeline, but
    // without Moon Cresta's program decryption.
    {
        const galaxian_rom_load_result result =
            galaxian_rom_loader::load(uniwars_path);
        if (!result) {
            std::fputs(result.error.c_str(), stderr);
            return 1;
        }
        assert(result.set == galaxian_rom_set::uniwars);
        assert(result.roms.complete());
        assert(any_nonzero(result.roms.program));
        assert(any_nonzero(result.roms.char_rom));
        assert(any_nonzero(result.roms.mooncrst_palette_prom));
        std::printf("Loaded %s: program=%zu char_rom=%zu palette=%zu\n",
                    galaxian_rom_loader::set_display_name(result.set),
                    result.roms.program.size(),
                    result.roms.char_rom.size(),
                    result.roms.mooncrst_palette_prom.size());
    }

    return 0;
}
