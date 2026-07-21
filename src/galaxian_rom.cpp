// galaxian_rom.cpp - Galaxian-family ROM loading
//
// Both ZIP archives and extracted directories are accepted via a local
// source_reader (matches the model1_rom / model2_rom convention). Case
// insensitivity applies to the basename match; a board's distinctive
// program + palette filenames drive identify_set.

#include "galaxian_rom.h"

#include <minizip/unzip.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// Bit-shuffle helper used by the Moon Cresta post-decode step. Mirrors
// zarcade's GalaxianRoms.decodeMoonCresta path: only the even bytes are
// shuffled; both halves are XOR-patched based on selected bits.
uint8_t bitswap(uint8_t v, int b7, int b2, int b5, int b4, int b3, int b6,
                int b1, int b0) {
    auto bit = [&](int b) -> uint8_t {
        return static_cast<uint8_t>((v >> b) & 1);
    };
    return static_cast<uint8_t>((bit(b7) << 7) | (bit(b2) << 6) |
                                (bit(b5) << 5) | (bit(b4) << 4) |
                                (bit(b3) << 3) | (bit(b6) << 2) |
                                (bit(b1) << 1) | bit(b0));
}

template <typename ByteRange>
void decode_mooncrst(ByteRange& program) {
    for (std::size_t offs = 0; offs < program.size(); ++offs) {
        uint8_t data = program[offs];
        uint8_t res = data;
        if (((data >> 1) & 1) != 0) res ^= 0x40;
        if (((data >> 5) & 1) != 0) res ^= 0x04;
        if ((offs & 1) == 0) res = bitswap(res, 7, 2, 5, 4, 3, 6, 1, 0);
        program[offs] = res;
    }
}

struct source_reader {
    explicit source_reader(fs::path source_path)
        : source(std::move(source_path)) {}

    fs::path source;

    static std::string lower(std::string value) {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char character) {
                           return static_cast<char>(std::tolower(character));
                       });
        return value;
    }

    bool is_zip() const {
        return lower(source.extension().string()) == ".zip";
    }

    bool contains(const std::string& wanted) const {
        std::vector<uint8_t> ignored;
        return read(wanted, ignored, false);
    }

    bool read(const std::string& wanted, std::vector<uint8_t>& output,
              bool read_contents = true) const {
        output.clear();
        if (!is_zip()) {
            std::error_code error;
            if (!fs::is_directory(source, error)) return false;
            const std::string wanted_lower = lower(wanted);
            for (fs::directory_iterator iterator(source, error), end;
                 !error && iterator != end; iterator.increment(error)) {
                if (lower(iterator->path().filename().string()) !=
                    wanted_lower)
                    continue;
                if (!read_contents) return true;
                std::FILE* file =
                    std::fopen(iterator->path().string().c_str(), "rb");
                if (!file) return false;
                if (std::fseek(file, 0, SEEK_END) != 0) {
                    std::fclose(file);
                    return false;
                }
                const long length = std::ftell(file);
                if (length < 0 || std::fseek(file, 0, SEEK_SET) != 0) {
                    std::fclose(file);
                    return false;
                }
                output.resize(static_cast<std::size_t>(length));
                const std::size_t count = output.empty() ? 0 :
                    std::fread(output.data(), 1, output.size(), file);
                std::fclose(file);
                if (count == output.size()) return true;
                output.clear();
                return false;
            }
            return false;
        }

        unzFile archive = unzOpen64(source.string().c_str());
        if (!archive) return false;
        const std::string wanted_lower = lower(wanted);
        bool found = false;
        if (unzGoToFirstFile(archive) == UNZ_OK) {
            do {
                unz_file_info64 info{};
                std::array<char, 1024> name{};
                if (unzGetCurrentFileInfo64(archive, &info, name.data(),
                                            name.size(), nullptr, 0, nullptr,
                                            0) != UNZ_OK)
                    break;
                if (lower(fs::path(name.data()).filename().string()) !=
                    wanted_lower)
                    continue;
                found = true;
                if (!read_contents) break;
                if (info.uncompressed_size >
                        static_cast<ZPOS64_T>(
                            std::numeric_limits<std::size_t>::max()) ||
                    unzOpenCurrentFile(archive) != UNZ_OK) {
                    found = false;
                    break;
                }
                output.resize(
                    static_cast<std::size_t>(info.uncompressed_size));
                std::size_t total = 0;
                while (total < output.size()) {
                    const unsigned request = static_cast<unsigned>(
                        std::min<std::size_t>(output.size() - total,
                                              1u << 20));
                    const int count = unzReadCurrentFile(
                        archive, output.data() + total, request);
                    if (count <= 0) break;
                    total += static_cast<std::size_t>(count);
                }
                const int close_result = unzCloseCurrentFile(archive);
                if (total != output.size() || close_result != UNZ_OK) {
                    output.clear();
                    found = false;
                }
                break;
            } while (unzGoToNextFile(archive) == UNZ_OK);
        }
        unzClose(archive);
        return found;
    }
};

constexpr std::size_t kProgramSize = 0x4000;

constexpr std::array<std::pair<const char*, std::size_t>, 8>
    kPhoenixProgram = {{
        {"ic45", 0x0000},   {"ic46", 0x0800},   {"ic47", 0x1000},
        {"ic48", 0x1800},   {"h5-ic49.5a", 0x2000}, {"h6-ic50.6a", 0x2800},
        {"h7-ic51.7a", 0x3000}, {"h8-ic52.8a", 0x3800},
}};

constexpr std::array<std::pair<const char*, std::size_t>, 2>
    kPhoenixBackgroundGraphics = {{
        {"ic23.3d", 0x0000}, {"ic24.4d", 0x0800},
}};

constexpr std::array<std::pair<const char*, std::size_t>, 2>
    kPhoenixForegroundGraphics = {{
        {"b1-ic39.3b", 0x0000}, {"b2-ic40.4b", 0x0800},
}};

constexpr std::array<std::pair<const char*, std::size_t>, 2>
    kPhoenixPaletteProms = {{
        {"mmi6301.ic40", 0x0000}, {"mmi6301.ic41", 0x0100},
}};

constexpr std::array<const char*, 8> kMooncrstProgram = {
    "mc1", "mc2", "mc3", "mc4",
    "mc5.7r", "mc6.8d", "mc7.8e", "mc8",
};

constexpr std::array<const char*, 4> kMooncrstCharRoms = {
    "mcs_b", "mcs_d", "mcs_a", "mcs_c",
};

constexpr const char* kMooncrstPaletteRom = "mmi6331.6l";

constexpr std::array<const char*, 8> kUniwarsProgram = {
    "f07_1a.bin", "h07_2a.bin", "k07_3a.bin", "m07_4a.bin",
    "d08p_5a.bin", "gg6", "m08p_7a.bin", "n08p_8a.bin",
};

constexpr std::array<const char*, 4> kUniwarsCharRoms = {
    "egg10", "h01_2.bin", "egg9", "k01_2.bin",
};

constexpr const char* kUniwarsPaletteRom = "uniwars.clr";

template <std::size_t RegionSize, std::size_t N>
bool load_region(const source_reader& source,
                 const std::array<std::pair<const char*, std::size_t>, N>&
                     files,
                 std::array<uint8_t, RegionSize>& destination,
                 std::string& error) {
    for (const auto& [name, offset] : files) {
        std::vector<uint8_t> bytes;
        if (!source.read(name, bytes) || offset + bytes.size() > RegionSize) {
            error += "Galaxian-family ROM missing or invalid: ";
            error += name;
            error += '\n';
            return false;
        }
        std::copy(bytes.begin(), bytes.end(),
                  destination.begin() + offset);
    }
    return true;
}

bool load_mooncrst_program(const source_reader& source,
                           std::array<uint8_t, kProgramSize>& program,
                           std::string& error) {
    program.fill(0xff);
    for (std::size_t i = 0; i < kMooncrstProgram.size(); ++i) {
        std::vector<uint8_t> bytes;
        const char* name = kMooncrstProgram[i];
        if (!source.read(name, bytes) || bytes.size() != 0x800 ||
            (i + 1) * 0x800 > program.size()) {
            error += "Galaxian-family ROM missing or invalid: ";
            error += name;
            error += '\n';
            return false;
        }
        std::copy(bytes.begin(), bytes.end(), program.begin() + i * 0x800);
    }
    decode_mooncrst(program);
    return true;
}

bool load_mooncrst_char_rom(const source_reader& source,
                            std::array<uint8_t, 0x2000>& char_rom,
                            std::string& error) {
    char_rom.fill(0);
    for (std::size_t i = 0; i < kMooncrstCharRoms.size(); ++i) {
        std::vector<uint8_t> bytes;
        const char* name = kMooncrstCharRoms[i];
        if (!source.read(name, bytes) || bytes.size() != 0x800 ||
            (i + 1) * 0x800 > char_rom.size()) {
            error += "Galaxian-family ROM missing or invalid: ";
            error += name;
            error += '\n';
            return false;
        }
        std::copy(bytes.begin(), bytes.end(), char_rom.begin() + i * 0x800);
    }
    return true;
}

bool load_mooncrst_palette(const source_reader& source,
                           std::array<uint8_t, 32>& palette,
                           std::string& error) {
    std::vector<uint8_t> bytes;
    if (!source.read(kMooncrstPaletteRom, bytes) || bytes.size() < 32) {
        error += "Galaxian-family ROM missing or invalid: ";
        error += kMooncrstPaletteRom;
        error += '\n';
        return false;
    }
    std::copy(bytes.begin(), bytes.begin() + 32, palette.begin());
    return true;
}

bool load_fixed_2k_roms(const source_reader& source,
                        const std::array<const char*, 8>& files,
                        std::array<uint8_t, kProgramSize>& destination,
                        std::string& error) {
    destination.fill(0xff);
    for (std::size_t index = 0; index < files.size(); ++index) {
        std::vector<uint8_t> bytes;
        if (!source.read(files[index], bytes) || bytes.size() != 0x800) {
            error += "Galaxian-family ROM missing or invalid: ";
            error += files[index];
            error += '\n';
            return false;
        }
        std::copy(bytes.begin(), bytes.end(),
                  destination.begin() + index * 0x800);
    }
    return true;
}

bool load_fixed_2k_graphics(const source_reader& source,
                            const std::array<const char*, 4>& files,
                            std::array<uint8_t, 0x2000>& destination,
                            std::string& error) {
    destination.fill(0);
    for (std::size_t index = 0; index < files.size(); ++index) {
        std::vector<uint8_t> bytes;
        if (!source.read(files[index], bytes) || bytes.size() != 0x800) {
            error += "Galaxian-family graphics ROM missing or invalid: ";
            error += files[index];
            error += '\n';
            return false;
        }
        std::copy(bytes.begin(), bytes.end(),
                  destination.begin() + index * 0x800);
    }
    return true;
}

bool load_palette32(const source_reader& source, const char* name,
                    std::array<uint8_t, 32>& destination,
                    std::string& error) {
    std::vector<uint8_t> bytes;
    if (!source.read(name, bytes) || bytes.size() < destination.size()) {
        error += "Galaxian-family palette PROM missing or invalid: ";
        error += name;
        error += '\n';
        return false;
    }
    std::copy_n(bytes.begin(), destination.size(), destination.begin());
    return true;
}

galaxian_roms load_phoenix(const source_reader& source, std::string& error) {
    galaxian_roms roms;
    if (!load_region<kProgramSize>(source, kPhoenixProgram, roms.program,
                                   error))
        return {};
    if (!load_region<0x1000>(source, kPhoenixBackgroundGraphics,
                             roms.background_graphics, error))
        return {};
    if (!load_region<0x1000>(source, kPhoenixForegroundGraphics,
                             roms.foreground_graphics, error))
        return {};
    if (!load_region<0x0200>(source, kPhoenixPaletteProms, roms.palette_prom,
                             error))
        return {};
    return roms;
}

galaxian_roms load_mooncrst(const source_reader& source, std::string& error) {
    galaxian_roms roms;
    if (!load_mooncrst_program(source, roms.program, error)) return {};
    if (!load_mooncrst_char_rom(source, roms.char_rom, error)) return {};
    if (!load_mooncrst_palette(source, roms.mooncrst_palette_prom, error))
        return {};
    return roms;
}

galaxian_roms load_uniwars(const source_reader& source, std::string& error) {
    galaxian_roms roms;
    if (!load_fixed_2k_roms(source, kUniwarsProgram, roms.program, error))
        return {};
    if (!load_fixed_2k_graphics(source, kUniwarsCharRoms, roms.char_rom,
                                error))
        return {};
    if (!load_palette32(source, kUniwarsPaletteRom,
                        roms.mooncrst_palette_prom, error))
        return {};
    return roms;
}

}  // namespace

bool galaxian_roms::complete() const {
    // Phoenix: program populated + at least one non-zero byte in each of
    // the background graphics, foreground graphics, and palette PROMs.
    auto any_nonzero = [](const auto& range) {
        for (auto b : range) {
            if (b != 0) return true;
        }
        return false;
    };
    if (any_nonzero(program) && any_nonzero(background_graphics) &&
        any_nonzero(foreground_graphics) && any_nonzero(palette_prom))
        return true;
    // Moon Cresta / UniWar S: program populated + 4 char ROMs concatenated
    // to 0x2000 (any non-zero char byte implies the load reached the file
    // stage) + 32 palette bytes loaded.
    return any_nonzero(program) && any_nonzero(char_rom) &&
           any_nonzero(mooncrst_palette_prom);
}

galaxian_rom_set galaxian_rom_loader::identify_set(const std::string& path) {
    if (path.empty()) return galaxian_rom_set::unknown;
    const source_reader source(path);

    // Phoenix: 2-of-3 probe against the program + first palette PROM.
    int phoenix_hits = 0;
    if (source.contains("ic45")) ++phoenix_hits;
    if (source.contains("h5-ic49.5a")) ++phoenix_hits;
    if (source.contains("mmi6301.ic40")) ++phoenix_hits;
    if (phoenix_hits >= 2) return galaxian_rom_set::phoenix;

    // Moon Cresta: 2-of-3 probe against program + first char ROM + palette.
    int mooncrst_hits = 0;
    if (source.contains("mc1")) ++mooncrst_hits;
    if (source.contains("mcs_b")) ++mooncrst_hits;
    if (source.contains("mmi6331.6l")) ++mooncrst_hits;
    if (mooncrst_hits >= 2) return galaxian_rom_set::mooncrst;

    // UniWar S: Pisces-derived board with a distinctive 0x6002 graphics bank.
    int uniwars_hits = 0;
    if (source.contains("f07_1a.bin")) ++uniwars_hits;
    if (source.contains("egg10")) ++uniwars_hits;
    if (source.contains("uniwars.clr")) ++uniwars_hits;
    if (uniwars_hits >= 2) return galaxian_rom_set::uniwars;

    return galaxian_rom_set::unknown;
}

const char* galaxian_rom_loader::set_short_name(galaxian_rom_set set) {
    switch (set) {
    case galaxian_rom_set::phoenix: return "phoenix";
    case galaxian_rom_set::mooncrst: return "mooncrst";
    case galaxian_rom_set::uniwars: return "uniwars";
    case galaxian_rom_set::unknown: return "";
    }
    return "";
}

const char* galaxian_rom_loader::set_display_name(galaxian_rom_set set) {
    switch (set) {
    case galaxian_rom_set::phoenix:
        return "Phoenix";
    case galaxian_rom_set::mooncrst:
        return "Moon Cresta";
    case galaxian_rom_set::uniwars:
        return "UniWar S";
    case galaxian_rom_set::unknown:
        return "Unsupported Galaxian-family ROM set";
    }
    return "Unsupported Galaxian-family ROM set";
}

galaxian_rom_load_result galaxian_rom_loader::load(const std::string& path) {
    const source_reader source(path);
    std::string error;
    switch (identify_set(path)) {
    case galaxian_rom_set::phoenix: {
        galaxian_roms roms = load_phoenix(source, error);
        return {galaxian_rom_set::phoenix, std::move(roms), std::move(error)};
    }
    case galaxian_rom_set::mooncrst: {
        galaxian_roms roms = load_mooncrst(source, error);
        return {galaxian_rom_set::mooncrst, std::move(roms), std::move(error)};
    }
    case galaxian_rom_set::uniwars: {
        galaxian_roms roms = load_uniwars(source, error);
        return {galaxian_rom_set::uniwars, std::move(roms), std::move(error)};
    }
    case galaxian_rom_set::unknown:
        return {galaxian_rom_set::unknown, {},
                "Unsupported Galaxian-family ROM set: " + path + "\n"};
    }
    return {galaxian_rom_set::unknown, {},
            "Unsupported Galaxian-family ROM set: " + path + "\n"};
}
