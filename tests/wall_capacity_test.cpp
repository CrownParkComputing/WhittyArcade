// How many cabinets the wall offers. Getting this wrong is felt immediately:
// too many and every column stutters, too few and a big machine is wasted.
#include "wall_capacity.h"

#include <cstdio>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const std::string& what) {
    if (condition) return;
    std::printf("FAIL: %s\n", what.c_str());
    ++failures;
}

} // namespace

int main() {
    // A big machine on a wide display: bounded by the processor, not by a
    // number in the source.
    check(wall_column_capacity(16, 5120) == 5,
          "16 threads across 5120px should offer 5 columns");
    check(wall_column_capacity(32, 5120) == 8,
          "a very large machine is held at the sensible ceiling");

    // A modest laptop must not be offered a wall it cannot run.
    check(wall_column_capacity(4, 1920) == 2,
          "4 threads should offer only the minimum wall");
    check(wall_column_capacity(8, 3840) == 2,
          "8 threads is two boards' worth, not three");
    check(wall_column_capacity(12, 3840) == 3,
          "12 threads should offer 3 columns");

    // The display is the other limit: a fast machine on a narrow screen still
    // cannot show eight readable cabinets.
    check(wall_column_capacity(64, 1920) == 4,
          "1920px holds 4 columns however fast the processor");
    check(wall_column_capacity(64, 2560) == 6,
          "2560px holds 6");

    // Never below a real wall, whatever is reported.
    check(wall_column_capacity(1, 640) == wall_min_columns,
          "a tiny machine still offers the minimum rather than nothing");
    check(wall_column_capacity(0, 5120) == wall_min_columns,
          "an unknown processor is assumed small, not large");

    // Every column stays wide enough to be worth looking at.
    for (int width = 1280; width <= 7680; width += 160) {
        const int columns = wall_column_capacity(64, width);
        check(width / columns >= wall_min_column_width ||
                  columns == wall_min_columns,
              "columns stay above the minimum width at " +
                  std::to_string(width) + "px");
    }

    // Struggling is a sustained shortfall, not a slow frame.
    check(wall_column_is_struggling(48.0, 60.0),
          "48fps against 60 is struggling");
    check(!wall_column_is_struggling(58.0, 60.0),
          "58fps against 60 is not worth reporting");
    check(!wall_column_is_struggling(0.0, 60.0),
          "no measurement yet is not a complaint");
    check(!wall_column_is_struggling(60.0, 0.0),
          "no target means nothing to fall short of");

    if (failures == 0) std::printf("wall capacity tests passed\n");
    return failures == 0 ? 0 : 1;
}
