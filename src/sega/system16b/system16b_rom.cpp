// Shinobi (Sega System 16-B) ROM loader implementation.
//
// Finds the program image + graphics ROMs, builds the assembled sprite
// 16-bit BE region, and returns a populated roms struct. This is the
// singleton-file set: it accepts either:
//
//   (a) extracted directory with the MAME file basenames (shinobi_main.bin
//       for the program image, mpr-11363.a14, 11364.a15, 11365.a16 for
//       the tile ROMs, mpr-11366.b1, 11367.b2, 11368.b5, 11369.b6 for
//       the sprite ROMs), or
//   (b) a ZIP archive with the same files inside.
//
// Returns error.empty() on success. ROM staging is identical to the
// `tools/shinobi_shot.c:load_gfx` logic from the in-tree Amiga System 16
// ports (BSD-MIT Crown Park Computing reference), which itself mirrors
// MAME's `decrypt_opcode_table`/`shinobi` driver.

#include "sega/system16b/system16b_rom.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <minizip/unzip.h>

namespace system16b {

namespace {

bool load_zip_entry(unzFile archive, const char* name,
                    std::vector<uint8_t>& bytes) {
    if (!archive || !name) return false;
    if (unzLocateFile(archive, name, 0) != UNZ_OK) {
        // Merged MAME archives store per-set files in subdirectories
        // (e.g. "shinobi4/mpr-11363.a14"). Fall back to a basename
        // search across all entries.
        bool located = false;
        const char* want = std::strrchr(name, '/');
        want = want ? want + 1 : name;
        if (unzGoToFirstFile(archive) == UNZ_OK) {
            do {
                char entry[512];
                if (unzGetCurrentFileInfo(archive, nullptr, entry,
                                          sizeof(entry), nullptr, 0,
                                          nullptr, 0) != UNZ_OK)
                    break;
                const char* base = std::strrchr(entry, '/');
                base = base ? base + 1 : entry;
                if (std::strcmp(base, want) == 0) { located = true; break; }
            } while (unzGoToNextFile(archive) == UNZ_OK);
        }
        if (!located) return false;
    }
    unz_file_info info{};
    if (unzGetCurrentFileInfo(archive, &info, nullptr, 0, nullptr, 0,
                              nullptr, 0) != UNZ_OK ||
        unzOpenCurrentFile(archive) != UNZ_OK)
        return false;
    bytes.resize(info.uncompressed_size);
    if (bytes.empty()) { unzCloseCurrentFile(archive); return true; }
    const int got = unzReadCurrentFile(
        archive, bytes.data(),
        static_cast<unsigned>(bytes.size()));
    unzCloseCurrentFile(archive);
    return got == static_cast<int>(bytes.size());
}

bool load_dir_entry(const std::string& dir, const char* name,
                    std::vector<uint8_t>& bytes) {
    namespace fs = std::filesystem;
    fs::path p = fs::path(dir) / name;
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) {
        // Extracted merged sets keep files in per-set subdirectories;
        // search one level deep by basename.
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

bool read_entry(const std::string& archive_path,
                const char* name,
                std::vector<uint8_t>& bytes) {
    namespace fs = std::filesystem;
    fs::path p(archive_path);
    std::error_code ec;
    if (fs::is_directory(p, ec)) return load_dir_entry(archive_path, name, bytes);
    unzFile archive = unzOpen64(archive_path.c_str());
    if (!archive) return false;
    const bool loaded = load_zip_entry(archive, name, bytes);
    unzClose(archive);
    return loaded;
}

bool read_game_entry(const std::string& archive_path,
                     const char* name,
                     std::vector<uint8_t>& bytes) {
    if (read_entry(archive_path, name, bytes)) return true;
    namespace fs = std::filesystem;
    const fs::path selected(archive_path);
    std::error_code error;
    const fs::path directory = selected.parent_path();
    const std::array<const char*, 3> companions{
        "shinobi.zip", "shinobi4.zip", "shinobi6.zip"};
    for (fs::directory_iterator iterator(directory, error), end;
         !error && iterator != end; iterator.increment(error)) {
        std::string filename = iterator->path().filename().string();
        std::transform(filename.begin(), filename.end(), filename.begin(),
                       [](unsigned char character) {
                           return static_cast<char>(std::tolower(character));
                       });
        if (std::find(companions.begin(), companions.end(), filename) ==
                companions.end() || iterator->path() == selected)
            continue;
        if (read_entry(iterator->path().string(), name, bytes)) return true;
    }
    return false;
}

bool read_into(const std::string& archive_path,
               const char* name,
               void* dst, std::size_t dst_size,
               std::string* err) {
    std::vector<uint8_t> tmp;
    if (!read_game_entry(archive_path, name, tmp)) {
        if (err) *err = std::string("missing file: ") + name;
        return false;
    }
    if (tmp.size() != dst_size) {
        if (err) *err = std::string("bad size for ") + name +
                       ": got " + std::to_string(tmp.size()) +
                       ", want " + std::to_string(dst_size);
        return false;
    }
    std::memcpy(dst, tmp.data(), dst_size);
    return true;
}

}  // namespace

bool system16b_roms::complete() const {
    // We require the program image and the three tile planes; sprite
    // assembled region is left zero-padded if individual sprite ROMs
    // are missing, which renders silently.
    auto any_nonzero = [](const auto& buf) {
        for (auto v : buf) if (v) return true;
        return false;
    };
    return any_nonzero(program) &&
           any_nonzero(tile_plane0) &&
           any_nonzero(tile_plane1) &&
           any_nonzero(tile_plane2);
}

system16b_rom_set system16b_rom_loader::identify_set(const std::string& path) {
    namespace fs = std::filesystem;
    if (path.empty()) return system16b_rom_set::unknown;
    std::error_code ec;
    if (!fs::exists(path, ec)) return system16b_rom_set::unknown;

    std::vector<uint8_t> tmp;
    // Alien Syndrome (set 4, unprotected): identified by its 68000 pair.
    if (read_entry(path, "epr-11083.a4", tmp) &&
        read_entry(path, "epr-11080.a1", tmp))
        return system16b_rom_set::alien_syndrome;
    // Aurail (set 3, US, unprotected): identified by its 68000 pair.
    if (read_entry(path, "epr-13577.a7", tmp) &&
        read_entry(path, "epr-13576.a5", tmp))
        return system16b_rom_set::aurail;
    // A directory must contain either shinobi_main.bin (the staged
    // 256 KiB program image) OR the MAME file names. A ZIP must contain
    // at least one of shinobi_main.bin or mpr-11363.a14.
    const bool found_main = read_entry(path, "shinobi_main.bin", tmp) ||
                            read_entry(path, "mpr-11363.a14", tmp) ||
                            read_entry(path, "epr-11360.a7", tmp);
    return found_main ? system16b_rom_set::shinobi_us
                      : system16b_rom_set::unknown;
}

const char* system16b_rom_loader::set_short_name(system16b_rom_set set) {
    switch (set) {
    case system16b_rom_set::shinobi_us: return "shinobi4";
    case system16b_rom_set::alien_syndrome: return "aliensyn";
    case system16b_rom_set::aurail: return "aurail";
    case system16b_rom_set::unknown: return "";
    }
    return "";
}

const char* system16b_rom_loader::set_display_name(system16b_rom_set set) {
    switch (set) {
    case system16b_rom_set::shinobi_us: return "Shinobi (Sega System 16B, US)";
    case system16b_rom_set::alien_syndrome:
        return "Alien Syndrome (Sega System 16B, unprotected)";
    case system16b_rom_set::aurail:
        return "Aurail (Sega System 16B, US, unprotected)";
    case system16b_rom_set::unknown:    return "Unknown Shinobi-style set";
    }
    return "Unknown Shinobi-style set";
}

namespace {

// Alien Syndrome (MAME `aliensyn`, set 4, ROM board 171-5358, unprotected).
// Same board shape as Shinobi: 68000 byte pairs, three planar tile ROMs
// (half-size, padded), four word-interleaved sprite pairs and the Z80 +
// banked uPD7759 sample layout - three 32 KiB sample ROMs that sit at
// 0x10000 gaps in the bank window, exactly where MAME's soundcpu region
// places them (window base = region + 0x10000).
system16b_rom_load_result load_alien_syndrome(const std::string& path) {
    system16b_rom_load_result r{};
    r.set = system16b_rom_set::alien_syndrome;

    struct pair_spec { const char* even; const char* odd; std::size_t base; };
    static constexpr std::array<pair_spec, 3> program_pairs{{
        {"epr-11083.a4", "epr-11080.a1", 0x00000},
        {"epr-11084.a5", "epr-11081.a2", 0x10000},
        {"epr-11085.a6", "epr-11082.a3", 0x20000},
    }};
    r.roms.program.fill(0xff);
    for (const pair_spec& pair : program_pairs) {
        std::vector<uint8_t> even, odd;
        if (!read_game_entry(path, pair.even, even) ||
            !read_game_entry(path, pair.odd, odd) ||
            even.size() != 0x8000 || odd.size() != 0x8000) {
            r.error = std::string("missing program pair ") + pair.even +
                      " + " + pair.odd;
            return r;
        }
        for (std::size_t i = 0; i < 0x8000; ++i) {
            r.roms.program[pair.base + i * 2]     = even[i];
            r.roms.program[pair.base + i * 2 + 1] = odd[i];
        }
    }

    // Tiles: three 64 KiB planes (half the buffer; the rest stays clear so
    // out-of-range tile codes render as pen 0).
    const auto read_tile = [&](const char* name, auto& plane) {
        std::vector<uint8_t> tmp;
        if (!read_game_entry(path, name, tmp) || tmp.size() != 0x10000)
            return false;
        plane.fill(0);
        std::memcpy(plane.data(), tmp.data(), tmp.size());
        return true;
    };
    if (!read_tile("epr-10702.b9",  r.roms.tile_plane0) ||
        !read_tile("epr-10703.b10", r.roms.tile_plane1) ||
        !read_tile("epr-10704.b11", r.roms.tile_plane2)) {
        r.error = "Missing one of the tile ROMs (epr-10702/03/04)";
        return r;
    }

    // Sprites: four word-interleaved pairs of 64 KiB ROMs, even file on
    // even bytes.
    struct sprite_spec { const char* even; const char* odd; std::size_t base; };
    static constexpr std::array<sprite_spec, 4> sprite_pairs{{
        {"epr-10713.b5", "epr-10709.b1", 0x00000},
        {"epr-10714.b6", "epr-10710.b2", 0x20000},
        {"epr-10715.b7", "epr-10711.b3", 0x40000},
        {"epr-10716.b8", "epr-10712.b4", 0x60000},
    }};
    for (const sprite_spec& pair : sprite_pairs) {
        std::vector<uint8_t> even, odd;
        if (!read_game_entry(path, pair.even, even) ||
            !read_game_entry(path, pair.odd, odd) ||
            even.size() != 0x10000 || odd.size() != 0x10000)
            continue;  // like Shinobi: missing sprites render silently
        for (std::size_t i = 0; i < 0x10000; ++i) {
            r.roms.sprite_gfx[pair.base + i * 2]     = even[i];
            r.roms.sprite_gfx[pair.base + i * 2 + 1] = odd[i];
        }
    }

    // Z80 program + the three scattered sample ROMs.
    {
        std::vector<uint8_t> tmp;
        if (read_game_entry(path, "epr-10723.a7", tmp) &&
            tmp.size() == r.roms.sound_prog.size())
            std::memcpy(r.roms.sound_prog.data(), tmp.data(), tmp.size());
        else
            r.roms.sound_prog.fill(0xff);
        r.roms.sound_data.fill(0);
        static constexpr std::array<std::pair<const char*, std::size_t>, 3>
            samples{{{"epr-10724.a8", 0x00000},
                     {"epr-10725.a9", 0x10000},
                     {"epr-10726.a10", 0x20000}}};
        for (const auto& [name, base] : samples) {
            if (read_game_entry(path, name, tmp) && tmp.size() == 0x8000)
                std::memcpy(r.roms.sound_data.data() + base, tmp.data(),
                            tmp.size());
        }
    }
    return r;
}

// Aurail (MAME `aurail`, set 3 US, ROM board 171-5704, unprotected).
// Twice Shinobi's program (four byte-interleaved 128 KiB ROMs), two chips
// per tile plane, sixteen word-interleaved sprite ROMs filling all eight
// 128 KiB banks, and a single 128 KiB uPD7759 sample ROM.
system16b_rom_load_result load_aurail(const std::string& path) {
    system16b_rom_load_result r{};
    r.set = system16b_rom_set::aurail;

    struct pair_spec { const char* even; const char* odd; std::size_t base; };
    static constexpr std::array<pair_spec, 2> program_pairs{{
        {"epr-13577.a7", "epr-13576.a5", 0x00000},
        {"epr-13447.a8", "epr-13445.a6", 0x40000},
    }};
    r.roms.program.fill(0xff);
    for (const pair_spec& pair : program_pairs) {
        std::vector<uint8_t> even, odd;
        if (!read_game_entry(path, pair.even, even) ||
            !read_game_entry(path, pair.odd, odd) ||
            even.size() != 0x20000 || odd.size() != 0x20000) {
            r.error = std::string("missing program pair ") + pair.even +
                      " + " + pair.odd;
            return r;
        }
        for (std::size_t i = 0; i < 0x20000; ++i) {
            r.roms.program[pair.base + i * 2]     = even[i];
            r.roms.program[pair.base + i * 2 + 1] = odd[i];
        }
    }

    // Tiles: each plane is two 128 KiB chips end to end, giving the eight
    // 4096-tile pages the 5704 bank register selects from.
    struct tile_spec { const char* low; const char* high; };
    static constexpr std::array<tile_spec, 3> tile_chips{{
        {"mpr-13450.a14", "mpr-13465.b14"},
        {"mpr-13451.a15", "mpr-13466.b15"},
        {"mpr-13452.a16", "mpr-13467.b16"},
    }};
    const auto read_plane = [&](const tile_spec& spec, auto& plane) {
        std::vector<uint8_t> low, high;
        if (!read_game_entry(path, spec.low, low) ||
            !read_game_entry(path, spec.high, high) ||
            low.size() != 0x20000 || high.size() != 0x20000)
            return false;
        plane.fill(0);
        std::memcpy(plane.data(), low.data(), low.size());
        std::memcpy(plane.data() + 0x20000, high.data(), high.size());
        return true;
    };
    if (!read_plane(tile_chips[0], r.roms.tile_plane0) ||
        !read_plane(tile_chips[1], r.roms.tile_plane1) ||
        !read_plane(tile_chips[2], r.roms.tile_plane2)) {
        r.error = "Missing one of the tile ROMs (mpr-13450/51/52/65/66/67)";
        return r;
    }

    // Sprites: eight word-interleaved pairs, even file on even bytes.
    struct sprite_spec { const char* even; const char* odd; std::size_t base; };
    static constexpr std::array<sprite_spec, 8> sprite_pairs{{
        {"mpr-13457.b5",  "mpr-13453.b1", 0x000000},
        {"mpr-13458.b6",  "mpr-13454.b2", 0x040000},
        {"mpr-13459.b7",  "mpr-13455.b3", 0x080000},
        {"mpr-13460.b8",  "mpr-13456.b4", 0x0c0000},
        {"mpr-13461.b10", "mpr-13440.a1", 0x100000},
        {"mpr-13462.b11", "mpr-13441.a2", 0x140000},
        {"mpr-13463.b12", "mpr-13442.a3", 0x180000},
        {"mpr-13464.b13", "mpr-13443.a4", 0x1c0000},
    }};
    for (const sprite_spec& pair : sprite_pairs) {
        std::vector<uint8_t> even, odd;
        if (!read_game_entry(path, pair.even, even) ||
            !read_game_entry(path, pair.odd, odd) ||
            even.size() != 0x20000 || odd.size() != 0x20000)
            continue;  // like Shinobi: missing sprites render silently
        for (std::size_t i = 0; i < 0x20000; ++i) {
            r.roms.sprite_gfx[pair.base + i * 2]     = even[i];
            r.roms.sprite_gfx[pair.base + i * 2 + 1] = odd[i];
        }
    }

    // Z80 program + the banked sample ROM (window base = MAME region
    // + 0x10000, so the single chip sits at offset 0 here).
    {
        std::vector<uint8_t> tmp;
        if (read_game_entry(path, "epr-13448.a10", tmp) &&
            tmp.size() == r.roms.sound_prog.size())
            std::memcpy(r.roms.sound_prog.data(), tmp.data(), tmp.size());
        else
            r.roms.sound_prog.fill(0xff);
        r.roms.sound_data.fill(0);
        if (read_game_entry(path, "mpr-13449.a11", tmp) &&
            tmp.size() == 0x20000)
            std::memcpy(r.roms.sound_data.data(), tmp.data(), tmp.size());
    }
    return r;
}

}  // namespace

system16b_rom_load_result system16b_rom_loader::load(const std::string& path) {
    system16b_rom_load_result r{};
    r.set = identify_set(path);
    if (r.set == system16b_rom_set::unknown) {
        r.error = "No Shinobi program image or graphics ROMs found in: " + path;
        return r;
    }
    if (r.set == system16b_rom_set::alien_syndrome)
        return load_alien_syndrome(path);
    if (r.set == system16b_rom_set::aurail)
        return load_aurail(path);

    // 1) Program image (256 KiB - Shinobi's own size, half the shared
    //    buffer). Prefer the consolidated flat image; otherwise assemble
    //    it from the MAME `shinobi4` pair, which is word-interleaved:
    //    epr-11360.a7 -> even bytes, epr-11359.a5 -> odd.
    constexpr std::size_t kShinobiProgramBytes = 0x40000;
    r.roms.program.fill(0xff);
    if (!read_into(path, "shinobi_main.bin",
                   r.roms.program.data(), kShinobiProgramBytes,
                   nullptr)) {
        std::vector<uint8_t> even, odd;
        if (!read_game_entry(path, "epr-11360.a7", even) ||
            !read_game_entry(path, "epr-11359.a5", odd) ||
            even.size() + odd.size() != kShinobiProgramBytes) {
            r.error = "missing program image: need shinobi_main.bin or the "
                      "epr-11360.a7 + epr-11359.a5 pair";
            return r;
        }
        const std::size_t half = kShinobiProgramBytes / 2;
        for (std::size_t i = 0; i < half; ++i) {
            r.roms.program[i * 2]     = even[i];
            r.roms.program[i * 2 + 1] = odd[i];
        }
    }

    // 2) Tile graphics: three planar 8x8 ROMs. Some Shinobi sets have
    //    0x10000-byte ROMs; my loader target buffer is 0x20000, so
    //    a short read is padded with 0xff and treated as loaded.
    auto read_tile = [&](const char* name, void* dst) -> bool {
        std::vector<uint8_t> tmp;
        if (!read_game_entry(path, name, tmp)) return false;
        const std::size_t cap = kGfxFileBytes;
        const std::size_t got = std::min(tmp.size(), cap);
        std::memset(dst, 0xff, cap);
        std::memcpy(dst, tmp.data(), got);
        return true;
    };
    bool tile0 = read_tile("mpr-11363.a14", r.roms.tile_plane0.data());
    bool tile1 = read_tile("mpr-11364.a15", r.roms.tile_plane1.data());
    bool tile2 = read_tile("mpr-11365.a16", r.roms.tile_plane2.data());
    if (!tile0 || !tile1 || !tile2) {
        r.error = "Missing one of the tile ROMs (mpr-11363/64/65)";
        return r;
    }

    // 3) Sprite graphics: 4 ROMs assembled into a 16-bit BE region.
    //   - mpr-11368.b5 -> even bytes 0..0x1ffff
    //   - mpr-11366.b1 -> odd  bytes 0..0x1ffff
    //   - mpr-11369.b6 -> even bytes 0x20000..0x3ffff  (offset 0..0x1ffff, *2+0x40000)
    //   - mpr-11367.b2 -> odd  bytes 0x20000..0x3ffff
    // In the assembled region the odd bank lives at +1; the even bank
    // lives at offsets where (i*2) lands the byte. See
    // tools/shinobi_shot.c:load_gfx in the in-tree System 16 port.
    auto assemble_sprite = [&](const char* bank_name, std::size_t byte_base) {
        std::vector<uint8_t> rom;
        if (!read_game_entry(path, bank_name, rom)) return false;
        if (rom.size() < kGfxFileBytes) return false;
        for (std::size_t i = 0; i < kGfxFileBytes; ++i) {
            r.roms.sprite_gfx[byte_base + i * 2] = rom[i];
        }
        return true;
    };

    // All four banks are word-interleaved (strided), matching
    // tools/shinobi_shot.c:load_gfx: even banks at +0, odd banks at +1.
    bool s_lo_e = assemble_sprite("mpr-11368.b5", 0x00000);          // even low
    bool s_lo_o = assemble_sprite("mpr-11366.b1", 0x00001);          // odd  low
    bool s_hi_e = assemble_sprite("mpr-11369.b6", 0x40000);          // even high
    bool s_hi_o = assemble_sprite("mpr-11367.b2", 0x40001);          // odd  high

    if (!s_lo_e || !s_lo_o || !s_hi_e || !s_hi_o) {
        // Sprite ROMs missing is non-fatal: render the rest of the board
        // and skip sprites. Clear the error so the boot proceeds; the
        // missing ROMs produce silent sprite output.
        r.error.clear();
    }

    // 4) Sound CPU program (Z80) -- `epr-11361.a10` (32 KiB) +
    //    `mpr-11362.a11` (128 KiB banked data/sample region).
    //    Both are optional; the boot still proceeds without them but the
    //    sound CPU will execute NOPs and the 68000 boot ROM will stay
    //    stuck at the sound hand-shake wait (the same stall we are
    //    breaking).
    auto read_padded = [&](const char* name, void* dst, std::size_t cap) {
        std::vector<uint8_t> tmp;
        if (!read_game_entry(path, name, tmp)) return false;
        std::memset(dst, 0xff, cap);
        const std::size_t got = std::min(tmp.size(), cap);
        std::memcpy(dst, tmp.data(), got);
        return true;
    };
    // Sound CPU program (Z80): epr-11361.a10 (32 KiB) is the PLAIN Z80
    // sound program. epr-11377.a10 shipped with the shinobi4 set is the
    // same code encrypted for the MC-8123B (317-0054) -- do NOT load it
    // unless we grow MC-8123 opcode decryption.
    // Sound PCM/data: mpr-11362.a11 (128 KiB bank window, padded).
    bool prog_ok = read_padded("epr-11361.a10", r.roms.sound_prog.data(), r.roms.sound_prog.size());
    // The bank buffer is now double the ROM: mirror the 128 KiB image into
    // the upper half so bank offsets past 0x20000 read the same bytes the
    // old modulo-by-0x20000 addressing produced.
    bool data_ok = read_padded("mpr-11362.a11", r.roms.sound_data.data(), 0x20000);
    std::memcpy(r.roms.sound_data.data() + 0x20000, r.roms.sound_data.data(),
                0x20000);
    (void)prog_ok;
    (void)data_ok;

    return r;
}

}  // namespace system16b
