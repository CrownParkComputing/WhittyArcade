// Exercises the bezel helpers without a network or a filesystem: the URL and
// cache-path rules, and the cutout detector that has to find a game viewport
// in an image nobody has annotated.

#include "bezel_artwork.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

// Builds an RGBA bezel: opaque everywhere except a transparent rectangle.
std::vector<uint8_t> make_bezel(int width, int height, int x, int y, int w,
                                int h) {
    std::vector<uint8_t> pixels(
        static_cast<std::size_t>(width) * height * 4, 255);
    for (int row = y; row < y + h; ++row) {
        for (int column = x; column < x + w; ++column) {
            if (row < 0 || row >= height || column < 0 || column >= width)
                continue;
            pixels[(static_cast<std::size_t>(row) * width + column) * 4 + 3] =
                0;
        }
    }
    return pixels;
}

void test_urls() {
    check(bezel::artwork_url("galaxian") ==
              "https://raw.githubusercontent.com/thebezelproject/"
              "bezelproject-MAME/master/retroarch/overlay/ArcadeBezels/"
              "galaxian.png",
          "the bezel URL is built from the MAME short name");
    // A trailing separator on the root must not double up.
    check(bezel::artwork_path("/art", "srallyc") == "/art/srallyc.png",
          "cache path joins a root without a separator");
    check(bezel::artwork_path("/art/", "srallyc") == "/art/srallyc.png",
          "cache path does not double an existing separator");
}

void test_cutout_found() {
    // A typical bezel: 1920x1080 with a centred 4:3 opening.
    const int width = 1920;
    const int height = 1080;
    const std::vector<uint8_t> pixels =
        make_bezel(width, height, 480, 60, 960, 960);
    const bezel::cutout found =
        bezel::find_cutout(pixels.data(), width, height);
    check(found.valid, "a centred opening is found");
    check(found.x == 480 && found.width == 960,
          "the opening's horizontal extent is exact");
    check(found.y == 60 && found.height == 960,
          "the opening's vertical extent is exact");
}

void test_decorative_transparency_rejected() {
    // Transparent corners with no real opening: the game must be drawn
    // normally rather than squeezed into a decorative hole.
    const int width = 400;
    const int height = 300;
    std::vector<uint8_t> pixels(
        static_cast<std::size_t>(width) * height * 4, 255);
    for (int row = 0; row < 8; ++row)
        for (int column = 0; column < 8; ++column)
            pixels[(static_cast<std::size_t>(row) * width + column) * 4 + 3] =
                0;
    const bezel::cutout found =
        bezel::find_cutout(pixels.data(), width, height);
    check(!found.valid, "a small transparent detail is not treated as a screen");
}

void test_off_centre_details_ignored() {
    // A real opening in the middle plus a transparent strip elsewhere. The
    // centre-line measurement must report the opening, not the wider strip -
    // a plain bounding box would merge the two.
    const int width = 800;
    const int height = 600;
    std::vector<uint8_t> pixels =
        make_bezel(width, height, 200, 150, 400, 300);
    for (int column = 0; column < width; ++column)
        pixels[(static_cast<std::size_t>(2) * width + column) * 4 + 3] = 0;
    const bezel::cutout found =
        bezel::find_cutout(pixels.data(), width, height);
    check(found.valid, "the real opening is still found");
    check(found.x == 200 && found.width == 400,
          "a transparent strip away from the centre does not widen the cutout");
}

void test_degenerate_inputs() {
    check(!bezel::find_cutout(nullptr, 100, 100).valid,
          "a null image yields no cutout");
    const std::vector<uint8_t> opaque(4 * 4 * 4, 255);
    check(!bezel::find_cutout(opaque.data(), 4, 4).valid,
          "a fully opaque image yields no cutout");
}

} // namespace


// A generated surround stands in for the boards The Bezel Project has nothing
// for - every 3D-era board, as it turns out. It has to behave like a
// downloaded bezel or nothing downstream works: an opaque frame, a
// transparent opening, and a cutout the finder can actually recover.
void test_generated_bezel() {
    const int width = 1920;
    const int height = 1440;
    const std::vector<uint8_t> art =
        bezel::generated_bezel(width, height, 4, 3);
    check(art.size() == static_cast<std::size_t>(width) * height * 4,
          "generated bezel should be a full RGBA image");
    if (art.size() != static_cast<std::size_t>(width) * height * 4) return;

    const auto alpha_at = [&](int x, int y) {
        return art[(static_cast<std::size_t>(y) * width + x) * 4 + 3];
    };
    check(alpha_at(width / 2, height / 2) == 0,
          "the middle of a generated bezel must be see-through");
    check(alpha_at(1, 1) == 255,
          "the corner of a generated bezel must be solid");

    const bezel::cutout window =
        bezel::find_cutout(art.data(), width, height);
    check(window.valid, "a generated bezel must have a findable opening");
    // The opening carries the board's shape, not the artwork's, or the game
    // would be squeezed into a hole cut for a different picture.
    const int expected_height = window.width * 3 / 4;
    check(std::abs(window.height - expected_height) <= 4,
          "the opening should carry the board's aspect");
    check(window.width < width && window.height < height,
          "the opening must leave a surround to draw");

    // A wide board gets a wide opening: the fallback is not fixed at 4:3.
    const std::vector<uint8_t> wide =
        bezel::generated_bezel(width, height, 16, 9);
    const bezel::cutout wide_window =
        bezel::find_cutout(wide.data(), width, height);
    check(wide_window.valid, "a wide generated bezel must have an opening");
    check(wide_window.height < window.height,
          "a 16:9 board should get a shallower opening than a 4:3 one");
}

int main() {
    test_urls();
    test_cutout_found();
    test_decorative_transparency_rejected();
    test_off_centre_details_ignored();
    test_degenerate_inputs();
    test_generated_bezel();
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("Bezel URL, cache path and cutout detection verified.\n");
    return 0;
}
