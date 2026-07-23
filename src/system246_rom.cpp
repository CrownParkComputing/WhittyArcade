#include "system246_rom.h"
#include "platform_paths.h"

#include <minizip/unzip.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>

namespace fs = std::filesystem;

namespace {

constexpr const char* dongle_name = "rrv3vera.ic002";
constexpr uint64_t dongle_size = 0x800000;
constexpr uint32_t dongle_crc = 0xdd20c4a2;
constexpr const char* disc_name = "rrv1-a.chd";
constexpr uint64_t disc_logical_bytes = 258684928;
constexpr const char* disc_sha1 = "77bb70407511cbb12ab999410e797dcaf0779229";
constexpr std::size_t chd_v5_header_size = 124;

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return value;
}

uint32_t read_be32(const uint8_t* bytes) {
    return (static_cast<uint32_t>(bytes[0]) << 24) |
           (static_cast<uint32_t>(bytes[1]) << 16) |
           (static_cast<uint32_t>(bytes[2]) << 8) |
           static_cast<uint32_t>(bytes[3]);
}

uint64_t read_be64(const uint8_t* bytes) {
    return (static_cast<uint64_t>(read_be32(bytes)) << 32) |
           read_be32(bytes + 4);
}

std::string hex_bytes(const uint8_t* bytes, std::size_t size) {
    std::ostringstream text;
    text << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < size; ++index)
        text << std::setw(2) << static_cast<unsigned>(bytes[index]);
    return text.str();
}

struct zip_entry {
    bool found{};
    uint64_t size{};
    uint32_t crc{};
};

zip_entry find_zip_entry(unzFile archive, const char* wanted) {
    if (!archive || unzGoToFirstFile(archive) != UNZ_OK) return {};
    const std::string wanted_lower = lower(wanted);
    do {
        unz_file_info64 info{};
        std::array<char, 1024> name{};
        if (unzGetCurrentFileInfo64(archive, &info, name.data(), name.size(),
                                    nullptr, 0, nullptr, 0) != UNZ_OK)
            return {};
        if (lower(fs::path(name.data()).filename().string()) == wanted_lower)
            return {true, static_cast<uint64_t>(info.uncompressed_size),
                    static_cast<uint32_t>(info.crc)};
    } while (unzGoToNextFile(archive) == UNZ_OK);
    return {};
}

bool zip_has_dongle(const fs::path& path) {
    unzFile archive = unzOpen64(path.string().c_str());
    if (!archive) return false;
    const zip_entry entry = find_zip_entry(archive, dongle_name);
    unzClose(archive);
    return entry.found && entry.size == dongle_size &&
           entry.crc == dongle_crc;
}

fs::path directory_entry(const fs::path& directory, const char* wanted) {
    std::error_code error;
    if (!fs::is_directory(directory, error)) return {};
    const std::string wanted_lower = lower(wanted);
    for (fs::recursive_directory_iterator iterator(
             directory, fs::directory_options::skip_permission_denied,
             error), end;
         !error && iterator != end; iterator.increment(error)) {
        std::error_code type_error;
        if (iterator->is_regular_file(type_error) &&
            lower(iterator->path().filename().string()) == wanted_lower)
            return iterator->path();
    }
    return {};
}

bool directory_has_dongle(const fs::path& path) {
    const fs::path entry = directory_entry(path, dongle_name);
    if (entry.empty()) return false;
    std::error_code error;
    if (fs::file_size(entry, error) != dongle_size || error) return false;
    std::ifstream input(entry, std::ios::binary);
    if (!input) return false;
    std::array<uint8_t, 1 << 16> buffer{};
    uLong crc = crc32(0, Z_NULL, 0);
    while (input) {
        input.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
        const std::streamsize count = input.gcount();
        if (count > 0)
            crc = crc32(crc, buffer.data(), static_cast<uInt>(count));
    }
    return static_cast<uint32_t>(crc) == dongle_crc;
}

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

fs::path disc_below(const fs::path& directory) {
    fs::path candidate = find_child_case_insensitive(directory, disc_name);
    std::error_code error;
    if (!candidate.empty() && fs::is_regular_file(candidate, error))
        return candidate;

    for (const std::string& subdirectory : {std::string("rrvac")}) {
        const fs::path child =
            find_child_case_insensitive(directory, subdirectory);
        candidate = find_child_case_insensitive(child, disc_name);
        error.clear();
        if (!candidate.empty() && fs::is_regular_file(candidate, error))
            return candidate;
    }
    return {};
}

bool read_dongle(const fs::path& path, std::vector<uint8_t>& output,
                 std::string& error) {
    output.clear();
    std::error_code type_error;
    if (fs::is_directory(path, type_error)) {
        const fs::path entry = directory_entry(path, dongle_name);
        if (entry.empty()) {
            error = std::string("Missing System 246 dongle ROM: ") +
                    dongle_name;
            return false;
        }
        std::ifstream input(entry, std::ios::binary);
        output.assign(std::istreambuf_iterator<char>(input),
                      std::istreambuf_iterator<char>());
    } else {
        unzFile archive = unzOpen64(path.string().c_str());
        if (!archive) {
            error = "Could not open System 246 ZIP archive.";
            return false;
        }
        const zip_entry entry = find_zip_entry(archive, dongle_name);
        if (!entry.found || entry.size >
                static_cast<uint64_t>(std::numeric_limits<std::size_t>::max()) ||
            unzOpenCurrentFile(archive) != UNZ_OK) {
            unzClose(archive);
            error = std::string("Missing System 246 dongle ROM: ") +
                    dongle_name;
            return false;
        }
        output.resize(static_cast<std::size_t>(entry.size));
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
        unzClose(archive);
        if (total != output.size() || close_result != UNZ_OK) {
            output.clear();
            error = "System 246 dongle ROM could not be decompressed or "
                    "failed its ZIP CRC check.";
            return false;
        }
    }

    if (output.size() != dongle_size) {
        error = "Wrong size for rrv3vera.ic002: expected 0x800000 bytes.";
        output.clear();
        return false;
    }
    uLong actual_crc = crc32(0, Z_NULL, 0);
    actual_crc = crc32(actual_crc, output.data(),
                       static_cast<uInt>(output.size()));
    if (static_cast<uint32_t>(actual_crc) != dongle_crc) {
        std::ostringstream message;
        message << "Wrong CRC for rrv3vera.ic002: expected dd20c4a2, got "
                << std::hex << std::setfill('0') << std::setw(8)
                << static_cast<uint32_t>(actual_crc) << '.';
        error = message.str();
        output.clear();
        return false;
    }
    return true;
}

} // namespace

system246_rom_set system246_rom_loader::identify_set(const std::string& path) {
    if (path.empty()) return system246_rom_set::unknown;
    const fs::path source(path);
    std::error_code error;
    if (fs::is_directory(source, error))
        return directory_has_dongle(source) ?
            system246_rom_set::ridge_racer_v_arcade_battle :
            system246_rom_set::unknown;
    if (!fs::is_regular_file(source, error) ||
        lower(source.extension().string()) != ".zip")
        return system246_rom_set::unknown;
    return zip_has_dongle(source) ?
        system246_rom_set::ridge_racer_v_arcade_battle :
        system246_rom_set::unknown;
}

const char* system246_rom_loader::set_short_name(system246_rom_set set) {
    switch (set) {
    case system246_rom_set::ridge_racer_v_arcade_battle: return "rrvac";
    case system246_rom_set::unknown: return "";
    }
    return "";
}

const char* system246_rom_loader::set_display_name(system246_rom_set set) {
    switch (set) {
    case system246_rom_set::ridge_racer_v_arcade_battle:
        return "Ridge Racer V: Arcade Battle (RRV3 Ver.A)";
    case system246_rom_set::unknown:
        return "Unknown Namco System 246 set";
    }
    return "Unknown Namco System 246 set";
}

std::string system246_rom_loader::find_disc_path(
        const std::string& selected_path) {
    if (selected_path.empty()) return {};
    const fs::path selected(selected_path);
    std::error_code error;
    if (fs::is_regular_file(selected, error) &&
        lower(selected.filename().string()) == disc_name)
        return selected.lexically_normal().string();

    const fs::path directory = fs::is_directory(selected, error) ?
        selected : selected.parent_path();
    fs::path found = disc_below(directory);
    if (!found.empty()) return found.lexically_normal().string();

    // An extracted set may be selected as the rrvac directory while the CHD
    // remains at its MAME collection root.
    if (fs::is_directory(selected, error)) {
        found = disc_below(selected.parent_path());
        if (!found.empty()) return found.lexically_normal().string();
    }

    // WhittyArcade reads disc images from its dedicated CHD folder. Fall back
    // to <data>/WhittyArcade/chd/rrv1-a.chd so a disc kept there is found even
    // when the selected set ZIP has no sibling copy.
    const fs::path chd_candidate =
        whitty_platform::data_root() / "WhittyArcade" / "chd" / disc_name;
    if (fs::is_regular_file(chd_candidate, error))
        return chd_candidate.lexically_normal().string();
    return {};
}

system246_disc_info system246_rom_loader::inspect_disc(
        const std::string& path) {
    system246_disc_info result;
    if (path.empty()) {
        result.error = "Missing rrv1-a.chd disc image.";
        return result;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        result.error = "Could not open rrv1-a.chd.";
        return result;
    }
    std::array<uint8_t, chd_v5_header_size> header{};
    input.read(reinterpret_cast<char*>(header.data()), header.size());
    if (input.gcount() != static_cast<std::streamsize>(header.size())) {
        result.error = "rrv1-a.chd is truncated.";
        return result;
    }
    if (std::memcmp(header.data(), "MComprHD", 8) != 0 ||
        read_be32(header.data() + 8) != chd_v5_header_size ||
        read_be32(header.data() + 12) != 5) {
        result.error = "rrv1-a.chd is not a CHD v5 image.";
        return result;
    }

    result.logical_bytes = read_be64(header.data() + 32);
    const uint64_t metadata_offset = read_be64(header.data() + 48);
    result.hunk_bytes = read_be32(header.data() + 56);
    result.unit_bytes = read_be32(header.data() + 60);
    // CHD v5 stores the raw SHA-1 first and the overall SHA-1 at offset 84.
    result.sha1 = hex_bytes(header.data() + 84, 20);

    if (metadata_offset == 0 || metadata_offset >
            static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        result.error = "rrv1-a.chd has no readable media metadata.";
        return result;
    }
    input.clear();
    input.seekg(static_cast<std::streamoff>(metadata_offset), std::ios::beg);
    std::array<char, 4> tag{};
    input.read(tag.data(), tag.size());
    if (input.gcount() != static_cast<std::streamsize>(tag.size())) {
        result.error = "rrv1-a.chd media metadata is truncated.";
        return result;
    }
    result.metadata_tag.assign(tag.data(), tag.size());
    if (result.metadata_tag == "GDDD")
        result.container = system246_disc_container::hard_disk_chd;
    else if (result.metadata_tag == "CHT2")
        result.container = system246_disc_container::cdrom_chd;
    else {
        result.error = "rrv1-a.chd has unsupported CHD metadata tag '" +
                       result.metadata_tag + "'.";
        return result;
    }

    if (result.logical_bytes != disc_logical_bytes) {
        result.error = "rrv1-a.chd has the wrong logical size.";
        return result;
    }
    if (result.sha1 != disc_sha1) {
        result.error = "rrv1-a.chd SHA-1 does not match the supported "
                       "MAME disk image.";
        return result;
    }
    if (result.hunk_bytes == 0 || result.unit_bytes == 0) {
        result.error = "rrv1-a.chd has invalid hunk or unit geometry.";
        return result;
    }
    return result;
}

system246_rom_load_result system246_rom_loader::load(
        const std::string& path) {
    system246_rom_load_result result;
    result.set = identify_set(path);
    if (result.set == system246_rom_set::unknown) {
        result.error = "Archive is not Ridge Racer V: Arcade Battle "
                       "(missing or invalid rrv3vera.ic002).";
        return result;
    }
    result.dongle_archive = path;
    if (!read_dongle(fs::path(path), result.dongle, result.error))
        return result;
    result.disc_path = find_disc_path(path);
    result.disc = inspect_disc(result.disc_path);
    if (!result.disc) result.error = result.disc.error;
    return result;
}
