// Killer Instinct (Midway Wolf Unit) ROM loader.
//
// Identifies kinst and kinst2 by probing for the boot ROM.
// Boot ROM files vary across MAME revisions but share consistent
// sizes and locations.

#include "midway/midway_rom.h"
#include "platform_paths.h"

#include <minizip/unzip.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

namespace {

// ---- Boot ROM identification ----------------------------------------------
//
// Killer Instinct ships a single 512 KiB boot ROM. The filename varies
// across MAME revisions, but the size and the presence of certain magic
// bytes (the MIPS reset exception vector at the beginning of the ROM)
// identify it reliably.
//
// Known boot ROM filenames for kinst (v1.5d):
//   u10-laser.uc3  (MAME 0.172+ merged/split)
//   kinst.u10      (older MAME naming)
//
// For kinst2 (v1.4):
//   kinst2.u10     (MAME naming convention)
//   u10-laser.u10
//
// Size is always 524288 (0x80000) bytes.

constexpr std::size_t boot_rom_size = 0x80000;  // 512 KiB

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return value;
}

// ---- ZIP helpers ----------------------------------------------------------

struct zip_entry {
    bool found{};
    uint64_t size{};
};

zip_entry find_zip_file(unzFile archive, const char* wanted) {
    if (!archive || unzGoToFirstFile(archive) != UNZ_OK) return {};
    const std::string wanted_lower = lower(wanted);
    const char* want_basename = std::strrchr(wanted, '/');
    want_basename = want_basename ? want_basename + 1 : wanted;
    do {
        unz_file_info64 info{};
        std::array<char, 1024> name{};
        if (unzGetCurrentFileInfo64(archive, &info, name.data(), name.size(),
                                    nullptr, 0, nullptr, 0) != UNZ_OK)
            return {};
        const char* base = std::strrchr(name.data(), '/');
        base = base ? base + 1 : name.data();
        // Match by basename OR full name
        if (lower(base) == wanted_lower ||
            lower(name.data()) == wanted_lower)
            return {true, static_cast<uint64_t>(info.uncompressed_size)};
    } while (unzGoToNextFile(archive) == UNZ_OK);
    return {};
}

bool read_zip_entry(unzFile archive, const char* name,
                    std::vector<uint8_t>& bytes) {
    const auto entry = find_zip_file(archive, name);
    if (!entry.found || entry.size > (1ULL << 30)) return false;
    if (unzLocateFile(archive, name, 0) != UNZ_OK) {
        // Fall back: locate by basename-walk
        if (unzGoToFirstFile(archive) != UNZ_OK) return false;
        const std::string wanted = lower(std::strrchr(name, '/') ?
            std::strrchr(name, '/') + 1 : name);
        bool found = false;
        do {
            std::array<char, 1024> entry_name{};
            if (unzGetCurrentFileInfo64(archive, nullptr, entry_name.data(),
                                        entry_name.size(), nullptr, 0,
                                        nullptr, 0) != UNZ_OK)
                break;
            const char* base = std::strrchr(entry_name.data(), '/');
            base = base ? base + 1 : entry_name.data();
            if (lower(base) == wanted) { found = true; break; }
        } while (unzGoToNextFile(archive) == UNZ_OK);
        if (!found) return false;
    }
    unz_file_info64 info{};
    if (unzGetCurrentFileInfo64(archive, &info, nullptr, 0, nullptr, 0,
                                nullptr, 0) != UNZ_OK ||
        unzOpenCurrentFile(archive) != UNZ_OK)
        return false;
    bytes.resize(static_cast<std::size_t>(info.uncompressed_size));
    if (bytes.empty()) { unzCloseCurrentFile(archive); return true; }
    const int got = unzReadCurrentFile(
        archive, bytes.data(),
        static_cast<unsigned>(bytes.size()));
    unzCloseCurrentFile(archive);
    return got == static_cast<int>(bytes.size());
}

// ---- Directory helpers ----------------------------------------------------

bool read_dir_file(const std::string& dir, const char* name,
                   std::vector<uint8_t>& bytes) {
    fs::path path = fs::path(dir) / name;
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        // Search one level deep for merged-set subdirectories
        const std::string want = fs::path(name).filename().string();
        for (const auto& sub : fs::directory_iterator(dir, ec)) {
            if (!sub.is_directory(ec)) continue;
            fs::path cand = sub.path() / want;
            if (fs::is_regular_file(cand, ec)) { path = cand; break; }
        }
        if (!fs::is_regular_file(path, ec)) return false;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    bytes.assign(std::istreambuf_iterator<char>(input),
                 std::istreambuf_iterator<char>());
    return !bytes.empty();
}

bool read_entry(const std::string& archive_path, const char* name,
                std::vector<uint8_t>& bytes) {
    fs::path p(archive_path);
    std::error_code ec;
    if (fs::is_directory(p, ec))
        return read_dir_file(archive_path, name, bytes);
    unzFile archive = unzOpen64(archive_path.c_str());
    if (!archive) return false;
    const bool loaded = read_zip_entry(archive, name, bytes);
    unzClose(archive);
    return loaded;
}

// ---- Boot ROM detection ---------------------------------------------------

// Known boot ROM filenames, in order of preference (most specific first).
constexpr const char* kinst_boot_names[] = {
    "u10-laser.uc3", "kinst.u10", "ki-l15d.u98", "u10-laser",
    "boot.bin", "u10-laser.uc",
};
constexpr const char* kinst2_boot_names[] = {
    "kinst2.u10", "ki2_l1.u10", "u10-laser.u10", "ki2_l14.u98",
    "kinst2.boot", "boot.bin",
};

// Probe for any boot ROM file and determine which set it belongs to.
// Killer Instinct 2 is probed first because some collections include
// both and kinst2 has more specific file names.
//
// Falls back to scanning for any 524288-byte file when named matches fail,
// since MAME ROM naming conventions drift across revisions.
midway_rom_set probe_set(const std::string& path, std::string& boot_name) {
    // Probe for KI2 first (named files)
    for (const char* name : kinst2_boot_names) {
        std::vector<uint8_t> tmp;
        if (read_entry(path, name, tmp) && tmp.size() >= 0x40000) {
            boot_name = name;
            return midway_rom_set::killer_instinct_2;
        }
    }
    // Then KI1 (named files)
    for (const char* name : kinst_boot_names) {
        std::vector<uint8_t> tmp;
        if (read_entry(path, name, tmp) && tmp.size() >= 0x40000) {
            boot_name = name;
            return midway_rom_set::killer_instinct;
        }
    }

    // Fallback: scan archive for any file of exactly 524288 bytes.
    // That size uniquely identifies a Wolf Unit boot ROM in a KI set.
    // Determine KI1 vs KI2 by the presence of "ki2" or "kinst2" in
    // the filename or parent ZIP name.
    {
        fs::path p(path);
        bool is_ki2 = lower(p.stem().string()).find("kinst2") != std::string::npos ||
                      lower(p.stem().string()).find("ki2") != std::string::npos;

        const auto try_fallback = [&](bool prefer_ki2) -> midway_rom_set {
            unzFile archive = unzOpen64(path.c_str());
            if (!archive) return midway_rom_set::unknown;
            midway_rom_set result = midway_rom_set::unknown;
            if (unzGoToFirstFile(archive) == UNZ_OK) {
                do {
                    unz_file_info64 info{};
                    std::array<char, 1024> name_buf{};
                    if (unzGetCurrentFileInfo64(archive, &info, name_buf.data(),
                                                name_buf.size(), nullptr, 0,
                                                nullptr, 0) != UNZ_OK)
                        continue;
                    if (info.uncompressed_size != 0x80000) continue;
                    const std::string name_lower = lower(name_buf.data());
                    // Must look like a ROM (not a subdirectory, text, etc.)
                    if (name_lower.find('/') != std::string::npos &&
                        name_lower.find("kinst2uk/") == std::string::npos)
                        continue;
                    // Decide KI1 vs KI2 by filename clues
                    const bool looks_ki2 = name_lower.find("ki2") != std::string::npos;
                    if (prefer_ki2 && looks_ki2) {
                        boot_name = name_buf.data();
                        result = midway_rom_set::killer_instinct_2;
                        break;
                    }
                    if (!prefer_ki2 && !looks_ki2) {
                        boot_name = name_buf.data();
                        result = midway_rom_set::killer_instinct;
                        break;
                    }
                    // First match of any kind
                    if (result == midway_rom_set::unknown) {
                        boot_name = name_buf.data();
                        result = looks_ki2 ? midway_rom_set::killer_instinct_2
                                           : midway_rom_set::killer_instinct;
                    }
                } while (unzGoToNextFile(archive) == UNZ_OK);
            }
            unzClose(archive);
            return result;
        };

        const midway_rom_set fallback = try_fallback(is_ki2);
        if (fallback != midway_rom_set::unknown) return fallback;
        // If first pass failed, try the opposite preference
        return try_fallback(!is_ki2);
    }

    return midway_rom_set::unknown;
}

// ---- CHD disc discovery ---------------------------------------------------

// Known CHD filenames mapped to sets.
struct disc_map {
    midway_rom_set set;
    const char* filename;
};
constexpr disc_map known_discs[] = {
    {midway_rom_set::killer_instinct,   "kinst.chd"},
    {midway_rom_set::killer_instinct_2, "kinst2.chd"},
};

fs::path find_child_case_insensitive(const fs::path& directory,
                                     const std::string& name) {
    std::error_code error;
    if (!fs::is_directory(directory, error)) return {};
    const std::string wanted = lower(name);
    for (fs::directory_iterator iterator(directory, error), end;
         !error && iterator != end; iterator.increment(error)) {
        if (lower(iterator->path().filename().string()) == wanted)
            return iterator->path();
    }
    return {};
}

} // namespace

midway_rom_set midway_rom_loader::identify_set(const std::string& path) {
    if (path.empty()) return midway_rom_set::unknown;
    // Also identify CHD files directly so the library scanner can
    // recognise them.
    if (lower(fs::path(path).extension().string()) == ".chd") {
        const std::string stem = lower(fs::path(path).stem().string());
        if (stem == "kinst")  return midway_rom_set::killer_instinct;
        if (stem == "kinst2") return midway_rom_set::killer_instinct_2;
        return midway_rom_set::unknown;
    }
    std::string unused;
    return probe_set(path, unused);
}

const char* midway_rom_loader::set_short_name(midway_rom_set set) {
    switch (set) {
    case midway_rom_set::killer_instinct:   return "kinst";
    case midway_rom_set::killer_instinct_2: return "kinst2";
    case midway_rom_set::unknown:           return "";
    }
    return "";
}

const char* midway_rom_loader::set_display_name(midway_rom_set set) {
    switch (set) {
    case midway_rom_set::killer_instinct:
        return "Killer Instinct (Midway Wolf Unit, v1.5d)";
    case midway_rom_set::killer_instinct_2:
        return "Killer Instinct 2 (Midway Wolf Unit, v1.4)";
    case midway_rom_set::unknown:
        return "Unknown Midway Wolf Unit set";
    }
    return "Unknown Midway Wolf Unit set";
}

std::string midway_rom_loader::find_disc_path(
        const std::string& selected_path,
        const std::string& configured_chd_directory) {
    if (selected_path.empty()) return {};

    const fs::path selected(selected_path);
    std::error_code error;

    // If the selection is itself a CHD, use it directly.
    if (fs::is_regular_file(selected, error) &&
        lower(selected.extension().string()) == ".chd")
        return selected.lexically_normal().string();

    // Determine which set we're looking for.
    const midway_rom_set set = identify_set(selected_path);
    const char* disc_name = nullptr;
    for (const disc_map& disc : known_discs) {
        if (disc.set == set) { disc_name = disc.filename; break; }
    }
    if (!disc_name) return {};

    // Check beside the ROM first.
    const fs::path directory = fs::is_directory(selected, error) ?
        selected : selected.parent_path();
    fs::path found = find_child_case_insensitive(directory, disc_name);
    if (!found.empty()) return found.lexically_normal().string();

    // Check the configured CHD directory.
    const fs::path chd_root = configured_chd_directory.empty() ?
        manx_platform::data_root() / "MANX" / "chd" :
        fs::path(configured_chd_directory);
    found = find_child_case_insensitive(chd_root, disc_name);
    if (!found.empty()) return found.lexically_normal().string();

    return {};
}

midway_rom_load_result midway_rom_loader::load(
        const std::string& path,
        const std::string& configured_chd_directory) {
    midway_rom_load_result result;
    std::string boot_name;
    result.set = probe_set(path, boot_name);
    if (result.set == midway_rom_set::unknown) {
        result.error = "Archive is not a recognized Killer Instinct "
                       "ROM set (missing boot ROM).";
        return result;
    }
    if (!read_entry(path, boot_name.c_str(), result.boot_rom)) {
        result.error = "Failed to read boot ROM: " + boot_name;
        result.set = midway_rom_set::unknown;
        return result;
    }
    // Pad or truncate to standard boot ROM size.
    result.boot_rom.resize(boot_rom_size, 0xFF);

    // Load game code ROMs (u10-l1 through u36-l1).
    // The R4600 has a 64-bit data bus. Game ROMs are 16-bit chips
    // interleaved in groups of 4:
    //   Group A (0x1F800000-0x1F9FFFFF): u10, u11, u12, u13
    //   Group B (0x1FA00000-0x1FBFFFFF): u33, u34, u35, u36
    // Interleave pattern: every 8 bytes = 2 bytes from each of 4 chips.
    static const char* game_rom_names[] = {
        "u10-l1", "u11-l1", "u12-l1", "u13-l1",
        "u33-l1", "u34-l1", "u35-l1", "u36-l1",
    };
    constexpr int game_rom_count = 8;
    constexpr int chip_size = 512 * 1024;  // 512KB per chip
    constexpr int group_size = chip_size * 4;  // 2MB per group

    // Load each ROM chip.
    std::vector<uint8_t> rom_chips[game_rom_count];
    int loaded_chip_count = 0;
    for (int i = 0; i < game_rom_count; ++i) {
        rom_chips[i].resize(chip_size, 0xFF);
        std::vector<uint8_t> tmp;
        if (read_entry(path, game_rom_names[i], tmp) && !tmp.empty()) {
            std::memcpy(rom_chips[i].data(), tmp.data(),
                       std::min(tmp.size(), static_cast<std::size_t>(chip_size)));
            ++loaded_chip_count;
        }
    }
    if (loaded_chip_count == 0) {
        result.error = "No game ROM chips (u10-u36) found in archive.";
        result.set = midway_rom_set::unknown;
        return result;
    }

    // Interleave into flat 4MB buffer.
    result.game_roms.resize(group_size * 2, 0xFF);
    for (int group = 0; group < 2; ++group) {
        int src_off = 0;  // advances by 2 per 8-byte output chunk
        for (int offset = 0; offset < group_size; offset += 8) {
            for (int chip = 0; chip < 4; ++chip) {
                int chip_idx = group * 4 + chip;
                int dst = group * group_size + offset + chip * 2;
                result.game_roms[dst]     = rom_chips[chip_idx][src_off];
                result.game_roms[dst + 1] = rom_chips[chip_idx][src_off + 1];
            }
            src_off += 2;
        }
    }

    result.disc_path = find_disc_path(path, configured_chd_directory);
    if (result.disc_path.empty()) {
        result.error = "CHD disc image not found. Killer Instinct requires "
                       "kinst.chd or kinst2.chd in the same folder or "
                       "configured CHD directory.";
        result.set = midway_rom_set::unknown;
        return result;
    }
    return result;
}
