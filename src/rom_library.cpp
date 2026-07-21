#include "rom_library.h"

#include "arcade_catalog.h"
#include "galaxian_rom.h"
#include "model1_rom.h"
#include "model2_rom.h"
#include "shinobi_rom.h"
#include "system22_rom.h"

#include <minizip/unzip.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <system_error>
#include <unordered_set>

namespace fs = std::filesystem;

namespace {

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return value;
}

std::string normalized_path(const fs::path& path) {
    std::error_code error;
    fs::path absolute = fs::absolute(path, error);
    return (error ? path : absolute).lexically_normal().string();
}

fs::path data_root() {
    if (const char* data_home = std::getenv("XDG_DATA_HOME"))
        return fs::path(data_home);
    if (const char* user_home = std::getenv("HOME"))
        return fs::path(user_home) / ".local" / "share";
    return fs::current_path();
}

struct identified_archive {
    const rom_set_manifest* manifest{};
    arcade_board_type board{arcade_board_type::system22};
    bool companion{};
    std::string companion_label;
};

struct companion_archive {
    const char* short_name;
    arcade_board_type board;
    const char* label;
};

// Shared/device archives are importable but do not represent a playable game.
// Keeping this as data avoids duplicating their routing in every importer.
constexpr std::array companion_archives{
    companion_archive{"namcoc71", arcade_board_type::system22,
                      "namcoc71.zip (C71 device firmware)"},
    companion_archive{"namcoc74", arcade_board_type::system22,
                      "namcoc74.zip (C74 device firmware)"},
    companion_archive{"vr", arcade_board_type::model1,
                      "vr.zip (vformula parent)"},
    companion_archive{"segabill", arcade_board_type::model2,
                      "segabill.zip (optional billboard CPU)"},
    companion_archive{"shinobi", arcade_board_type::shinobi,
                      "shinobi.zip (Shinobi merged parent)"},
    companion_archive{"shinobi6", arcade_board_type::shinobi,
                      "shinobi6.zip (Shinobi sound companion)"},
};

// Legacy/descriptive filenames accepted while scanning collections. Archive
// contents still have to pass an owning loader's probe before import.
constexpr std::array collection_aliases{
    "ridgera28", "virtua_formula", "virtua_fighter",
    "star_wars_arcade", "wing_war",
};

identified_archive identify_archive(const fs::path& path) {
    const std::string value = path.string();
    if (const auto identity = identify_arcade_game(value))
        return {find_supported_rom_set(identity->short_name), identity->board,
                false, {}};

    const std::string stem = lower(path.stem().string());
    const auto companion = std::find_if(
        companion_archives.begin(), companion_archives.end(),
        [&stem](const companion_archive& item) {
            return stem == item.short_name;
        });
    if (companion != companion_archives.end())
        return {nullptr, companion->board, true, companion->label};
    return {};
}

bool archive_contains(const fs::path& archive_path, const char* basename) {
    unzFile archive = unzOpen64(archive_path.string().c_str());
    if (!archive) return false;
    const std::string wanted = lower(basename);
    bool found = false;
    if (unzGoToFirstFile(archive) == UNZ_OK) {
        do {
            std::array<char, 1024> name{};
            if (unzGetCurrentFileInfo64(archive, nullptr, name.data(),
                                        name.size(), nullptr, 0, nullptr, 0) !=
                    UNZ_OK)
                break;
            if (lower(fs::path(name.data()).filename().string()) == wanted) {
                found = true;
                break;
            }
        } while (unzGoToNextFile(archive) == UNZ_OK);
    }
    unzClose(archive);
    return found;
}

bool sibling_exists(const fs::path& selected, const std::string& filename) {
    const fs::path directory = fs::is_directory(selected) ?
        selected : selected.parent_path();
    std::error_code error;
    if (!fs::is_directory(directory, error)) return false;
    const std::string wanted = lower(filename);
    for (fs::directory_iterator iterator(directory, error), end;
         !error && iterator != end; iterator.increment(error)) {
        if (lower(iterator->path().filename().string()) == wanted) return true;
    }
    return false;
}

std::string readiness_suffix(const fs::path& candidate,
                             const rom_set_manifest& manifest) {
    std::vector<std::string> missing;
    if (manifest.board == arcade_board_type::system22) {
        if (!sibling_exists(candidate, "namcoc71.zip") &&
            !archive_contains(candidate, "c71.bin"))
            missing.emplace_back("namcoc71.zip");
        if (std::string(manifest.short_name) != "timecris" &&
            !sibling_exists(candidate, "namcoc74.zip") &&
            !archive_contains(candidate, "c74.bin"))
            missing.emplace_back("namcoc74.zip");
    } else if (std::string(manifest.short_name) == "vformula" &&
               !archive_contains(candidate, "mpr-14890.26") &&
               !sibling_exists(candidate, "vr.zip")) {
        missing.emplace_back("vr.zip (split parent)");
    } else if (std::string(manifest.short_name) == "shinobi4" &&
               !archive_contains(candidate, "epr-11361.a10") &&
               !sibling_exists(candidate, "shinobi6.zip") &&
               lower(candidate.filename().string()) != "shinobi.zip") {
        missing.emplace_back("shinobi6.zip (sound)");
    }
    if (missing.empty()) return manifest.working ? "  [ready]" : "  [not working]";
    std::string result = "  [needs ";
    for (std::size_t index = 0; index < missing.size(); ++index) {
        if (index) result += ", ";
        result += missing[index];
    }
    result += "]";
    return result;
}

bool zip_extension(const fs::path& path) {
    return lower(path.extension().string()) == ".zip";
}

bool known_collection_filename(const fs::path& path) {
    const std::string stem = lower(path.stem().string());
    const auto& games = supported_rom_sets();
    if (std::any_of(games.begin(), games.end(),
                    [&stem](const rom_set_manifest& item) {
                        return stem == item.short_name;
                    }))
        return true;
    if (std::any_of(companion_archives.begin(), companion_archives.end(),
                    [&stem](const companion_archive& item) {
                        return stem == item.short_name;
                    }))
        return true;
    return std::find(collection_aliases.begin(), collection_aliases.end(),
                     stem) != collection_aliases.end();
}

bool copy_archive(const fs::path& source, arcade_board_type board,
                  const std::string& destination_filename,
                  fs::path& destination, std::string& error_text) {
    destination = data_root() / "WhittyArcade" / "roms" /
                  arcade_board(board).rom_directory /
                  destination_filename;
    std::error_code error;
    fs::create_directories(destination.parent_path(), error);
    if (error) {
        error_text = "Could not create ROM library directory: " +
                     error.message();
        return false;
    }
    if (normalized_path(source) == normalized_path(destination)) return true;
    fs::copy_file(source, destination, fs::copy_options::overwrite_existing,
                  error);
    if (error) {
        error_text = "Could not import " + source.string() + ": " +
                     error.message();
        return false;
    }
    return true;
}

std::vector<fs::path> candidates_from(const fs::path& source,
                                      std::size_t& scanned,
                                      std::string& error_text) {
    std::vector<fs::path> result;
    std::error_code error;
    if (fs::is_regular_file(source, error)) {
        if (!zip_extension(source)) {
            error_text = "Only ZIP archives or directories can be imported: " +
                         source.string();
            return result;
        }
        ++scanned;
        result.push_back(source);
        return result;
    }
    if (!fs::is_directory(source, error)) {
        error_text = "Import path does not exist: " + source.string();
        return result;
    }
    fs::recursive_directory_iterator iterator(
        source, fs::directory_options::skip_permission_denied, error), end;
    while (!error && iterator != end) {
        std::error_code type_error;
        if (iterator->is_regular_file(type_error) &&
            zip_extension(iterator->path())) {
            ++scanned;
            // A directory may be a complete MAME collection containing many
            // thousands of games. Probe only names this build can consume.
            if (known_collection_filename(iterator->path()))
                result.push_back(iterator->path());
        }
        iterator.increment(error);
    }
    if (error)
        error_text = "ROM directory scan stopped early: " + error.message();
    return result;
}

} // namespace

std::string rom_library_path() {
    return (data_root() / "WhittyArcade" / "roms").string();
}

std::vector<rom_choice> discover_library_roms(const std::string& current_path) {
    std::vector<rom_choice> choices;
    std::unordered_set<std::string> seen_paths;
    std::unordered_set<std::string> seen_sets;
    const std::string normalized_current = normalized_path(current_path);

    auto add = [&](const fs::path& candidate, bool prefer) {
        std::error_code error;
        if (!fs::is_directory(candidate, error) &&
            (!fs::is_regular_file(candidate, error) || !zip_extension(candidate)))
            return;
        const std::string normalized = normalized_path(candidate);
        if (!seen_paths.insert(normalized).second) return;
        const identified_archive identified = identify_archive(candidate);
        if (!identified.manifest) return;
        const std::string identity = identified.manifest->short_name;
        if (seen_sets.count(identity) && !prefer) return;
        if (prefer && seen_sets.count(identity)) {
            choices.erase(std::remove_if(choices.begin(), choices.end(),
                [&](const rom_choice& choice) {
                    const identified_archive old = identify_archive(choice.path);
                    return old.manifest &&
                           std::string(old.manifest->short_name) == identity;
                }), choices.end());
        }
        seen_sets.insert(identity);
        std::string label = identified.manifest->display_name;
        label += "  (" + candidate.filename().string() + ")";
        label += readiness_suffix(candidate, *identified.manifest);
        if (normalized == normalized_current) label += "  [current]";
        choices.push_back({normalized, std::move(label), identified.board});
    };

    const fs::path current(current_path);
    add(current, false);
    std::error_code error;
    const fs::path current_directory = fs::is_directory(current, error) ?
        current : current.parent_path();

    auto scan = [&](const fs::path& directory, bool recursive, bool prefer) {
        error.clear();
        if (directory.empty() || !fs::is_directory(directory, error)) return;
        auto consider = [&](const fs::directory_entry& entry) {
            std::error_code type_error;
            if (entry.is_regular_file(type_error) && zip_extension(entry.path()))
                add(entry.path(), prefer);
        };
        if (recursive) {
            fs::recursive_directory_iterator iterator(
                directory, fs::directory_options::skip_permission_denied,
                error), end;
            while (!error && iterator != end) {
                consider(*iterator);
                iterator.increment(error);
            }
        } else {
            fs::directory_iterator iterator(directory, error), end;
            while (!error && iterator != end) {
                consider(*iterator);
                iterator.increment(error);
            }
        }
    };

    scan(current_directory, false, false);
    // Imported copies win over transitory downloads and command-line peers.
    scan(rom_library_path(), true, true);
    if (!std::getenv("WHITTYARCADE_NO_LEGACY_ROM_SCAN")) {
        if (const char* user_home = std::getenv("HOME")) {
            const fs::path downloads = fs::path(user_home) / "Downloads";
            scan(downloads / "WhittyArcade-Roms", true, false);
            scan(downloads, false, false);
        }
    }
    if (const char* search_path = std::getenv("WHITTYARCADE_ROM_PATH")) {
        std::stringstream paths(search_path);
        std::string path;
        while (std::getline(paths, path, ':'))
            if (!path.empty()) scan(path, true, false);
    }

    std::sort(choices.begin(), choices.end(),
              [](const rom_choice& left, const rom_choice& right) {
                  if (left.board != right.board) return left.board < right.board;
                  return left.label < right.label;
              });
    return choices;
}

rom_import_result import_rom_path(const std::string& source_path) {
    return import_rom_paths({source_path});
}

rom_import_result import_rom_paths(const std::vector<std::string>& source_paths) {
    rom_import_result result;
    std::vector<std::pair<std::string, fs::path>> imported_games;
    for (const std::string& source_text : source_paths) {
        std::string scan_error;
        const std::vector<fs::path> candidates = candidates_from(
            fs::path(source_text), result.archives_scanned, scan_error);
        if (!scan_error.empty()) {
            if (!result.error.empty()) result.error += '\n';
            result.error += scan_error;
        }
        for (const fs::path& candidate : candidates) {
            const identified_archive identified = identify_archive(candidate);
            if (!identified.manifest && !identified.companion) continue;
            if (identified.manifest) ++result.games_found;
            fs::path destination;
            std::string copy_error;
            const std::string destination_filename = identified.manifest ?
                std::string(identified.manifest->short_name) + ".zip" :
                lower(candidate.filename().string());
            if (!copy_archive(candidate, identified.board,
                              destination_filename, destination,
                              copy_error)) {
                if (!result.error.empty()) result.error += '\n';
                result.error += copy_error;
                continue;
            }
            ++result.archives_imported;
            const std::string label = identified.manifest ?
                identified.manifest->display_name : identified.companion_label;
            result.details.push_back(label + " -> " + destination.string());
            if (identified.manifest)
                imported_games.emplace_back(identified.manifest->short_name,
                                             destination);
        }
    }
    std::sort(imported_games.begin(), imported_games.end(),
              [](const auto& left, const auto& right) {
                  return left.second.string() < right.second.string();
              });
    imported_games.erase(std::unique(
        imported_games.begin(), imported_games.end(),
        [](const auto& left, const auto& right) {
            return left.second == right.second;
        }), imported_games.end());
    for (const auto& [short_name, path] : imported_games) {
        const rom_audit_result audit = audit_rom_path(path.string());
        result.details.push_back(std::string(audit.success ? "Audit OK: " :
                                                              "Audit failed: ") +
                                 short_name + " - " + audit.message);
        if (!audit.success) {
            if (!result.error.empty()) result.error += '\n';
            result.error += short_name + ": " + audit.message;
        }
    }
    return result;
}

rom_audit_result audit_rom_path(const std::string& path) {
    const identified_archive identified = identify_archive(fs::path(path));
    if (!identified.manifest)
        return {false, {}, "Archive is not a supported WhittyArcade set."};
    const rom_set_manifest& manifest = *identified.manifest;
    if (!manifest.working)
        return {false, manifest.short_name,
                "Set is recognised but marked not working."};

    bool valid = false;
    std::string loader_error;
    switch (manifest.board) {
    case arcade_board_type::system22: {
        const ridge_racer_roms roms = rom_loader::load_ridge_racer(
            path, fs::path(path).parent_path().string());
        valid = roms.has_complete_program() && !roms.mcu_rom.empty() &&
                !roms.texture_rom.empty() && !roms.tilemap_rom.empty() &&
                !roms.point_rom.empty() && !roms.c352_samples.empty() &&
                (roms.super_system22 ? !roms.sprite_rom.empty() :
                                       !roms.gamma_proms.empty()) &&
                roms.has_c71_firmware() && roms.has_mcu_firmware();
        if (!valid)
            loader_error = "One or more game ROMs or required device firmware "
                           "archives are missing or invalid.";
        break;
    }
    case arcade_board_type::model1: {
        const model1_rom_load_result loaded = model1_rom_loader::load(path);
        valid = static_cast<bool>(loaded);
        loader_error = loaded.error;
        break;
    }
    case arcade_board_type::model2: {
        const model2_rom_load_result loaded = model2_rom_loader::load(path);
        valid = static_cast<bool>(loaded);
        loader_error = loaded.error;
        break;
    }
    case arcade_board_type::phoenix:
    case arcade_board_type::mooncrst: {
        const galaxian_rom_load_result loaded = galaxian_rom_loader::load(path);
        valid = static_cast<bool>(loaded);
        loader_error = loaded.error;
        break;
    }
    case arcade_board_type::shinobi: {
        const shinobi::shinobi_rom_load_result loaded =
            shinobi::shinobi_rom_loader::load(path);
        const auto populated = [](const auto& bytes) {
            return std::any_of(bytes.begin(), bytes.end(),
                               [](uint8_t value) {
                                   return value != 0 && value != 0xff;
                               });
        };
        valid = static_cast<bool>(loaded) && populated(loaded.roms.sprite_gfx) &&
                populated(loaded.roms.sound_prog) &&
                populated(loaded.roms.sound_data);
        loader_error = loaded.error;
        if (!valid && loader_error.empty())
            loader_error = "Gameplay ROMs were found, but sprite or "
                           "unencrypted sound ROM data is incomplete.";
        break;
    }
    }
    if (!valid) {
        if (loader_error.empty()) loader_error = "ROM audit failed.";
        while (!loader_error.empty() && loader_error.back() == '\n')
            loader_error.pop_back();
        return {false, manifest.short_name, std::move(loader_error)};
    }
    return {true, manifest.short_name,
            "All required entries passed this loader's validation checks."};
}

std::string rom_import_result::summary() const {
    std::ostringstream text;
    text << "Scanned " << archives_scanned << " ZIP archive";
    if (archives_scanned != 1) text << 's';
    text << ".\nFound " << games_found << " supported game";
    if (games_found != 1) text << 's';
    text << " and imported " << archives_imported << " archive";
    if (archives_imported != 1) text << 's';
    text << ".";
    if (!error.empty()) text << "\n\n" << error;
    return text.str();
}

std::string required_rom_sets_text() {
    std::ostringstream text;
    text << "Use current MAME ZIP archives. WhittyArcade keeps them unchanged; "
            "merged, split and non-merged layouts are accepted.\n";
    for (const arcade_board_descriptor& board : arcade_boards()) {
        text << '\n' << board.display_name << ":\n";
        for (const rom_set_manifest& manifest : supported_rom_sets()) {
            if (manifest.board != board.type) continue;
            text << "  " << manifest.short_name << " - "
                 << manifest.display_name;
            if (!manifest.working) text << " [not working]";
            if (manifest.split_parent[0] != '\0')
                text << "\n    Split parent: " << manifest.split_parent;
            if (manifest.extra_archives[0] != '\0')
                text << "\n    Additional: " << manifest.extra_archives;
            text << '\n';
        }
    }
    text << "\nIn a non-merged collection each game ZIP is self-contained, "
            "so split parent companions are not needed.";
    return text.str();
}
