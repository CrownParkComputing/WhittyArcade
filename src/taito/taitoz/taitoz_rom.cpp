// Taito Z System ROM loading implementation.
//
// Each assembly form below was validated against the regions MAME actually
// builds (dumped through its Lua memory-region API and compared byte for
// byte), because the interleaves here are invisible to `-listxml`.

#include "taito/taitoz/taitoz_rom.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>

#include <minizip/unzip.h>

namespace taitoz {

namespace {

bool read_zip_entry(const std::string& archive_path, const char* name,
                    std::vector<uint8_t>& bytes) {
    unzFile archive = unzOpen64(archive_path.c_str());
    if (!archive) return false;
    bool located = unzLocateFile(archive, name, 0) == UNZ_OK;
    if (!located) {
        // Merged archives keep per-set files in subdirectories, so fall
        // back to matching on the basename alone.
        const char* want = std::strrchr(name, '/');
        want = want ? want + 1 : name;
        if (unzGoToFirstFile(archive) == UNZ_OK) {
            do {
                char entry[512];
                if (unzGetCurrentFileInfo(archive, nullptr, entry,
                                          sizeof(entry), nullptr, 0, nullptr,
                                          0) != UNZ_OK)
                    break;
                const char* base = std::strrchr(entry, '/');
                base = base ? base + 1 : entry;
                if (std::strcmp(base, want) == 0) { located = true; break; }
            } while (unzGoToNextFile(archive) == UNZ_OK);
        }
    }
    if (!located) { unzClose(archive); return false; }
    unz_file_info info{};
    if (unzGetCurrentFileInfo(archive, &info, nullptr, 0, nullptr, 0, nullptr,
                              0) != UNZ_OK ||
        unzOpenCurrentFile(archive) != UNZ_OK) {
        unzClose(archive);
        return false;
    }
    bytes.resize(info.uncompressed_size);
    const int got = bytes.empty() ? 0 :
        unzReadCurrentFile(archive, bytes.data(),
                           static_cast<unsigned>(bytes.size()));
    unzCloseCurrentFile(archive);
    unzClose(archive);
    return got == static_cast<int>(bytes.size());
}

bool read_dir_entry(const std::string& dir, const char* name,
                    std::vector<uint8_t>& bytes) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path p = fs::path(dir) / name;
    if (!fs::is_regular_file(p, ec)) {
        const std::string want = fs::path(name).filename().string();
        for (const auto& sub : fs::directory_iterator(dir, ec)) {
            if (!sub.is_directory(ec)) continue;
            fs::path cand = sub.path() / want;
            if (fs::is_regular_file(cand, ec)) { p = cand; break; }
        }
        if (!fs::is_regular_file(p, ec)) return false;
    }
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    bytes.assign(std::istreambuf_iterator<char>(f),
                 std::istreambuf_iterator<char>{});
    return !bytes.empty();
}

bool read_entry(const std::string& path, const char* name,
                std::vector<uint8_t>& bytes) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (fs::is_directory(fs::path(path), ec))
        return read_dir_entry(path, name, bytes);
    return read_zip_entry(path, name, bytes);
}

// A chip that must be present, with the size the region layout assumes.
struct chip_spec {
    const char* name;
    std::size_t size;
};

bool read_sized(const std::string& path, const chip_spec& spec,
                std::vector<uint8_t>& bytes, std::string& error) {
    if (!read_entry(path, spec.name, bytes)) {
        error = std::string("missing ROM: ") + spec.name;
        return false;
    }
    if (bytes.size() != spec.size) {
        error = std::string("bad size for ") + spec.name + ": got " +
                std::to_string(bytes.size()) + ", want " +
                std::to_string(spec.size);
        return false;
    }
    return true;
}

// ROM_LOAD16_BYTE pair: the 68000 is big-endian, so the "even" chip
// supplies byte 0 of each word and the "odd" chip byte 1.
std::vector<uint8_t> load16_byte(const std::vector<uint8_t>& even,
                                 const std::vector<uint8_t>& odd) {
    std::vector<uint8_t> out(even.size() * 2);
    for (std::size_t i = 0; i < even.size(); ++i) {
        out[i * 2]     = even[i];
        out[i * 2 + 1] = odd[i];
    }
    return out;
}

// ROM_LOAD16_WORD_SWAP: byteswap every 16-bit word.
std::vector<uint8_t> load16_word_swap(const std::vector<uint8_t>& rom) {
    std::vector<uint8_t> out(rom.size());
    for (std::size_t i = 0; i + 1 < rom.size(); i += 2) {
        out[i]     = rom[i + 1];
        out[i + 1] = rom[i];
    }
    return out;
}

// ROM_LOAD64_WORD_SWAP x4: chip `plane` occupies byte offsets
// plane*2 and plane*2+1 of every 8-byte group, with the two bytes of each
// source word swapped (ROM_GROUPWORD | ROM_REVERSE | ROM_SKIP(6)).
std::vector<uint8_t> load64_word_swap(
        const std::array<const std::vector<uint8_t>*, 4>& chips) {
    const std::size_t n = chips[0]->size();
    std::vector<uint8_t> out(n * 4, 0);
    for (std::size_t plane = 0; plane < chips.size(); ++plane) {
        const std::vector<uint8_t>& rom = *chips[plane];
        const std::size_t off = plane * 2;
        for (std::size_t i = 0; i < n / 2; ++i) {
            const std::size_t base = off + i * 8;
            out[base]     = rom[2 * i + 1];
            out[base + 1] = rom[2 * i];
        }
    }
    return out;
}

// Continental Circus (MAME `contcirc`, the World 3D-effects set).
constexpr chip_spec kCcMainEven{"b33-ww.ic25", 0x20000};
constexpr chip_spec kCcMainOdd {"b33-xx.ic26", 0x20000};
constexpr chip_spec kCcSubEven {"b33-yy.ic35", 0x20000};
constexpr chip_spec kCcSubOdd  {"cc_36.bin",   0x20000};
constexpr chip_spec kCcAudio   {"b33-30.11",   0x10000};
constexpr chip_spec kCcScn     {"b33-02.57",   0x80000};
constexpr chip_spec kCcRoad    {"b33-01.3",    0x80000};
constexpr chip_spec kCcSprMap  {"b33-07.64",   0x80000};
constexpr std::array<chip_spec, 4> kCcSprites{{
    {"b33-06", 0x80000}, {"b33-05", 0x80000},
    {"b33-04", 0x80000}, {"b33-03", 0x80000},
}};
constexpr std::array<chip_spec, 2> kCcAdpcmA{{
    {"b33-09.18", 0x80000}, {"b33-10.17", 0x80000},
}};
constexpr chip_spec kCcAdpcmB{"b33-08.19", 0x80000};

taitoz_rom_load_result load_contcirc(const std::string& path) {
    taitoz_rom_load_result r{};
    r.set = taitoz_rom_set::contcirc;
    std::vector<uint8_t> a, b;

    if (!read_sized(path, kCcMainEven, a, r.error) ||
        !read_sized(path, kCcMainOdd, b, r.error))
        return r;
    r.roms.main_cpu = load16_byte(a, b);

    if (!read_sized(path, kCcSubEven, a, r.error) ||
        !read_sized(path, kCcSubOdd, b, r.error))
        return r;
    r.roms.sub_cpu = load16_byte(a, b);

    if (!read_sized(path, kCcAudio, r.roms.audio_cpu, r.error)) return r;

    if (!read_sized(path, kCcScn, a, r.error)) return r;
    r.roms.scn_gfx = load16_word_swap(a);

    // The road and spritemap regions are ROM16_LE and load raw.
    if (!read_sized(path, kCcRoad, r.roms.road_gfx, r.error)) return r;
    if (!read_sized(path, kCcSprMap, r.roms.sprite_map, r.error)) return r;

    {
        std::array<std::vector<uint8_t>, 4> chips;
        for (std::size_t i = 0; i < kCcSprites.size(); ++i)
            if (!read_sized(path, kCcSprites[i], chips[i], r.error)) return r;
        const std::array<const std::vector<uint8_t>*, 4> refs{
            &chips[0], &chips[1], &chips[2], &chips[3]};
        r.roms.sprite_gfx = load64_word_swap(refs);
    }

    r.roms.adpcm_a.reserve(kAdpcmABytes);
    for (const chip_spec& spec : kCcAdpcmA) {
        if (!read_sized(path, spec, a, r.error)) return r;
        r.roms.adpcm_a.insert(r.roms.adpcm_a.end(), a.begin(), a.end());
    }
    if (!read_sized(path, kCcAdpcmB, r.roms.adpcm_b, r.error)) return r;

    return r;
}

}  // namespace

bool taitoz_roms::complete() const {
    return main_cpu.size()   == kMainCpuBytes &&
           sub_cpu.size()    == kSubCpuBytes &&
           audio_cpu.size()  == kAudioCpuBytes &&
           scn_gfx.size()    == kScnGfxBytes &&
           sprite_gfx.size() == kSpriteGfxBytes &&
           road_gfx.size()   == kRoadGfxBytes &&
           sprite_map.size() == kSpriteMapBytes &&
           adpcm_a.size()    == kAdpcmABytes &&
           adpcm_b.size()    == kAdpcmBBytes;
}

taitoz_rom_set taitoz_rom_loader::identify_set(const std::string& path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (path.empty() || !fs::exists(path, ec)) return taitoz_rom_set::unknown;

    std::vector<uint8_t> tmp;
    // Two distinctive entries: the CPU A even chip and the road gfx.
    if (read_entry(path, kCcMainEven.name, tmp) &&
        read_entry(path, kCcRoad.name, tmp))
        return taitoz_rom_set::contcirc;
    return taitoz_rom_set::unknown;
}

const char* taitoz_rom_loader::set_short_name(taitoz_rom_set set) {
    switch (set) {
    case taitoz_rom_set::contcirc: return "contcirc";
    case taitoz_rom_set::unknown:  return "";
    }
    return "";
}

const char* taitoz_rom_loader::set_display_name(taitoz_rom_set set) {
    switch (set) {
    case taitoz_rom_set::contcirc:
        return "Continental Circus (Taito Z System)";
    case taitoz_rom_set::unknown:
        return "Unsupported Taito Z ROM set";
    }
    return "Unsupported Taito Z ROM set";
}

taitoz_rom_load_result taitoz_rom_loader::load(const std::string& path) {
    taitoz_rom_load_result r{};
    r.set = identify_set(path);
    if (r.set == taitoz_rom_set::unknown) {
        r.error = "No supported Taito Z ROM set found in: " + path;
        return r;
    }
    return load_contcirc(path);
}

}  // namespace taitoz
