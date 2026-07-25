#include "bezel_artwork.h"

#include <algorithm>
#include <string>

namespace bezel {
namespace {

// The Bezel Project keeps the real arcade bezels here; the GameBezels/MAME
// folder alongside them holds only a README pointing at this one.
constexpr const char* arcade_bezel_root =
    "https://raw.githubusercontent.com/thebezelproject/bezelproject-MAME/"
    "master/retroarch/overlay/ArcadeBezels/";

// Measures the longest run of transparent pixels crossing the centre of one
// axis. `advance` steps along the axis being measured; `line` is the fixed
// coordinate of the centre line.
struct run {
    int start{};
    int length{};
};

run centre_run(const uint8_t* rgba, int width, int height, bool horizontal,
               uint8_t alpha_threshold) {
    const int span = horizontal ? width : height;
    const int line = (horizontal ? height : width) / 2;
    run best;
    run current{0, 0};
    for (int index = 0; index < span; ++index) {
        const int x = horizontal ? index : line;
        const int y = horizontal ? line : index;
        const std::size_t offset =
            (static_cast<std::size_t>(y) * width + x) * 4 + 3;
        if (rgba[offset] <= alpha_threshold) {
            if (current.length == 0) current.start = index;
            ++current.length;
            if (current.length > best.length) best = current;
        } else {
            current = {0, 0};
        }
    }
    return best;
}

} // namespace

std::string artwork_url(std::string_view short_name) {
    return std::string(arcade_bezel_root) + std::string(short_name) + ".png";
}

std::string artwork_path(std::string_view artwork_root,
                         std::string_view short_name) {
    std::string path(artwork_root);
    if (!path.empty() && path.back() != '/' && path.back() != '\\')
        path.push_back('/');
    path += short_name;
    path += ".png";
    return path;
}

cutout find_cutout(const uint8_t* rgba, int width, int height,
                   uint8_t alpha_threshold) {
    cutout result;
    if (!rgba || width <= 0 || height <= 0) return result;
    const run horizontal =
        centre_run(rgba, width, height, true, alpha_threshold);
    const run vertical =
        centre_run(rgba, width, height, false, alpha_threshold);
    if (horizontal.length <= 0 || vertical.length <= 0) return result;
    // A genuine screen opening is most of the bezel. Anything smaller is a
    // decorative transparent detail - a logo cut-out, a rounded corner - and
    // using it would shrink the game to a postage stamp.
    constexpr int minimum_percent = 25;
    if (horizontal.length * 100 < width * minimum_percent ||
        vertical.length * 100 < height * minimum_percent)
        return result;
    result.x = horizontal.start;
    result.y = vertical.start;
    result.width = horizontal.length;
    result.height = vertical.length;
    result.valid = true;
    return result;
}

std::vector<uint8_t> generated_bezel(int width, int height, int aspect_width,
                                     int aspect_height) {
    if (width <= 0 || height <= 0) return {};
    if (aspect_width <= 0 || aspect_height <= 0) {
        aspect_width = 4;
        aspect_height = 3;
    }
    std::vector<uint8_t> rgba(
        static_cast<std::size_t>(width) * height * 4, 0);

    // The opening: as large as the board's aspect allows inside a margin, so
    // the surround reads as a cabinet rather than as a thin border.
    const int usable_w = width - width / 8;
    const int usable_h = height - height / 10;
    int open_w = usable_w;
    int open_h = open_w * aspect_height / aspect_width;
    if (open_h > usable_h) {
        open_h = usable_h;
        open_w = open_h * aspect_width / aspect_height;
    }
    open_w = std::max(open_w, 1);
    open_h = std::max(open_h, 1);
    const int open_x = (width - open_w) / 2;
    const int open_y = (height - open_h) / 2;

    // Matches the surround and accent the twin-cabinet frame already draws,
    // so a generated bezel and a framed pane look like the same machine.
    const uint8_t surround[3]{13, 29, 39};
    const uint8_t accent[3]{56, 198, 255};
    const int edge = std::max(std::min(width, height) / 160, 2);

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const bool inside = x >= open_x && x < open_x + open_w &&
                                y >= open_y && y < open_y + open_h;
            uint8_t* texel =
                rgba.data() +
                (static_cast<std::size_t>(y) * width + x) * 4;
            if (inside) continue;  // transparent: the game shows through
            // A thin lit edge around the opening, the way a real bezel's
            // inner lip catches the monitor.
            const bool lip = x >= open_x - edge && x < open_x + open_w + edge &&
                             y >= open_y - edge && y < open_y + open_h + edge;
            const uint8_t* colour = lip ? accent : surround;
            texel[0] = colour[0];
            texel[1] = colour[1];
            texel[2] = colour[2];
            texel[3] = 255;
        }
    }
    return rgba;
}

} // namespace bezel

