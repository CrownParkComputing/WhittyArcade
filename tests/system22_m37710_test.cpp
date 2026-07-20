#include "m37710.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>

namespace {
bool expect(bool condition, const char* message) {
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
    return condition;
}
}

int main() {
    std::array<uint8_t, 0x4000> firmware{};
    // Reset at C000, execute NOP, then loop back to C000.
    firmware[0x0000] = 0xea;
    firmware[0x0001] = 0x80;
    firmware[0x0002] = 0xfd;
    firmware[0x3ffe] = 0x00;
    firmware[0x3fff] = 0xc0;

    m37710_cpu_device cpu;
    bool ok = true;
    ok &= expect(!cpu.load_internal_rom(firmware.data(), firmware.size() - 1),
                 "C74 core rejects a truncated internal ROM");
    ok &= expect(cpu.load_internal_rom(firmware.data(), firmware.size()),
                 "C74 core accepts an exact 16 KiB internal ROM");
    cpu.reset();
    ok &= expect(cpu.program_counter() == 0x00c000,
                 "M37702 reset vector is fetched little-endian");
    ok &= expect(cpu.execute(1) > 0, "M37702 executes an instruction");
    ok &= expect(cpu.program_counter() == 0x00c001,
                 "M37702 advances past a NOP");
    ok &= expect(cpu.execute(1) > 0, "M37702 executes a relative branch");
    ok &= expect(cpu.program_counter() == 0x00c000,
                 "M37702 relative branch target is correct");

    // Timer A0 at f/16 with a reload value of 0x001f has a period of
    // 16 * (31 + 1) input clocks. This is the cadence used by the C74 sound
    // firmware; halving it makes speech and music sequences run twice fast.
    std::array<uint8_t, 0x4000> timer_firmware{};
    const std::array<uint8_t, 24> timer_program{{
        0xe2, 0x20,             // SEP #$20: 8-bit accumulator
        0xa9, 0x40,             // LDA #$40: f/16 timer mode
        0x8d, 0x56, 0x00,       // STA $0056: Timer A0 mode
        0xa9, 0x1f,
        0x8d, 0x46, 0x00,       // Timer A0 low byte
        0xa9, 0x00,
        0x8d, 0x47, 0x00,       // Timer A0 high byte
        0xa9, 0x01,
        0x8d, 0x40, 0x00,       // start Timer A0
        0x80, 0xfe,             // loop
    }};
    std::copy(timer_program.begin(), timer_program.end(), timer_firmware.begin());
    timer_firmware[0x3ffe] = 0x00;
    timer_firmware[0x3fff] = 0xc0;
    m37710_cpu_device timer_cpu;
    ok &= expect(timer_cpu.load_internal_rom(timer_firmware.data(),
                                              timer_firmware.size()),
                 "M37702 accepts timer regression firmware");
    timer_cpu.reset();
    timer_cpu.execute(100);
    ok &= expect(timer_cpu.timer_reload_cycles(0) == 512,
                 "Timer A0 preserves full input-clock cadence");

    if (ok) std::puts("M37702 core tests passed");
    return ok ? 0 : 1;
}
