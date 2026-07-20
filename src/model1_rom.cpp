#include "model1_rom.h"

#include <minizip/unzip.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <sstream>

namespace fs = std::filesystem;

namespace {
constexpr std::size_t main_cpu_size = 0x2000000;
constexpr std::size_t sound_cpu_size = 0x0c0000;
constexpr std::size_t polygon_data_size = 0x1000000;
constexpr std::size_t copro_data_size = 0x200000;
constexpr std::size_t lookup_table_size = 0x0e0000;

struct rom_spec {
    const char* name;
    std::size_t size;
    uint32_t crc;
};

struct source_reader {
    explicit source_reader(fs::path source_path)
        : sources{std::move(source_path)} {}
    source_reader(fs::path source_path, std::vector<fs::path> companions)
        : sources{std::move(source_path)} {
        for (fs::path& companion : companions) {
            std::error_code error;
            if (fs::exists(companion, error))
                sources.push_back(std::move(companion));
        }
    }

    std::vector<fs::path> sources;

    static std::string lower(std::string value) {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char character) {
                           return static_cast<char>(std::tolower(character));
                       });
        return value;
    }

    bool contains(const std::string& wanted) const {
        std::vector<uint8_t> ignored;
        return read(wanted, ignored, false);
    }

    bool read(const std::string& wanted, std::vector<uint8_t>& output,
              bool read_contents = true) const {
        output.clear();
        for (const fs::path& source : sources) {
            if (read_one(source, wanted, output, read_contents)) return true;
        }
        return false;
    }

private:
    static bool read_one(const fs::path& source, const std::string& wanted,
                         std::vector<uint8_t>& output, bool read_contents) {
        output.clear();
        if (lower(source.extension().string()) != ".zip") {
            std::error_code error;
            if (!fs::is_directory(source, error)) return false;
            const std::string wanted_lower = lower(wanted);
            for (fs::recursive_directory_iterator iterator(
                     source, fs::directory_options::skip_permission_denied,
                     error), end;
                 !error && iterator != end; iterator.increment(error)) {
                std::error_code type_error;
                if (!iterator->is_regular_file(type_error) ||
                    lower(iterator->path().filename().string()) != wanted_lower)
                    continue;
                if (!read_contents) return true;
                std::FILE* file = std::fopen(iterator->path().string().c_str(), "rb");
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
                        static_cast<ZPOS64_T>(std::numeric_limits<std::size_t>::max()) ||
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
};

std::vector<fs::path> companion_sources(const fs::path& selected,
                                        model1_rom_set set) {
    if (set != model1_rom_set::virtua_formula) return {};
    std::error_code error;
    const fs::path directory = selected.parent_path();
    std::vector<fs::path> result;
    if (!fs::is_directory(directory, error)) return result;
    for (fs::directory_iterator iterator(directory, error), end;
         !error && iterator != end; iterator.increment(error)) {
        const std::string filename =
            source_reader::lower(iterator->path().filename().string());
        if (filename == "vr.zip" || filename == "vr")
            result.push_back(iterator->path());
    }
    return result;
}

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

bool load_alias(const source_reader& source,
                std::initializer_list<rom_spec> aliases,
                std::vector<uint8_t>& output, std::ostringstream& errors,
                bool required = true) {
    for (const rom_spec& spec : aliases) {
        if (!source.contains(spec.name)) continue;
        return load_checked(source, spec, output, errors, required);
    }
    if (required && aliases.size() != 0)
        errors << "Missing ROM: " << aliases.begin()->name << '\n';
    return false;
}

bool copy_linear(const source_reader& source, const rom_spec& spec,
                 std::vector<uint8_t>& destination, std::size_t offset,
                 std::ostringstream& errors, bool required = true) {
    std::vector<uint8_t> file;
    if (!load_checked(source, spec, file, errors, required)) return false;
    if (offset > destination.size() || file.size() > destination.size() - offset) {
        errors << "ROM region overflow for " << spec.name << '\n';
        return false;
    }
    std::copy(file.begin(), file.end(), destination.begin() +
              static_cast<std::vector<uint8_t>::difference_type>(offset));
    return true;
}

bool copy_interleaved_byte(const source_reader& source, const rom_spec& spec,
                           std::vector<uint8_t>& destination,
                           std::size_t offset, std::size_t stride,
                           std::ostringstream& errors) {
    std::vector<uint8_t> file;
    if (!load_checked(source, spec, file, errors)) return false;
    if (file.empty() || offset >= destination.size() ||
        (file.size() - 1) * stride > destination.size() - 1 - offset) {
        errors << "ROM region overflow for " << spec.name << '\n';
        return false;
    }
    for (std::size_t index = 0; index < file.size(); ++index)
        destination[offset + index * stride] = file[index];
    return true;
}

bool copy_interleaved_word(const source_reader& source, const rom_spec& spec,
                           std::vector<uint8_t>& destination,
                           std::size_t offset, std::ostringstream& errors,
                           std::initializer_list<rom_spec> aliases = {}) {
    std::vector<uint8_t> file;
    const bool loaded = aliases.size() == 0 ?
        load_checked(source, spec, file, errors) :
        load_alias(source, aliases, file, errors);
    if (!loaded) return false;
    if ((file.size() & 1) != 0 || file.empty() || offset + 1 >= destination.size() ||
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

model1_rom_load_result load_virtua_formula(const source_reader& source) {
    model1_rom_load_result result;
    result.set = model1_rom_set::virtua_formula;
    model1_roms& roms = result.roms;
    std::ostringstream errors;

    roms.main_cpu.assign(main_cpu_size, 0xff);
    copy_interleaved_byte(source, {"epr-15638.14", 0x80000, 0xb9db21a2},
                          roms.main_cpu, 0x200000, 2, errors);
    copy_interleaved_byte(source, {"epr-15639.15", 0x80000, 0x4c3796f5},
                          roms.main_cpu, 0x200001, 2, errors);
    copy_linear(source, {"epr-15623.4", 0x20000, 0x155fa5be},
                roms.main_cpu, 0xfc0000, errors);
    copy_linear(source, {"epr-15622.5", 0x20000, 0x18007f6f},
                roms.main_cpu, 0xfe0000, errors);

    constexpr std::array<rom_spec, 8> rom_o{{
        {"epr-15641.6", 0x80000, 0xbf01e4d5},
        {"epr-15640.7", 0x80000, 0x3e6d83dc},
        {"mpr-14884.8", 0x80000, 0x6cf9c026},
        {"mpr-14885.9", 0x80000, 0xf65c9262},
        {"mpr-14886.10", 0x80000, 0x92868734},
        {"mpr-14887.11", 0x80000, 0x10c7c636},
        {"mpr-14888.12", 0x80000, 0x04bfdc5b},
        {"mpr-14889.13", 0x80000, 0xc49f0486},
    }};
    for (std::size_t index = 0; index < rom_o.size(); ++index) {
        const std::size_t bank = index / 2;
        const std::size_t lane = index & 1;
        copy_interleaved_byte(source, rom_o[index], roms.main_cpu,
                              0x1000000 + bank * 0x100000 + lane, 2, errors);
    }

    roms.sound_cpu.assign(sound_cpu_size, 0);
    std::vector<uint8_t> sound;
    if (load_checked(source, {"epr-14870a.7", 0x20000, 0x919d9b75},
                     sound, errors)) {
        for (std::size_t index = 0; index < sound.size(); index += 2) {
            roms.sound_cpu[index] = sound[index + 1];
            roms.sound_cpu[index + 1] = sound[index];
        }
    }

    roms.samples_1.assign(0x400000, 0);
    roms.samples_2.assign(0x400000, 0);
    copy_linear(source, {"mpr-14873.32", 0x200000, 0xb1965190},
                roms.samples_1, 0, errors);
    copy_linear(source, {"mpr-14876.4", 0x200000, 0xba6b2327},
                roms.samples_2, 0, errors);

    roms.polygon_data.assign(polygon_data_size, 0);
    constexpr std::array<rom_spec, 7> polygon{{
        {"mpr-14890.26", 0x200000, 0xdcbe006b},
        {"mpr-14891.27", 0x200000, 0x25832b38},
        {"mpr-14892.28", 0x200000, 0x5136f3ba},
        {"mpr-14893.29", 0x200000, 0x1c531ada},
        {"mpr-14894.30", 0x200000, 0x830a71bc},
        {"mpr-14895.31", 0x200000, 0xaf027ac5},
        {"mpr-14896.32", 0x200000, 0x382091dc},
    }};
    for (std::size_t index = 0; index < polygon.size(); ++index)
        copy_interleaved_word(source, polygon[index], roms.polygon_data,
                              (index / 2) * 0x400000 + (index & 1) * 2, errors);
    const rom_spec final_polygon{"mpr-14897.33", 0x200000, 0x74873195};
    copy_interleaved_word(
        source, final_polygon, roms.polygon_data, 0xc00002, errors,
        {{"mpr-14897.33", 0x200000, 0x74873195},
         {"mpr-14879.33", 0x200000, 0x74873195}});

    roms.copro_data.assign(copro_data_size, 0);
    constexpr std::array<rom_spec, 4> copro{{
        {"mpr-14898.39", 0x80000, 0x61da2bb6},
        {"mpr-14899.40", 0x80000, 0x2cd58bee},
        {"mpr-14900.41", 0x80000, 0xaa7c017d},
        {"mpr-14901.42", 0x80000, 0x175b7a9a},
    }};
    for (std::size_t lane = 0; lane < copro.size(); ++lane)
        copy_interleaved_byte(source, copro[lane], roms.copro_data, lane, 4,
                              errors);

    roms.lookup_tables.assign(lookup_table_size, 0);
    copy_interleaved_word(source, {"opr14742.bin", 0x20000, 0x446a1085},
                          roms.lookup_tables, 0, errors);
    copy_interleaved_word(source, {"opr14743.bin", 0x20000, 0xe8953554},
                          roms.lookup_tables, 2, errors);
    constexpr std::array<rom_spec, 5> lookup{{
        {"opr14744.bin", 0x20000, 0x730ea9e0},
        {"opr14745.bin", 0x20000, 0x4c934d96},
        {"opr14746.bin", 0x20000, 0x2a266cbd},
        {"opr14747.bin", 0x20000, 0xa4ad5e19},
        {"opr14748.bin", 0x20000, 0x4a532cb8},
    }};
    for (std::size_t index = 0; index < lookup.size(); ++index)
        copy_linear(source, lookup[index], roms.lookup_tables,
                    0x40000 + index * 0x20000, errors);

    load_checked(source, {"epr-15624.17", 0x20000, 0x9b3ba315},
                 roms.communication_cpu, errors);
    load_alias(source,
               {{"epr-14869.25", 0x10000, 0x6187cd7a},
                {"epr-14869b.25", 0x10000, 0x2d093304}},
               roms.io_cpu, errors, false);
    load_checked(source, {"vr_defaults.nv", 0x100, 0x5ccdc835},
                 roms.default_nvram, errors, false);
    load_checked(source, {"vr-tgp.bin", 0x2000, 0x3de33c7f},
                 roms.legacy_tgp, errors, false);

    result.error = errors.str();
    return result;
}

model1_rom_load_result load_virtua_fighter(const source_reader& source) {
    model1_rom_load_result result;
    result.set = model1_rom_set::virtua_fighter;
    model1_roms& roms = result.roms;
    std::ostringstream errors;

    roms.main_cpu.assign(main_cpu_size, 0xff);
    copy_interleaved_byte(source, {"epr-16082.14", 0x80000, 0xb23f22ee},
                          roms.main_cpu, 0x200000, 2, errors);
    copy_interleaved_byte(source, {"epr-16083.15", 0x80000, 0xd12c77f8},
                          roms.main_cpu, 0x200001, 2, errors);
    copy_linear(source, {"epr-16080.4", 0x20000, 0x3662e1a5},
                roms.main_cpu, 0xfc0000, errors);
    copy_linear(source, {"epr-16081.5", 0x20000, 0x6dec06ce},
                roms.main_cpu, 0xfe0000, errors);

    constexpr std::array<rom_spec, 8> rom_o{{
        {"mpr-16084.6", 0x80000, 0x483f453b},
        {"mpr-16085.7", 0x80000, 0x5fa01277},
        {"mpr-16086.8", 0x80000, 0xdeac47a1},
        {"mpr-16087.9", 0x80000, 0x7a64daac},
        {"mpr-16088.10", 0x80000, 0xfcda2d1e},
        {"mpr-16089.11", 0x80000, 0x39befbe0},
        {"mpr-16090.12", 0x80000, 0x90c76831},
        {"mpr-16091.13", 0x80000, 0x53115448},
    }};
    for (std::size_t index = 0; index < rom_o.size(); ++index) {
        const std::size_t bank = index / 2;
        const std::size_t lane = index & 1;
        copy_interleaved_byte(source, rom_o[index], roms.main_cpu,
                              0x1000000 + bank * 0x100000 + lane, 2, errors);
    }

    roms.sound_cpu.assign(sound_cpu_size, 0);
    constexpr std::array<rom_spec, 2> sound_roms{{
        {"epr-16120.7", 0x20000, 0x2bff8378},
        {"epr-16121.8", 0x20000, 0xff6723f9},
    }};
    for (std::size_t rom_index = 0; rom_index < sound_roms.size(); ++rom_index) {
        std::vector<uint8_t> sound;
        if (!load_checked(source, sound_roms[rom_index], sound, errors)) continue;
        const std::size_t base = rom_index * 0x20000;
        for (std::size_t index = 0; index < sound.size(); index += 2) {
            roms.sound_cpu[base + index] = sound[index + 1];
            roms.sound_cpu[base + index + 1] = sound[index];
        }
    }

    roms.samples_1.assign(0x400000, 0);
    roms.samples_2.assign(0x400000, 0);
    copy_linear(source, {"mpr-16122.32", 0x200000, 0x568bc64e},
                roms.samples_1, 0, errors);
    copy_linear(source, {"mpr-16123.33", 0x200000, 0x15d78844},
                roms.samples_1, 0x200000, errors);
    copy_linear(source, {"mpr-16124.4", 0x200000, 0x45520ba1},
                roms.samples_2, 0, errors);
    copy_linear(source, {"mpr-16125.5", 0x200000, 0x9b4998b6},
                roms.samples_2, 0x200000, errors);

    roms.polygon_data.assign(polygon_data_size, 0);
    constexpr std::array<rom_spec, 8> polygon{{
        {"mpr-16096.26", 0x200000, 0xa92b0bf3},
        {"mpr-16097.27", 0x200000, 0x0232955a},
        {"mpr-16098.28", 0x200000, 0xcf2e1b84},
        {"mpr-16099.29", 0x200000, 0x20e46854},
        {"mpr-16100.30", 0x200000, 0xe13e983d},
        {"mpr-16101.31", 0x200000, 0x0dbed94d},
        {"mpr-16102.32", 0x200000, 0x4cb41fb6},
        {"mpr-16103.33", 0x200000, 0x526d1c76},
    }};
    for (std::size_t index = 0; index < polygon.size(); ++index)
        copy_interleaved_word(source, polygon[index], roms.polygon_data,
                              (index / 2) * 0x400000 + (index & 1) * 2, errors);

    // VF's geometry program is self-contained. There is no separate data ROM
    // board, but the TGP address space still reads as a populated zero image.
    roms.copro_data.assign(copro_data_size, 0);
    roms.lookup_tables.assign(lookup_table_size, 0);
    copy_interleaved_word(source, {"opr14742.bin", 0x20000, 0x446a1085},
                          roms.lookup_tables, 0, errors);
    copy_interleaved_word(source, {"opr14743.bin", 0x20000, 0xe8953554},
                          roms.lookup_tables, 2, errors);
    copy_interleaved_word(source, {"opr-14744.58", 0x20000, 0x730ea9e0},
                          roms.lookup_tables, 0x40000, errors);
    copy_interleaved_word(source, {"opr-14745.59", 0x20000, 0x4c934d96},
                          roms.lookup_tables, 0x40002, errors);
    copy_interleaved_word(source, {"opr-14746.62", 0x20000, 0x2a266cbd},
                          roms.lookup_tables, 0x80000, errors);
    copy_interleaved_word(source, {"opr-14747.63", 0x20000, 0xa4ad5e19},
                          roms.lookup_tables, 0x80002, errors);
    copy_linear(source, {"opr14748.bin", 0x20000, 0x4a532cb8},
                roms.lookup_tables, 0xc0000, errors);

    load_checked(source, {"315-5724.bin", 0x2000, 0x4b4f330e},
                 roms.legacy_tgp, errors);
    load_alias(source,
               {{"epr-14869b.25", 0x10000, 0x2d093304},
                {"epr-14869.25", 0x10000, 0x6187cd7a}},
               roms.io_cpu, errors);
    // VF has no communications-board program. Keep the open socket as a
    // correctly sized empty region so shared Model 1 bus code stays generic.
    roms.communication_cpu.assign(0x20000, 0xff);

    result.error = errors.str();
    return result;
}

model1_rom_load_result load_star_wars_arcade(const source_reader& source) {
    model1_rom_load_result result;
    result.set = model1_rom_set::star_wars_arcade;
    model1_roms& roms = result.roms;
    std::ostringstream errors;

    roms.main_cpu.assign(main_cpu_size, 0xff);
    copy_interleaved_byte(source, {"epr-16669.14", 0x80000, 0x52e5e7e4},
                          roms.main_cpu, 0x200000, 2, errors);
    copy_interleaved_byte(source, {"epr-16670.15", 0x80000, 0x1e7ecabd},
                          roms.main_cpu, 0x200001, 2, errors);
    std::vector<uint8_t> boot;
    if (load_checked(source, {"epr-16668.5", 0x80000, 0x9e112425},
                     boot, errors)) {
        std::copy(boot.begin(), boot.end(), roms.main_cpu.begin());
        std::copy(boot.begin(), boot.end(), roms.main_cpu.begin() + 0x080000);
        std::copy(boot.begin(), boot.end(), roms.main_cpu.begin() + 0xf80000);
    }

    roms.sound_cpu.assign(sound_cpu_size, 0);
    std::vector<uint8_t> sound;
    if (load_checked(source, {"epr-16470.7", 0x20000, 0x7da18cf7},
                     sound, errors)) {
        for (std::size_t index = 0; index < sound.size(); index += 2) {
            roms.sound_cpu[index] = sound[index + 1];
            roms.sound_cpu[index + 1] = sound[index];
        }
    }

    roms.samples_1.assign(0x400000, 0);
    roms.samples_2.assign(0x400000, 0);
    copy_linear(source, {"mpr-16486.32", 0x200000, 0x7df50533},
                roms.samples_1, 0, errors);
    copy_linear(source, {"mpr-16487.33", 0x200000, 0x31b28dfa},
                roms.samples_1, 0x200000, errors);
    copy_linear(source, {"mpr-16484.4", 0x200000, 0x9d4c334d},
                roms.samples_2, 0, errors);
    copy_linear(source, {"mpr-16485.5", 0x200000, 0x95aadcad},
                roms.samples_2, 0x200000, errors);

    load_checked(source, {"epr-16471.2", 0x20000, 0xf4ee84a4},
                 roms.dsb_cpu, errors);
    // The US board decodes a 23-bit MPEG address bus. Two 2 MiB devices are
    // populated; the remaining sockets stay at the board's erased value.
    roms.dsb_mpeg.assign(0x800000, 0);
    copy_linear(source, {"mpr-16514.57", 0x200000, 0x3175b0be},
                roms.dsb_mpeg, 0, errors);
    copy_linear(source, {"mpr-16515.58", 0x200000, 0x3114d748},
                roms.dsb_mpeg, 0x200000, errors);

    roms.polygon_data.assign(polygon_data_size, 0);
    constexpr std::array<rom_spec, 6> polygon{{
        {"mpr-16476.26", 0x200000, 0xd48609ae},
        {"mpr-16477.27", 0x200000, 0xb979b082},
        {"mpr-16478.28", 0x200000, 0x80c780f7},
        {"mpr-16479.29", 0x200000, 0xe43183b3},
        {"mpr-16480.30", 0x200000, 0x3185547a},
        {"mpr-16481.31", 0x200000, 0xce8d76fe},
    }};
    for (std::size_t index = 0; index < polygon.size(); ++index)
        copy_interleaved_word(source, polygon[index], roms.polygon_data,
                              (index / 2) * 0x400000 + (index & 1) * 2, errors);

    roms.copro_data.assign(copro_data_size, 0);
    constexpr std::array<rom_spec, 4> copro{{
        {"mpr-16472.39", 0x80000, 0x5a0d7553},
        {"mpr-16473.40", 0x80000, 0x876c5399},
        {"mpr-16474.41", 0x80000, 0x5864a26f},
        {"mpr-16475.42", 0x80000, 0xb9266be9},
    }};
    for (std::size_t lane = 0; lane < copro.size(); ++lane)
        copy_interleaved_byte(source, copro[lane], roms.copro_data, lane, 4,
                              errors);

    roms.lookup_tables.assign(lookup_table_size, 0);
    copy_interleaved_word(source, {"opr14742.bin", 0x20000, 0x446a1085},
                          roms.lookup_tables, 0, errors);
    copy_interleaved_word(source, {"opr14743.bin", 0x20000, 0xe8953554},
                          roms.lookup_tables, 2, errors);
    copy_interleaved_word(source, {"opr-14744.58", 0x20000, 0x730ea9e0},
                          roms.lookup_tables, 0x40000, errors);
    copy_interleaved_word(source, {"opr-14745.59", 0x20000, 0x4c934d96},
                          roms.lookup_tables, 0x40002, errors);
    copy_interleaved_word(source, {"opr-14746.62", 0x20000, 0x2a266cbd},
                          roms.lookup_tables, 0x80000, errors);
    copy_interleaved_word(source, {"opr-14747.63", 0x20000, 0xa4ad5e19},
                          roms.lookup_tables, 0x80002, errors);
    copy_linear(source, {"opr14748.bin", 0x20000, 0x4a532cb8},
                roms.lookup_tables, 0xc0000, errors);
    load_checked(source, {"315-5711.bin", 0x2000, 0xc5ddb8fc},
                 roms.legacy_tgp, errors);
    load_alias(source,
               {{"epr-14869b.25", 0x10000, 0x2d093304},
                {"epr-14869.25", 0x10000, 0x6187cd7a}},
               roms.io_cpu, errors);
    roms.communication_cpu.assign(0x20000, 0xff);

    result.error = errors.str();
    return result;
}

model1_rom_load_result load_wing_war(const source_reader& source) {
    model1_rom_load_result result;
    result.set = model1_rom_set::wing_war;
    model1_roms& roms = result.roms;
    std::ostringstream errors;

    roms.main_cpu.assign(main_cpu_size, 0xff);
    copy_interleaved_byte(source, {"epr-16729.14", 0x80000, 0x7edec2cc},
                          roms.main_cpu, 0x200000, 2, errors);
    copy_interleaved_byte(source, {"epr-16730.15", 0x80000, 0xbab24dee},
                          roms.main_cpu, 0x200001, 2, errors);
    std::vector<uint8_t> boot_low;
    std::vector<uint8_t> boot_high;
    if (load_checked(source, {"epr-16953.4", 0x20000, 0xc821a920},
                     boot_low, errors)) {
        std::copy(boot_low.begin(), boot_low.end(), roms.main_cpu.begin());
        std::copy(boot_low.begin(), boot_low.end(),
                  roms.main_cpu.begin() + 0xfc0000);
    }
    if (load_checked(source, {"epr-16952.5", 0x20000, 0x03a3ecc5},
                     boot_high, errors)) {
        std::copy(boot_high.begin(), boot_high.end(),
                  roms.main_cpu.begin() + 0x020000);
        std::copy(boot_high.begin(), boot_high.end(),
                  roms.main_cpu.begin() + 0xfe0000);
    }

    constexpr std::array<rom_spec, 6> rom_o{{
        {"mpr-16738.6", 0x80000, 0x51518ffa},
        {"mpr-16737.7", 0x80000, 0x37b1379c},
        {"mpr-16736.8", 0x80000, 0x10b6a025},
        {"mpr-16735.9", 0x80000, 0xc82fd198},
        {"mpr-16734.10", 0x80000, 0xf76371c1},
        {"mpr-16733.11", 0x80000, 0xe105847b},
    }};
    for (std::size_t index = 0; index < rom_o.size(); ++index) {
        const std::size_t bank = index / 2;
        const std::size_t lane = index & 1;
        copy_interleaved_byte(source, rom_o[index], roms.main_cpu,
                              0x1000000 + bank * 0x100000 + lane, 2, errors);
    }

    roms.sound_cpu.assign(sound_cpu_size, 0);
    constexpr std::array<rom_spec, 2> sound_roms{{
        {"epr-17126.7", 0x20000, 0x50178e40},
        {"epr-16752.8", 0x20000, 0x6541c48f},
    }};
    for (std::size_t rom_index = 0; rom_index < sound_roms.size(); ++rom_index) {
        std::vector<uint8_t> sound;
        if (!load_checked(source, sound_roms[rom_index], sound, errors)) continue;
        const std::size_t base = rom_index * 0x20000;
        for (std::size_t index = 0; index < sound.size(); index += 2) {
            roms.sound_cpu[base + index] = sound[index + 1];
            roms.sound_cpu[base + index + 1] = sound[index];
        }
    }

    roms.samples_1.assign(0x400000, 0);
    roms.samples_2.assign(0x400000, 0);
    copy_linear(source, {"mpr-16753.32", 0x200000, 0x324a8333},
                roms.samples_1, 0, errors);
    copy_linear(source, {"mpr-16754.33", 0x200000, 0x144f3cf5},
                roms.samples_1, 0x200000, errors);
    copy_linear(source, {"mpr-16755.4", 0x200000, 0x4baaf878},
                roms.samples_2, 0, errors);
    copy_linear(source, {"mpr-16756.5", 0x200000, 0xd9c40672},
                roms.samples_2, 0x200000, errors);

    roms.polygon_data.assign(polygon_data_size, 0);
    constexpr std::array<rom_spec, 8> polygon{{
        {"mpr-16743.26", 0x200000, 0xa710d33c},
        {"mpr-16744.27", 0x200000, 0xde796e1f},
        {"mpr-16745.28", 0x200000, 0x905b689c},
        {"mpr-16746.29", 0x200000, 0x163b312e},
        {"mpr-16747.30", 0x200000, 0x7353bb12},
        {"mpr-16748.31", 0x200000, 0x8ce98d3a},
        {"mpr-16749.32", 0x200000, 0x0e36dc1a},
        {"mpr-16750.33", 0x200000, 0xe4f0b98d},
    }};
    for (std::size_t index = 0; index < polygon.size(); ++index)
        copy_interleaved_word(source, polygon[index], roms.polygon_data,
                              (index / 2) * 0x400000 + (index & 1) * 2, errors);

    roms.copro_data.assign(copro_data_size, 0);
    constexpr std::array<rom_spec, 4> copro{{
        {"mpr-16741.39", 0x80000, 0x84b2ffd8},
        {"mpr-16742.40", 0x80000, 0xe9cc12bb},
        {"mpr-16739.41", 0x80000, 0x6c73e98f},
        {"mpr-16740.42", 0x80000, 0x44b31007},
    }};
    for (std::size_t lane = 0; lane < copro.size(); ++lane)
        copy_interleaved_byte(source, copro[lane], roms.copro_data, lane, 4,
                              errors);

    roms.lookup_tables.assign(lookup_table_size, 0);
    copy_interleaved_word(source, {"opr14742.bin", 0x20000, 0x446a1085},
                          roms.lookup_tables, 0, errors);
    copy_interleaved_word(source, {"opr14743.bin", 0x20000, 0xe8953554},
                          roms.lookup_tables, 2, errors);
    copy_interleaved_word(source, {"opr-14744.58", 0x20000, 0x730ea9e0},
                          roms.lookup_tables, 0x40000, errors);
    copy_interleaved_word(source, {"opr-14745.59", 0x20000, 0x4c934d96},
                          roms.lookup_tables, 0x40002, errors);
    copy_interleaved_word(source, {"opr-14746.62", 0x20000, 0x2a266cbd},
                          roms.lookup_tables, 0x80000, errors);
    copy_interleaved_word(source, {"opr-14747.63", 0x20000, 0xa4ad5e19},
                          roms.lookup_tables, 0x80002, errors);
    copy_linear(source, {"opr14748.bin", 0x20000, 0x4a532cb8},
                roms.lookup_tables, 0xc0000, errors);
    load_checked(source, {"315-5711.bin", 0x2000, 0xc5ddb8fc},
                 roms.legacy_tgp, errors);
    roms.communication_cpu.assign(0x20000, 0xff);

    result.error = errors.str();
    return result;
}
} // namespace

bool model1_roms::complete() const {
    return main_cpu.size() == main_cpu_size &&
           sound_cpu.size() == sound_cpu_size &&
           samples_1.size() == 0x400000 && samples_2.size() == 0x400000 &&
           polygon_data.size() == polygon_data_size &&
           copro_data.size() == copro_data_size &&
           lookup_tables.size() == lookup_table_size &&
           communication_cpu.size() == 0x20000;
}

model1_rom_set model1_rom_loader::identify_set(const std::string& path) {
    const source_reader source(path);
    if (source.contains("epr-15638.14") && source.contains("epr-15639.15"))
        return model1_rom_set::virtua_formula;
    if (source.contains("epr-16082.14") && source.contains("epr-16083.15"))
        return model1_rom_set::virtua_fighter;
    if (source.contains("epr-16669.14") && source.contains("epr-16670.15"))
        return model1_rom_set::star_wars_arcade;
    if (source.contains("epr-16729.14") && source.contains("epr-16730.15"))
        return model1_rom_set::wing_war;
    return model1_rom_set::unknown;
}

const char* model1_rom_loader::set_short_name(model1_rom_set set) {
    switch (set) {
    case model1_rom_set::virtua_formula: return "vformula";
    case model1_rom_set::virtua_fighter: return "vf";
    case model1_rom_set::star_wars_arcade: return "swa";
    case model1_rom_set::wing_war: return "wingwar";
    case model1_rom_set::unknown: return "";
    }
    return "";
}

const char* model1_rom_loader::set_display_name(model1_rom_set set) {
    switch (set) {
    case model1_rom_set::virtua_formula:
        return "Virtua Formula";
    case model1_rom_set::virtua_fighter:
        return "Virtua Fighter";
    case model1_rom_set::star_wars_arcade:
        return "Star Wars Arcade (US)";
    case model1_rom_set::wing_war:
        return "Wing War (World)";
    case model1_rom_set::unknown:
        return "Unsupported Model 1 ROM set";
    }
    return "Unsupported Model 1 ROM set";
}

model1_rom_load_result model1_rom_loader::load(const std::string& path) {
    const model1_rom_set set = identify_set(path);
    const source_reader source(path, companion_sources(fs::path(path), set));
    switch (set) {
    case model1_rom_set::virtua_formula:
        return load_virtua_formula(source);
    case model1_rom_set::virtua_fighter:
        return load_virtua_fighter(source);
    case model1_rom_set::star_wars_arcade:
        return load_star_wars_arcade(source);
    case model1_rom_set::wing_war:
        return load_wing_war(source);
    case model1_rom_set::unknown:
        return {model1_rom_set::unknown, {},
                "Unsupported Sega Model 1 ROM set: " + path + "\n"};
    }
    return {model1_rom_set::unknown, {},
            "Unsupported Sega Model 1 ROM set: " + path + "\n"};
}
