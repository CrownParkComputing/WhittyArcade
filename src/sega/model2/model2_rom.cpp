#include "sega/model2/model2_rom.h"

#include <minizip/unzip.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <sstream>

namespace fs = std::filesystem;

namespace {
struct rom_spec {
    const char* name;
    std::size_t size;
    uint32_t crc;
};

class source_reader {
public:
    explicit source_reader(fs::path source) : m_source(std::move(source)) {}

    bool valid() const {
        std::error_code error;
        return fs::is_directory(m_source, error) ||
               (fs::is_regular_file(m_source, error) &&
                lower(m_source.extension().string()) == ".zip");
    }

    bool contains(const std::string& wanted) const {
        std::vector<uint8_t> ignored;
        return read(wanted, ignored, false);
    }

    bool read(const std::string& wanted, std::vector<uint8_t>& output,
              bool read_contents = true) const {
        output.clear();
        if (lower(m_source.extension().string()) != ".zip")
            return read_directory(wanted, output, read_contents);

        unzFile archive = unzOpen64(m_source.string().c_str());
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
                output.resize(static_cast<std::size_t>(info.uncompressed_size));
                std::size_t total = 0;
                while (total < output.size()) {
                    const unsigned request = static_cast<unsigned>(
                        std::min<std::size_t>(output.size() - total, 1u << 20));
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

private:
    fs::path m_source;

    static std::string lower(std::string value) {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char character) {
                           return static_cast<char>(std::tolower(character));
                       });
        return value;
    }

    bool read_directory(const std::string& wanted,
                        std::vector<uint8_t>& output,
                        bool read_contents) const {
        std::error_code error;
        if (!fs::is_directory(m_source, error)) return false;
        const std::string wanted_lower = lower(wanted);
        fs::recursive_directory_iterator iterator(m_source, error), end;
        while (!error && iterator != end) {
            std::error_code type_error;
            if (iterator->is_regular_file(type_error) &&
                lower(iterator->path().filename().string()) == wanted_lower) {
                if (!read_contents) return true;
                std::FILE* file = std::fopen(
                    iterator->path().string().c_str(), "rb");
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
            iterator.increment(error);
        }
        return false;
    }
};

uint32_t data_crc(const std::vector<uint8_t>& data) {
    uLong value = crc32(0, Z_NULL, 0);
    std::size_t offset = 0;
    while (offset < data.size()) {
        const uInt length = static_cast<uInt>(
            std::min<std::size_t>(data.size() - offset,
                                  std::numeric_limits<uInt>::max()));
        value = crc32(value, data.data() + offset, length);
        offset += length;
    }
    return static_cast<uint32_t>(value);
}

bool load_checked(const source_reader& source, const rom_spec& spec,
                  std::vector<uint8_t>& output, std::ostringstream& errors,
                  bool required = true) {
    if (!source.read(spec.name, output)) {
        if (required) errors << "Missing ROM: " << spec.name << '\n';
        return false;
    }
    if (output.size() != spec.size) {
        errors << "Wrong size for " << spec.name << ": expected 0x"
               << std::hex << spec.size << ", got 0x" << output.size()
               << std::dec << '\n';
        output.clear();
        return false;
    }
    const uint32_t actual_crc = data_crc(output);
    if (actual_crc != spec.crc) {
        errors << "Wrong CRC for " << spec.name << ": expected "
               << std::hex << spec.crc << ", got " << actual_crc
               << std::dec << '\n';
        output.clear();
        return false;
    }
    return true;
}

bool copy_linear(const source_reader& source, const rom_spec& spec,
                 std::vector<uint8_t>& destination, std::size_t offset,
                 std::ostringstream& errors) {
    std::vector<uint8_t> file;
    if (!load_checked(source, spec, file, errors)) return false;
    if (offset > destination.size() ||
        file.size() > destination.size() - offset) {
        errors << "ROM region overflow for " << spec.name << '\n';
        return false;
    }
    std::copy(file.begin(), file.end(), destination.begin() +
              static_cast<std::vector<uint8_t>::difference_type>(offset));
    return true;
}

bool copy_word32(const source_reader& source, const rom_spec& spec,
                 std::vector<uint8_t>& destination, std::size_t offset,
                 std::ostringstream& errors) {
    std::vector<uint8_t> file;
    if (!load_checked(source, spec, file, errors)) return false;
    if ((file.size() & 1) != 0 || file.empty() ||
        offset + 1 >= destination.size() ||
        (file.size() / 2 - 1) * 4 > destination.size() - 2 - offset) {
        errors << "ROM region overflow for " << spec.name << '\n';
        return false;
    }
    for (std::size_t index = 0; index < file.size(); index += 2) {
        const std::size_t target = offset + (index / 2) * 4;
        destination[target] = file[index];
        destination[target + 1] = file[index + 1];
    }
    return true;
}

bool copy_word_swapped(const source_reader& source, const rom_spec& spec,
                       std::vector<uint8_t>& destination,
                       std::size_t offset, std::ostringstream& errors) {
    std::vector<uint8_t> file;
    if (!load_checked(source, spec, file, errors)) return false;
    if ((file.size() & 1) != 0 || offset > destination.size() ||
        file.size() > destination.size() - offset) {
        errors << "ROM region overflow for " << spec.name << '\n';
        return false;
    }
    for (std::size_t index = 0; index < file.size(); index += 2) {
        destination[offset + index] = file[index + 1];
        destination[offset + index + 1] = file[index];
    }
    return true;
}

bool copy_byte32(const source_reader& source, const rom_spec& spec,
                 std::vector<uint8_t>& destination, std::size_t lane,
                 std::ostringstream& errors) {
    std::vector<uint8_t> file;
    if (!load_checked(source, spec, file, errors)) return false;
    if (file.empty() || lane >= destination.size() ||
        (file.size() - 1) * 4 > destination.size() - 1 - lane) {
        errors << "ROM region overflow for " << spec.name << '\n';
        return false;
    }
    for (std::size_t index = 0; index < file.size(); ++index)
        destination[lane + index * 4] = file[index];
    return true;
}

model2_rom_load_result load_sega_rally(const source_reader& source,
                                       const fs::path& selected_path) {
    model2_rom_load_result result;
    result.set = model2_rom_set::sega_rally_revision_c;
    model2_roms& roms = result.roms;
    roms.set = model2_rom_set::sega_rally_revision_c;
    std::ostringstream errors;

    roms.main_cpu.assign(0x200000, 0xff);
    copy_word32(source, {"epr-17888c.12", 0x080000, 0x3d6808aa},
                roms.main_cpu, 0x000000, errors);
    copy_word32(source, {"epr-17889c.13", 0x080000, 0xf43c7802},
                roms.main_cpu, 0x000002, errors);

    roms.main_data.assign(0x2400000, 0xff);
    constexpr std::array<rom_spec, 6> main_data{{
        {"mpr-17746.10", 0x200000, 0x8fe311f4},
        {"mpr-17747.11", 0x200000, 0x543593fd},
        {"mpr-17744.8",  0x200000, 0x71fed098},
        {"mpr-17745.9",  0x200000, 0x8ecca705},
        {"mpr-17884.6",  0x200000, 0x4cfc95e1},
        {"mpr-17885.7",  0x200000, 0xa08d2467},
    }};
    for (std::size_t index = 0; index < main_data.size(); ++index)
        copy_word32(source, main_data[index], roms.main_data,
                    (index / 2) * 0x400000 + (index & 1) * 2, errors);

    roms.copro_data.assign(0x800000, 0xff);
    copy_word32(source, {"mpr-17754.28", 0x200000, 0x81a84f67},
                roms.copro_data, 0, errors);
    copy_word32(source, {"mpr-17755.29", 0x200000, 0x2a6e7da4},
                roms.copro_data, 2, errors);

    roms.drive_cpu.assign(0x10000, 0xff);
    copy_linear(source, {"epr-17891.ic12", 0x10000, 0x9a33b437},
                roms.drive_cpu, 0, errors);

    roms.polygon_data.assign(0x2000000, 0xff);
    constexpr std::array<rom_spec, 4> polygons{{
        {"mpr-17748.16", 0x200000, 0x3148a2b2},
        {"mpr-17750.20", 0x200000, 0x232aec29},
        {"mpr-17749.17", 0x200000, 0x0838d184},
        {"mpr-17751.21", 0x200000, 0xed87ac62},
    }};
    for (std::size_t index = 0; index < polygons.size(); ++index)
        copy_word32(source, polygons[index], roms.polygon_data,
                    (index / 2) * 0x400000 + (index & 1) * 2, errors);

    roms.texture_data.assign(0x1000000, 0xff);
    copy_word32(source, {"mpr-17753.25", 0x200000, 0x6db0eb36},
                roms.texture_data, 0, errors);
    copy_word32(source, {"mpr-17752.24", 0x200000, 0xd6aa86ce},
                roms.texture_data, 2, errors);

    roms.communication_cpu.assign(0x20000, 0xff);
    copy_linear(source, {"epr-16726.bin", 0x20000, 0xc179b8c7},
                roms.communication_cpu, 0, errors);

    roms.sound_cpu.assign(0x80000, 0xff);
    copy_word_swapped(source, {"epr-17890a.30", 0x40000, 0x5bac3fa1},
                      roms.sound_cpu, 0, errors);

    roms.samples.assign(0x800000, 0xff);
    constexpr std::array<rom_spec, 4> samples{{
        {"mpr-17756.31", 0x200000, 0x7725f111},
        {"mpr-17757.32", 0x200000, 0x1616e649},
        {"mpr-17886.36", 0x200000, 0x54a72923},
        {"mpr-17887.37", 0x200000, 0x38c31fdd},
    }};
    for (std::size_t index = 0; index < samples.size(); ++index)
        copy_word_swapped(source, samples[index], roms.samples,
                          index * 0x200000, errors);

    roms.copro_tgp_tables.assign(0x40000, 0xff);
    copy_word32(source, {"opr-14742a.45", 0x20000, 0x90c6b117},
                roms.copro_tgp_tables, 0, errors);
    copy_word32(source, {"opr-14743a.46", 0x20000, 0xae7f446b},
                roms.copro_tgp_tables, 2, errors);

    roms.other_data.assign(0x80000, 0xff);
    constexpr std::array<rom_spec, 4> other{{
        {"opr-14744.58", 0x20000, 0x730ea9e0},
        {"opr-14745.59", 0x20000, 0x4c934d96},
        {"opr-14746.62", 0x20000, 0x2a266cbd},
        {"opr-14747.63", 0x20000, 0xa4ad5e19},
    }};
    for (std::size_t index = 0; index < other.size(); ++index)
        copy_word32(source, other[index], roms.other_data,
                    (index / 2) * 0x40000 + (index & 1) * 2, errors);

    roms.video_tables.assign(0x200000, 0);
    copy_byte32(source, {"mpr-16310.15", 0x80000, 0xc078a780},
                roms.video_tables, 0, errors);
    copy_byte32(source, {"mpr-16311.16", 0x80000, 0x452a492b},
                roms.video_tables, 1, errors);
    copy_byte32(source, {"mpr-16312.14", 0x80000, 0xa25fef5b},
                roms.video_tables, 2, errors);

    const rom_spec billboard{"epr-18022.ic2", 0x10000, 0x0ca70f80};
    std::ostringstream ignored;
    if (!load_checked(source, billboard, roms.billboard_cpu, ignored, false)) {
        const fs::path directory = fs::is_directory(selected_path) ?
            selected_path : selected_path.parent_path();
        const source_reader companion(directory / "segabill.zip");
        if (companion.valid())
            load_checked(companion, billboard, roms.billboard_cpu, ignored,
                         false);
    }

    result.error = errors.str();
    return result;
}

model2_rom_load_result load_virtua_cop_2(const source_reader& source) {
    // Virtua Cop 2 is a Model 2A-CRX light-gun game. It shares the
    // TGP lookup tables (opr-1474X) and video board tables (mpr-16310
    // / 16311 / 16312) byte-for-byte with Sega Rally; everything else
    // is unique to the game. There is no drive CPU, no M2COMM comm
    // board, and no billboard.
    //
    // MAME reference: src/mame/sega/model2.cpp ROM_START(vcop2)
    //                 (Model 2A, Sega Game ID# 833-12266).
    model2_rom_load_result result;
    result.set = model2_rom_set::virtua_cop_2;
    model2_roms& roms = result.roms;
    roms.set = model2_rom_set::virtua_cop_2;
    std::ostringstream errors;

    // i960 program: 4x 0x80000 word32 ROMs. The first pair interleaves
    // at word 0 (offset 0x000000 lane 0, offset 0x000002 lane 2); the
    // second pair starts at word 0x80000 (offset 0x100000 lane 0,
    // offset 0x100002 lane 2), not at lane 0 of the next word.
    roms.main_cpu.assign(0x200000, 0xff);
    copy_word32(source, {"epr-18524.12", 0x080000, 0x1858988b},
                roms.main_cpu, 0x000000, errors);
    copy_word32(source, {"epr-18525.13", 0x080000, 0x0c13df3f},
                roms.main_cpu, 0x000002, errors);
    copy_word32(source, {"epr-18518.14", 0x080000, 0x7842951b},
                roms.main_cpu, 0x100000, errors);
    copy_word32(source, {"epr-18519.15", 0x080000, 0x31a30edc},
                roms.main_cpu, 0x100002, errors);

    // Main data bus. 0x2400000 total, with two 0x80000 EPRs in the
    // final 0x400000 region that MAME then ROM_COPY's three times
    // from 0xc00000 onto 0xd00000, 0xe00000 and 0xf00000. We mirror
    // that copy here so reads from 0x06000000+ hit the right bytes.
    roms.main_data.assign(0x2400000, 0xff);
    constexpr std::array<rom_spec, 6> main_data_2m{{
        {"mpr-18516.10", 0x200000, 0xa3928ff0},
        {"mpr-18517.11", 0x200000, 0x4bd73da4},
        {"mpr-18514.8",  0x200000, 0x791283c5},
        {"mpr-18515.9",  0x200000, 0x6ba1ffec},
        {"mpr-18522.6",  0x200000, 0x61d18536},
        {"mpr-18523.7",  0x200000, 0x61d08dc4},
    }};
    for (std::size_t index = 0; index < main_data_2m.size(); ++index)
        copy_word32(source, main_data_2m[index], roms.main_data,
                    (index / 2) * 0x400000 + (index & 1) * 2, errors);

    constexpr std::array<rom_spec, 2> main_data_512k{{
        {"epr-18520.4", 0x080000, 0x1d4ec5e8},
        {"epr-18521.5", 0x080000, 0xb8b3781c},
    }};
    for (std::size_t index = 0; index < main_data_512k.size(); ++index)
        copy_word32(source, main_data_512k[index], roms.main_data,
                    0xc00000 + (index & 1) * 2, errors);

    // Three ROM_COPY expansions from 0xc00000 to 0xd/e/f00000.
    for (std::size_t copy_target = 0xd00000; copy_target <= 0xf00000;
         copy_target += 0x100000) {
        std::copy(roms.main_data.begin() + 0xc00000,
                  roms.main_data.begin() + 0xd00000,
                  roms.main_data.begin() +
                      static_cast<std::vector<uint8_t>::difference_type>(
                          copy_target));
    }

    // Coprocessor data socket is empty for vcop2; MAME uses
    // ROMREGION_ERASE00 so the read path sees 0x00 instead of 0xff.
    roms.copro_data.assign(0x800000, 0x00);

    // Polygons: only two 0x200000 word32 ROMs (0x800000 total). The
    // board's polygon-fetch window remains the same; the games just
    // pack less geometry into it.
    roms.polygon_data.assign(0x800000, 0xff);
    copy_word32(source, {"mpr-18513.16", 0x200000, 0x777a3633},
                roms.polygon_data, 0x000000, errors);
    copy_word32(source, {"mpr-18510.20", 0x200000, 0xe83de997},
                roms.polygon_data, 0x000002, errors);

    // Textures: two 0x200000 word32 ROMs (0x400000 total). MAME
    // interleaves them with mpr-18512.25 at lane 0 and mpr-18511.24
    // at lane 2 — opposite of the srallyc layout, so we explicitly
    // pass the per-ROM offsets rather than relying on the srallyc
    // for-each pattern.
    roms.texture_data.assign(0x400000, 0xff);
    copy_word32(source, {"mpr-18512.25", 0x200000, 0xd9bc7e71},
                roms.texture_data, 0x000000, errors);
    copy_word32(source, {"mpr-18511.24", 0x200000, 0xcae77a4f},
                roms.texture_data, 0x000002, errors);

    // Sound CPU: full 0x80000, single word-swapped ROM.
    roms.sound_cpu.assign(0x80000, 0xff);
    copy_word_swapped(source, {"epr-18530.30", 0x80000, 0xac9c8357},
                      roms.sound_cpu, 0, errors);

    // SCSP samples: 4x 0x200000 word-swapped ROMs at 0/0x200000/
    // 0x400000/0x600000. Same total size as srallyc.
    roms.samples.assign(0x800000, 0xff);
    constexpr std::array<rom_spec, 4> samples{{
        {"mpr-18529.31", 0x200000, 0xf76715b1},
        {"mpr-18528.32", 0x200000, 0x287a2f9a},
        {"mpr-18527.36", 0x200000, 0xe6a49314},
        {"mpr-18526.37", 0x200000, 0x6516d9b5},
    }};
    for (std::size_t index = 0; index < samples.size(); ++index)
        copy_word_swapped(source, samples[index], roms.samples,
                          index * 0x200000, errors);

    // TGP lookup tables and 1/x, 1/sqrt(x) tables. Same CRCs and
    // filenames as srallyc — these ROMs are part of the Model 2A-CRX
    // CPU board and are not game-specific.
    roms.copro_tgp_tables.assign(0x40000, 0xff);
    copy_word32(source, {"opr-14742a.45", 0x20000, 0x90c6b117},
                roms.copro_tgp_tables, 0, errors);
    copy_word32(source, {"opr-14743a.46", 0x20000, 0xae7f446b},
                roms.copro_tgp_tables, 2, errors);

    roms.other_data.assign(0x80000, 0xff);
    constexpr std::array<rom_spec, 4> other{{
        {"opr-14744.58", 0x20000, 0x730ea9e0},
        {"opr-14745.59", 0x20000, 0x4c934d96},
        {"opr-14746.62", 0x20000, 0x2a266cbd},
        {"opr-14747.63", 0x20000, 0xa4ad5e19},
    }};
    for (std::size_t index = 0; index < other.size(); ++index)
        copy_word32(source, other[index], roms.other_data,
                    (index / 2) * 0x40000 + (index & 1) * 2, errors);

    // Video board tables — same CRCs and filenames as srallyc; the
    // Sega Model 2A video board carries these regardless of game.
    roms.video_tables.assign(0x200000, 0);
    copy_byte32(source, {"mpr-16310.15", 0x80000, 0xc078a780},
                roms.video_tables, 0, errors);
    copy_byte32(source, {"mpr-16311.16", 0x80000, 0x452a492b},
                roms.video_tables, 1, errors);
    copy_byte32(source, {"mpr-16312.14", 0x80000, 0xa25fef5b},
                roms.video_tables, 2, errors);

    result.error = errors.str();
    return result;
}

model2_rom_load_result load_virtua_cop(const source_reader& source) {
    // Virtua Cop (Revision B) is an original Sega Model 2 light-gun game
    // (Sega Game ID# 833-11127). It shares the TGP CPU board math tables
    // (opr-1474X) with Sega Rally / Virtua Cop 2 but differs from the Model
    // 2A sets in three ways: the i960 data bus is the smaller 0x2000000
    // window, there is no Model 2A video board (mpr-1631X), and sound is the
    // Sega Model 1 board (segam1audio: 68000 + YM3438 + two MultiPCM buses)
    // rather than the SCSP. The MultiPCM regions ride in sound_cpu and the two
    // multipcm_samples buses; the SCSP `samples` region is left empty.
    //
    // MAME reference: src/mame/sega/model2.cpp ROM_START(vcop).
    model2_rom_load_result result;
    result.set = model2_rom_set::virtua_cop;
    model2_roms& roms = result.roms;
    roms.set = model2_rom_set::virtua_cop;
    std::ostringstream errors;

    // i960 program: two word32 pairs. The revision-B program sits at word 0
    // (0x000000 lane 0 / 0x000002 lane 2); the shared revision-A second half
    // sits at word 0x20000 (0x040000 lane 0 / 0x040002 lane 2).
    roms.main_cpu.assign(0x200000, 0xff);
    copy_word32(source, {"epr-17166b.12", 0x020000, 0xa5647c59},
                roms.main_cpu, 0x000000, errors);
    copy_word32(source, {"epr-17167b.13", 0x020000, 0xf5dde26a},
                roms.main_cpu, 0x000002, errors);
    copy_word32(source, {"epr-17160a.14", 0x020000, 0x267f3242},
                roms.main_cpu, 0x040000, errors);
    copy_word32(source, {"epr-17161a.15", 0x020000, 0xf7126876},
                roms.main_cpu, 0x040002, errors);

    // Main data bus: 0x2000000 window (no ROM_COPY expansion, unlike the 2A
    // sets). Two 0x200000 word32 pairs at 0x000000 and 0x400000, then a
    // 0x80000 word32 pair at 0x800000.
    roms.main_data.assign(0x2000000, 0xff);
    copy_word32(source, {"mpr-17164.10", 0x200000, 0xac5fc501},
                roms.main_data, 0x000000, errors);
    copy_word32(source, {"mpr-17165.11", 0x200000, 0x82296d00},
                roms.main_data, 0x000002, errors);
    copy_word32(source, {"mpr-17162.8", 0x200000, 0x60ddd41e},
                roms.main_data, 0x400000, errors);
    copy_word32(source, {"mpr-17163.9", 0x200000, 0x8c1f9dc8},
                roms.main_data, 0x400002, errors);
    copy_word32(source, {"epr-17168a.6", 0x080000, 0x59091a37},
                roms.main_data, 0x800000, errors);
    copy_word32(source, {"epr-17169a.7", 0x080000, 0x0495808d},
                roms.main_data, 0x800002, errors);

    // Coprocessor data socket is empty; the bus masks reads with
    // (size/4 - 1), so the region must stay a non-empty power of two.
    roms.copro_data.assign(0x800000, 0x00);

    // Polygons and textures: one 0x200000 word32 pair each. The region sizes
    // match MAME's ROM_START(vcop) so the geometry unit's modulo addressing
    // (polygon_rom.size()/4, texture_rom.size()/2) wraps identically.
    roms.polygon_data.assign(0x1000000, 0xff);
    copy_word32(source, {"mpr-17159.16", 0x200000, 0xe218727d},
                roms.polygon_data, 0x000000, errors);
    copy_word32(source, {"mpr-17156.20", 0x200000, 0xc4f4aabf},
                roms.polygon_data, 0x000002, errors);

    roms.texture_data.assign(0x1000000, 0xff);
    copy_word32(source, {"mpr-17158.25", 0x200000, 0x1108d1ec},
                roms.texture_data, 0x000000, errors);
    copy_word32(source, {"mpr-17157.24", 0x200000, 0xcf31e33d},
                roms.texture_data, 0x000002, errors);

    // Sega Model 1 sound board. 68000 program in a 0xc0000 region, two
    // 0x20000 word-swapped halves at 0x000000 and 0x020000.
    roms.sound_cpu.assign(0xc0000, 0xff);
    copy_word_swapped(source, {"epr-17170.7", 0x020000, 0x06a38ae2},
                      roms.sound_cpu, 0x000000, errors);
    copy_word_swapped(source, {"epr-17171.8", 0x020000, 0xb5e436f8},
                      roms.sound_cpu, 0x020000, errors);

    // MultiPCM bus 1: two 0x100000 samples at 0x000000 and 0x200000.
    roms.multipcm_samples_1.assign(0x400000, 0x00);
    copy_linear(source, {"mpr-17172.32", 0x100000, 0xab22cac3},
                roms.multipcm_samples_1, 0x000000, errors);
    copy_linear(source, {"mpr-17172.32", 0x100000, 0xab22cac3},
                roms.multipcm_samples_1, 0x100000, errors);
    copy_linear(source, {"mpr-17173.33", 0x100000, 0x3cb4005c},
                roms.multipcm_samples_1, 0x200000, errors);
    copy_linear(source, {"mpr-17173.33", 0x100000, 0x3cb4005c},
                roms.multipcm_samples_1, 0x300000, errors);

    // MultiPCM bus 2: two 0x200000 samples at 0x000000 and 0x200000.
    roms.multipcm_samples_2.assign(0x400000, 0x00);
    copy_linear(source, {"mpr-17174.4", 0x200000, 0xa50369cc},
                roms.multipcm_samples_2, 0x000000, errors);
    copy_linear(source, {"mpr-17175.5", 0x200000, 0x9136d43c},
                roms.multipcm_samples_2, 0x200000, errors);

    // TGP CPU-board math tables — identical ROMs to Sega Rally / Virtua Cop 2.
    roms.copro_tgp_tables.assign(0x40000, 0xff);
    copy_word32(source, {"opr-14742a.45", 0x20000, 0x90c6b117},
                roms.copro_tgp_tables, 0, errors);
    copy_word32(source, {"opr-14743a.46", 0x20000, 0xae7f446b},
                roms.copro_tgp_tables, 2, errors);

    roms.other_data.assign(0x80000, 0xff);
    constexpr std::array<rom_spec, 4> other{{
        {"opr-14744.58", 0x20000, 0x730ea9e0},
        {"opr-14745.59", 0x20000, 0x4c934d96},
        {"opr-14746.62", 0x20000, 0x2a266cbd},
        {"opr-14747.63", 0x20000, 0xa4ad5e19},
    }};
    for (std::size_t index = 0; index < other.size(); ++index)
        copy_word32(source, other[index], roms.other_data,
                    (index / 2) * 0x40000 + (index & 1) * 2, errors);

    // Cabinet I/O board firmware (Sega model1io2, TMPZ84C015). 64 KiB Z80
    // program shared with the i960 through dual-port RAM. This is the extra
    // epr-17181.6 in the archive that the Model 2A sets do not carry.
    roms.io_cpu.assign(0x10000, 0xff);
    copy_linear(source, {"epr-17181.6", 0x10000, 0x1add2b82},
                roms.io_cpu, 0, errors);

    // No Model 2A video board (mpr-1631X), no drive CPU, no comm board.

    result.error = errors.str();
    return result;
}

model2_rom_load_result load_daytona(const source_reader& source,
                                    const fs::path& selected_path) {
    // Daytona USA (Revision A) is an original Sega Model 2 driving game
    // (Sega Game ID# 833-10651 DAYTONA TWIN). Like Virtua Cop it pairs the
    // i960 board with the Sega Model 1 sound board (segam1audio) and an I/O
    // board over dual-port RAM - here the earlier model1io (315-5338A)
    // driving board whose firmware ships in the separate model1io.zip device
    // archive, the same board the Model 1 racers use. The cabinet also
    // carries the SJ25-0207-01 drive-feedback Z80 and the M2COMM link board
    // whose program is the same epr-16726.bin Sega Rally loads.
    //
    // MAME reference: src/mame/sega/model2.cpp ROM_START(daytona).
    model2_rom_load_result result;
    result.set = model2_rom_set::daytona;
    model2_roms& roms = result.roms;
    roms.set = model2_rom_set::daytona;
    std::ostringstream errors;

    // i960 program: a single word32 pair of 0x20000 EPRs.
    roms.main_cpu.assign(0x200000, 0xff);
    copy_word32(source, {"epr-16722a.12", 0x020000, 0x48b94318},
                roms.main_cpu, 0x000000, errors);
    copy_word32(source, {"epr-16723a.13", 0x020000, 0x8af8b32d},
                roms.main_cpu, 0x000002, errors);

    // Main data bus: the 0x2000000 original-Model 2 window. Two 0x200000
    // pairs at 0x000000 and 0x400000, a 0x80000 pair at 0x800000, then
    // MAME's seven ROM_COPY expansions of that last 0x100000 across
    // 0x900000..0xf00000.
    roms.main_data.assign(0x2000000, 0xff);
    copy_word32(source, {"mpr-16528.10", 0x200000, 0x9ce591f6},
                roms.main_data, 0x000000, errors);
    copy_word32(source, {"mpr-16529.11", 0x200000, 0xf7095eaf},
                roms.main_data, 0x000002, errors);
    copy_word32(source, {"mpr-16808.8", 0x200000, 0x44f1f5a0},
                roms.main_data, 0x400000, errors);
    copy_word32(source, {"mpr-16809.9", 0x200000, 0x37a2dd12},
                roms.main_data, 0x400002, errors);
    copy_word32(source, {"epr-16724a.6", 0x080000, 0x469f10fd},
                roms.main_data, 0x800000, errors);
    copy_word32(source, {"epr-16725a.7", 0x080000, 0xba0df8db},
                roms.main_data, 0x800002, errors);
    for (std::size_t copy_target = 0x900000; copy_target <= 0xf00000;
         copy_target += 0x100000) {
        std::copy(roms.main_data.begin() + 0x800000,
                  roms.main_data.begin() + 0x900000,
                  roms.main_data.begin() +
                      static_cast<std::vector<uint8_t>::difference_type>(
                          copy_target));
    }

    // Coprocessor extra data (collision / height map) in the COPRO socket.
    roms.copro_data.assign(0x800000, 0xff);
    copy_word32(source, {"mpr-16537.ic28", 0x200000, 0x36b7c35a},
                roms.copro_data, 0, errors);
    copy_word32(source, {"mpr-16536.ic29", 0x200000, 0x6d6afed9},
                roms.copro_data, 2, errors);

    // Models: four word32 pairs filling the full 0x1000000 region.
    roms.polygon_data.assign(0x1000000, 0xff);
    constexpr std::array<rom_spec, 8> polygons{{
        {"mpr-16523.ic16", 0x200000, 0x2f484d42},
        {"mpr-16518.ic20", 0x200000, 0xdf683bf7},
        {"mpr-16524.ic17", 0x200000, 0x34658bd7},
        {"mpr-16519.ic21", 0x200000, 0xfacd1c81},
        {"mpr-16525.ic18", 0x200000, 0xfb517521},
        {"mpr-16520.ic22", 0x200000, 0xd66bd9bd},
        {"mpr-16772.ic19", 0x200000, 0x770ed912},
        {"mpr-16771.ic23", 0x200000, 0xa2205124},
    }};
    for (std::size_t index = 0; index < polygons.size(); ++index)
        copy_word32(source, polygons[index], roms.polygon_data,
                    (index / 2) * 0x400000 + (index & 1) * 2, errors);

    // Textures: two pairs, the second starting at 0x800000.
    roms.texture_data.assign(0x1000000, 0xff);
    copy_word32(source, {"mpr-16522.25", 0x200000, 0x55d39a57},
                roms.texture_data, 0x000000, errors);
    copy_word32(source, {"mpr-16521.24", 0x200000, 0xaf1934fb},
                roms.texture_data, 0x000002, errors);
    copy_word32(source, {"mpr-16770.27", 0x200000, 0xf9fa7bfb},
                roms.texture_data, 0x800000, errors);
    copy_word32(source, {"mpr-16769.26", 0x200000, 0xe57429e9},
                roms.texture_data, 0x800002, errors);

    // M2COMM communication board program - byte-identical to the ROM the
    // Sega Rally twin cabinets run.
    roms.communication_cpu.assign(0x20000, 0xff);
    copy_linear(source, {"epr-16726.bin", 0x20000, 0xc179b8c7},
                roms.communication_cpu, 0, errors);

    // Sega Model 1 sound board: 68000 program in a 0xc0000 region, two
    // 0x20000 word-swapped halves.
    roms.sound_cpu.assign(0xc0000, 0xff);
    copy_word_swapped(source, {"epr-16720.7", 0x020000, 0x8e73cffd},
                      roms.sound_cpu, 0x000000, errors);
    copy_word_swapped(source, {"epr-16721.8", 0x020000, 0x1bb3b7b7},
                      roms.sound_cpu, 0x020000, errors);

    // MultiPCM bus 1 and 2: two 0x200000 sample ROMs each.
    roms.multipcm_samples_1.assign(0x400000, 0x00);
    copy_linear(source, {"mpr-16491.32", 0x200000, 0x89920903},
                roms.multipcm_samples_1, 0x000000, errors);
    copy_linear(source, {"mpr-16492.33", 0x200000, 0x459e701b},
                roms.multipcm_samples_1, 0x200000, errors);

    roms.multipcm_samples_2.assign(0x400000, 0x00);
    copy_linear(source, {"mpr-16493.4", 0x200000, 0x9990db15},
                roms.multipcm_samples_2, 0x000000, errors);
    copy_linear(source, {"mpr-16494.5", 0x200000, 0x600e1d6c},
                roms.multipcm_samples_2, 0x200000, errors);

    // TGP CPU-board math tables - identical ROMs to the other Model 2 sets.
    roms.copro_tgp_tables.assign(0x40000, 0xff);
    copy_word32(source, {"opr-14742a.45", 0x20000, 0x90c6b117},
                roms.copro_tgp_tables, 0, errors);
    copy_word32(source, {"opr-14743a.46", 0x20000, 0xae7f446b},
                roms.copro_tgp_tables, 2, errors);

    roms.other_data.assign(0x80000, 0xff);
    constexpr std::array<rom_spec, 4> other{{
        {"opr-14744.58", 0x20000, 0x730ea9e0},
        {"opr-14745.59", 0x20000, 0x4c934d96},
        {"opr-14746.62", 0x20000, 0x2a266cbd},
        {"opr-14747.63", 0x20000, 0xa4ad5e19},
    }};
    for (std::size_t index = 0; index < other.size(); ++index)
        copy_word32(source, other[index], roms.other_data,
                    (index / 2) * 0x40000 + (index & 1) * 2, errors);

    // SJ25-0207-01 drive-feedback board program (revision A preferred).
    roms.drive_cpu.assign(0x10000, 0xff);
    {
        std::ostringstream ignored;
        if (!copy_linear(source, {"epr-16488a.ic12", 0x10000, 0x546c5d1a},
                         roms.drive_cpu, 0, ignored))
            copy_linear(source, {"epr-16488.ic12", 0x10000, 0x4f0b8114},
                        roms.drive_cpu, 0, errors);
    }

    // model1io driving I/O board firmware. Not part of the game archive -
    // it is the shared device board - so it is searched in the game archive
    // first (repacked sets) and then in model1io.zip beside it. Any of the
    // three firmware revisions runs the cabinet; prefer the newest.
    roms.io_cpu.assign(0x10000, 0xff);
    {
        constexpr std::array<rom_spec, 3> io_firmware{{
            {"epr-14869c.25", 0x10000, 0x24b68e64},
            {"epr-14869b.25", 0x10000, 0x2d093304},
            {"epr-14869.25", 0x10000, 0x6187cd7a},
        }};
        const fs::path directory = fs::is_directory(selected_path) ?
            selected_path : selected_path.parent_path();
        const source_reader companion(directory / "model1io.zip");
        bool loaded = false;
        for (const rom_spec& spec : io_firmware) {
            std::ostringstream ignored;
            if (copy_linear(source, spec, roms.io_cpu, 0, ignored) ||
                (companion.valid() &&
                 copy_linear(companion, spec, roms.io_cpu, 0, ignored))) {
                loaded = true;
                break;
            }
        }
        if (!loaded)
            errors << "Missing ROM: epr-14869c.25 (model1io.zip I/O board "
                      "firmware, place it beside the game archive)\n";
    }

    // Original Model 2 video board: no mpr-1631X tables.

    result.error = errors.str();
    return result;
}

model2_rom_load_result load_virtua_fighter_2(const source_reader& source) {
    // Virtua Fighter 2 (Version 2.1) is a Model 2A fighter (Sega game#
    // 833-11341): SCSP sound, no drive CPU, no comm board, no I/O board -
    // the two fighter pads read straight off the 315-5296.
    //
    // MAME reference: src/mame/sega/model2.cpp ROM_START(vf2).
    model2_rom_load_result result;
    result.set = model2_rom_set::virtua_fighter_2;
    model2_roms& roms = result.roms;
    roms.set = model2_rom_set::virtua_fighter_2;
    std::ostringstream errors;

    // i960 program: two word32 pairs of 0x20000 EPRs.
    roms.main_cpu.assign(0x200000, 0xff);
    copy_word32(source, {"epr-18385.12", 0x020000, 0x78ed2d41},
                roms.main_cpu, 0x000000, errors);
    copy_word32(source, {"epr-18386.13", 0x020000, 0x3418f428},
                roms.main_cpu, 0x000002, errors);
    copy_word32(source, {"epr-18387.14", 0x020000, 0x124a8453},
                roms.main_cpu, 0x040000, errors);
    copy_word32(source, {"epr-18388.15", 0x020000, 0x8d347980},
                roms.main_cpu, 0x040002, errors);

    // Main data bus: four 0x200000 word32 pairs, no ROM_COPY expansion.
    roms.main_data.assign(0x2400000, 0xff);
    constexpr std::array<rom_spec, 8> main_data{{
        {"mpr-17560.10", 0x200000, 0xd1389864},
        {"mpr-17561.11", 0x200000, 0xb98d0101},
        {"mpr-17558.8",  0x200000, 0x4b15f5a6},
        {"mpr-17559.9",  0x200000, 0xd3264de6},
        {"mpr-17566.6",  0x200000, 0xfb41ef98},
        {"mpr-17567.7",  0x200000, 0xc3396922},
        {"mpr-17564.4",  0x200000, 0xd8062489},
        {"mpr-17565.5",  0x200000, 0x0517c6e9},
    }};
    for (std::size_t index = 0; index < main_data.size(); ++index)
        copy_word32(source, main_data[index], roms.main_data,
                    (index / 2) * 0x400000 + (index & 1) * 2, errors);

    // Coprocessor data socket is empty (ROMREGION_ERASE00).
    roms.copro_data.assign(0x800000, 0x00);

    // Models: three word32 pairs in the 0x2000000 window.
    roms.polygon_data.assign(0x2000000, 0xff);
    constexpr std::array<rom_spec, 6> polygons{{
        {"mpr-17554.16", 0x200000, 0x27896d82},
        {"mpr-17548.20", 0x200000, 0xc95facc2},
        {"mpr-17555.17", 0x200000, 0x4df2810b},
        {"mpr-17549.21", 0x200000, 0xe0bce0e6},
        {"mpr-17556.18", 0x200000, 0x41a47616},
        {"mpr-17550.22", 0x200000, 0xc36ff3f5},
    }};
    for (std::size_t index = 0; index < polygons.size(); ++index)
        copy_word32(source, polygons[index], roms.polygon_data,
                    (index / 2) * 0x400000 + (index & 1) * 2, errors);

    // Textures: two pairs, the second starting at 0x800000 (ERASEFF).
    roms.texture_data.assign(0x1000000, 0xff);
    copy_word32(source, {"mpr-17553.25", 0x200000, 0x5da1c5d3},
                roms.texture_data, 0x000000, errors);
    copy_word32(source, {"mpr-17552.24", 0x200000, 0xe91e7427},
                roms.texture_data, 0x000002, errors);
    copy_word32(source, {"mpr-17547.27", 0x200000, 0xbe940431},
                roms.texture_data, 0x800000, errors);
    copy_word32(source, {"mpr-17546.26", 0x200000, 0x042a194b},
                roms.texture_data, 0x800002, errors);

    // SCSP sound program and samples.
    roms.sound_cpu.assign(0x80000, 0xff);
    copy_word_swapped(source, {"epr-17574.30", 0x80000, 0x4d4c3a55},
                      roms.sound_cpu, 0, errors);

    roms.samples.assign(0x800000, 0xff);
    constexpr std::array<rom_spec, 4> samples{{
        {"mpr-17573.31", 0x200000, 0xe43557fe},
        {"mpr-17572.32", 0x200000, 0x4febecc8},
        {"mpr-17571.36", 0x200000, 0x51caa584},
        {"mpr-17570.37", 0x200000, 0xbccd324b},
    }};
    for (std::size_t index = 0; index < samples.size(); ++index)
        copy_word_swapped(source, samples[index], roms.samples,
                          index * 0x200000, errors);

    // TGP CPU-board math tables and Model 2A video board tables - the same
    // board ROMs every Model 2A set carries.
    roms.copro_tgp_tables.assign(0x40000, 0xff);
    copy_word32(source, {"opr-14742a.45", 0x20000, 0x90c6b117},
                roms.copro_tgp_tables, 0, errors);
    copy_word32(source, {"opr-14743a.46", 0x20000, 0xae7f446b},
                roms.copro_tgp_tables, 2, errors);

    roms.other_data.assign(0x80000, 0xff);
    constexpr std::array<rom_spec, 4> other{{
        {"opr-14744.58", 0x20000, 0x730ea9e0},
        {"opr-14745.59", 0x20000, 0x4c934d96},
        {"opr-14746.62", 0x20000, 0x2a266cbd},
        {"opr-14747.63", 0x20000, 0xa4ad5e19},
    }};
    for (std::size_t index = 0; index < other.size(); ++index)
        copy_word32(source, other[index], roms.other_data,
                    (index / 2) * 0x40000 + (index & 1) * 2, errors);

    roms.video_tables.assign(0x200000, 0);
    copy_byte32(source, {"mpr-16310.15", 0x80000, 0xc078a780},
                roms.video_tables, 0, errors);
    copy_byte32(source, {"mpr-16311.16", 0x80000, 0x452a492b},
                roms.video_tables, 1, errors);
    copy_byte32(source, {"mpr-16312.14", 0x80000, 0xa25fef5b},
                roms.video_tables, 2, errors);

    result.error = errors.str();
    return result;
}

model2_rom_load_result load_manx_tt(const source_reader& source) {
    // Manx TT Superbike DX/Twin (Revision C) is a Model 2A motorbike racer:
    // SCSP sound, throttle/brake/bank on the 315-5296 ADC, an M2COMM link
    // board running EPR-18643, and a factory 93C46 image that selects Twin
    // mode. No drive CPU, no I/O board.
    //
    // MAME reference: src/mame/sega/model2.cpp ROM_START(manxttc).
    model2_rom_load_result result;
    result.set = model2_rom_set::manx_tt;
    model2_roms& roms = result.roms;
    roms.set = model2_rom_set::manx_tt;
    std::ostringstream errors;

    roms.main_cpu.assign(0x200000, 0xff);
    copy_word32(source, {"epr-18822c.12", 0x020000, 0xc7b3e45a},
                roms.main_cpu, 0x000000, errors);
    copy_word32(source, {"epr-18823c.13", 0x020000, 0x6b0c1dfb},
                roms.main_cpu, 0x000002, errors);
    copy_word32(source, {"epr-18824c.14", 0x020000, 0x352bb817},
                roms.main_cpu, 0x040000, errors);
    copy_word32(source, {"epr-18825c.15", 0x020000, 0xf88b036c},
                roms.main_cpu, 0x040002, errors);

    // Main data: three 0x200000 pairs, a 0x80000 pair at 0xc00000, then
    // MAME's three ROM_COPY expansions of that last 0x100000.
    roms.main_data.assign(0x2400000, 0xff);
    constexpr std::array<rom_spec, 6> main_data{{
        {"mpr-18751.10", 0x200000, 0x773ad43d},
        {"mpr-18752.11", 0x200000, 0x4da3719e},
        {"mpr-18749.8",  0x200000, 0xc3fe0eea},
        {"mpr-18750.9",  0x200000, 0x40b55494},
        {"mpr-18747.6",  0x200000, 0xa65ec1e8},
        {"mpr-18748.7",  0x200000, 0x375e3748},
    }};
    for (std::size_t index = 0; index < main_data.size(); ++index)
        copy_word32(source, main_data[index], roms.main_data,
                    (index / 2) * 0x400000 + (index & 1) * 2, errors);
    copy_word32(source, {"epr-18862.4", 0x080000, 0x9adc3a30},
                roms.main_data, 0xc00000, errors);
    copy_word32(source, {"epr-18863.5", 0x080000, 0x603742e9},
                roms.main_data, 0xc00002, errors);
    for (std::size_t copy_target = 0xd00000; copy_target <= 0xf00000;
         copy_target += 0x100000) {
        std::copy(roms.main_data.begin() + 0xc00000,
                  roms.main_data.begin() + 0xd00000,
                  roms.main_data.begin() +
                      static_cast<std::vector<uint8_t>::difference_type>(
                          copy_target));
    }

    roms.copro_data.assign(0x800000, 0xff);
    copy_word32(source, {"mpr-18761.28", 0x200000, 0x4e39ec05},
                roms.copro_data, 0, errors);
    copy_word32(source, {"mpr-18762.29", 0x200000, 0x4ab165d8},
                roms.copro_data, 2, errors);

    roms.polygon_data.assign(0x2000000, 0xff);
    constexpr std::array<rom_spec, 6> polygons{{
        {"mpr-18753.16", 0x200000, 0x33ddaa0d},
        {"mpr-18756.20", 0x200000, 0x28713617},
        {"mpr-18754.17", 0x200000, 0x09aabde5},
        {"mpr-18757.21", 0x200000, 0x25fc92e9},
        {"mpr-18755.18", 0x200000, 0xbf094d9e},
        {"mpr-18758.22", 0x200000, 0x1b5473d0},
    }};
    for (std::size_t index = 0; index < polygons.size(); ++index)
        copy_word32(source, polygons[index], roms.polygon_data,
                    (index / 2) * 0x400000 + (index & 1) * 2, errors);

    roms.texture_data.assign(0x1000000, 0xff);
    copy_word32(source, {"mpr-18760.25", 0x200000, 0x4e3a4a89},
                roms.texture_data, 0x000000, errors);
    copy_word32(source, {"mpr-18759.24", 0x200000, 0x278d8742},
                roms.texture_data, 0x000002, errors);

    roms.sound_cpu.assign(0x80000, 0xff);
    copy_word_swapped(source, {"epr-18826.30", 0x40000, 0xed9fe4c1},
                      roms.sound_cpu, 0, errors);

    roms.samples.assign(0x800000, 0xff);
    constexpr std::array<rom_spec, 4> samples{{
        {"mpr-18827.31", 0x200000, 0x58d78ca1},
        {"mpr-18764.32", 0x200000, 0x0dc6a860},
        {"mpr-18765.36", 0x200000, 0xca4a803c},
        {"mpr-18766.37", 0x200000, 0xe41892ea},
    }};
    for (std::size_t index = 0; index < samples.size(); ++index)
        copy_word_swapped(source, samples[index], roms.samples,
                          index * 0x200000, errors);

    // M2COMM link board program (EPR-18643 revision of the ring firmware).
    roms.communication_cpu.assign(0x20000, 0xff);
    copy_word_swapped(source, {"epr-18643.7", 0x20000, 0x7166fca7},
                      roms.communication_cpu, 0, errors);

    // Factory 93C46 configured for Twin mode - the set ships it.
    {
        std::ostringstream ignored;
        load_checked(source, {"manxttc_twin_nvran", 0x80, 0xf3be38fe},
                     roms.default_eeprom, ignored, false);
    }

    roms.copro_tgp_tables.assign(0x40000, 0xff);
    copy_word32(source, {"opr-14742a.45", 0x20000, 0x90c6b117},
                roms.copro_tgp_tables, 0, errors);
    copy_word32(source, {"opr-14743a.46", 0x20000, 0xae7f446b},
                roms.copro_tgp_tables, 2, errors);

    roms.other_data.assign(0x80000, 0xff);
    constexpr std::array<rom_spec, 4> other{{
        {"opr-14744.58", 0x20000, 0x730ea9e0},
        {"opr-14745.59", 0x20000, 0x4c934d96},
        {"opr-14746.62", 0x20000, 0x2a266cbd},
        {"opr-14747.63", 0x20000, 0xa4ad5e19},
    }};
    for (std::size_t index = 0; index < other.size(); ++index)
        copy_word32(source, other[index], roms.other_data,
                    (index / 2) * 0x40000 + (index & 1) * 2, errors);

    roms.video_tables.assign(0x200000, 0);
    copy_byte32(source, {"mpr-16310.15", 0x80000, 0xc078a780},
                roms.video_tables, 0, errors);
    copy_byte32(source, {"mpr-16311.16", 0x80000, 0x452a492b},
                roms.video_tables, 1, errors);
    copy_byte32(source, {"mpr-16312.14", 0x80000, 0xa25fef5b},
                roms.video_tables, 2, errors);

    result.error = errors.str();
    return result;
}

model2_rom_load_result load_motor_raid(const source_reader& source) {
    // Motor Raid (Sega game ID# 833-13232 MOTOR RAID TWIN) is a Model 2A
    // combat bike racer on the same cabinet hardware as Manx TT: SCSP
    // sound, throttle/brake/bank ADC, and the two IN1 buttons are Punch
    // and Kick rather than a gear shifter.
    //
    // MAME reference: src/mame/sega/model2.cpp ROM_START(motoraid).
    model2_rom_load_result result;
    result.set = model2_rom_set::motor_raid;
    model2_roms& roms = result.roms;
    roms.set = model2_rom_set::motor_raid;
    std::ostringstream errors;

    roms.main_cpu.assign(0x200000, 0xff);
    copy_word32(source, {"epr-20007.12", 0x080000, 0xf040c108},
                roms.main_cpu, 0x000000, errors);
    copy_word32(source, {"epr-20008.13", 0x080000, 0x78976e1a},
                roms.main_cpu, 0x000002, errors);

    // Main data: three 0x400000 pairs, a 0x80000 pair at 0x1800000, then
    // MAME's seven ROM_COPY expansions of that last 0x100000.
    roms.main_data.assign(0x2400000, 0xff);
    constexpr std::array<rom_spec, 6> main_data{{
        {"mpr-20019.10", 0x400000, 0x49053727},
        {"mpr-20020.11", 0x400000, 0xcc5ddb15},
        {"mpr-20017.8",  0x400000, 0x4e206acd},
        {"mpr-20018.9",  0x400000, 0xe7ed0e85},
        {"mpr-20015.6",  0x400000, 0x23427339},
        {"mpr-20016.7",  0x400000, 0xc99a83f4},
    }};
    for (std::size_t index = 0; index < main_data.size(); ++index)
        copy_word32(source, main_data[index], roms.main_data,
                    (index / 2) * 0x800000 + (index & 1) * 2, errors);
    copy_word32(source, {"epr-20013.4", 0x080000, 0xa4478f52},
                roms.main_data, 0x1800000, errors);
    copy_word32(source, {"epr-20014.5", 0x080000, 0x1aa541be},
                roms.main_data, 0x1800002, errors);
    for (std::size_t copy_target = 0x1900000; copy_target <= 0x1f00000;
         copy_target += 0x100000) {
        std::copy(roms.main_data.begin() + 0x1800000,
                  roms.main_data.begin() + 0x1900000,
                  roms.main_data.begin() +
                      static_cast<std::vector<uint8_t>::difference_type>(
                          copy_target));
    }

    roms.polygon_data.assign(0x2000000, 0xff);
    constexpr std::array<rom_spec, 6> polygons{{
        {"mpr-20023.16", 0x400000, 0x016be8d6},
        {"mpr-20026.20", 0x400000, 0x20044a30},
        {"mpr-20024.17", 0x400000, 0x62fd2d5b},
        {"mpr-20027.21", 0x400000, 0xb2504ea6},
        {"mpr-20025.18", 0x400000, 0xd4ecd0be},
        {"mpr-20028.22", 0x400000, 0x3147e0e1},
    }};
    for (std::size_t index = 0; index < polygons.size(); ++index)
        copy_word32(source, polygons[index], roms.polygon_data,
                    (index / 2) * 0x800000 + (index & 1) * 2, errors);

    roms.copro_data.assign(0x800000, 0xff);
    copy_word32(source, {"epr-20011.28", 0x100000, 0x794c026c},
                roms.copro_data, 0, errors);
    copy_word32(source, {"epr-20012.29", 0x100000, 0xf53db4e3},
                roms.copro_data, 2, errors);

    roms.texture_data.assign(0x1000000, 0xff);
    copy_word32(source, {"mpr-20022.25", 0x400000, 0x9e47b3c2},
                roms.texture_data, 0x000000, errors);
    copy_word32(source, {"mpr-20021.24", 0x400000, 0x3cbf36cb},
                roms.texture_data, 0x000002, errors);

    roms.sound_cpu.assign(0x80000, 0xff);
    copy_word_swapped(source, {"epr-20029.30", 0x80000, 0x927d31b9},
                      roms.sound_cpu, 0, errors);

    roms.samples.assign(0x800000, 0xff);
    constexpr std::array<rom_spec, 4> samples{{
        {"mpr-20030.31", 0x200000, 0xb70ab686},
        {"mpr-20031.32", 0x200000, 0x84da70e4},
        {"mpr-20032.36", 0x200000, 0x15516d35},
        {"mpr-20033.37", 0x200000, 0x8c8ed187},
    }};
    for (std::size_t index = 0; index < samples.size(); ++index)
        copy_word_swapped(source, samples[index], roms.samples,
                          index * 0x200000, errors);

    roms.copro_tgp_tables.assign(0x40000, 0xff);
    copy_word32(source, {"opr-14742a.45", 0x20000, 0x90c6b117},
                roms.copro_tgp_tables, 0, errors);
    copy_word32(source, {"opr-14743a.46", 0x20000, 0xae7f446b},
                roms.copro_tgp_tables, 2, errors);

    roms.other_data.assign(0x80000, 0xff);
    constexpr std::array<rom_spec, 4> other{{
        {"opr-14744.58", 0x20000, 0x730ea9e0},
        {"opr-14745.59", 0x20000, 0x4c934d96},
        {"opr-14746.62", 0x20000, 0x2a266cbd},
        {"opr-14747.63", 0x20000, 0xa4ad5e19},
    }};
    for (std::size_t index = 0; index < other.size(); ++index)
        copy_word32(source, other[index], roms.other_data,
                    (index / 2) * 0x40000 + (index & 1) * 2, errors);

    roms.video_tables.assign(0x200000, 0);
    copy_byte32(source, {"mpr-16310.15", 0x80000, 0xc078a780},
                roms.video_tables, 0, errors);
    copy_byte32(source, {"mpr-16311.16", 0x80000, 0x452a492b},
                roms.video_tables, 1, errors);
    copy_byte32(source, {"mpr-16312.14", 0x80000, 0xa25fef5b},
                roms.video_tables, 2, errors);

    result.error = errors.str();
    return result;
}
} // namespace

bool model2_roms::complete() const {
    return model2_rom_loader::is_complete(set, *this);
}

model2_rom_set model2_rom_loader::identify_set(const std::string& path) {
    const source_reader source(path);
    if (source.contains("epr-17888c.12") &&
        source.contains("epr-17889c.13"))
        return model2_rom_set::sega_rally_revision_c;
    if (source.contains("epr-18524.12") &&
        source.contains("epr-18525.13"))
        return model2_rom_set::virtua_cop_2;
    if (source.contains("epr-17166b.12") &&
        source.contains("epr-17167b.13"))
        return model2_rom_set::virtua_cop;
    if (source.contains("epr-16722a.12") &&
        source.contains("epr-16723a.13"))
        return model2_rom_set::daytona;
    if (source.contains("epr-18385.12") &&
        source.contains("epr-18386.13"))
        return model2_rom_set::virtua_fighter_2;
    if (source.contains("epr-18822c.12") &&
        source.contains("epr-18823c.13"))
        return model2_rom_set::manx_tt;
    if (source.contains("epr-20007.12") &&
        source.contains("epr-20008.13"))
        return model2_rom_set::motor_raid;
    return model2_rom_set::unknown;
}

const char* model2_rom_loader::set_short_name(model2_rom_set set) {
    switch (set) {
    case model2_rom_set::sega_rally_revision_c: return "srallyc";
    case model2_rom_set::virtua_cop_2: return "vcop2";
    case model2_rom_set::virtua_cop: return "vcop";
    case model2_rom_set::daytona: return "daytona";
    case model2_rom_set::virtua_fighter_2: return "vf2";
    case model2_rom_set::manx_tt: return "manxttc";
    case model2_rom_set::motor_raid: return "motoraid";
    case model2_rom_set::unknown: return "";
    }
    return "";
}

const char* model2_rom_loader::set_display_name(model2_rom_set set) {
    switch (set) {
    case model2_rom_set::sega_rally_revision_c:
        return "Sega Rally Championship (Revision C)";
    case model2_rom_set::virtua_cop_2:
        return "Virtua Cop 2 (Model 2A)";
    case model2_rom_set::virtua_cop:
        return "Virtua Cop (Model 2)";
    case model2_rom_set::daytona:
        return "Daytona USA (Revision A)";
    case model2_rom_set::virtua_fighter_2:
        return "Virtua Fighter 2 (Version 2.1)";
    case model2_rom_set::manx_tt:
        return "Manx TT Superbike - Twin (Revision C)";
    case model2_rom_set::motor_raid:
        return "Motor Raid - Twin";
    case model2_rom_set::unknown:
        return "Unsupported Sega Model 2 ROM set";
    }
    return "Unsupported Sega Model 2 ROM set";
}

model2_game_profile model2_rom_loader::profile_for(model2_rom_set set) {
    using sound = model2_game_profile::sound_board;
    using io = model2_game_profile::io_kind;
    switch (set) {
    case model2_rom_set::sega_rally_revision_c:
        // Model 2A-CRX driving cabinet: SCSP sound, 315-5296 wheel/pedal I/O.
        return {sound::scsp, io::crx_wheel, false, "srallyc"};
    case model2_rom_set::virtua_cop_2:
        // Model 2A-CRX light-gun cabinet: SCSP sound, 315-5296 serial gun mux.
        return {sound::scsp, io::crx_gun, true, "vcop2"};
    case model2_rom_set::virtua_cop:
        // Original Model 2 light-gun cabinet: Sega Model 1 sound board and a
        // model1io2 I/O board over dual-port RAM.
        return {sound::segam1audio, io::model1io2_dpram, true, "vcop", true};
    case model2_rom_set::daytona:
        // Original Model 2 driving cabinet: Sega Model 1 sound board and the
        // model1io (315-5338A) driving I/O board over dual-port RAM.
        return {sound::segam1audio, io::model1io_dpram, false, "daytona",
                true};
    case model2_rom_set::virtua_fighter_2:
        // Model 2A-CRX fighter: SCSP sound, two digital pads on the
        // 315-5296.
        return {sound::scsp, io::crx_fighter, false, "vf2"};
    case model2_rom_set::manx_tt:
        // Model 2A-CRX motorbike cabinet: SCSP sound, throttle/brake/bank
        // on the 315-5296 ADC.
        return {sound::scsp, io::crx_bike, false, "manxttc"};
    case model2_rom_set::motor_raid:
        // Same motorbike cabinet; the two IN1 buttons are Punch and Kick.
        return {sound::scsp, io::crx_bike, false, "motoraid"};
    case model2_rom_set::unknown:
        break;
    }
    return {};
}

bool model2_rom_loader::is_complete(model2_rom_set set,
                                    const model2_roms& roms) {
    // Per-set expected sizes. Anything marked optional is required only
    // when the corresponding board is present on this cabinet; the
    // light-gun vcop2 has none of these and the loader leaves them
    // empty. Unknown defaults to "all common regions populated" so
    // partial loads still report failure.
    struct layout {
        std::size_t main_cpu;
        std::size_t main_data;
        std::size_t polygon_data;
        std::size_t texture_data;
        std::size_t sound_cpu;
        // SCSP `samples` region (Model 2A sets) and, alternatively, the size
        // of each MultiPCM bus (original Model 2 sound board). Exactly one is
        // non-zero per set; the other region is left empty and unchecked.
        std::size_t samples;
        std::size_t multipcm;
        std::size_t copro_tgp_tables;
        std::size_t other_data;
        std::size_t video_tables;
        std::size_t io_cpu;
        bool require_drive_cpu;
        bool require_comm_cpu;
    };
    const layout* expected = nullptr;
    switch (set) {
    case model2_rom_set::sega_rally_revision_c: {
        static constexpr layout srallyc{
            0x200000, 0x2400000, 0x2000000, 0x1000000, 0x80000,
            0x800000, 0, 0x40000, 0x80000, 0x200000, 0, true, true,
        };
        expected = &srallyc;
        break;
    }
    case model2_rom_set::virtua_cop_2: {
        static constexpr layout vcop2{
            0x200000, 0x2400000, 0x800000, 0x400000, 0x80000,
            0x800000, 0, 0x40000, 0x80000, 0x200000, 0, false, false,
        };
        expected = &vcop2;
        break;
    }
    case model2_rom_set::virtua_cop: {
        static constexpr layout vcop{
            0x200000, 0x2000000, 0x1000000, 0x1000000, 0xc0000,
            0, 0x400000, 0x40000, 0x80000, 0, 0x10000, false, false,
        };
        expected = &vcop;
        break;
    }
    case model2_rom_set::daytona: {
        static constexpr layout daytona{
            0x200000, 0x2000000, 0x1000000, 0x1000000, 0xc0000,
            0, 0x400000, 0x40000, 0x80000, 0, 0x10000, true, true,
        };
        expected = &daytona;
        break;
    }
    case model2_rom_set::virtua_fighter_2: {
        static constexpr layout vf2{
            0x200000, 0x2400000, 0x2000000, 0x1000000, 0x80000,
            0x800000, 0, 0x40000, 0x80000, 0x200000, 0, false, false,
        };
        expected = &vf2;
        break;
    }
    case model2_rom_set::manx_tt: {
        static constexpr layout manxtt{
            0x200000, 0x2400000, 0x2000000, 0x1000000, 0x80000,
            0x800000, 0, 0x40000, 0x80000, 0x200000, 0, false, true,
        };
        expected = &manxtt;
        break;
    }
    case model2_rom_set::motor_raid: {
        static constexpr layout motoraid{
            0x200000, 0x2400000, 0x2000000, 0x1000000, 0x80000,
            0x800000, 0, 0x40000, 0x80000, 0x200000, 0, false, false,
        };
        expected = &motoraid;
        break;
    }
    case model2_rom_set::unknown:
    default:
        return false;
    }
    if (roms.main_cpu.size() != expected->main_cpu) return false;
    if (roms.main_data.size() != expected->main_data) return false;
    if (roms.polygon_data.size() != expected->polygon_data) return false;
    if (roms.texture_data.size() != expected->texture_data) return false;
    if (roms.sound_cpu.size() != expected->sound_cpu) return false;
    if (expected->samples && roms.samples.size() != expected->samples)
        return false;
    if (expected->multipcm &&
        (roms.multipcm_samples_1.size() != expected->multipcm ||
         roms.multipcm_samples_2.size() != expected->multipcm))
        return false;
    if (roms.copro_tgp_tables.size() != expected->copro_tgp_tables)
        return false;
    if (roms.other_data.size() != expected->other_data) return false;
    if (expected->video_tables &&
        roms.video_tables.size() != expected->video_tables)
        return false;
    if (expected->io_cpu && roms.io_cpu.size() != expected->io_cpu)
        return false;
    if (expected->require_drive_cpu &&
        roms.drive_cpu.size() != 0x10000) return false;
    if (expected->require_comm_cpu &&
        roms.communication_cpu.size() != 0x20000) return false;
    return true;
}

model2_rom_load_result model2_rom_loader::load(const std::string& path) {
    const source_reader source(path);
    switch (identify_set(path)) {
    case model2_rom_set::sega_rally_revision_c:
        return load_sega_rally(source, fs::path(path));
    case model2_rom_set::virtua_cop_2:
        return load_virtua_cop_2(source);
    case model2_rom_set::virtua_cop:
        return load_virtua_cop(source);
    case model2_rom_set::daytona:
        return load_daytona(source, fs::path(path));
    case model2_rom_set::virtua_fighter_2:
        return load_virtua_fighter_2(source);
    case model2_rom_set::manx_tt:
        return load_manx_tt(source);
    case model2_rom_set::motor_raid:
        return load_motor_raid(source);
    case model2_rom_set::unknown:
        return {model2_rom_set::unknown, {},
                "Unsupported Sega Model 2 ROM set: " + path + "\n"};
    }
    return {model2_rom_set::unknown, {},
            "Unsupported Sega Model 2 ROM set: " + path + "\n"};
}
