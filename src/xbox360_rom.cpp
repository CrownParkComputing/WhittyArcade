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

// An STFS package announces itself with one of three signature types at offset
// zero: LIVE for content signed by Microsoft's servers, CON for content signed
// by a console, PIRS for content signed offline. Note the trailing space in
// "CON " - the magic is always four bytes.
constexpr const char* stfs_magics[] = {"LIVE", "CON ", "PIRS"};
constexpr std::size_t stfs_magic_size = 4;
// Fields of the metadata that follows the signature and licence table.
constexpr std::size_t stfs_header_size_offset = 0x340;
constexpr std::size_t stfs_title_id_offset = 0x360;
// Enough of the file to hold everything above.
constexpr std::size_t stfs_metadata_size = stfs_title_id_offset + 4;

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

// Everything the board needs to know about one title.
//
// Readiness is decided by the shape of the copy in hand. For an extracted copy,
// `required` lists the data files the game reads beside its default.xex; a copy
// whose files are all present is ready to launch, and one that is missing any of
// them says so with the list rather than with a generic failure. A packaged copy
// consults no such list - there is nothing beside the package, because
// everything it reads is sealed inside it - so its readiness is the package's own
// integrity instead: an STFS signature, the title ID this set is registered
// under, and all of the bytes its header declares.
struct rom_set_definition {
    xbox360_rom_set set;
    // The shape the title is played from when a machine holds both, which for a
    // title dumped only one way is simply the shape it has.
    xbox360_content_shape preferred_shape;
    // True when the other shape plays as well. Only a title that was actually
    // dumped both ways sets this: it says "either copy is a real copy of this
    // game", not "try it and see".
    bool plays_either_shape;
    std::uint32_t title_id;
    const char* short_name;
    const char* display_name;
    const char* required[4];
};

constexpr rom_set_definition kRomSets[] = {
    {xbox360_rom_set::robotron_2084, xbox360_content_shape::extracted, false,
     0x584107e0u, "robotron", "Robotron: 2084 (offline Xbox 360)",
     {"classic/fw.sr", "classic/robotron.sr", "media/uiresource.xpr",
      nullptr}},
    // Geometry Wars keeps its data flat beside default.xex: the level/entity
    // tables in the .dat files and the sound banks in the .xwb wave banks.
    {xbox360_rom_set::geometry_wars, xbox360_content_shape::extracted, false,
     0x584107edu, "geometrywars", "Geometry Wars: Retro Evolved (Xbox 360)",
     {"GeometryWars1.dat", "GW1.xwb", nullptr, nullptr}},
    // The sequel was never extracted: it is played straight out of the signed
    // package it was downloaded as, which the native runtime opens itself.
    {xbox360_rom_set::geometry_wars_2, xbox360_content_shape::package, false,
     0x584108ffu, "geometrywars2",
     "Geometry Wars: Retro Evolved 2 (Xbox 360)",
     {nullptr, nullptr, nullptr, nullptr}},
    // Space Giraffe exists here both ways and plays either way, so the package
    // is preferred rather than the extraction being refused. The package is the
    // shape that shipped and the only one whose completeness is a fact about the
    // file itself: its header declares its length, so it is either whole or
    // short. An extraction can only be judged against a guessed list of the
    // files the game reads, and a copy that is missing most of Media/ passes
    // every check a XEX can offer and then dereferences a null asset pointer.
    //
    // Which is why the two required files below are both under Media/ - the
    // module data the game streams levels out of, and the wave bank all of its
    // sound comes from. Deliberately not Media/clit.png: the title asks for it
    // and it is in neither the extraction nor the package, so requiring it would
    // condemn a copy that plays.
    {xbox360_rom_set::space_giraffe, xbox360_content_shape::package, true,
     0x5841080cu, "spacegiraffe", "Space Giraffe (Xbox 360)",
     {"Media/modules.ox", "Media/sounds/GRsounds.xwb", nullptr, nullptr}},
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

// Reads the metadata every STFS package starts with. False when the file is not
// a package at all, which is how a candidate is told apart from an ordinary
// file: a package is named after the SHA-1 of its content, so there is nothing
// in the path to go on.
bool read_stfs_metadata(const fs::path& file,
                        std::array<std::uint8_t, stfs_metadata_size>& metadata) {
    std::ifstream input(file, std::ios::binary);
    if (!input) return false;
    input.read(reinterpret_cast<char*>(metadata.data()), metadata.size());
    if (!input) return false;
    for (const char* magic : stfs_magics)
        if (std::memcmp(metadata.data(), magic, stfs_magic_size) == 0)
            return true;
    return false;
}

bool is_stfs_package(const fs::path& file) {
    std::error_code error;
    if (!fs::is_regular_file(file, error)) return false;
    std::array<std::uint8_t, stfs_metadata_size> metadata{};
    return read_stfs_metadata(file, metadata);
}

// What was found at the selected path: exactly one of these is set.
struct located_content {
    fs::path xex;
    fs::path package;

    explicit operator bool() const {
        return !xex.empty() || !package.empty();
    }
};

located_content locate_content(const fs::path& selected) {
    std::error_code error;
    if (fs::is_regular_file(selected, error)) {
        if (lower(selected.filename().string()) == "default.xex")
            return {selected, {}};
        if (is_stfs_package(selected)) return {{}, selected};
        return {};
    }
    if (fs::is_directory(selected, error)) {
        const fs::path xex = child_case_insensitive(selected, "default.xex");
        if (!xex.empty()) return {xex, {}};
        // A package's filename is a content hash, so the only way to find one
        // in a directory is to look inside the files. The console stores each
        // package alone in its content-type directory, so the first one found
        // is the one that was selected.
        for (fs::directory_iterator iterator(selected, error), end;
             !error && iterator != end; iterator.increment(error)) {
            std::error_code type_error;
            if (!iterator->is_regular_file(type_error)) continue;
            if (is_stfs_package(iterator->path()))
                return {{}, iterator->path()};
        }
    }
    return {};
}

bool read_package_title_id(const fs::path& package, std::uint32_t& title_id,
                           std::string& error) {
    std::array<std::uint8_t, stfs_metadata_size> metadata{};
    if (!read_stfs_metadata(package, metadata)) {
        error = "That file is not an Xbox 360 STFS package (no LIVE, CON or "
                "PIRS signature).";
        return false;
    }
    title_id = read_be32(metadata.data() + stfs_title_id_offset);
    return true;
}

// A package holds every byte the title reads, so "complete" means the file is
// all there. Its own header says how long the header is, and a partial download
// or an interrupted copy is short of that.
//
// Deliberately not checked: the content size at 0x34c. It is the space the
// console allocated, which is rounded up past the end of the file - Geometry
// Wars 2 declares 0x02844000 bytes and is 0x02843000 long - so comparing
// against it would call a working package truncated.
bool package_is_whole(const fs::path& package, const std::string& display_name,
                      std::string& error) {
    std::array<std::uint8_t, stfs_metadata_size> metadata{};
    if (!read_stfs_metadata(package, metadata)) {
        error = std::string(display_name) + " package could not be read: " +
                package.string();
        return false;
    }
    std::error_code size_error;
    const std::uintmax_t size = fs::file_size(package, size_error);
    if (size_error) {
        error = std::string(display_name) + " package could not be read: " +
                package.string();
        return false;
    }
    const std::uint32_t declared =
        read_be32(metadata.data() + stfs_header_size_offset);
    if (size < declared) {
        error = std::string(display_name) + " package is incomplete: " +
                package.filename().string() + " is " + std::to_string(size) +
                " bytes, and its own header declares " +
                std::to_string(declared) + ".";
        return false;
    }
    return true;
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
        result.error = "No Xbox 360 game was selected.";
        return result;
    }
    const located_content content = locate_content(fs::path(path));
    if (!content) {
        // Name what was wanted: this is usually reached with a path that was
        // saved when the game was there, and which of the two shapes it had
        // decides what has to be put back.
        result.error = "No Xbox 360 game content was found at " + path +
                       ". Select an extracted game directory (with its "
                       "default.xex) or a signed STFS package (LIVE, CON or "
                       "PIRS).";
        return result;
    }

    const bool packaged = !content.package.empty();
    if (packaged) {
        if (!read_package_title_id(content.package, result.title_id,
                                   result.error))
            return result;
    } else if (!read_title_id(content.xex, result.title_id, result.error)) {
        return result;
    }
    const rom_set_definition* definition = definition_for_title(result.title_id);
    if (definition == nullptr) {
        result.error = packaged ?
            "The package's title ID is not a supported Xbox 360 title." :
            "The XEX title ID is not a supported Xbox 360 title.";
        return result;
    }

    // The shape found is the shape it is played from: a packaged copy is handed
    // to the runtime as one file, and an extracted one as a XEX plus the
    // directory its data sits in. A title dumped both ways plays from either; for
    // one dumped a single way, finding the other shape is worth saying plainly,
    // because the game is present and only in the wrong form.
    const xbox360_content_shape found = packaged ?
        xbox360_content_shape::package : xbox360_content_shape::extracted;
    if (found != definition->preferred_shape && !definition->plays_either_shape) {
        result.error = std::string(definition->display_name) +
            (packaged ?
                 " is played from an extracted default.xex, but an STFS "
                 "package was found at " :
                 " is played from its signed STFS package, but an extracted "
                 "default.xex was found at ") + path + '.';
        return result;
    }

    // Recorded before readiness is judged, so a copy that is identified but
    // incomplete still says which of the two shapes it is. A browser has to be
    // able to report that without being told whether to look for a short package
    // or for missing data files.
    result.shape = found;

    const fs::path root =
        packaged ? content.package.parent_path() : content.xex.parent_path();
    if (require_game_data) {
        if (packaged) {
            if (!package_is_whole(content.package, definition->display_name,
                                  result.error))
                return result;
        } else {
            std::string missing;
            for (const char* relative : definition->required) {
                if (relative == nullptr) break;
                const fs::path resolved = resolve_relative(root, relative);
                std::error_code type_error;
                if (!resolved.empty() &&
                    fs::is_regular_file(resolved, type_error))
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
    }

    result.set = definition->set;
    result.game_root = root.lexically_normal().string();
    if (packaged)
        result.package_path = content.package.lexically_normal().string();
    else
        result.xex_path = content.xex.lexically_normal().string();
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

xbox360_content_shape xbox360_rom_loader::set_preferred_shape(
        xbox360_rom_set set) {
    const rom_set_definition* definition = definition_for(set);
    return definition != nullptr ? definition->preferred_shape :
                                   xbox360_content_shape::extracted;
}

bool xbox360_rom_loader::set_plays_shape(xbox360_rom_set set,
                                        xbox360_content_shape shape) {
    const rom_set_definition* definition = definition_for(set);
    if (definition == nullptr) return false;
    return definition->plays_either_shape || definition->preferred_shape == shape;
}
