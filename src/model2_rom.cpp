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
} // namespace

bool model2_roms::complete() const {
    return main_cpu.size() == 0x200000 && main_data.size() == 0x2400000 &&
           copro_data.size() == 0x800000 &&
           polygon_data.size() == 0x2000000 &&
           texture_data.size() == 0x1000000 &&
           copro_tgp_tables.size() == 0x40000 &&
           other_data.size() == 0x80000 &&
           video_tables.size() == 0x200000 &&
           sound_cpu.size() == 0x80000 && samples.size() == 0x800000 &&
           communication_cpu.size() == 0x20000 &&
           drive_cpu.size() == 0x10000;
}

model2_rom_set model2_rom_loader::identify_set(const std::string& path) {
    const source_reader source(path);
    if (source.contains("epr-17888c.12") &&
        source.contains("epr-17889c.13"))
        return model2_rom_set::sega_rally_revision_c;
    return model2_rom_set::unknown;
}

const char* model2_rom_loader::set_short_name(model2_rom_set set) {
    switch (set) {
    case model2_rom_set::sega_rally_revision_c: return "srallyc";
    case model2_rom_set::unknown: return "";
    }
    return "";
}

const char* model2_rom_loader::set_display_name(model2_rom_set set) {
    switch (set) {
    case model2_rom_set::sega_rally_revision_c:
        return "Sega Rally Championship (Revision C)";
    case model2_rom_set::unknown:
        return "Unsupported Sega Model 2 ROM set";
    }
    return "Unsupported Sega Model 2 ROM set";
}

model2_rom_load_result model2_rom_loader::load(const std::string& path) {
    const source_reader source(path);
    switch (identify_set(path)) {
    case model2_rom_set::sega_rally_revision_c:
        return load_sega_rally(source, fs::path(path));
    case model2_rom_set::unknown:
        return {model2_rom_set::unknown, {},
                "Unsupported Sega Model 2 ROM set: " + path + "\n"};
    }
    return {model2_rom_set::unknown, {},
            "Unsupported Sega Model 2 ROM set: " + path + "\n"};
}
