#include "arcade_types.h"

#include <cassert>

int main() {
    using action = arcade_host_action;

    assert(classify_arcade_host_event(false, false, false) ==
           action::continue_running);
    assert(classify_arcade_host_event(false, true, false) ==
           action::return_to_menu);
    assert(classify_arcade_host_event(false, true, true) ==
           action::continue_running);
    assert(classify_arcade_host_event(true, false, false) ==
           action::exit_application);
    assert(classify_arcade_host_event(true, true, false) ==
           action::exit_application);
    return 0;
}
