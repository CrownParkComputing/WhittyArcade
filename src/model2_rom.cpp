#include "model2_rom.h"

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
    copy_linear(source, {"mpr-17173.33", 0x100000, 0x3cb4005c},
                roms.multipcm_samples_1, 0x200000, errors);

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
    return model2_rom_set::unknown;
}

const char* model2_rom_loader::set_short_name(model2_rom_set set) {
    switch (set) {
    case model2_rom_set::sega_rally_revision_c: return "srallyc";
    case model2_rom_set::virtua_cop_2: return "vcop2";
    case model2_rom_set::virtua_cop: return "vcop";
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
        return {sound::segam1audio, io::model1io2_dpram, true, "vcop"};
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
    case model2_rom_set::unknown:
        return {model2_rom_set::unknown, {},
                "Unsupported Sega Model 2 ROM set: " + path + "\n"};
    }
    return {model2_rom_set::unknown, {},
            "Unsupported Sega Model 2 ROM set: " + path + "\n"};
}
