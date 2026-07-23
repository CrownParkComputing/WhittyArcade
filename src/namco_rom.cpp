#include "namco_rom.h"

#include <minizip/unzip.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace namco {
namespace {

struct file_spec {
    const char* name;
    std::size_t size;
    uint32_t crc;
};

constexpr file_spec galaga_main[] = {
    {"gg1_1b.3p", 0x1000, 0xab036c9f},
    {"gg1_2b.3m", 0x1000, 0xd9232240},
    {"gg1_3.2m",  0x1000, 0x753ce503},
    {"gg1_4b.2l", 0x1000, 0x499fcc76},
};
constexpr file_spec galaga_sub{"gg1_5b.3f", 0x1000, 0xbb5caae3};
constexpr file_spec galaga_sound{"gg1_7b.2c", 0x1000, 0xd016686b};
constexpr file_spec galaga_chars{"gg1_9.4l", 0x1000, 0x58b2f47c};
constexpr file_spec galaga_sprites[] = {
    {"gg1_11.4d", 0x1000, 0xad447c80},
    {"gg1_10.4f", 0x1000, 0xdd6f1afc},
};
constexpr file_spec galaga_palette{"prom-5.5n", 0x20, 0x54603c6b};
constexpr file_spec galaga_char_lut{"prom-4.2n", 0x100, 0x59b6edab};
constexpr file_spec galaga_sprite_lut{"prom-3.1c", 0x100, 0x4a04bb6b};
constexpr file_spec galaga_wave{"prom-1.1d", 0x100, 0x7a2815b4};

constexpr file_spec pac_program6{"pn_prg-6.bin", 0x20000, 0xfe94900c};
constexpr file_spec pac_program7{"pn2_p7.bin", 0x10000, 0x462fa4fd};
constexpr file_spec pac_sound0{"pn2_s0.bin", 0x10000, 0xc10370fa};
constexpr file_spec pac_sound1{"pn2_s1.bin", 0x10000, 0xf761ed5a};
constexpr file_spec pac_mcu{"cus64-64a1.mcu", 0x1000, 0xffb5c0bd};
constexpr file_spec pac_voice{"pn2_v0.bin", 0x10000, 0x1ad5788f};
constexpr file_spec pac_chars[] = {
    {"pn_chr-0.bin", 0x20000, 0x7c57644c},
    {"pn_chr-1.bin", 0x20000, 0x7eaa67ed},
    {"pn_chr-2.bin", 0x20000, 0x27e739ac},
    {"pn_chr-3.bin", 0x20000, 0x1dfda293},
};
constexpr file_spec pac_mask{"pn2_c8.bin", 0x10000, 0xf3afd65d};
constexpr file_spec pac_sprite0{"pn_obj-0.bin", 0x20000, 0xfda57e8b};
constexpr file_spec pac_sprite1{"pnx_obj1.bin", 0x20000, 0x4c08affe};

bool read_zip(const std::string& path, const char* wanted,
              std::vector<uint8_t>& out) {
    unzFile zip = unzOpen64(path.c_str());
    if (!zip) return false;
    bool found = false;
    if (unzGoToFirstFile(zip) == UNZ_OK) {
        do {
            char name[1024]{};
            unz_file_info64 info{};
            if (unzGetCurrentFileInfo64(zip, &info, name, sizeof(name),
                                        nullptr, 0, nullptr, 0) != UNZ_OK)
                break;
            const char* base = std::strrchr(name, '/');
            base = base ? base + 1 : name;
            if (std::strcmp(base, wanted) != 0) continue;
            if (unzOpenCurrentFile(zip) != UNZ_OK) break;
            out.resize(static_cast<std::size_t>(info.uncompressed_size));
            std::size_t position = 0;
            while (position < out.size()) {
                const unsigned chunk = static_cast<unsigned>(
                    std::min<std::size_t>(out.size() - position, 1u << 20));
                const int got = unzReadCurrentFile(zip, out.data() + position,
                                                   chunk);
                if (got <= 0) break;
                position += static_cast<std::size_t>(got);
            }
            unzCloseCurrentFile(zip);
            found = position == out.size();
            break;
        } while (unzGoToNextFile(zip) == UNZ_OK);
    }
    unzClose(zip);
    return found;
}

bool read_directory(const std::string& path, const char* wanted,
                    std::vector<uint8_t>& out) {
    namespace fs = std::filesystem;
    std::error_code error;
    fs::path found = fs::path(path) / wanted;
    if (!fs::is_regular_file(found, error)) {
        for (fs::recursive_directory_iterator it(path, error), end;
             !error && it != end; it.increment(error)) {
            if (it->is_regular_file(error) &&
                it->path().filename() == wanted) {
                found = it->path();
                break;
            }
        }
    }
    if (!fs::is_regular_file(found, error)) return false;
    std::ifstream input(found, std::ios::binary);
    out.assign(std::istreambuf_iterator<char>(input), {});
    return static_cast<bool>(input) || input.eof();
}

bool read_file(const std::string& path, const char* wanted,
               std::vector<uint8_t>& out) {
    std::error_code error;
    return std::filesystem::is_directory(path, error)
        ? read_directory(path, wanted, out)
        : read_zip(path, wanted, out);
}

bool checked(const std::string& path, const file_spec& spec,
             std::vector<uint8_t>& out, std::string& error) {
    if (!read_file(path, spec.name, out)) {
        error = std::string("Missing ROM file: ") + spec.name;
        return false;
    }
    if (out.size() != spec.size) {
        error = std::string("Wrong size for ") + spec.name;
        return false;
    }
    const uint32_t crc = static_cast<uint32_t>(
        crc32(0, out.data(), static_cast<uInt>(out.size())));
    if (crc != spec.crc) {
        char message[128];
        std::snprintf(message, sizeof(message),
                      "Wrong CRC for %s: got %08x, expected %08x",
                      spec.name, crc, spec.crc);
        error = message;
        return false;
    }
    return true;
}

template <std::size_t N>
bool checked_array(const std::string& path, const file_spec& spec,
                   std::array<uint8_t, N>& out, std::size_t offset,
                   std::string& error) {
    std::vector<uint8_t> bytes;
    if (!checked(path, spec, bytes, error)) return false;
    std::copy(bytes.begin(), bytes.end(), out.begin() + offset);
    return true;
}

} // namespace

rom_set rom_loader::identify_set(const std::string& path) {
    std::vector<uint8_t> bytes;
    if (read_file(path, galaga_main[0].name, bytes) &&
        bytes.size() == galaga_main[0].size &&
        crc32(0, bytes.data(), static_cast<uInt>(bytes.size())) ==
            galaga_main[0].crc)
        return rom_set::galaga;
    if (read_file(path, pac_program6.name, bytes) &&
        bytes.size() == pac_program6.size &&
        crc32(0, bytes.data(), static_cast<uInt>(bytes.size())) ==
            pac_program6.crc)
        return rom_set::pacmania;
    return rom_set::unknown;
}

const char* rom_loader::set_short_name(rom_set set) noexcept {
    if (set == rom_set::galaga) return "galaga";
    if (set == rom_set::pacmania) return "pacmania";
    return "";
}

const char* rom_loader::set_display_name(rom_set set) noexcept {
    if (set == rom_set::galaga) return "Galaga (Namco rev. B)";
    if (set == rom_set::pacmania) return "Pac-Mania (World)";
    return "Unknown Namco set";
}

load_result rom_loader::load(const std::string& path) {
    load_result result;
    result.set = identify_set(path);
    if (result.set == rom_set::unknown) {
        result.error = "Not a supported Galaga or Pac-Mania parent set";
        return result;
    }
    if (result.set == rom_set::galaga) {
        for (std::size_t i = 0; i < std::size(galaga_main); ++i)
            if (!checked_array(path, galaga_main[i], result.galaga.main,
                               i * 0x1000, result.error))
                return result;
        if (!checked_array(path, galaga_sub, result.galaga.sub, 0,
                           result.error) ||
            !checked_array(path, galaga_sound, result.galaga.sound, 0,
                           result.error) ||
            !checked_array(path, galaga_chars, result.galaga.chars, 0,
                           result.error) ||
            !checked_array(path, galaga_sprites[0], result.galaga.sprites, 0,
                           result.error) ||
            !checked_array(path, galaga_sprites[1], result.galaga.sprites,
                           0x1000, result.error) ||
            !checked_array(path, galaga_palette, result.galaga.palette, 0,
                           result.error) ||
            !checked_array(path, galaga_char_lut,
                           result.galaga.char_lookup, 0, result.error) ||
            !checked_array(path, galaga_sprite_lut,
                           result.galaga.sprite_lookup, 0, result.error) ||
            !checked_array(path, galaga_wave, result.galaga.waveform, 0,
                           result.error))
            return result;
        result.galaga.valid = true;
        return result;
    }

    pacmania_roms& p = result.pacmania;
    if (!checked(path, pac_program6, p.program6, result.error) ||
        !checked(path, pac_program7, p.program7, result.error) ||
        !checked(path, pac_sound0, p.sound0, result.error) ||
        !checked(path, pac_sound1, p.sound1, result.error) ||
        !checked(path, pac_mcu, p.mcu, result.error) ||
        !checked(path, pac_voice, p.voice, result.error))
        return result;
    for (std::size_t i = 0; i < p.chars.size(); ++i)
        if (!checked(path, pac_chars[i], p.chars[i], result.error))
            return result;
    if (!checked(path, pac_mask, p.mask, result.error) ||
        !checked(path, pac_sprite0, p.sprite0, result.error) ||
        !checked(path, pac_sprite1, p.sprite1, result.error))
        return result;
    p.valid = true;
    return result;
}

} // namespace namco
