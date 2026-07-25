#include "system16_data.h"

#include "platform_paths.h"

#include "json.hpp"

#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>

namespace fs = std::filesystem;

namespace system16 {
namespace {

fs::path data_root() {
    const fs::path root = whitty_platform::data_root();
    return (root.empty() ? fs::current_path() : root) / "WhittyArcade" /
           "artwork" / "system16";
}

struct harvested {
    std::map<std::string, spec_list> boards;
    std::map<std::string, std::string> art;
};

// Read once, on first use: this is a small JSON file and the launcher asks
// for it repeatedly while drawing.
const harvested& data() {
    static harvested loaded = [] {
        harvested out;
        std::ifstream input(data_root() / "catalog.json");
        if (!input) return out;
        const nlohmann::json parsed =
            nlohmann::json::parse(input, nullptr, false);
        if (parsed.is_discarded()) return out;
        const auto boards = parsed.find("boards");
        if (boards != parsed.end()) {
            for (const auto& board : boards->items()) {
                const auto specs = board.value().find("specs");
                if (specs == board.value().end()) continue;
                spec_list list;
                for (const auto& spec : specs->items())
                    if (spec.value().is_string())
                        list.emplace_back(spec.key(),
                                          spec.value().get<std::string>());
                out.boards[board.key()] = std::move(list);
            }
        }
        const auto map = parsed.find("mame_map");
        if (map != parsed.end()) {
            for (const auto& entry : map->items()) {
                const auto art = entry.value().find("art");
                if (art == entry.value().end() || !art->is_string()) continue;
                const fs::path path =
                    data_root() / art->get<std::string>();
                std::error_code error;
                if (fs::is_regular_file(path, error))
                    out.art[entry.key()] = path.string();
            }
        }
        return out;
    }();
    return loaded;
}

} // namespace

// Boards system16.com does not document. Phoenix is Amstar's own board and
// Galaxian hardware is Namco's, and neither has a page there - but both are
// plain Z80 machines whose specifications are well established, so the
// launcher can still describe them properly.
const spec_list& built_in_specs(const std::string& board_id) {
    static const std::map<std::string, spec_list> known{
        {"phoenix", {
            {"Main CPU", "Z80 @ 2.75 MHz"},
            {"Sound", "2 x custom analogue sound circuits (TMS36xx melody "
                      "generator plus discrete noise)"},
            {"Video resolution", "208 x 256 (vertical)"},
            {"Colours", "8 colours from a 16-entry palette PROM"},
            {"Board composition", "Single PCB"},
            {"Hardware Features", "Two independent scrolling tile layers "
                                  "with per-layer palette selection, no "
                                  "sprite hardware - everything is drawn "
                                  "from character tiles."},
        }},
        {"xbox360", {
            {"Main CPU", "IBM Xenon: 3 x PowerPC cores @ 3.2 GHz, 2 threads "
                         "each"},
            {"Graphics", "ATI Xenos @ 500 MHz with 10 MB embedded EDRAM"},
            {"Memory", "512 MB GDDR3 @ 700 MHz, unified"},
            {"Sound", "Xenos audio, 320 independent decompression channels"},
            {"Video resolution", "Up to 1920 x 1080"},
            {"Board composition", "Console mainboard"},
            {"Hardware Features", "Unified shader architecture, hardware "
                                  "tessellation and MSAA resolve through "
                                  "EDRAM - the arcade titles here run as "
                                  "native recompiled executables rather "
                                  "than emulated code."},
        }},
        {"capcom_gng", {
            {"Main CPU", "2 x MC6809 @ 1.5 MHz"},
            {"Sound CPU", "Z80 @ 3 MHz"},
            {"Sound chip", "2 x YM2203 @ 1.5 MHz"},
            {"Video resolution", "256 x 224"},
            {"Colours", "256 colours from a 1024-entry palette"},
            {"Board composition", "Two stacked PCBs"},
            {"Hardware Features", "Scrolling background tilemap, separate "
                                  "text layer and hardware sprites with "
                                  "per-sprite palette selection."},
        }},
        {"galaxian", {
            {"Main CPU", "Z80 @ 3.072 MHz"},
            {"Sound", "Custom Namco discrete/WSG audio circuits"},
            {"Video resolution", "224 x 256 (vertical)"},
            {"Colours", "32 colours from PROM"},
            {"Board composition", "Single PCB"},
            {"Hardware Features", "One scrolling star field, one tilemap "
                                  "layer and 7 hardware sprites - the "
                                  "template countless 1979-82 arcade boards "
                                  "were built from."},
        }},
    };
    static const spec_list empty;
    const auto found = known.find(board_id);
    return found == known.end() ? empty : found->second;
}

const spec_list& board_specs(const std::string& board_id) {
    const auto found = data().boards.find(board_id);
    // A harvest that produced only a line or two parsed badly; the built-in
    // description is better than a near-empty page.
    if (found != data().boards.end() && found->second.size() > 2)
        return found->second;
    const spec_list& fallback = built_in_specs(board_id);
    if (!fallback.empty()) return fallback;
    static const spec_list empty;
    return found != data().boards.end() ? found->second : empty;
}

std::string game_art_path(const std::string& short_name) {
    const auto found = data().art.find(short_name);
    return found == data().art.end() ? std::string() : found->second;
}

} // namespace system16
