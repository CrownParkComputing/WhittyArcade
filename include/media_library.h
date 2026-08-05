// media_library.h - private MAME media packs and MANX's curated local mirror.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace manx_media {

struct import_result {
    bool success{};
    std::size_t directories_created{};
    std::size_t files_copied{};
    std::uintmax_t bytes_copied{};
    std::size_t games_matched{};
    std::size_t descriptions_imported{};
    std::string message;
};

// Per-user destination. The NAS is only the source; browsing and launching do
// not depend on it remaining mounted after an import.
std::filesystem::path local_root();

// Best still image for a ROM name from an extracted MAME media tree.
std::filesystem::path artwork_path(const std::filesystem::path& root,
                                   const std::string& short_name,
                                   const std::string& preferred_category = {});

// Same lookup with several valid names for one installed game. This covers
// renamed MAME sets such as manxtt.zip -> canonical MANX id `manxttc`.
std::filesystem::path artwork_path(
    const std::filesystem::path& root,
    const std::vector<std::string>& short_names,
    const std::string& preferred_category = {});

// Imported game synopsis from the media pack's gamelist.xml. Descriptions
// are stored as one small UTF-8 file per canonical ROM name, so browsing never
// has to parse the multi-megabyte XML or touch the NAS.
std::string description(const std::filesystem::path& root,
                        const std::string& short_name);
std::string description(const std::filesystem::path& root,
                        const std::vector<std::string>& short_names);

// Mirrors the source directory structure, then copies media belonging to the
// supplied ROM short names. A file belongs to a game when its stem is exactly
// the short name (or starts with short_name plus '-', '_' or '.'); every file
// under a directory named exactly after the ROM is also included.
import_result import_installed_games(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    const std::vector<std::string>& short_names);

} // namespace manx_media
