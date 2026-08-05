#include "media_library.h"
#include "platform_paths.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::string lowercase(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return text;
}

const std::map<std::string, std::string>& media_aliases() {
    // Media packs track current MAME short names, while MANX also supports
    // older set names that people already have in their libraries.
    static const std::map<std::string, std::string> aliases{
        {"daytona", "daytona93"},
        {"ddux1", "ddux"},
        {"goldnaxe2", "goldnaxe"},
        {"manxttc", "manxtt"},
        {"ridgeracf", "ridgerac"},
        {"shinobi4", "shinobi"},
        {"tturfu", "tturf"},
        {"vformula", "vr"},
    };
    return aliases;
}

std::string canonical_game(const std::string& name,
                           const std::set<std::string>& games) {
    if (games.count(name)) return name;
    for (const auto& [game, alias] : media_aliases())
        if (name == alias && games.count(game)) return game;
    return {};
}

bool stem_matches(const fs::path& path, const std::set<std::string>& games) {
    const std::string stem = lowercase(path.stem().string());
    for (const std::string& game : games) {
        if (stem == game) return true;
        if (stem.size() <= game.size() || stem.compare(0, game.size(), game))
            continue;
        const char separator = stem[game.size()];
        if (separator == '-' || separator == '_' || separator == '.')
            return true;
    }
    return false;
}

bool inside_game_directory(const fs::path& relative,
                           const std::set<std::string>& games,
                           std::string& matched) {
    for (const fs::path& component : relative.parent_path()) {
        const std::string name = lowercase(component.string());
        if (games.count(name)) {
            matched = name;
            return true;
        }
    }
    return false;
}

bool path_is_within(const fs::path& child, const fs::path& parent) {
    auto child_part = child.begin();
    for (auto parent_part = parent.begin(); parent_part != parent.end();
         ++parent_part, ++child_part) {
        if (child_part == child.end() || *child_part != *parent_part)
            return false;
    }
    return true;
}

std::string xml_value(const std::string& block, const char* tag) {
    const std::string open = std::string("<") + tag + ">";
    const std::string close = std::string("</") + tag + ">";
    const std::size_t begin = block.find(open);
    if (begin == std::string::npos) return {};
    const std::size_t value_begin = begin + open.size();
    const std::size_t end = block.find(close, value_begin);
    return end == std::string::npos ? std::string{} :
                                      block.substr(value_begin,
                                                   end - value_begin);
}

void append_utf8(std::string& out, unsigned codepoint) {
    if (codepoint <= 0x7f) {
        out.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ff) {
        out.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else if (codepoint <= 0xffff) {
        out.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else if (codepoint <= 0x10ffff) {
        out.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    }
}

std::string decode_xml(std::string text) {
    std::string decoded;
    decoded.reserve(text.size());
    for (std::size_t index = 0; index < text.size();) {
        if (text[index] != '&') {
            decoded.push_back(text[index++]);
            continue;
        }
        const std::size_t semicolon = text.find(';', index + 1);
        if (semicolon == std::string::npos) {
            decoded.push_back(text[index++]);
            continue;
        }
        const std::string entity =
            text.substr(index + 1, semicolon - index - 1);
        if (entity == "amp") decoded.push_back('&');
        else if (entity == "lt") decoded.push_back('<');
        else if (entity == "gt") decoded.push_back('>');
        else if (entity == "quot") decoded.push_back('"');
        else if (entity == "apos") decoded.push_back('\'');
        else if (entity.size() > 1 && entity[0] == '#') {
            try {
                const bool hexadecimal = entity.size() > 2 &&
                    (entity[1] == 'x' || entity[1] == 'X');
                const unsigned value = static_cast<unsigned>(std::stoul(
                    entity.substr(hexadecimal ? 2 : 1), nullptr,
                    hexadecimal ? 16 : 10));
                append_utf8(decoded, value);
            } catch (...) {
                decoded.append(text, index, semicolon - index + 1);
            }
        } else {
            decoded.append(text, index, semicolon - index + 1);
        }
        index = semicolon + 1;
    }
    return decoded;
}

std::size_t import_gamelist_descriptions(
        const fs::path& source, const fs::path& destination,
        const std::set<std::string>& games) {
    fs::path gamelist = source.parent_path() / "gamelist.xml";
    std::error_code error;
    if (!fs::is_regular_file(gamelist, error)) {
        error.clear();
        gamelist = source / "gamelist.xml";
        if (!fs::is_regular_file(gamelist, error)) return 0;
    }

    std::ifstream input(gamelist, std::ios::binary);
    if (!input) return 0;
    std::ostringstream contents;
    contents << input.rdbuf();
    const std::string xml = contents.str();
    const fs::path metadata = destination / "metadata";
    fs::create_directories(metadata, error);
    if (error) return 0;

    std::size_t imported = 0;
    std::size_t position = 0;
    while ((position = xml.find("<game>", position)) != std::string::npos) {
        const std::size_t end = xml.find("</game>", position + 6);
        if (end == std::string::npos) break;
        const std::string block = xml.substr(position, end + 7 - position);
        position = end + 7;
        const std::string path_text = decode_xml(xml_value(block, "path"));
        const std::string synopsis = decode_xml(xml_value(block, "desc"));
        if (path_text.empty() || synopsis.empty()) continue;
        const std::string rom_name = lowercase(fs::path(path_text).stem().string());
        const std::string canonical = canonical_game(rom_name, games);
        if (canonical.empty()) continue;
        std::ofstream output(metadata / (canonical + ".txt"),
                             std::ios::binary | std::ios::trunc);
        if (!output) continue;
        output << synopsis;
        if (output) ++imported;
    }
    return imported;
}

} // namespace

namespace manx_media {

fs::path local_root() {
    const fs::path root = manx_platform::data_root();
    return (root.empty() ? fs::path(".") : root) /
           "MANX" / "media";
}

fs::path artwork_path(const fs::path& root,
                      const std::vector<std::string>& short_names,
                      const std::string& preferred_category) {
    if (root.empty() || short_names.empty()) return {};
    static constexpr std::array<const char*, 21> directories{{
        "box2d", "box3d", "covers", "cover", "flyers", "boxback", "titles",
        "title", "images", "snap", "snaps", "thumbnails", "marquee",
        "marquees", "fanarts", "cartridges", "cabinets", "artwork", "icons",
        "icon", "",
    }};
    static constexpr std::array<const char*, 5> extensions{{
        ".png", ".jpg", ".jpeg", ".webp", ".bmp",
    }};
    std::error_code error;
    std::vector<std::string> names;
    for (const std::string& short_name : short_names) {
        const std::string name = lowercase(short_name);
        if (name.empty() ||
            std::find(names.begin(), names.end(), name) != names.end())
            continue;
        names.push_back(name);
        if (const auto alias = media_aliases().find(name);
            alias != media_aliases().end() &&
            std::find(names.begin(), names.end(), alias->second) == names.end())
            names.push_back(alias->second);
    }
    std::vector<std::string> ordered_directories;
    if (!preferred_category.empty()) {
        const bool valid = std::any_of(
            directories.begin(), directories.end(),
            [&](const char* directory) {
                return directory && preferred_category == directory;
            });
        if (valid) ordered_directories.push_back(preferred_category);
    }
    // A category explicitly chosen in the GUI is strict. Falling back to a
    // different category made switching from covers to titles appear broken
    // whenever one game lacked a title image. With no preference, callers
    // still get the general best-image search in the order above.
    if (ordered_directories.empty())
        for (const char* directory : directories)
            if (directory) ordered_directories.emplace_back(directory);
    for (const std::string& name : names) {
        for (const std::string& directory : ordered_directories) {
            const fs::path folder = directory.empty() ? root :
                                                      root / directory;
            for (const char* extension : extensions) {
                const fs::path candidate = folder / (name + extension);
                if (fs::is_regular_file(candidate, error)) return candidate;
                error.clear();
            }
        }
    }
    return {};
}

fs::path artwork_path(const fs::path& root, const std::string& short_name,
                      const std::string& preferred_category) {
    return artwork_path(root, std::vector<std::string>{short_name},
                        preferred_category);
}

fs::path video_path(const fs::path& root,
                    const std::vector<std::string>& short_names) {
    if (root.empty() || short_names.empty()) return {};
    static constexpr std::array<const char*, 4> directories{{
        "videos", "video", "snap", "",
    }};
    static constexpr std::array<const char*, 4> extensions{{
        ".mp4", ".mkv", ".avi", ".webm",
    }};
    std::error_code error;
    std::vector<std::string> names;
    for (const std::string& short_name : short_names) {
        const std::string name = lowercase(short_name);
        if (name.empty() ||
            std::find(names.begin(), names.end(), name) != names.end())
            continue;
        names.push_back(name);
        if (const auto alias = media_aliases().find(name);
            alias != media_aliases().end() &&
            std::find(names.begin(), names.end(), alias->second) == names.end())
            names.push_back(alias->second);
    }
    for (const std::string& name : names) {
        for (const char* directory : directories) {
            const fs::path folder = *directory ? root / directory : root;
            for (const char* extension : extensions) {
                const fs::path candidate = folder / (name + extension);
                if (fs::is_regular_file(candidate, error)) return candidate;
                error.clear();
            }
        }
    }
    return {};
}

std::string description(const fs::path& root,
                        const std::vector<std::string>& short_names) {
    for (const std::string& short_name : short_names) {
        std::vector<std::string> names{lowercase(short_name)};
        if (const auto alias = media_aliases().find(names.front());
            alias != media_aliases().end())
            names.push_back(alias->second);
        for (const std::string& name : names) {
            std::ifstream input(root / "metadata" / (name + ".txt"),
                                std::ios::binary);
            if (!input) continue;
            std::ostringstream text;
            text << input.rdbuf();
            if (!text.str().empty()) return text.str();
        }
    }
    return {};
}

std::string description(const fs::path& root,
                        const std::string& short_name) {
    return description(root, std::vector<std::string>{short_name});
}

import_result import_installed_games(
        const fs::path& source, const fs::path& destination,
        const std::vector<std::string>& short_names) {
    import_result result;
    std::error_code error;
    if (!fs::is_directory(source, error)) {
        result.message = "The media source is unavailable: " + source.string();
        return result;
    }
    error.clear();
    fs::create_directories(destination, error);
    if (error) {
        result.message = "Could not create the local media folder: " +
                         error.message();
        return result;
    }

    const fs::path source_key = fs::weakly_canonical(source, error);
    error.clear();
    const fs::path destination_key = fs::weakly_canonical(destination, error);
    if (!error && path_is_within(destination_key, source_key)) {
        result.message = "The local media folder cannot be inside its source.";
        return result;
    }

    std::set<std::string> games;
    for (const std::string& short_name : short_names)
        if (!short_name.empty()) games.insert(lowercase(short_name));
    if (games.empty()) {
        result.message = "No installed ROM names were available to match.";
        return result;
    }
    std::set<std::string> match_names = games;
    for (const auto& [game, alias] : media_aliases())
        if (games.count(game)) match_names.insert(alias);

    result.descriptions_imported =
        import_gamelist_descriptions(source, destination, games);

    std::set<std::string> matched_games;
    fs::recursive_directory_iterator current(
        source, fs::directory_options::skip_permission_denied, error);
    const fs::recursive_directory_iterator end;
    if (error) {
        result.message = "Could not read the media source: " + error.message();
        return result;
    }
    for (; current != end; current.increment(error)) {
        if (error) {
            error.clear();
            continue;
        }
        const fs::directory_entry& entry = *current;
        const fs::path relative = entry.path().lexically_relative(source);
        if (relative.empty()) continue;
        if (entry.is_directory(error)) {
            const bool created =
                fs::create_directories(destination / relative, error);
            if (!error && created) ++result.directories_created;
            error.clear();
            continue;
        }
        if (!entry.is_regular_file(error)) {
            error.clear();
            continue;
        }

        std::string matched;
        const std::string stem = lowercase(entry.path().stem().string());
        if (match_names.count(stem)) matched = stem;
        const bool wanted = !matched.empty() ||
            stem_matches(entry.path(), match_names) ||
            inside_game_directory(relative, match_names, matched);
        if (!wanted) continue;
        if (matched.empty()) {
            for (const std::string& game : match_names) {
                if (stem.compare(0, game.size(), game) == 0) {
                    matched = game;
                    break;
                }
            }
        }

        const fs::path target = destination / relative;
        fs::create_directories(target.parent_path(), error);
        if (error) {
            error.clear();
            continue;
        }
        fs::copy_file(entry.path(), target,
                      fs::copy_options::overwrite_existing, error);
        if (error) {
            error.clear();
            continue;
        }
        ++result.files_copied;
        result.bytes_copied += entry.file_size(error);
        error.clear();
        if (!matched.empty()) {
            const std::string canonical = canonical_game(matched, games);
            if (!canonical.empty()) matched_games.insert(canonical);
        }
    }

    result.games_matched = matched_games.size();
    result.success = true;
    result.message = "Imported " + std::to_string(result.files_copied) +
        " media file(s) for " + std::to_string(result.games_matched) +
        " installed game(s), plus " +
        std::to_string(result.descriptions_imported) +
        " game description(s).";
    return result;
}

} // namespace manx_media
