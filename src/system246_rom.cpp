#include "system246_rom.h"
#include "platform_paths.h"

#include <minizip/unzip.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>

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

// Case-insensitively return the sole ".acgame" manifest directly inside a
// directory, or an empty path when there is none.
fs::path acgame_in_directory(const fs::path& directory) {
    std::error_code error;
    if (!fs::is_directory(directory, error)) return {};
    for (fs::directory_iterator iterator(directory, error), end;
         !error && iterator != end; iterator.increment(error)) {
        std::error_code type_error;
        if (iterator->is_regular_file(type_error) &&
            lower(iterator->path().extension().string()) == ".acgame")
            return iterator->path();
    }
    return {};
}

// Resolve the ".acgame" manifest for a selection: the file itself when it is
// one, the manifest directly inside a directory, or empty otherwise. A plain
// ROM file (e.g. a MAME .zip) never resolves through its neighbours.
fs::path resolve_acgame(const fs::path& path) {
    std::error_code error;
    if (fs::is_regular_file(path, error) &&
        lower(path.extension().string()) == ".acgame")
        return path;
    if (fs::is_directory(path, error))
        return acgame_in_directory(path);
    return {};
}

// The lowercased basename (without extension) of a selection's ".acgame"
// manifest -- the game's short name, e.g. "rrvac" or "motogp". Empty when the
// selection has no manifest.
std::string acgame_game_key(const fs::path& path) {
    const fs::path manifest = resolve_acgame(path);
    return manifest.empty() ? std::string() : lower(manifest.stem().string());
}

void trim_inplace(std::string& text) {
    const auto not_space = [](unsigned char character) {
        return std::isspace(character) == 0;
    };
    text.erase(text.begin(),
               std::find_if(text.begin(), text.end(), not_space));
    text.erase(std::find_if(text.rbegin(), text.rend(), not_space).base(),
               text.end());
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

    // PCSX2 arcade builds identify a System 246/256 game by its ".acgame"
    // manifest, whose basename is the game's short name. This is the primary
    // path now that the board boots through the isolated PCSX2 module.
    const std::string acgame_key = acgame_game_key(source);
    if (acgame_key == "rrvac")
        return system246_rom_set::ridge_racer_v_arcade_battle;
    if (acgame_key == "motogp")
        return system246_rom_set::motogp;

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

bool system246_rom_loader::acgame_two_player(const std::string& short_name) {
    // Seats two players on ONE board. The gun mappings in the arcade core
    // settle which games those are: Vampire Night wires a second trigger and
    // start, while Time Crisis 3 and 4 wire only player one - they are twin
    // LINKED cabinets, a board and a gun per player, so they do not belong
    // here. Listing them offered a two-player launch that could only ever
    // have produced two separate one-player games.
    static constexpr const char* two_player_games[] = {
        "soulcal2", "tekken5", // two control panels on one board
    };
    for (const char* game : two_player_games)
        if (short_name == game) return true;
    return false;
}

std::string system246_rom_loader::acgame_short_name(const std::string& path) {
    if (path.empty()) return std::string();
    return acgame_game_key(fs::path(path));
}

const char* system246_rom_loader::set_short_name(system246_rom_set set) {
    switch (set) {
    case system246_rom_set::ridge_racer_v_arcade_battle: return "rrvac";
    case system246_rom_set::motogp: return "motogp";
    case system246_rom_set::unknown: return "";
    }
    return "";
}

const char* system246_rom_loader::set_display_name(system246_rom_set set) {
    switch (set) {
    case system246_rom_set::ridge_racer_v_arcade_battle:
        return "Ridge Racer V: Arcade Battle (RRV3 Ver.A)";
    case system246_rom_set::motogp:
        return "MotoGP";
    case system246_rom_set::unknown:
        return "Unknown Namco System 246 set";
    }
    return "Unknown Namco System 246 set";
}

namespace {
// Parse the [data] fields from a .acgame manifest. Returns false on IO error.
// elf/dongle/mediasrc/card/subdir are the raw values (card may be key=value).
bool parse_acgame_data(const fs::path& manifest, std::string& elf,
                       std::string& dongle, std::string& mediasrc,
                       std::string& card, std::string& subdir) {
    std::ifstream input(manifest);
    if (!input) return false;
    std::string line;
    while (std::getline(input, line)) {
        const std::size_t comment = line.find(';');
        if (comment != std::string::npos) line.erase(comment);
        const std::size_t equals = line.find('=');
        if (equals == std::string::npos) continue;
        std::string key = line.substr(0, equals);
        std::string value = line.substr(equals + 1);
        trim_inplace(key);
        trim_inplace(value);
        key = lower(key);
        if (key == "elf") elf = value;
        else if (key == "dongle") dongle = value;
        else if (key == "mediasrc") mediasrc = value;
        else if (key == "card") card = value;
        else if (key == "subdir") subdir = value;
    }
    return true;
}

// The directory the manifest's data files resolve against (its parent, plus
// the optional subdir). Shared by the readiness checks.
fs::path acgame_data_dir(const fs::path& manifest, const std::string& subdir) {
    fs::path directory = manifest.parent_path();
    if (!subdir.empty()) directory /= subdir;
    return directory;
}
} // namespace

bool system246_rom_loader::acgame_ready(const std::string& path,
                                        std::string* missing) {
    const auto report = [missing](const std::string& item) {
        if (missing) *missing = item;
        return false;
    };
    if (path.empty()) return report("<game>.acgame");
    const fs::path manifest = resolve_acgame(fs::path(path));
    if (manifest.empty()) return report("<game>.acgame");

    // The module reads elf/dongle/mediasrc from the "[data]" section. Confirm
    // each referenced file exists beside the manifest so a half-copied game is
    // reported as not ready rather than failing only at boot.
    std::string elf, dongle, mediasrc, card, subdir;
    if (!parse_acgame_data(manifest, elf, dongle, mediasrc, card, subdir))
        return report(manifest.filename().string());

    const fs::path directory = acgame_data_dir(manifest, subdir);

    // "card=" may name the file directly or as key=value (a save-card slot);
    // only the file name matters for the readiness check.
    if (const std::size_t assign = card.find('='); assign != std::string::npos)
        card = card.substr(assign + 1);

    for (const std::string& referenced : {elf, dongle, mediasrc, card}) {
        if (referenced.empty()) continue;
        std::error_code error;
        if (!fs::exists(directory / referenced, error))
            return report(referenced);
    }
    return true;
}

bool system246_rom_loader::acgame_pack_ready(const std::string& path,
                                             std::string* missing) {
    const auto report = [missing](const std::string& item) {
        if (missing) *missing = item;
        return false;
    };
    if (path.empty()) return report("<game>.acgame");
    const fs::path manifest = resolve_acgame(fs::path(path));
    if (manifest.empty()) return report("<game>.acgame");

    // A squashfs cache's bootability depends on the in-pack files: the elf and
    // the media (mediasrc/card). The dongle is centralized in the memcards dir
    // and never sits beside the manifest, so it is deliberately excluded.
    std::string elf, dongle, mediasrc, card, subdir;
    if (!parse_acgame_data(manifest, elf, dongle, mediasrc, card, subdir))
        return report(manifest.filename().string());

    const fs::path directory = acgame_data_dir(manifest, subdir);
    if (const std::size_t assign = card.find('='); assign != std::string::npos)
        card = card.substr(assign + 1);

    for (const std::string& referenced : {elf, mediasrc, card}) {
        if (referenced.empty()) continue;
        std::error_code error;
        if (!fs::exists(directory / referenced, error))
            return report(referenced);
    }
    return true;
}

std::string system246_rom_loader::find_disc_path(
        const std::string& selected_path,
        const std::string& configured_chd_directory) {
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

    // MANX reads disc images from its configured CHD folder. Keep the
    // historical per-user location as the default for upgraded/CLI installs.
    const fs::path chd_root = configured_chd_directory.empty() ?
        manx_platform::data_root() / "MANX" / "chd" :
        fs::path(configured_chd_directory);
    const fs::path chd_candidate = chd_root / disc_name;
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
        const std::string& path,
        const std::string& configured_chd_directory) {
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
    result.disc_path = find_disc_path(path, configured_chd_directory);
    result.disc = inspect_disc(result.disc_path);
    if (!result.disc) result.error = result.disc.error;
    return result;
}

// ---------------------------------------------------------------------------
// Squashfs (packed collection) support
// ---------------------------------------------------------------------------
//
// The Batocera/Namco System 246/256 packs ship each game as a single
// .squashfs image whose contents are:
//
//   squashfs-root/<Name>.acgame     <- PCSX2 manifest (name/elf/dongle/mediasrc)
//   squashfs-root/<name>.elf        <- arcade ELF loader
//   squashfs-root/<...> (HDD|CD-ROM|DVD-ROM).chd   <- the disc image
//   squashfs-root/sram.bin, title.txt
//
// MANX keeps these images squashed and unpacks one to a per-game cache
// directory only when the game is selected. `unsquashfs` (from squashfs-tools)
// is used for both the cheap manifest peek (listing + `-cat`) and the full
// unpack. A game is treated as System 246/256 when its image contains an
// .acgame manifest.

namespace {

float parse_progress_token(const std::string& token) {
    const std::size_t slash = token.find('/');
    if (slash == std::string::npos) return -1.0f;
    const float current = std::strtof(token.c_str(), nullptr);
    const float total =
        std::strtof(token.c_str() + slash + 1, nullptr);
    if (total <= 0.0f || current < 0.0f) return -1.0f;
    return current / total;
}

bool run_command(const std::vector<std::string>& args, std::string* out,
                 std::string* err_out,
                 system246_rom_loader::squashfs_progress_fn progress = nullptr,
                 void* progress_user = nullptr) {
    // Build argv for fork/exec. C++17 has no portable way to hand a vector to
    // execv without a contiguous char** that stays alive for the child.
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const std::string& arg : args)
        argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);

    int pipe_out[2];
    int pipe_err[2];
    if (pipe(pipe_out) != 0 || pipe(pipe_err) != 0) return false;

    const pid_t pid = fork();
    if (pid < 0) {
        close(pipe_out[0]); close(pipe_out[1]);
        close(pipe_err[0]); close(pipe_err[1]);
        return false;
    }
    if (pid == 0) {
        // Child: redirect stdout/stderr to the pipes, exec.
        dup2(pipe_out[1], STDOUT_FILENO);
        dup2(pipe_err[1], STDERR_FILENO);
        close(pipe_out[0]); close(pipe_out[1]);
        close(pipe_err[0]); close(pipe_err[1]);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    // Parent.
    close(pipe_out[1]); close(pipe_err[1]);
    std::string collected_out, collected_err;
    char buffer[4096];
    ssize_t n;
    while ((n = ::read(pipe_out[0], buffer, sizeof(buffer))) > 0) {
        collected_out.append(buffer, static_cast<std::size_t>(n));
        if (progress) {
            // unsquashfs overwrites one progress line with \r; find the last
            // \r-delimited token and report its fraction.
            const std::size_t last_cr = collected_out.find_last_of('\r');
            const std::size_t start =
                last_cr == std::string::npos ? 0 : last_cr + 1;
            const std::string tail =
                collected_out.substr(start, collected_out.size() - start);
            const std::size_t sp = tail.find_last_of(' ');
            const std::string token =
                sp == std::string::npos ? tail : tail.substr(sp + 1);
            const float fraction = parse_progress_token(token);
            if (fraction >= 0.0f)
                progress(fraction, progress_user);
        }
    }
    while ((n = ::read(pipe_err[0], buffer, sizeof(buffer))) > 0)
        collected_err.append(buffer, static_cast<std::size_t>(n));
    close(pipe_out[0]); close(pipe_err[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    const bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (out) *out = std::move(collected_out);
    if (err_out) *err_out = std::move(collected_err);
    return ok;
}

std::string trim_copy(std::string text) {
    const auto not_space = [](unsigned char character) {
        return std::isspace(character) == 0;
    };
    const auto first = std::find_if(text.begin(), text.end(), not_space);
    if (first == text.end()) return std::string();
    const auto last = std::find_if(text.rbegin(), text.rend(), not_space).base();
    return std::string(first, last);
}

// The .acgame manifest inside an image, from a `-ll` listing: the last
// whitespace-separated token of the matching line. Empty when there is none.
std::string squashfs_acgame_from_listing(const std::string& listing) {
    std::istringstream stream(listing);
    std::string line;
    while (std::getline(stream, line)) {
        if (lower(fs::path(trim_copy(line)).extension().string()) == ".acgame")
            return fs::path(trim_copy(line)).filename().string();
    }
    return {};
}

} // namespace

bool system246_rom_loader::is_squashfs(const std::string& path) {
    return lower(fs::path(path).extension().string()) == ".squashfs";
}

std::string system246_rom_loader::squashfs_game_name(
        const std::string& squashfs_path) {
    if (squashfs_path.empty() || !is_squashfs(squashfs_path))
        return fs::path(squashfs_path).stem().string();

    // Read the <name>.acgame manifest inside the image (no full extraction).
    std::string listing, error;
    if (!run_command({"unsquashfs", "-ll", squashfs_path}, &listing, &error))
        return fs::path(squashfs_path).stem().string();
    const std::string acgame = squashfs_acgame_from_listing(listing);
    if (acgame.empty()) return fs::path(squashfs_path).stem().string();

    std::string manifest;
    if (!run_command({"unsquashfs", "-cat", squashfs_path, acgame},
                     &manifest, &error))
        return fs::path(squashfs_path).stem().string();

    // Pull the "name =" line from the [game] section.
    std::istringstream stream(manifest);
    std::string line;
    while (std::getline(stream, line)) {
        // Strip a trailing \r (the published manifests use CRLF).
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::size_t equals = line.find('=');
        if (equals == std::string::npos) continue;
        if (lower(trim_copy(line.substr(0, equals))) == "name") {
            const std::string value = trim_copy(line.substr(equals + 1));
            if (!value.empty()) return value;
        }
    }
    return fs::path(squashfs_path).stem().string();
}

bool system246_rom_loader::squashfs_is_system246(
        const std::string& squashfs_path) {
    if (squashfs_path.empty() || !is_squashfs(squashfs_path)) return false;
    std::string listing, error;
    if (!run_command({"unsquashfs", "-ll", squashfs_path}, &listing, &error))
        return false;
    return !squashfs_acgame_from_listing(listing).empty();
}

std::string system246_rom_loader::squashfs_cache_dir(
        const std::string& squashfs_path) {
    if (squashfs_path.empty()) return {};
    // Sanitise the stem for use as a directory name (keep letters/digits,
    // spaces, dots, dashes; collapse runs to a single separator-safe form).
    std::string stem = fs::path(squashfs_path).stem().string();
    if (stem.empty()) return {};
    std::string safe;
    safe.reserve(stem.size());
    bool last_was_sep = false;
    for (unsigned char c : stem) {
        const bool ok = std::isalnum(c) || c == ' ' || c == '.' || c == '-';
        if (ok) {
            safe.push_back(static_cast<char>(c));
            last_was_sep = false;
        } else if (!last_was_sep && !safe.empty()) {
            safe.push_back('_');
            last_was_sep = true;
        }
    }
    if (safe.empty()) return {};

    fs::path root = manx_platform::data_root();
    if (root.empty()) root = fs::current_path();
    return (root / "MANX" / "squashfs" / safe).string();
}

std::string system246_rom_loader::squashfs_cached_acgame(
        const std::string& squashfs_path) {
    const std::string dir = squashfs_cache_dir(squashfs_path);
    if (dir.empty()) return {};
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return {};
    const fs::path manifest = acgame_in_directory(dir);
    if (manifest.empty()) return {};
    // Only treat a cache as "ready to boot" when it is actually complete.
    // A partial unpack (e.g. interrupted mid-extraction) can leave the .acgame
    // but drop the large media CHD, which would fail to open at boot. The
    // in-pack files that determine bootability are the elf and the media
    // (mediasrc/card); the dongle is centralized in the memcards directory and
    // is never beside the manifest, so it is excluded here. Returning empty
    // forces the caller down the unpack path (squashfs_unpack), which re-
    // verifies and re-extracts.
    std::string missing;
    if (!acgame_pack_ready(manifest.string(), &missing)) return {};
    return manifest.string();
}

std::string system246_rom_loader::squashfs_unpack(
        const std::string& squashfs_path, std::string* error,
        squashfs_progress_fn progress, void* progress_user) {
    if (error) error->clear();
    if (squashfs_path.empty() || !is_squashfs(squashfs_path)) {
        if (error) *error = "Not a .squashfs image.";
        return {};
    }

    const std::string dir = squashfs_cache_dir(squashfs_path);
    if (dir.empty()) {
        if (error) *error = "Could not resolve the unpack cache directory.";
        return {};
    }
    std::error_code ec;
    fs::create_directories(dir, ec);

    // Idempotent: reuse an already-unpacked copy. A cache is only complete when
    // the in-pack files the manifest references (elf, mediasrc/card) exist
    // beside it -- a partial unpack (e.g. interrupted mid-extraction) can leave
    // the .acgame but drop the large media CHD, which would boot broken. So the
    // check re-verifies readiness (the dongle is centralized in memcards and is
    // deliberately excluded), not just the manifest's presence.
    const fs::path existing = acgame_in_directory(dir);
    if (!existing.empty()) {
        std::string missing;
        if (acgame_pack_ready(existing.string(), &missing)) {
            if (progress) progress(1.0f, progress_user);
            return existing.string();
        }
        // Incomplete cache: fall through and re-extract over it (-f overwrites).
    }

    std::string err;
    const bool ok = run_command(
        {"unsquashfs", "-f", "-d", dir, squashfs_path}, nullptr, &err,
        progress, progress_user);
    if (!ok) {
        if (error) *error = err.empty() ? "unsquashfs failed." : err;
        return {};
    }

    const fs::path manifest = acgame_in_directory(dir);
    if (manifest.empty()) {
        if (error) *error = "Image unpacked but contains no .acgame manifest.";
        return {};
    }
    if (progress) progress(1.0f, progress_user);
    return manifest.string();
}
