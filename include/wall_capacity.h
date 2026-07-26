// wall_capacity.h - how many cabinets this machine can actually drive.
//
// A wall column is not a view of a shared scene: it is a whole emulated board
// in its own process, with its own CPU emulation, its own audio device and its
// own renderer. Adding one costs a real slice of the machine, so the number
// offered is worked out from the machine rather than being a number somebody
// once typed. Two limits apply and the smaller wins:
//
//   - the processor, because a board that cannot run at full speed is not
//     worth putting on the wall at all;
//   - the display, because past a point the columns are too narrow to see and
//     nobody wants to play a game two inches wide.
#pragma once

#include <algorithm>

// Below this a column is too narrow to be worth having. A 4:3 cabinet at this
// width is around 320 tall, which is the original resolution of most of these
// boards.
inline constexpr int wall_min_column_width = 420;

// Fewer than this and the wall is not a wall.
inline constexpr int wall_min_columns = 2;

// A ceiling that has nothing to do with the machine: beyond this the wall is
// a wall of postage stamps however fast the processor is.
inline constexpr int wall_max_columns = 8;

// Hardware threads per column. A column needs its emulation thread and its
// audio thread running without fighting, and the process still has a renderer
// and the compositor to feed - so a column is budgeted three threads, and the
// first is reserved for everything that is not a column.
inline constexpr int wall_threads_per_column = 3;

// The most columns this machine should be offered. `hardware_threads` is
// std::thread::hardware_concurrency() (0 when it cannot be determined, which
// is treated as a small machine); `surface_width` is the width of the display
// the wall will cover.
inline int wall_column_capacity(unsigned hardware_threads, int surface_width) {
    const int threads = static_cast<int>(
        std::min<unsigned>(hardware_threads, 1024u));
    // Unknown processor: assume little, because guessing high produces a wall
    // that stutters and guessing low only offers fewer columns.
    const int by_cpu = threads <= 0 ? wall_min_columns :
        (threads - 1) / wall_threads_per_column;
    const int by_display = surface_width / wall_min_column_width;
    int capacity = std::min(by_cpu, by_display);
    capacity = std::min(capacity, wall_max_columns);
    return std::max(capacity, wall_min_columns);
}

// True when a column that has been running is not holding its speed. Sampled
// over a window rather than per frame: a single slow frame is a scheduling
// hiccup, a sustained shortfall means the wall is wider than the machine.
inline bool wall_column_is_struggling(double measured_fps, double target_fps) {
    if (target_fps <= 0.0 || measured_fps <= 0.0) return false;
    return measured_fps < target_fps * 0.85;
}
