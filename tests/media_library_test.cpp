#include "media_library.h"
#include "test_platform.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

void touch(const fs::path& path, const std::string& contents) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << contents;
}

} // namespace

int main() {
    const fs::path root = fs::temp_directory_path() /
        ("manx-media-test-" + std::to_string(test_process_id()));
    const fs::path source = root / "nas" / "Roms" / "v3_0.285" / "media";
    const fs::path local = root / "local";

    touch(source / "titles" / "daytona.png", "title");
    touch(source / "titles" / "daytona93.png", "current MAME alias");
    touch(source / "box2d" / "daytona.png", "preferred cover");
    touch(source / "box2d" / "manxtt.png", "archive-name cover");
    touch(source / "snap" / "vf.jpg", "snapshot");
    touch(source / "marquees" / "daytona_alt.png", "marquee");
    touch(source / "videos" / "daytona" / "preview.mp4", "video");
    touch(source / "titles" / "daytonausa.png", "not a short-name match");
    touch(source / "flyers" / "uninstalled.png", "not installed");
    fs::create_directories(source / "cabinets" / "empty-category");
    touch(source.parent_path() / "gamelist.xml",
          "<?xml version='1.0'?><gameList>"
          "<game><path>./daytona93.zip</path>"
          "<desc>Fast &amp; furious &quot;stock cars&quot;.</desc></game>"
          "<game><path>./manxtt.zip</path>"
          "<desc>Race superbikes across the Isle of Man.</desc></game>"
          "<game><path>./uninstalled.zip</path>"
          "<desc>Must not be imported.</desc></game></gameList>");

    const manx_media::import_result imported =
        manx_media::import_installed_games(
            source, local,
            std::vector<std::string>{"daytona", "vf", "manxtt"});
    if (!imported.success || imported.files_copied != 7 ||
        imported.games_matched != 3 || imported.descriptions_imported != 2)
        return 1;
    if (!fs::exists(local / "titles" / "daytona.png") ||
        !fs::exists(local / "titles" / "daytona93.png") ||
        !fs::exists(local / "box2d" / "daytona.png") ||
        !fs::exists(local / "box2d" / "manxtt.png") ||
        !fs::exists(local / "snap" / "vf.jpg") ||
        !fs::exists(local / "marquees" / "daytona_alt.png") ||
        !fs::exists(local / "videos" / "daytona" / "preview.mp4"))
        return 2;
    if (fs::exists(local / "titles" / "daytonausa.png") ||
        fs::exists(local / "flyers" / "uninstalled.png") ||
        !fs::is_directory(local / "cabinets" / "empty-category"))
        return 3;
    if (manx_media::artwork_path(local, "daytona") !=
        local / "box2d" / "daytona.png")
        return 4;
    if (manx_media::artwork_path(local, "daytona", "titles") !=
        local / "titles" / "daytona.png")
        return 5;
    if (manx_media::artwork_path(
            local, std::vector<std::string>{"manxtt", "manxttc"}, "box2d") !=
        local / "box2d" / "manxtt.png")
        return 6;
    if (manx_media::artwork_path(local, "manxttc", "box2d") !=
        local / "box2d" / "manxtt.png")
        return 7;
    if (!manx_media::artwork_path(local, "manxttc", "marquee").empty())
        return 8;
    touch(local / "videos" / "manxtt.mp4", "video snap");
    if (manx_media::video_path(local, std::vector<std::string>{"ManxTT"}) !=
        local / "videos" / "manxtt.mp4")
        return 13;
    if (manx_media::video_path(local, std::vector<std::string>{"manxttc"}) !=
        local / "videos" / "manxtt.mp4")
        return 14;
    if (!manx_media::video_path(
             local, std::vector<std::string>{"daytona"}).empty())
        return 15;
    if (manx_media::description(local, "daytona") !=
            "Fast & furious \"stock cars\"." ||
        manx_media::description(
            local, std::vector<std::string>{"manxttc", "manxtt"}) !=
            "Race superbikes across the Isle of Man." ||
        !manx_media::description(local, "uninstalled").empty())
        return 12;

    fs::remove(local / "box2d" / "daytona.png");
    fs::remove(local / "titles" / "daytona.png");
    if (manx_media::artwork_path(local, "daytona") !=
        local / "titles" / "daytona93.png")
        return 9;

    if (!test_set_environment("XDG_DATA_HOME", root / "xdg")) return 10;
    if (manx_media::local_root() !=
        root / "xdg" / "MANX" / "media")
        return 11;

    fs::remove_all(root);
    return 0;
}
