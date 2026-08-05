#include "native_title_library.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

namespace {

std::string environment_value(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
}

// A signed STFS package begins with one of these. Distinguishing a package from
// an extracted title matters because the two are handed to the runtime
// differently, and the wrong form is invisible until the game does not start.
bool looks_like_package(const fs::path& file) {
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) return false;
    std::ifstream in(file, std::ios::binary);
    char magic[4] = {};
    if (!in.read(magic, 4)) return false;
    return std::memcmp(magic, "LIVE", 4) == 0 ||
           std::memcmp(magic, "PIRS", 4) == 0 ||
           std::memcmp(magic, "CON ", 4) == 0;
}


// Searches a directory for a signed package, or for a default.xex beside its
// data. Bounded depth: a console dump is <title id>/<content type>/<file>, so
// four levels reaches it without walking somebody's entire Downloads folder.
std::string find_game_within(const fs::path& directory) {
    std::error_code ec;
    if (!fs::is_directory(directory, ec)) return {};
    fs::recursive_directory_iterator walk(
        directory, fs::directory_options::skip_permission_denied, ec);
    if (ec) return {};
    const fs::recursive_directory_iterator done;
    for (; walk != done; walk.increment(ec)) {
        if (ec) break;
        if (walk.depth() > 4) {
            walk.disable_recursion_pending();
            continue;
        }
        if (!walk->is_regular_file(ec)) continue;
        std::string leaf = walk->path().filename().string();
        std::transform(leaf.begin(), leaf.end(), leaf.begin(),
                       [](unsigned char c) {
                           return static_cast<char>(::tolower(c));
                       });
        if (leaf == "default.xex")
            return walk->path().string();
        if (walk->file_size(ec) > (1u << 20) && looks_like_package(walk->path()))
            return walk->path().string();
    }
    return {};
}

} // namespace

imported_assets imported_assets_for(const std::string& short_name) {
    imported_assets found;
    if (short_name.empty()) return found;
    const std::string home = environment_value("HOME");
    const std::string data = environment_value("XDG_DATA_HOME");
    const fs::path root = !data.empty() ? fs::path(data)
                                        : fs::path(home) / ".local" / "share";
    const fs::path bundle = root / "MANX" / "games" / short_name;
    std::error_code ec;
    const auto count = [&](const fs::path& directory, const char* extension) {
        std::size_t total = 0;
        if (!fs::is_directory(directory, ec)) return total;
        for (const fs::directory_entry& entry :
             fs::directory_iterator(directory, ec)) {
            if (ec) break;
            if (!entry.is_regular_file(ec)) continue;
            if (extension == nullptr ||
                entry.path().extension() == extension)
                ++total;
        }
        return total;
    };
    found.sounds = count(bundle / "sfx", ".wav");
    found.artwork = count(bundle / "art", nullptr);
    return found;
}

std::vector<std::string> native_title_catalog_candidates() {
    std::vector<std::string> candidates;
    const std::string explicit_path = environment_value("MANX_XBOX_TITLES");
    if (!explicit_path.empty()) candidates.push_back(explicit_path);
    // Xbox titles are not part of the MANX launcher library. In particular,
    // never probe a development checkout under Downloads during ordinary
    // startup. The conversion tools can still opt in with the explicit path
    // above without imposing filesystem work on arcade-only users.
    return candidates;
}

std::string find_native_title_catalog() {
    std::error_code ec;
    for (const std::string& candidate : native_title_catalog_candidates())
        if (fs::is_regular_file(candidate, ec)) return candidate;
    return {};
}

void native_title_library::consider(const std::string& slug,
                                    const std::string& name,
                                    const std::string& title_id,
                                    const std::string& binary,
                                    const std::string& source,
                                    const std::string& status) {
    if (slug.empty()) {
        m_rejected.push_back(rejected_title{"(unnamed)", "title has no slug"});
        return;
    }
    std::error_code ec;

    // Both halves are required, and BOTH are checked here rather than at launch.
    // A converted title whose binary was cleaned away, or whose owned game has
    // been moved off the machine, must not be offered as playable - the failure
    // otherwise arrives as a window that opens and closes.
    if (binary.empty() || !fs::is_regular_file(binary, ec)) {
        m_rejected.push_back(rejected_title{
            slug, binary.empty()
                      ? "no converted binary recorded"
                      : "converted binary is missing: " + binary});
        return;
    }
    if (source.empty()) {
        m_rejected.push_back(
            rejected_title{slug, "no owned game recorded to run it against"});
        return;
    }
    const bool source_is_file = fs::is_regular_file(source, ec);
    const bool source_is_dir = fs::is_directory(source, ec);
    if (!source_is_file && !source_is_dir) {
        m_rejected.push_back(rejected_title{
            slug,
            "the owned game is not where the workbench recorded it: " + source});
        return;
    }

    native_title title;
    title.short_name = slug;
    title.display_name = name.empty() ? slug : name;
    title.title_id = title_id;
    title.binary_path = binary;
    title.status = status;

    if (source_is_file && looks_like_package(source)) {
        // A signed package holds the executable and every data file inside it,
        // so the runtime is given the one path and nothing else.
        title.package_path = source;
    } else if (source_is_dir) {
        // An extracted title: find its default.xex and hand over both the image
        // and the directory its data sits in.
        const fs::path xex = fs::path(source) / "default.xex";
        if (!fs::is_regular_file(xex, ec)) {
            m_rejected.push_back(rejected_title{
                slug, "extracted game has no default.xex: " + source});
            return;
        }
        title.xex_path = xex.string();
        title.game_root = source;
    } else {
        // A file that is neither a package nor an executable image. This is
        // usually a stale record pointing at the archive a game ARRIVED in
        // rather than at the game: accepting it produced a title that appeared
        // in the launcher and then failed the moment it was chosen, with the
        // runtime's "cannot read" as the only clue.
        std::ifstream image(source, std::ios::binary);
        char magic[4] = {};
        const bool is_xex = image.read(magic, 4) &&
                            std::memcmp(magic, "XEX2", 4) == 0;
        if (!is_xex) {
            // Almost always an archive the game arrived in, recorded before it
            // was unpacked. The unpacked copy is conventionally the archive's
            // name without its extension, beside it - so that is looked for
            // before giving up, which is the difference between a title that
            // works and one that needs its record edited by hand.
            const fs::path unpacked =
                fs::path(source).parent_path() / fs::path(source).stem();
            const std::string found = find_game_within(unpacked);
            if (found.empty()) {
                m_rejected.push_back(rejected_title{
                    slug,
                    "the recorded game is an archive, and no unpacked copy was "
                    "found beside it: " + source + " - unpack it, or point the "
                    "record at the game"});
                return;
            }
            if (looks_like_package(found)) {
                title.package_path = found;
            } else {
                title.xex_path = found;
                title.game_root = fs::path(found).parent_path().string();
            }
            m_titles.push_back(std::move(title));
            return;
        }
        title.xex_path = source;
        title.game_root = fs::path(source).parent_path().string();
    }

    const auto clash = std::find_if(
        m_titles.begin(), m_titles.end(), [&](const native_title& existing) {
            return existing.short_name == title.short_name;
        });
    if (clash != m_titles.end()) {
        m_rejected.push_back(rejected_title{
            slug, "already provided by " + clash->binary_path});
        return;
    }
    m_titles.push_back(std::move(title));
}

void native_title_library::scan(const std::string& catalog_path) {
    m_titles.clear();
    m_rejected.clear();
    std::error_code ec;
    if (catalog_path.empty() || !fs::is_regular_file(catalog_path, ec)) return;

    nlohmann::json parsed;
    {
        std::ifstream in(catalog_path);
        if (!in) {
            m_rejected.push_back(
                rejected_title{"(catalogue)", "cannot read " + catalog_path});
            return;
        }
        // A malformed catalogue is reported rather than thrown: one bad file
        // must not stop MANX from starting.
        parsed = nlohmann::json::parse(in, nullptr, false);
    }
    if (parsed.is_discarded() || !parsed.is_array()) {
        m_rejected.push_back(rejected_title{
            "(catalogue)", catalog_path + " is not a list of titles"});
        return;
    }

    for (const nlohmann::json& entry : parsed) {
        if (!entry.is_object()) continue;
        const auto text = [&entry](const char* key) {
            const auto found = entry.find(key);
            return found != entry.end() && found->is_string()
                       ? found->get<std::string>()
                       : std::string();
        };
        consider(text("slug"), text("name"), text("title_id"),
                 text("binary_path"), text("source_path"), text("status"));
    }

    // Stable order for the launcher, independent of the file's order.
    std::sort(m_titles.begin(), m_titles.end(),
              [](const native_title& a, const native_title& b) {
                  return a.short_name < b.short_name;
              });
}

const native_title* native_title_library::find(
    std::string_view short_name) const noexcept {
    for (const native_title& title : m_titles)
        if (title.short_name == short_name) return &title;
    return nullptr;
}
