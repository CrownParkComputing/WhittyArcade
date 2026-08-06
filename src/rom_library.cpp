#include "rom_library.h"
#include "game_plugin_host.h"

#include "arcade_catalog.h"
#include "arcade_settings.h"
#include "namco/galaxian/galaxian_rom.h"
#include "capcom/gng/gng_rom.h"
#include "sega/model1/model1_rom.h"
#include "sega/model2/model2_rom.h"
#include "namco/namco_rom.h"
#include "platform_paths.h"
#include "sega/system16b/system16b_rom.h"
#include "taito/taitoz/taitoz_rom.h"
#include "midway/midway_rom.h"
#include "namco/system22/system22_rom.h"
#include "system246_rom.h"

#include <minizip/unzip.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <unordered_map>
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
    const fs::path root = manx_platform::data_root();
    return root.empty() ? fs::current_path() : root;
}

struct identified_archive {
    const rom_set_manifest* manifest{};
    arcade_board_type board{arcade_board_type::system22};
    bool companion{};
    bool disc{};
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
    companion_archive{"shinobi", arcade_board_type::system16b,
                      "shinobi.zip (Shinobi merged parent)"},
    companion_archive{"shinobi6", arcade_board_type::system16b,
                      "shinobi6.zip (Shinobi sound companion)"},
};

// Legacy/descriptive filenames accepted while scanning collections. Archive
// contents still have to pass an owning loader's probe before import.
constexpr std::array collection_aliases{
    "ridgera28", "virtua_formula", "virtua_fighter",
    "star_wars_arcade", "wing_war",
    // Merged collections name the archive after the PARENT set; the games
    // MANX ships from inside them can be a specific revision with a
    // different short name (manxtt.zip holds the Twin-mode manxttc).
    "manxtt",
};

identified_archive identify_archive(const fs::path& path) {
    if (lower(path.extension().string()) == ".chd") {
        const system246_disc_info disc =
            system246_rom_loader::inspect_disc(path.string());
        if (disc)
            return {nullptr, arcade_board_type::system246, true, true,
                    "rrv1-a.chd (Ridge Racer V disc)"};
        return {};
    }
    const std::string value = path.string();
    if (const auto identity = identify_arcade_game(value))
        return {find_supported_rom_set(identity->short_name), identity->board,
                false, false, {}};

    const std::string stem = lower(path.stem().string());
    const auto companion = std::find_if(
        companion_archives.begin(), companion_archives.end(),
        [&stem](const companion_archive& item) {
            return stem == item.short_name;
        });
    if (companion != companion_archives.end())
        return {nullptr, companion->board, true, false, companion->label};
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
        const std::string short_name(manifest.short_name);
        if (short_name != "timecris" && short_name != "dirtdash" &&
            short_name != "aquajet" &&
            !sibling_exists(candidate, "namcoc74.zip") &&
            !archive_contains(candidate, "c74.bin"))
            missing.emplace_back("namcoc74.zip");
    } else if (manifest.board == arcade_board_type::system246) {
        // System 246/256 games boot the PCSX2 arcade core from a "<name>.acgame"
        // manifest (RRV, MotoGP, ...). A game is ready when that manifest and
        // the files it references are present in its directory.
        std::string missing_item;
        if (system246_rom_loader::acgame_ready(candidate.string(),
                                               &missing_item)) {
            // Ready: nothing to add.
        } else if (!system246_rom_loader::find_disc_path(
                       candidate.string(), chd_library_path()).empty() &&
                   system246_rom_loader::inspect_disc(
                       system246_rom_loader::find_disc_path(
                           candidate.string(), chd_library_path()))) {
            // Legacy MAME RRV set (dongle + rrv1-a.chd) with no PCSX2 manifest
            // still counts as ready.
        } else {
            missing.emplace_back(missing_item.empty() ? "<game>.acgame"
                                                      : missing_item);
        }
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

bool chd_extension(const fs::path& path) {
    return lower(path.extension().string()) == ".chd";
}

bool known_collection_filename(const fs::path& path) {
    if (chd_extension(path))
        return lower(path.filename().string()) == "rrv1-a.chd" || lower(path.filename().string()) == "kinst.chd" || lower(path.filename().string()) == "kinst2.chd";
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

bool eight_hex_digits(const std::string& name) {
    return name.size() == 8 &&
           std::all_of(name.begin(), name.end(), [](unsigned char character) {
               return std::isxdigit(character) != 0;
           });
}

// Xbox 360 titles that were never extracted are single signed STFS packages,
// stored the way the console stores them and the way a download arrives:
//     <anywhere>/<TITLE ID>/<content type>/<content hash>
// with both directory names eight hex digits and the leaf file named after the
// SHA-1 of its content. Nothing in the path or the filename says which game it
// is - only the package's own header does - so this collects the files that
// could be packages and lets the loader identify them.
//
// Directories are walked to a bounded depth and files are only listed where that
// layout matches (or directly in a scan root, so a package dropped in the ROM
// folder is still found). Pointing this at a large Downloads tree therefore
// costs one directory read per directory and nothing else.
void collect_package_candidates(const fs::path& directory, int depth,
                                bool list_files,
                                std::vector<fs::path>& candidates) {
    std::error_code error;
    if (!fs::is_directory(directory, error)) return;
    const bool content_directory =
        eight_hex_digits(directory.filename().string()) &&
        eight_hex_digits(directory.parent_path().filename().string());
    list_files = list_files || content_directory;
    for (fs::directory_iterator iterator(
             directory, fs::directory_options::skip_permission_denied, error),
         end; !error && iterator != end; iterator.increment(error)) {
        std::error_code type_error;
        if (iterator->is_directory(type_error)) {
            // A content-type directory holds the package and nothing below it.
            if (!content_directory && depth > 0)
                collect_package_candidates(iterator->path(), depth - 1, false,
                                           candidates);
        } else if (list_files && iterator->is_regular_file(type_error)) {
            candidates.push_back(iterator->path());
        }
    }
}

} // namespace

std::string rom_library_path() {
    const emulator_settings settings = load_settings();
    return settings.rom_directory.empty() ?
        (data_root() / "MANX" / "roms").string() :
        normalized_path(settings.rom_directory);
}

std::string chd_library_path() {
    const emulator_settings settings = load_settings();
    return settings.chd_directory.empty() ?
        (data_root() / "MANX" / "chd").string() :
        normalized_path(settings.chd_directory);
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
        // The game's name, not its filename. Every set appears exactly once
        // - seen_sets above makes sure of it - so the zip was never telling
        // anybody which of two entries was which; it was just "galaxian.zip"
        // written after "Galaxian" on every line of the list.
        std::string label = identified.manifest->display_name;
        label += readiness_suffix(candidate, *identified.manifest);
        if (normalized == normalized_current) label += "  [current]";
        choices.push_back({normalized, std::move(label),
                           identified.board,
                           identified.manifest->publisher});
    };

    const fs::path current(current_path);
    add(current, false);
    std::error_code error;
    const fs::path current_directory = fs::is_directory(current, error) ?
        current : current.parent_path();

    auto scan = [&](const fs::path& directory, bool recursive, bool prefer,
                    bool known_names_only) {
        error.clear();
        if (directory.empty() || !fs::is_directory(directory, error)) return;
        auto consider = [&](const fs::directory_entry& entry) {
            std::error_code type_error;
            if (entry.is_regular_file(type_error) &&
                zip_extension(entry.path()) &&
                (!known_names_only ||
                 known_collection_filename(entry.path())))
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

    // Extracted sets are valid loader inputs too. Common ROM roots usually
    // contain games directly, while MANX's durable library groups
    // them below a board directory. Probe only those two shallow layouts so
    // an extracted game's large media tree is never walked as a set of
    // independent candidates.
    auto scan_extracted = [&](const fs::path& directory, bool prefer) {
        error.clear();
        if (directory.empty() || !fs::is_directory(directory, error)) return;
        fs::directory_iterator iterator(directory, error), end;
        while (!error && iterator != end) {
            std::error_code type_error;
            if (iterator->is_directory(type_error)) {
                const fs::path child = iterator->path();
                const std::string child_name = lower(
                    child.filename().string());
                const bool board_directory = std::any_of(
                    arcade_boards().begin(), arcade_boards().end(),
                    [&](const arcade_board_descriptor& board) {
                        return child_name == lower(board.rom_directory);
                    });
                if (board_directory) {
                    std::error_code child_error;
                    fs::directory_iterator child_iterator(child, child_error),
                                           child_end;
                    while (!child_error && child_iterator != child_end) {
                        std::error_code child_type_error;
                        if (child_iterator->is_directory(child_type_error))
                            add(child_iterator->path(), prefer);
                        child_iterator.increment(child_error);
                    }
                } else {
                    add(child, prefer);
                }
            }
            iterator.increment(error);
        }
    };

    // Make sure the folders exist so the user knows where to drop files, then
    // read the ROM folder directly. Nothing is imported, copied, or scanned
    // from anywhere else.
    fs::create_directories(rom_library_path(), error);
    fs::create_directories(chd_library_path(), error);
    scan(current_directory, false, false, false);
    scan(rom_library_path(), true, true, true);
    scan_extracted(rom_library_path(), true);

    // System 246/256 games are PCSX2 `.acgame` manifests living in the pcsx2x6
    // arcade roms tree, not MAME archives in the library folders -- the scans
    // above never see them. Surface each game directory's `<name>.acgame` as a
    // selectable choice, deduped against anything already found in the library
    // (e.g. Ridge Racer V via its MAME zip).
    {
        // Overridable so tests can point it at an empty path and stay isolated
        // from the machine's real PCSX2 arcade tree.
        const char* s246_env = std::getenv("MANX_SYSTEM246_ACGAME_ROOT");
        const fs::path s246_roms(s246_env && *s246_env ? s246_env :
                                 "/home/jon/pcsx2x6/build/bin/roms");
        std::error_code s246_ec;
        if (fs::is_directory(s246_roms, s246_ec)) {
            for (fs::directory_iterator it(s246_roms, s246_ec), end;
                 !s246_ec && it != end; it.increment(s246_ec)) {
                std::error_code de;
                if (!it->is_directory(de)) continue;
                fs::path acgame;
                for (fs::directory_iterator f(it->path(), de), fend;
                     !de && f != fend; f.increment(de)) {
                    if (f->is_regular_file(de) &&
                        lower(f->path().extension().string()) == ".acgame") {
                        acgame = f->path();
                        break;
                    }
                }
                if (acgame.empty()) continue;
                const std::string apath = acgame.string();

                // Identity for dedup: a KNOWN set reuses its canonical short
                // name so this entry collapses with the same game found as a
                // MAME zip in the library above (e.g. Ridge Racer V). An
                // UNKNOWN/new collection game keys off the manifest basename so
                // it still appears -- the board boots any .acgame directly, so
                // there is no per-title code to add. This is what makes the
                // board scale to the whole PCSX2X6 collection.
                const system246_rom_set set =
                    system246_rom_loader::identify_set(apath);
                const char* known_sn =
                    system246_rom_loader::set_short_name(set);
                const bool known =
                    set != system246_rom_set::unknown && known_sn && *known_sn &&
                    std::string(known_sn) != "unknown";
                const std::string identity =
                    known ? std::string(known_sn) : acgame.stem().string();
                if (identity.empty()) continue;
                if (!seen_sets.insert(identity).second) continue;

                // Display name: known sets use their curated title; unknown
                // games read the `name =` field from the .acgame manifest,
                // falling back to the basename.
                std::string label;
                if (known) {
                    label = system246_rom_loader::set_display_name(set);
                } else {
                    label = acgame.stem().string();
                    std::ifstream manifest(acgame);
                    std::string line;
                    while (std::getline(manifest, line)) {
                        const auto eq = line.find('=');
                        if (eq == std::string::npos) continue;
                        auto trim = [](std::string s) {
                            const auto b = s.find_first_not_of(" \t\r\n");
                            if (b == std::string::npos) return std::string();
                            const auto e = s.find_last_not_of(" \t\r\n");
                            return s.substr(b, e - b + 1);
                        };
                        if (trim(line.substr(0, eq)) == "name") {
                            std::string v = trim(line.substr(eq + 1));
                            if (!v.empty()) label = v;
                            break;
                        }
                    }
                }
                std::string missing;
                label += system246_rom_loader::acgame_ready(apath, &missing) ?
                    "  [ready]" : "  [missing files]";
                choices.push_back({apath, std::move(label),
                                   arcade_board_type::system246, "Namco"});
            }
        }
    }

    // Installed game plugins. They are not archives and no loader would claim
    // them, so they are offered directly from what discovery already found -
    // their bundle folder is the path, which is what identify_arcade_game
    // resolves back to a game.
    for (const rom_set_manifest& manifest : supported_rom_sets()) {
        if (manifest.board != arcade_board_type::game_plugin) continue;
        const discovered_game* plugin = find_plugin_game(manifest.short_name);
        if (plugin == nullptr) continue;
        const std::string normalized = normalized_path(plugin->bundle_path);
        if (!seen_paths.insert(normalized).second) continue;
        std::string label = manifest.display_name;
        label += "  (installed)";
        if (normalized == normalized_current) label += "  [current]";
        choices.push_back({normalized, std::move(label),
                           arcade_board_type::game_plugin,
                           plugin->publisher});
    }

    std::sort(choices.begin(), choices.end(),
              [](const rom_choice& left, const rom_choice& right) {
                  if (left.board != right.board) return left.board < right.board;
                  return left.label < right.label;
              });
    return choices;
}

rom_audit_result audit_rom_path(const std::string& path) {
    // A game plugin has no ROM set to audit. Discovery already opened its
    // library, checked the ABI and asked it to describe itself, so reaching
    // here at all means it is ready - reporting "audit failed" would mark a
    // working game as broken purely because it has no archive.
    if (const discovered_game* plugin = find_plugin_game(path))
        return {true, plugin->short_name, "Installed game plugin."};

    // System 246/256 collection games are ".acgame" manifests with no catalog
    // entry -- the board boots any of them, so there is nothing per-title to
    // look up. Audit them against the manifest's own file list instead of
    // reporting every one as an unsupported set.
    if (lower(fs::path(path).extension().string()) == ".acgame") {
        const std::string short_name =
            system246_rom_loader::acgame_short_name(path);
        std::string missing;
        if (!system246_rom_loader::acgame_ready(path, &missing))
            return {false, short_name, "Missing " + missing + "."};
        return {true, short_name, {}};
    }
    const identified_archive identified = identify_archive(fs::path(path));
    if (!identified.manifest)
        return {false, {}, "Archive is not a supported MANX set."};
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
    case arcade_board_type::system246: {
        // PCSX2 .acgame-based games (RRV, MotoGP) validate through their
        // manifest; fall back to the legacy MAME RRV load for a dongle+disc
        // set that carries no manifest.
        std::string missing_item;
        if (system246_rom_loader::acgame_ready(path, &missing_item)) {
            valid = true;
        } else {
            const system246_rom_load_result loaded =
                system246_rom_loader::load(path, chd_library_path());
            valid = static_cast<bool>(loaded);
            loader_error = !loaded.error.empty() ? loaded.error :
                (missing_item.empty() ? std::string() :
                     "Missing " + missing_item);
        }
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
    case arcade_board_type::galaxian: {
        const galaxian_rom_load_result loaded = galaxian_rom_loader::load(path);
        valid = static_cast<bool>(loaded);
        loader_error = loaded.error;
        break;
    }
    case arcade_board_type::system16b: {
        const system16b::system16b_rom_load_result loaded =
            system16b::system16b_rom_loader::load(path);
        const auto populated = [](const auto& bytes) {
            return std::any_of(bytes.begin(), bytes.end(),
                               [](uint8_t value) {
                                   return value != 0 && value != 0xff;
                               });
        };
        // Dynamite Dux and Wonder Boy III have no uPD7759 sample chip at
        // all, so demanding sample data of them reported a complete set as
        // broken. Ask the loader which sets actually carry samples.
        const bool wants_samples =
            system16b::system16b_rom_loader::set_has_sample_rom(loaded.set);
        valid = static_cast<bool>(loaded) && populated(loaded.roms.sprite_gfx) &&
                populated(loaded.roms.sound_prog) &&
                (!wants_samples || populated(loaded.roms.sound_data));
        loader_error = loaded.error;
        if (!valid && loader_error.empty())
            loader_error = "Gameplay ROMs were found, but sprite or "
                           "unencrypted sound ROM data is incomplete.";
        break;
    }
    case arcade_board_type::capcom_gng: {
        const gng::rom_load_result loaded = gng::rom_loader::load(path);
        valid = static_cast<bool>(loaded);
        loader_error = loaded.error;
        break;
    }
    case arcade_board_type::namco_galaga:
    case arcade_board_type::namco_system1: {
        const namco::load_result loaded = namco::rom_loader::load(path);
        valid = static_cast<bool>(loaded);
        loader_error = loaded.error;
        break;
    }
    case arcade_board_type::taito_z: {
        const taitoz::taitoz_rom_load_result loaded =
            taitoz::taitoz_rom_loader::load(path);
        valid = static_cast<bool>(loaded);
        loader_error = loaded.error;
        break;
    }
    case arcade_board_type::midway: {
        const midway_rom_load_result loaded =
            midway_rom_loader::load(path, chd_library_path());
        valid = static_cast<bool>(loaded);
        loader_error = loaded.error;
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

std::string required_rom_sets_text() {
    std::ostringstream text;
    text << "Use current MAME ZIP/CHD files. MANX keeps them "
            "unchanged; merged, split and non-merged layouts are accepted.\n";
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
