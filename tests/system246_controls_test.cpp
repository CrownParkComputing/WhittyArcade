#include "system246_controls.h"

#include <cassert>

int main() {
    input_state input;
    system246_drive_controls controls =
        translate_system246_controls(input);
    assert(controls.wheel == 0x80);
    assert(controls.gas_axis == 0x7f);
    assert(controls.brake_axis == 0x80);

    input.steering = 0x280;
    input.gas = 0x610;
    input.brake = 0x610;
    input.coin1 = true;
    input.start = true;
    input.test = true;
    input.shift_up = true;
    input.view3 = true;
    controls = translate_system246_controls(input);
    assert(controls.wheel == 0x00);
    assert(controls.gas_axis == 0x00);
    assert(controls.brake_axis == 0xff);
    assert(controls.coin && !controls.service && controls.start &&
           controls.test);
    assert(controls.shift_up && controls.view3);

    input.steering = 0xd80;
    input.coin1 = false;
    input.service = true;
    controls = translate_system246_controls(input);
    assert(controls.wheel == 0xff);
    assert(!controls.coin && controls.service);
    return 0;
}
