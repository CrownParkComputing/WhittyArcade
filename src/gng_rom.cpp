#include "gng_rom.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <vector>

#include <minizip/unzip.h>
#include <zlib.h>

namespace gng {
namespace {

struct file_spec {
    const char* name;
    std::size_t size;
    uint32_t crc;
};

constexpr file_spec kMain[] = {
    {"mm_c_04", 0x4000, 0x4f94130f},
    {"mm_c_03", 0x8000, 0x1def138a},
    {"mm_c_05", 0x8000, 0xed28e86e},
};
constexpr file_spec kSound{"gg2.bin", 0x8000, 0x615f5b6f};
constexpr file_spec kChars{"gg1.bin", 0x4000, 0xecfccf07};
constexpr file_spec kTiles[] = {
    {"gg11.bin", 0x4000, 0xddd56fa9}, {"gg10.bin", 0x4000, 0x7302529d},
    {"gg9.bin", 0x4000, 0x20035bda},  {"gg8.bin", 0x4000, 0xf12ba271},
    {"gg7.bin", 0x4000, 0xe525207d},  {"gg6.bin", 0x4000, 0x2d77e9b2},
};
constexpr file_spec kSprites[] = {
    {"gg17.bin", 0x4000, 0x93e50a8f}, {"gg16.bin", 0x4000, 0x06d7e5ca},
    {"gg15.bin", 0x4000, 0xbc1fe02d}, {"gg14.bin", 0x4000, 0x6aaf12f9},
    {"gg13.bin", 0x4000, 0xe80c3fca}, {"gg12.bin", 0x4000, 0x7780a925},
};
constexpr file_spec kProms[] = {
    {"tbp24s10.14k", 0x100, 0x0eaf5158},
    {"63s141.2e", 0x100, 0x4a1285a4},
};

bool read_zip(const std::string& path, const char* wanted,
              std::vector<uint8_t>& out) {
    unzFile zip = unzOpen64(path.c_str());
    if (!zip) return false;
    bool found = false;
    if (unzGoToFirstFile(zip) == UNZ_OK) {
        do {
            char entry[512]{};
            unz_file_info info{};
            if (unzGetCurrentFileInfo(zip, &info, entry, sizeof(entry),
                                      nullptr, 0, nullptr, 0) != UNZ_OK)
                break;
            const char* base = std::strrchr(entry, '/');
            base = base ? base + 1 : entry;
            if (std::strcmp(base, wanted) != 0) continue;
            if (unzOpenCurrentFile(zip) != UNZ_OK) break;
            out.resize(info.uncompressed_size);
            const int got = out.empty() ? 0 : unzReadCurrentFile(
                zip, out.data(), static_cast<unsigned>(out.size()));
            unzCloseCurrentFile(zip);
            found = out.empty() || got == static_cast<int>(out.size());
            break;
        } while (unzGoToNextFile(zip) == UNZ_OK);
    }
    unzClose(zip);
    return found;
}

bool read_directory(const std::string& path, const char* wanted,
                    std::vector<uint8_t>& out) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path file = fs::path(path) / wanted;
    if (!fs::is_regular_file(file, ec)) {
        for (fs::recursive_directory_iterator it(path, ec), end;
             !ec && it != end; it.increment(ec)) {
            if (it->is_regular_file(ec) &&
                it->path().filename() == wanted) {
                file = it->path();
                break;
            }
        }
    }
    if (!fs::is_regular_file(file, ec)) return false;
    std::ifstream stream(file, std::ios::binary);
    if (!stream) return false;
    out.assign(std::istreambuf_iterator<char>(stream), {});
    return true;
}

bool read_file(const std::string& path, const char* wanted,
               std::vector<uint8_t>& out) {
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec))
        return read_directory(path, wanted, out);
    return read_zip(path, wanted, out);
}

bool checked_read(const std::string& path, const file_spec& spec,
                  uint8_t* destination, std::string& error) {
    std::vector<uint8_t> bytes;
    if (!read_file(path, spec.name, bytes)) {
        error = std::string("Missing ROM file: ") + spec.name;
        return false;
    }
    if (bytes.size() != spec.size) {
        error = std::string("Wrong size for ") + spec.name + ": got " +
                std::to_string(bytes.size()) + ", expected " +
                std::to_string(spec.size);
        return false;
    }
    const uint32_t actual = static_cast<uint32_t>(
        crc32(0, bytes.data(), static_cast<uInt>(bytes.size())));
    if (actual != spec.crc) {
        char detail[96];
        std::snprintf(detail, sizeof(detail),
                      "Wrong CRC for %s: got %08x, expected %08x",
                      spec.name, actual, spec.crc);
        error = detail;
        return false;
    }
    std::copy(bytes.begin(), bytes.end(), destination);
    return true;
}

} // namespace

rom_set rom_loader::identify_set(const std::string& path) {
    std::vector<uint8_t> bytes;
    if (!read_file(path, kMain[0].name, bytes) ||
        bytes.size() != kMain[0].size)
        return rom_set::unknown;
    const uint32_t crc = static_cast<uint32_t>(
        crc32(0, bytes.data(), static_cast<uInt>(bytes.size())));
    return crc == kMain[0].crc ? rom_set::world_set_1 : rom_set::unknown;
}

const char* rom_loader::set_short_name(rom_set set) noexcept {
    return set == rom_set::world_set_1 ? "gng" : "";
}

const char* rom_loader::set_display_name(rom_set set) noexcept {
    return set == rom_set::world_set_1
        ? "Ghosts'n Goblins (World? set 1)"
        : "Unknown Ghosts'n Goblins set";
}

rom_load_result rom_loader::load(const std::string& path) {
    rom_load_result result{};
    result.set = identify_set(path);
    if (result.set == rom_set::unknown) {
        result.error = "Not the supported Ghosts'n Goblins parent ROM set";
        return result;
    }
    std::fill(result.data.sprites.begin(), result.data.sprites.end(), 0xff);

    if (!checked_read(path, kMain[0], result.data.main.data() + 0x4000,
                      result.error) ||
        !checked_read(path, kMain[1], result.data.main.data() + 0x8000,
                      result.error) ||
        !checked_read(path, kMain[2], result.data.main.data() + 0x10000,
                      result.error) ||
        !checked_read(path, kSound, result.data.sound.data(), result.error) ||
        !checked_read(path, kChars, result.data.chars.data(), result.error))
        return result;

    for (std::size_t index = 0; index < std::size(kTiles); ++index) {
        if (!checked_read(path, kTiles[index],
                          result.data.tiles.data() + index * 0x4000,
                          result.error))
            return result;
    }
    constexpr std::array<std::size_t, 6> sprite_offsets{
        0x00000, 0x04000, 0x08000, 0x10000, 0x14000, 0x18000};
    for (std::size_t index = 0; index < std::size(kSprites); ++index) {
        if (!checked_read(path, kSprites[index],
                          result.data.sprites.data() + sprite_offsets[index],
                          result.error))
            return result;
    }
    for (std::size_t index = 0; index < std::size(kProms); ++index) {
        if (!checked_read(path, kProms[index],
                          result.data.proms.data() + index * 0x100,
                          result.error))
            return result;
    }
    result.data.valid = true;
    return result;
}

} // namespace gng
