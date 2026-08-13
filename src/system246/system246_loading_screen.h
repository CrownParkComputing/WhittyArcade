// Loading screen for the System 246/256 board.
//
// These games boot from a DVD or hard-disk image and take a long time to put
// anything on screen -- Ridge Racer V is about forty-five seconds, Time Crisis
// 4 over a minute -- during which the emulated machine is running fine but the
// display is black. Without a sign of life that reads as a hang, so the board
// draws its own picture until the game produces its first frame.
//
// The picture is built here as plain RGBA pixels rather than through the
// renderer's font, so it needs no GL work and reaches every presentation
// backend by the same path the emulated frames take.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace system246_loading {

// Draw the loading picture for `title` at `seconds` into the wait. Fills
// `pixels` with width * height RGBA values (0xAABBGGRR, as the renderer's
// present_rgba_frame expects). When `progress` is >= 0 it is treated as an
// unpack fraction in [0,1] and a percentage bar is drawn in place of the
// sweeping block, so a squashed game's extraction is visible rather than
// reading as a hang.
void render(std::vector<std::uint32_t>& pixels, int width, int height,
            const std::string& title, double seconds,
            const std::string& status, unsigned long long frames,
            float progress = -1.0f);

} // namespace system246_loading
