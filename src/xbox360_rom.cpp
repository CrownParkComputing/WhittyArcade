#include "xbox360_rom.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {

constexpr std::uint32_t execution_info_key = 0x00040006;
constexpr std::size_t base_header_size = 0x18;
constexpr std::size_t execution_info_size = 0x18;

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return value;
}

std::uint32_t read_be32(const std::uint8_t* bytes) {
    return (static_cast<std::uint32_t>(bytes[0]) << 24) |
           (static_cast<std::uint32_t>(bytes[1]) << 16) |
           (static_cast<std::uint32_t>(bytes[2]) << 8) |
           static_cast<std::uint32_t>(bytes[3]);
}

fs::path child_case_insensitive(const fs::path& directory,
                                const char* wanted) {
    std::error_code error;
    if (!fs::is_directory(directory, error)) return {};
    const std::string wanted_lower = lower(wanted);
    for (fs::directory_iterator iterator(directory, error), end;
         !error && iterator != end; iterator.increment(error)) {
        if (lower(iterator->path().filename().string()) == wanted_lower)
            return iterator->path();
    }
    return {};
}

// A "classic/fw.sr"-style path, resolved a component at a time so a collection
// extracted with different capitalisation still validates.
fs::path resolve_relative(const fs::path& root, const char* relative) {
    fs::path current = root;
    for (const fs::path& part : fs::path(relative)) {
        current = child_case_insensitive(current, part.string().c_str());
        if (current.empty()) return {};
    }
    return current;
}

// Everything the board needs to know about one title. `required` lists the data
// files the game reads beside its default.xex; a title whose files are all
// present is ready to launch, and one that is missing any of them says so with
// the list rather than with a generic failure.
struct rom_set_definition {
    xbox360_rom_set set;
    std::uint32_t title_id;
    const char* short_name;
    const char* display_name;
    const char* required[4];
};

constexpr rom_set_definition kRomSets[] = {
    {xbox360_rom_set::robotron_2084, 0x584107e0u, "robotron",
     "Robotron: 2084 (offline Xbox 360)",
     {"classic/fw.sr", "classic/robotron.sr", "media/uiresource.xpr",
      nullptr}},
    // Geometry Wars keeps its data flat beside default.xex: the level/entity
    // tables in the .dat files and the sound banks in the .xwb wave banks.
    {xbox360_rom_set::geometry_wars, 0x584107edu, "geometrywars",
     "Geometry Wars: Retro Evolved (Xbox 360)",
     {"GeometryWars1.dat", "GW1.xwb", nullptr, nullptr}},
};

const rom_set_definition* definition_for(xbox360_rom_set set) {
    for (const rom_set_definition& candidate : kRomSets)
        if (candidate.set == set) return &candidate;
    return nullptr;
}

const rom_set_definition* definition_for_title(std::uint32_t title_id) {
    for (const rom_set_definition& candidate : kRomSets)
        if (candidate.title_id == title_id) return &candidate;
    return nullptr;
}

fs::path resolve_xex(const fs::path& selected) {
    std::error_code error;
    if (fs::is_regular_file(selected, error) &&
        lower(selected.filename().string()) == "default.xex")
        return selected;
    if (fs::is_directory(selected, error))
        return child_case_insensitive(selected, "default.xex");
    return {};
}

bool read_title_id(const fs::path& xex, std::uint32_t& title_id,
                   std::string& error) {
    std::ifstream input(xex, std::ios::binary);
    if (!input) {
        error = "Could not open default.xex.";
        return false;
    }
    input.seekg(0, std::ios::end);
    const std::streamoff end = input.tellg();
    if (end < static_cast<std::streamoff>(base_header_size)) {
        error = "default.xex is truncated.";
        return false;
    }
    const std::size_t size = static_cast<std::size_t>(end);
    input.seekg(0, std::ios::beg);
    std::array<std::uint8_t, base_header_size> header{};
    input.read(reinterpret_cast<char*>(header.data()), header.size());
    if (!input || std::memcmp(header.data(), "XEX2", 4) != 0) {
        error = "default.xex is not an Xbox 360 XEX2 image.";
        return false;
    }

    const std::uint32_t count = read_be32(header.data() + 0x14);
    if (count > 4096 || base_header_size +
            static_cast<std::size_t>(count) * 8 > size) {
        error = "default.xex has an invalid optional-header table.";
        return false;
    }
    std::array<std::uint8_t, 8> entry{};
    for (std::uint32_t index = 0; index < count; ++index) {
        input.read(reinterpret_cast<char*>(entry.data()), entry.size());
        if (!input) {
            error = "default.xex optional-header table is truncated.";
            return false;
        }
        if (read_be32(entry.data()) != execution_info_key) continue;
        const std::uint32_t offset = read_be32(entry.data() + 4);
        if (offset > size || size - offset < execution_info_size) {
            error = "default.xex execution-info header is out of bounds.";
            return false;
        }
        std::array<std::uint8_t, execution_info_size> execution{};
        input.seekg(offset, std::ios::beg);
        input.read(reinterpret_cast<char*>(execution.data()), execution.size());
        if (!input) {
            error = "default.xex execution-info header is truncated.";
            return false;
        }
        title_id = read_be32(execution.data() + 0x0c);
        return true;
    }
    error = "default.xex has no execution-info title ID.";
    return false;
}

} // namespace

xbox360_rom_info xbox360_rom_loader::inspect(
        const std::string& path, bool require_game_data) {
    xbox360_rom_info result;
    if (path.empty()) {
        result.error = "No Xbox 360 game directory was selected.";
        return result;
    }
    const fs::path xex = resolve_xex(fs::path(path));
    if (xex.empty()) {
        result.error = "Select the extracted game directory or its default.xex.";
        return result;
    }
    if (!read_title_id(xex, result.title_id, result.error)) return result;
    const rom_set_definition* definition = definition_for_title(result.title_id);
    if (definition == nullptr) {
        result.error = "The XEX title ID is not a supported Xbox 360 title.";
        return result;
    }

    const fs::path root = xex.parent_path();
    if (require_game_data) {
        std::string missing;
        for (const char* relative : definition->required) {
            if (relative == nullptr) break;
            const fs::path resolved = resolve_relative(root, relative);
            std::error_code type_error;
            if (!resolved.empty() && fs::is_regular_file(resolved, type_error))
                continue;
            if (!missing.empty()) missing += ", ";
            missing += relative;
        }
        if (!missing.empty()) {
            result.error = std::string(definition->display_name) +
                           " game data is incomplete: missing " + missing +
                           " beside default.xex.";
            return result;
        }
    }

    result.set = definition->set;
    result.game_root = root.lexically_normal().string();
    result.xex_path = xex.lexically_normal().string();
    return result;
}

xbox360_rom_set xbox360_rom_loader::identify_set(const std::string& path) {
    return inspect(path, false).set;
}

const char* xbox360_rom_loader::set_short_name(xbox360_rom_set set) {
    const rom_set_definition* definition = definition_for(set);
    return definition != nullptr ? definition->short_name : "";
}

const char* xbox360_rom_loader::set_display_name(xbox360_rom_set set) {
    const rom_set_definition* definition = definition_for(set);
    return definition != nullptr ? definition->display_name :
                                   "Unknown Xbox 360 title";
}
