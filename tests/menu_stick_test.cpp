// An analogue stick driving a menu. Every failure here is one a player feels
// immediately: a stick that does nothing, a list that runs away from a single
// push, or a menu that jitters because the stick rests off centre.
#include "menu_stick.h"

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
    // A deliberate push steps once, and holding it there steps no further -
    // otherwise one push scrolls the entire list.
    {
        menu_stick stick;
        check(stick.step(menu_stick::vertical, 30000) == 1,
              "a full push should step the menu");
        check(stick.step(menu_stick::vertical, 30000) == 0,
              "holding the stick must not keep stepping");
        check(stick.step(menu_stick::vertical, 25000) == 0,
              "easing off but staying engaged must not step again");
    }

    // Returning to centre re-arms it.
    {
        menu_stick stick;
        check(stick.step(menu_stick::vertical, 30000) == 1, "first push");
        check(stick.step(menu_stick::vertical, 0) == 0,
              "centring is not itself a step");
        check(stick.step(menu_stick::vertical, 30000) == 1,
              "a second push after centring should step again");
    }

    // The opposite direction steps immediately: a player flicking up then
    // down should not have to pass through centre first.
    {
        menu_stick stick;
        check(stick.step(menu_stick::vertical, 30000) == 1, "down");
        check(stick.step(menu_stick::vertical, -30000) == -1,
              "reversing should step the other way at once");
    }

    // A stick resting between release and engage must not chatter. This is
    // the case a single threshold gets wrong, and a worn stick sits here.
    {
        menu_stick stick;
        const int between = (menu_stick::release + menu_stick::engage) / 2;
        check(stick.step(menu_stick::vertical, 30000) == 1, "push");
        for (int tick = 0; tick < 20; ++tick) {
            check(stick.step(menu_stick::vertical, between) == 0,
                  "a stick resting off centre must not step");
        }
        // Still latched: it has not come back far enough to re-arm.
        check(stick.step(menu_stick::vertical, 30000) == 0,
              "a stick that never released must not step again");
        check(stick.step(menu_stick::vertical, menu_stick::release) == 0,
              "coming back inside release re-arms without stepping");
        check(stick.step(menu_stick::vertical, 30000) == 1,
              "after releasing, a push steps again");
    }

    // Drift below the engage point never steps at all.
    {
        menu_stick stick;
        for (int value = -menu_stick::engage + 1;
             value < menu_stick::engage; value += 1500) {
            check(stick.step(menu_stick::horizontal, value) == 0,
                  "drift short of the threshold must not step");
        }
    }

    // The two axes latch independently - holding left must not stop the menu
    // moving up and down.
    {
        menu_stick stick;
        check(stick.step(menu_stick::horizontal, -30000) == -1, "hold left");
        check(stick.step(menu_stick::vertical, 30000) == 1,
              "vertical must still step while horizontal is held");
        check(stick.step(menu_stick::horizontal, -30000) == 0,
              "horizontal stays latched");
    }

    if (failures == 0) std::printf("menu stick tests passed\n");
    return failures == 0 ? 0 : 1;
}
