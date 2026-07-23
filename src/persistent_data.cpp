#include "persistent_data.h"
#include "arcade_catalog.h"
#include "sega/model1/model1_cabinet.h"
#include "platform_paths.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <utility>

namespace fs = std::filesystem;

namespace {

fs::path config_root() {
    const fs::path root = whitty_platform::config_root();
    return root.empty() ? fs::current_path() : root;
}

fs::path nvram_root() {
    return config_root() / "WhittyArcade" / "nvram";
}

fs::path system246_save_root() {
    fs::path root = whitty_platform::data_root();
    if (root.empty()) root = fs::current_path();
    return root / "WhittyArcade" / "play" / "arcadesaves";
}

std::string timestamp(bool filename_safe = false) {
    const std::time_t now = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    std::ostringstream value;
    value << std::put_time(&local, filename_safe ? "%Y%m%d-%H%M%S" :
                                                   "%Y-%m-%d %H:%M:%S");
    return value.str();
}

std::string file_modified(const fs::path& path) {
    std::error_code error;
    const auto file_time = fs::last_write_time(path, error);
    if (error) return "unknown";
    const auto system_time = std::chrono::time_point_cast<
        std::chrono::system_clock::duration>(
        file_time - fs::file_time_type::clock::now() +
        std::chrono::system_clock::now());
    const std::time_t value = std::chrono::system_clock::to_time_t(system_time);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &value);
#else
    localtime_r(&value, &local);
#endif
    std::ostringstream text;
    text << std::put_time(&local, "%Y-%m-%d %H:%M");
    return text.str();
}

persistent_file_info make_file(const char* key, const char* label,
                               fs::path path, std::size_t expected_size) {
    persistent_file_info result;
    result.key = key;
    result.label = label;
    result.path = path.string();
    result.expected_size = expected_size;
    std::error_code error;
    result.exists = fs::is_regular_file(path, error);
    if (result.exists) {
        result.actual_size = static_cast<std::size_t>(fs::file_size(path, error));
        if (error) result.actual_size = 0;
        result.modified = file_modified(path);
    }
    return result;
}

std::vector<persistent_game_info> build_games() {
    const fs::path root = nvram_root();
    std::vector<persistent_game_info> games;
    for (const rom_set_manifest& manifest : supported_rom_sets()) {
        if (!manifest.working) continue;
        const fs::path game_root = root / manifest.short_name;
        std::vector<persistent_file_info> files;
        switch (manifest.board) {
        case arcade_board_type::system22:
            files.push_back(make_file("eeprom", "8 KiB board EEPROM",
                                      game_root / "eeprom", 0x2000));
            break;
        case arcade_board_type::model1:
            files.push_back(make_file(
                "eeprom", "128-byte operator EEPROM",
                root / (std::string(manifest.short_name) + ".nv"), 0x80));
            break;
        case arcade_board_type::model2:
            files.push_back(make_file("eeprom", "128-byte serial EEPROM",
                                      game_root / "eeprom", 0x80));
            files.push_back(make_file("backup1",
                                      "16 KiB battery-backed SRAM",
                                      game_root / "backup1", 0x4000));
            break;
        case arcade_board_type::system246:
            files.push_back(make_file(
                "backupram", "64 KiB System 246 backup RAM",
                system246_save_root() /
                    (std::string(manifest.short_name) + ".backupram"),
                0x10000));
            break;
        case arcade_board_type::xbox360:
            // ReXGlue owns Robotron's local profile and achievement files.
            // They are intentionally not exposed as fixed-size arcade NVRAM.
            break;
        case arcade_board_type::phoenix:
        case arcade_board_type::galaxian:
        case arcade_board_type::system16b:
        case arcade_board_type::capcom_gng:
        case arcade_board_type::namco_galaga:
        case arcade_board_type::namco_system1:
            break;
        }
        if (!files.empty())
            games.push_back({manifest.short_name, manifest.display_name,
                             std::move(files)});
    }
    return games;
}

bool read_exact(const fs::path& path, void* destination, std::size_t size) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    input.read(static_cast<char*>(destination),
               static_cast<std::streamsize>(size));
    return input.gcount() == static_cast<std::streamsize>(size) &&
           input.peek() == std::ifstream::traits_type::eof();
}

bool write_exact(const fs::path& path, const void* source, std::size_t size) {
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    if (error) return false;
    const fs::path temporary = path.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) return false;
        output.write(static_cast<const char*>(source),
                     static_cast<std::streamsize>(size));
        if (!output.good()) return false;
    }
    fs::rename(temporary, path, error);
    if (!error) return true;
    fs::remove(path, error);
    error.clear();
    fs::rename(temporary, path, error);
    return !error;
}

const persistent_game_info* find_game(const std::vector<persistent_game_info>& games,
                                      const std::string& short_name) {
    const auto found = std::find_if(games.begin(), games.end(),
        [&](const persistent_game_info& game) {
            return game.short_name == short_name;
        });
    return found == games.end() ? nullptr : &*found;
}

persistent_action_result copy_to_backup(const persistent_game_info& game,
                                        fs::path* backup_path = nullptr) {
    const fs::path destination = nvram_root() / "backups" /
        game.short_name / timestamp(true);
    std::error_code error;
    fs::create_directories(destination, error);
    if (error) return {false, "Could not create backup folder: " + error.message()};
    std::size_t copied = 0;
    for (const persistent_file_info& file : game.files) {
        if (!file.exists) continue;
        fs::copy_file(file.path, destination / file.key,
                      fs::copy_options::overwrite_existing, error);
        if (error)
            return {false, "Could not back up " + file.label + ": " +
                           error.message()};
        ++copied;
    }
    if (copied == 0) return {false, "No saved operator data exists yet."};
    if (backup_path) *backup_path = destination;
    return {true, "Backup created at:\n" + destination.string()};
}

} // namespace

std::string nvram_root_path() {
    return nvram_root().string();
}

std::vector<persistent_game_info> persistent_games() {
    return build_games();
}

const persistent_game_info* find_persistent_game(
    const std::vector<persistent_game_info>& games, const std::string& short_name) {
    return find_game(games, short_name);
}

std::string persistent_settings_report(const std::string& short_name) {
    const auto games = build_games();
    const persistent_game_info* game = find_game(games, short_name);
    if (!game) return "Unknown saved-data set: " + short_name;

    const rom_set_manifest* manifest = find_supported_rom_set(short_name);
    const bool system22 = manifest &&
        manifest->board == arcade_board_type::system22;
    const bool model1 = manifest &&
        manifest->board == arcade_board_type::model1;
    const bool model2 = manifest &&
        manifest->board == arcade_board_type::model2;

    std::ostringstream report;
    report << "Saved operator data\n\n";
    bool complete = true;
    for (const persistent_file_info& file : game->files) {
        report << file.label << ": ";
        if (!file.exists) {
            report << "not created";
            complete = false;
        } else {
            report << file.actual_size << " / " << file.expected_size
                   << " bytes";
            if (file.actual_size != file.expected_size) {
                report << "  [INVALID SIZE]";
                complete = false;
            } else {
                report << "  [OK]";
            }
            if (!file.modified.empty()) report << "\nLast changed: " << file.modified;
        }
        report << '\n';
    }

    report << "\nVerified settings\n\n";
    if (!complete) {
        report << "No complete settings image is available to decode. Start "
                  "the game once to create its factory operator data.\n";
    } else if (short_name == "vformula") {
        model1_cabinet::nvram_image image{};
        const fs::path path = game->files.front().path;
        if (!read_exact(path, image.data(), image.size())) {
            report << "The EEPROM could not be read safely.\n";
        } else {
            const bool signature = image[0] == 'S' && image[1] == 'E' &&
                                   image[2] == 'G' && image[3] == 'A';
            report << "Sega cabinet signature: "
                   << (signature ? "valid" : "INVALID") << '\n'
                   << "Settings checksum: "
                   << (model1_cabinet::checksum_valid(image) ? "valid" :
                                                                "INVALID")
                   << '\n'
                   << "Advertise / attract sound: "
                   << (model1_cabinet::attract_sound_enabled(image) ? "ON" :
                                                                      "OFF")
                   << '\n';
        }
    } else {
        report << "The image and size are valid, but this game's individual "
                  "option offsets have not been verified. WhittyArcade will "
                  "not turn unknown bytes into misleading setting names. Use "
                  "the original service screen below; it is authoritative.\n";
    }

    report << "\nHow to change settings\n\n";
    if (system22) {
        report << "1. Start this game and wait for its attract screen.\n"
                  "2. Press F2 to operate the cabinet TEST switch.\n"
                  "3. In the service screen, 9 is SERVICE (move/change) and "
                  "F2 is TEST (enter/select). Use 1/Start or the pedals when "
                  "the game's on-screen instructions request them.\n"
                  "4. Choose the service screen's EXIT/RESET item. Close the "
                  "emulator normally so the updated EEPROM is flushed.\n\n"
                  "D edits physical DIP switches; coinage, difficulty, laps "
                  "and sound are EEPROM options, not DIP settings.";
    } else if (model1) {
        if (short_name == "vformula") {
            report << "Quick change: while Virtua Formula is running, press "
                      "D and toggle Advertise / attract sound.\n\n";
        }
        report << "For all cabinet options, start the game and press F2 for "
                  "its TEST/service screen. Use 9 (SERVICE) to move/change and "
                  "F2 (TEST) to enter/select; follow any Start/pedal prompts "
                  "shown by the game. Exit through the service screen so its "
                  "checksum and EEPROM are committed.";
    } else if (model2) {
        report << "Start Sega Rally and press D once. WhittyArcade performs "
                  "a safe board reset with TEST held so the original service "
                  "screen opens. Once there, use 9 (SERVICE) to move/change "
                  "and F2 (TEST) to enter/select. Choose EXIT to return to "
                  "attract mode; EEPROM and battery RAM are then flushed.";
    } else {
        report << "This game does not expose EEPROM-backed operator options.";
    }
    report << "\n\nDo not hex-edit the live files. Use Export first if you "
              "need an external backup.";
    return report.str();
}

persistent_action_result backup_persistent_game(const std::string& short_name) {
    const auto games = build_games();
    const persistent_game_info* game = find_game(games, short_name);
    if (!game) return {false, "Unknown game save: " + short_name};
    return copy_to_backup(*game);
}

persistent_action_result reset_persistent_game(const std::string& short_name) {
    const auto games = build_games();
    const persistent_game_info* game = find_game(games, short_name);
    if (!game) return {false, "Unknown game save: " + short_name};
    fs::path backup;
    const persistent_action_result backed_up = copy_to_backup(*game, &backup);
    if (!backed_up.success) return backed_up;
    std::error_code error;
    for (const persistent_file_info& file : game->files) {
        if (!file.exists) continue;
        if (!fs::remove(file.path, error) || error)
            return {false, "Backup is safe at " + backup.string() +
                           ", but reset could not remove " + file.path + ": " +
                           error.message()};
    }
    return {true, "Factory reset complete. The previous data is recoverable at:\n" +
                  backup.string()};
}

persistent_action_result export_persistent_game(const std::string& short_name,
                                                const std::string& directory) {
    const auto games = build_games();
    const persistent_game_info* game = find_game(games, short_name);
    if (!game) return {false, "Unknown game save: " + short_name};
    const fs::path destination = fs::path(directory) /
        ("WhittyArcade-" + game->short_name + "-save");
    std::error_code error;
    fs::create_directories(destination, error);
    if (error) return {false, "Could not create export folder: " + error.message()};
    std::size_t copied = 0;
    for (const persistent_file_info& file : game->files) {
        if (!file.exists) continue;
        fs::copy_file(file.path, destination / file.key,
                      fs::copy_options::overwrite_existing, error);
        if (error) return {false, "Could not export " + file.label + ": " +
                                 error.message()};
        ++copied;
    }
    if (!copied) return {false, "No saved operator data exists yet."};
    return {true, "Exported to:\n" + destination.string()};
}

persistent_action_result import_persistent_game(const std::string& short_name,
                                                const std::string& source) {
    const auto games = build_games();
    const persistent_game_info* game = find_game(games, short_name);
    if (!game) return {false, "Unknown game save: " + short_name};
    const fs::path input(source);
    std::error_code error;
    const bool directory = fs::is_directory(input, error);
    if (!directory && game->files.size() != 1)
        return {false, "This game has multiple save chips. Select a folder "
                       "containing files named eeprom and backup1."};

    struct pending_copy { fs::path source; persistent_file_info target; };
    std::vector<pending_copy> pending;
    for (const persistent_file_info& file : game->files) {
        const fs::path candidate = directory ? input / file.key : input;
        if (!fs::is_regular_file(candidate, error)) {
            if (game->files.size() == 1)
                return {false, "Selected save file does not exist."};
            return {false, "Import folder is missing " + file.key + "."};
        }
        const std::uintmax_t size = fs::file_size(candidate, error);
        if (error || size != file.expected_size) {
            std::ostringstream message;
            message << "Wrong size for " << file.key << ": expected "
                    << file.expected_size << " bytes, got "
                    << (error ? 0 : size) << ".";
            return {false, message.str()};
        }
        pending.push_back({candidate, file});
    }

    // Preserve existing data before replacing any component.
    const bool any_existing = std::any_of(game->files.begin(), game->files.end(),
        [](const persistent_file_info& file) { return file.exists; });
    if (any_existing) {
        const persistent_action_result backup = copy_to_backup(*game);
        if (!backup.success) return backup;
    }
    for (const pending_copy& copy : pending) {
        std::vector<char> bytes(copy.target.expected_size);
        if (!read_exact(copy.source, bytes.data(), bytes.size()) ||
            !write_exact(copy.target.path, bytes.data(), bytes.size()))
            return {false, "Could not install " + copy.target.label + "."};
    }
    return {true, "Imported and validated saved data for " +
                  game->display_name + "."};
}

bool load_system22_eeprom(const std::string& short_name,
                          void* destination, std::size_t size) {
    return size == 0x2000 && read_exact(
        nvram_root() / short_name / "eeprom", destination, size);
}

bool save_system22_eeprom(const std::string& short_name,
                          const void* source, std::size_t size) {
    return size == 0x2000 && write_exact(
        nvram_root() / short_name / "eeprom", source, size);
}
