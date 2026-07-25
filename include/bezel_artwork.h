// bezel_artwork.h - cabinet bezels for the running game, sourced from The
// Bezel Project.
//
// Like cover artwork, a bezel is decoration and never a dependency: a machine
// with no network, or a game with no published bezel, simply shows the game
// the way it always did. Nothing here blocks a launch.
//
// The Bezel Project publishes one PNG per MAME short name at
//   .../bezelproject-MAME/master/retroarch/overlay/ArcadeBezels/<name>.png
// which is the same short name the ROM loader already resolves, so no title
// matching is involved. The paired .cfg carries no geometry - it only says
// "full screen" - so the viewport the game belongs in has to be recovered
// from the image's own alpha channel.
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <string_view>

namespace bezel {

// Download location for a board's bezel. short_name is the MAME set name,
// e.g. "galaxian".
std::string artwork_url(std::string_view short_name);

// Where a fetched bezel is kept. Bezels live beside the other library data
// rather than in the cache directory: they are user-visible artwork that
// should survive a cache clear.
std::string artwork_path(std::string_view artwork_root,
                         std::string_view short_name);

// The transparent window in a bezel image, in pixels from the top-left.
struct cutout {
    int x{};
    int y{};
    int width{};
    int height{};
    bool valid{false};
};

// Draws a plain cabinet surround for a game The Bezel Project has no artwork
// for. The 3D-era boards - Model 2, System 22, System 246 - are absent from
// its arcade collection entirely, so without this a wall of them has bare
// letterboxing beside neighbours that are properly framed.
//
// The result is an ordinary bezel: an opaque surround with a transparent
// opening of the given aspect, so every path downstream - the cutout search,
// the scaling, the drawing - treats it exactly like a downloaded one.
// aspect_width/aspect_height describe the board's picture, not the artwork.
std::vector<uint8_t> generated_bezel(int width, int height, int aspect_width,
                                     int aspect_height);

// Finds the game viewport in a decoded RGBA bezel.
//
// A global bounding box of transparent pixels is not good enough: many bezels
// have transparent corners, or a soft alpha edge around the artwork, either of
// which would swallow the whole image. Instead this measures the transparent
// run straight through the middle of each axis, which is necessarily the
// screen opening, and rejects anything too small to be one.
//
// Returns an invalid cutout when the image has no usable opening, which is the
// signal to draw the game normally and ignore the bezel.
cutout find_cutout(const uint8_t* rgba, int width, int height,
                   uint8_t alpha_threshold = 16);

} // namespace bezel
