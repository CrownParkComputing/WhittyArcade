#include "rr6_cloud_save.h"

#include "platform_paths.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace fs = std::filesystem;

namespace rr6_cloud_save {
namespace {

fs::path title_root() {
    return manx_platform::data_root() / "retro_recomp" / "content" /
           "4E4D07D3";
}

fs::path cloud_root() {
    return manx_platform::data_root() / "MANX" / "cloud_saves" /
           "4E4D07D3";
}

fs::path unique_path(const fs::path& parent, const std::string& stem) {
    const auto stamp = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return parent / (stem + "." + std::to_string(stamp));
}

bool write_snapshot_directory(const snapshot& save, const fs::path& directory,
                              const fs::path& seed_directory,
                              std::string& error) {
    std::error_code ec;
    fs::create_directories(directory, ec);
    if (ec) {
        error = "cannot create save staging directory: " + ec.message();
        return false;
    }
    const fs::path temporary = directory / (save.file_name + ".part");
    const fs::path final = directory / save.file_name;
    {
        std::ofstream output(temporary,
                             std::ios::binary | std::ios::trunc);
        if (!output) {
            error = "cannot open staged save file";
            return false;
        }
        output.write(reinterpret_cast<const char*>(save.payload.data()),
                     static_cast<std::streamsize>(save.payload.size()));
        output.flush();
        if (!output.good()) {
            error = "cannot finish staged save file";
            return false;
        }
    }
    fs::rename(temporary, final, ec);
    if (ec) {
        error = "cannot commit staged save file: " + ec.message();
        return false;
    }

    if (seed_directory.empty()) return true;
    for (const char* thumbnail : {"__thumbnail.png", ".thumbnail.png"}) {
        const fs::path source = seed_directory / thumbnail;
        if (!fs::is_regular_file(source, ec)) continue;
        ec.clear();
        fs::copy_file(source, directory / thumbnail,
                      fs::copy_options::overwrite_existing, ec);
        ec.clear(); // A thumbnail must never make a valid save unusable.
    }
    return true;
}

bool replace_directory(const snapshot& save, const fs::path& destination,
                       const fs::path& seed_directory, std::string& error) {
    const fs::path parent = destination.parent_path();
    const fs::path incoming = unique_path(parent, destination.filename().string() +
                                                      ".incoming");
    const fs::path previous = unique_path(cloud_root(), "previous");
    std::error_code ec;
    fs::create_directories(parent, ec);
    fs::create_directories(cloud_root(), ec);
    if (!write_snapshot_directory(save, incoming, seed_directory, error)) {
        fs::remove_all(incoming, ec);
        return false;
    }

    const bool had_destination = fs::exists(destination, ec);
    if (had_destination) {
        fs::rename(destination, previous, ec);
        if (ec) {
            const std::string reason = ec.message();
            std::error_code cleanup;
            fs::remove_all(incoming, cleanup);
            error = "cannot preserve existing save directory: " + reason;
            return false;
        }
    }
    fs::rename(incoming, destination, ec);
    if (ec) {
        std::error_code rollback;
        if (had_destination) fs::rename(previous, destination, rollback);
        error = "cannot activate replacement save: " + ec.message();
        return false;
    }
    if (had_destination) fs::remove_all(previous, ec);
    return true;
}

bool valid_snapshot(const snapshot& save) {
    return valid_file_name(save.file_name) &&
           save.payload.size() == payload_size &&
           save.checksum.size() == 16 &&
           checksum(save.payload) == save.checksum;
}

} // namespace

fs::path live_directory() { return title_root() / "Game_Data"; }
fs::path marker_path() { return cloud_root() / "uploaded.checksum"; }
fs::path last_good_directory() { return cloud_root() / "last_good"; }
fs::path conflict_directory() { return cloud_root() / "conflicts"; }

const char* describe(resolution what) {
    switch (what) {
    case resolution::nothing:        return "nothing to sync";
    case resolution::already_synced: return "already in sync";
    case resolution::take_cloud:     return "cloud save is authoritative";
    case resolution::publish_local:  return "local save is newer";
    case resolution::conflict:       return "local and cloud both changed";
    }
    return "unknown";
}

decision resolve(const std::optional<snapshot>& local,
                 const std::optional<snapshot>& cloud, const std::string& base,
                 const std::string& seed_checksum) {
    if (!cloud) {
        // Nothing to lose on the far side: publishing can only add.
        if (!local) return {resolution::nothing, "no save on either side"};
        return {resolution::publish_local, "no cloud save exists yet"};
    }
    if (!local)
        return {resolution::take_cloud, "no usable local save"};
    if (local->checksum == cloud->checksum)
        return {resolution::already_synced, "identical bytes"};

    // "Unmodified" means local still holds a state we know the cloud has
    // already seen - either the exact bytes we last uploaded, or, on a machine
    // that has never uploaded, the untouched factory save.
    const bool local_unmodified =
        (!base.empty() && local->checksum == base) ||
        (base.empty() && !seed_checksum.empty() &&
         local->checksum == seed_checksum);
    if (local_unmodified)
        return {resolution::take_cloud, "local save is unplayed since the base"};

    // Local has real progress. It may only be published if the cloud is still
    // exactly where we left it; otherwise the other machine has progress of
    // its own and overwriting it would destroy that.
    if (!base.empty() && cloud->checksum == base)
        return {resolution::publish_local, "cloud is still at our base"};

    return {resolution::conflict,
            base.empty() ? "this machine has never synced and both sides "
                           "hold progress"
                         : "both sides moved off the last synced state"};
}

std::string checksum(const std::vector<uint8_t>& payload) {
    uint64_t value = 14695981039346656037ull;
    for (uint8_t byte : payload) {
        value ^= byte;
        value *= 1099511628211ull;
    }
    std::ostringstream text;
    text << std::hex << std::setfill('0') << std::setw(16) << value;
    return text.str();
}

bool valid_file_name(const std::string& name) {
    return !name.empty() && name.size() <= 42 && name != "." && name != ".." &&
           name.find('/') == std::string::npos &&
           name.find('\\') == std::string::npos &&
           name.find('\0') == std::string::npos;
}

std::optional<snapshot> capture(const fs::path& directory,
                                std::string* error) {
    std::error_code ec;
    if (!fs::is_directory(directory, ec)) {
        if (error) *error = "save directory is missing";
        return std::nullopt;
    }
    fs::path selected;
    for (const fs::directory_entry& entry : fs::directory_iterator(directory, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec) || entry.file_size(ec) != payload_size)
            continue;
        if (!selected.empty()) {
            if (error) *error = "save directory contains multiple payloads";
            return std::nullopt;
        }
        selected = entry.path();
    }
    if (ec || selected.empty()) {
        if (error) *error = ec ? ec.message() : "72,192-byte RR6 save is missing";
        return std::nullopt;
    }
    const std::string name = selected.filename().string();
    if (!valid_file_name(name)) {
        if (error) *error = "save filename is unsafe";
        return std::nullopt;
    }

    const auto size_before = fs::file_size(selected, ec);
    const auto time_before = fs::last_write_time(selected, ec);
    if (ec || size_before != payload_size) {
        if (error) *error = "save changed before it could be read";
        return std::nullopt;
    }
    snapshot result;
    result.file_name = name;
    result.payload.resize(payload_size);
    {
        std::ifstream input(selected, std::ios::binary);
        input.read(reinterpret_cast<char*>(result.payload.data()),
                   static_cast<std::streamsize>(result.payload.size()));
        if (!input || input.peek() != std::ifstream::traits_type::eof()) {
            if (error) *error = "save payload could not be read completely";
            return std::nullopt;
        }
    }
    const auto size_after = fs::file_size(selected, ec);
    const auto time_after = fs::last_write_time(selected, ec);
    if (ec || size_after != size_before || time_after != time_before) {
        if (error) *error = "save changed while it was being read";
        return std::nullopt;
    }
    result.checksum = checksum(result.payload);
    return result;
}

std::optional<snapshot> capture_live(std::string* error) {
    return capture(live_directory(), error);
}

bool install(const snapshot& save, const fs::path& seed_directory,
             std::string& error) {
    if (!valid_snapshot(save)) {
        error = "replacement save failed RR6 integrity checks";
        return false;
    }
    return replace_directory(save, live_directory(), seed_directory, error);
}

bool preserve_last_good(const snapshot& save, std::string& error) {
    if (!valid_snapshot(save)) {
        error = "last-good save failed RR6 integrity checks";
        return false;
    }
    return replace_directory(save, last_good_directory(), {}, error);
}

bool preserve_conflict(const snapshot& save, std::string& error) {
    if (!valid_snapshot(save)) {
        error = "conflicting save failed RR6 integrity checks";
        return false;
    }
    // Keyed by checksum: relaunching into the same unresolved conflict rewrites
    // one directory instead of accumulating a copy per boot.
    return replace_directory(save, conflict_directory() / save.checksum, {},
                             error);
}

bool restore_last_good(std::string& error) {
    const auto held = capture(last_good_directory(), &error);
    return held && install(*held, {}, error);
}

bool install_seed(const fs::path& seed_directory, std::string& error) {
    const auto seed = capture(seed_directory, &error);
    return seed && install(*seed, seed_directory, error);
}

std::string read_marker() {
    std::ifstream input(marker_path());
    std::string marker;
    input >> marker;
    const bool valid = marker.size() == 16 &&
        std::all_of(marker.begin(), marker.end(), [](unsigned char value) {
            return std::isxdigit(value) != 0;
        });
    return valid ? marker : std::string();
}

online_cloud_save upload(const snapshot& save) {
    online_cloud_save result;
    result.title_id = title_id;
    result.file_name = save.file_name;
    result.payload = save.payload;
    result.checksum = save.checksum;
    result.sync_marker_path = marker_path().string();
    return result;
}

std::optional<snapshot> from_cloud(const online_cloud_save& save,
                                   std::string& error) {
    snapshot result{save.file_name, save.payload, save.checksum};
    if (save.title_id != title_id || !valid_snapshot(result)) {
        error = "cloud save failed RR6 integrity checks; local data kept";
        return std::nullopt;
    }
    return result;
}

} // namespace rr6_cloud_save
