#include "system246_loading_screen.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace system246_loading {
namespace {

// A 5x7 capital-only font, one byte per column with the low bit at the top.
// Only the characters a game title and the word LOADING can contain are
// carried; anything else prints as a blank, which is preferable to shipping a
// full font for a screen that shows two short lines.
struct glyph {
    char character;
    std::uint8_t column[5];
};

constexpr glyph glyphs[] = {
    {'A', {0x7e, 0x11, 0x11, 0x11, 0x7e}},
    {'B', {0x7f, 0x49, 0x49, 0x49, 0x36}},
    {'C', {0x3e, 0x41, 0x41, 0x41, 0x22}},
    {'D', {0x7f, 0x41, 0x41, 0x22, 0x1c}},
    {'E', {0x7f, 0x49, 0x49, 0x49, 0x41}},
    {'F', {0x7f, 0x09, 0x09, 0x09, 0x01}},
    {'G', {0x3e, 0x41, 0x49, 0x49, 0x7a}},
    {'H', {0x7f, 0x08, 0x08, 0x08, 0x7f}},
    {'I', {0x00, 0x41, 0x7f, 0x41, 0x00}},
    {'J', {0x20, 0x40, 0x41, 0x3f, 0x01}},
    {'K', {0x7f, 0x08, 0x14, 0x22, 0x41}},
    {'L', {0x7f, 0x40, 0x40, 0x40, 0x40}},
    {'M', {0x7f, 0x02, 0x0c, 0x02, 0x7f}},
    {'N', {0x7f, 0x04, 0x08, 0x10, 0x7f}},
    {'O', {0x3e, 0x41, 0x41, 0x41, 0x3e}},
    {'P', {0x7f, 0x09, 0x09, 0x09, 0x06}},
    {'Q', {0x3e, 0x41, 0x51, 0x21, 0x5e}},
    {'R', {0x7f, 0x09, 0x19, 0x29, 0x46}},
    {'S', {0x46, 0x49, 0x49, 0x49, 0x31}},
    {'T', {0x01, 0x01, 0x7f, 0x01, 0x01}},
    {'U', {0x3f, 0x40, 0x40, 0x40, 0x3f}},
    {'V', {0x1f, 0x20, 0x40, 0x20, 0x1f}},
    {'W', {0x3f, 0x40, 0x38, 0x40, 0x3f}},
    {'X', {0x63, 0x14, 0x08, 0x14, 0x63}},
    {'Y', {0x07, 0x08, 0x70, 0x08, 0x07}},
    {'Z', {0x61, 0x51, 0x49, 0x45, 0x43}},
    {'0', {0x3e, 0x51, 0x49, 0x45, 0x3e}},
    {'1', {0x00, 0x42, 0x7f, 0x40, 0x00}},
    {'2', {0x42, 0x61, 0x51, 0x49, 0x46}},
    {'3', {0x21, 0x41, 0x45, 0x4b, 0x31}},
    {'4', {0x18, 0x14, 0x12, 0x7f, 0x10}},
    {'5', {0x27, 0x45, 0x45, 0x45, 0x39}},
    {'6', {0x3c, 0x4a, 0x49, 0x49, 0x30}},
    {'7', {0x01, 0x71, 0x09, 0x05, 0x03}},
    {'8', {0x36, 0x49, 0x49, 0x49, 0x36}},
    {'9', {0x06, 0x49, 0x49, 0x29, 0x1e}},
    {'.', {0x00, 0x60, 0x60, 0x00, 0x00}},
    {'-', {0x08, 0x08, 0x08, 0x08, 0x08}},
    {':', {0x00, 0x36, 0x36, 0x00, 0x00}},
    {'/', {0x20, 0x10, 0x08, 0x04, 0x02}},
};

const glyph* find_glyph(char character) {
    if (character >= 'a' && character <= 'z')
        character = static_cast<char>(character - 'a' + 'A');
    for (const glyph& entry : glyphs)
        if (entry.character == character) return &entry;
    return nullptr;
}

void plot(std::vector<std::uint32_t>& pixels, int width, int height, int x,
          int y, std::uint32_t colour) {
    if (x < 0 || y < 0 || x >= width || y >= height) return;
    pixels[static_cast<std::size_t>(y) * width + x] = colour;
}

int text_width(const std::string& text, int scale) {
    return static_cast<int>(text.size()) * 6 * scale;
}

void draw_text(std::vector<std::uint32_t>& pixels, int width, int height,
               int x, int y, const std::string& text, int scale,
               std::uint32_t colour) {
    int pen = x;
    for (const char character : text) {
        if (const glyph* entry = find_glyph(character)) {
            for (int column = 0; column < 5; ++column) {
                for (int row = 0; row < 7; ++row) {
                    if (!(entry->column[column] & (1u << row))) continue;
                    for (int sy = 0; sy < scale; ++sy)
                        for (int sx = 0; sx < scale; ++sx)
                            plot(pixels, width, height,
                                 pen + column * scale + sx,
                                 y + row * scale + sy, colour);
                }
            }
        }
        pen += 6 * scale;
    }
}

} // namespace

void render(std::vector<std::uint32_t>& pixels, int width, int height,
            const std::string& title, double seconds,
            const std::string& status, unsigned long long frames,
            float progress) {
    if (width <= 0 || height <= 0) return;
    pixels.assign(static_cast<std::size_t>(width) * height, 0xff101014u);

    constexpr std::uint32_t white = 0xffffffffu;
    constexpr std::uint32_t grey = 0xff9a9a9au;
    constexpr std::uint32_t amber = 0xff30a0f0u; // 0xAABBGGRR

    const int title_scale = 3;
    const int label_scale = 2;
    draw_text(pixels, width, height,
              (width - text_width(title, title_scale)) / 2,
              height / 2 - 60, title, title_scale, white);

    // What the core is actually doing, rather than a bare "loading": these
    // boards are silent for the best part of a minute and the stage tells the
    // player whether it is still starting up or already reading the disc.
    const std::string label = status.empty() ? std::string("LOADING") : status;
    draw_text(pixels, width, height,
              (width - text_width(label, label_scale)) / 2,
              height / 2 - 10, label, label_scale, grey);

    const int track_width = width / 2;
    const int track_x = (width - track_width) / 2;
    const int track_y = height / 2 + 30;
    const int track_height = 10;
    for (int x = 0; x < track_width; ++x) {
        plot(pixels, width, height, track_x + x, track_y, grey);
        plot(pixels, width, height, track_x + x, track_y + track_height, grey);
    }

    if (progress >= 0.0f) {
        // Unpacking a squashed game: a real percentage bar, so the player can
        // see extraction is advancing rather than waiting blind.
        const float clamped = progress < 0.0f ? 0.0f :
                              (progress > 1.0f ? 1.0f : progress);
        const int fill = static_cast<int>(clamped * (track_width - 2));
        for (int x = 1; x <= fill && x < track_width - 1; ++x)
            for (int y = 1; y < track_height; ++y)
                plot(pixels, width, height, track_x + x, track_y + y, amber);
        char pct[32];
        std::snprintf(pct, sizeof(pct), "UNPACKING %d%%",
                      static_cast<int>(clamped * 100.0f + 0.5f));
        draw_text(pixels, width, height,
                  (width - text_width(pct, 1)) / 2, track_y + 30, pct, 1,
                  amber);
        return;
    }

    // A block sweeping along a track: it keeps moving for as long as the wait
    // lasts, so it cannot look stalled the way a percentage would if the
    // estimate were wrong.
    const int block_width = track_width / 6;
    const double cycle = std::fmod(seconds, 2.0) / 2.0;
    const double eased = 0.5 - 0.5 * std::cos(cycle * 2.0 * 3.14159265);
    const int block_x = track_x +
        static_cast<int>(eased * (track_width - block_width));
    for (int x = 0; x < block_width; ++x)
        for (int y = 1; y < track_height; ++y)
            plot(pixels, width, height, block_x + x, track_y + y, amber);

    // The elapsed time doubles as proof the machine is running rather than
    // stuck, which is the whole point of the screen.
    char elapsed[64];
    if (frames)
        std::snprintf(elapsed, sizeof(elapsed), "%d SECONDS - %llu FRAMES",
                      static_cast<int>(seconds), frames);
    else
        std::snprintf(elapsed, sizeof(elapsed), "%d SECONDS",
                      static_cast<int>(seconds));
    draw_text(pixels, width, height,
              (width - text_width(elapsed, 1)) / 2, track_y + 30, elapsed, 1,
              grey);
}

} // namespace system246_loading
