#include "arcade_types.h"
#include "namco/namco_rom.h"
#include "namco/system1/system1_machine.h"

extern "C" {
#include "namcos1_machine.h"
}

#include <cassert>
#include <atomic>
#include <array>
#include <chrono>
#include <cstdio>
#include <thread>

extern "C" void g88_ym2151_generate(int16_t* output, int frames, int level);

namespace {

void run(namco::system1_machine& machine, input_state state, int frames) {
    machine.set_input(state, 0xff);
    for (int frame = 0; frame < frames; ++frame) {
        machine.run_frame();
        if (machine.fault() != 0) {
            std::fprintf(stderr,
                "Pac-Mania fault=%d frame=%d main=%04x sub=%04x "
                "audio=%04x mcu=%04x\n",
                machine.fault(), frame, g88_pc(0), g88_pc(1), g88_pc(2),
                g88_mcu_pc());
            assert(false && "Pac-Mania processor faulted");
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s pacmania.zip\n", argv[0]);
        return 2;
    }
    const namco::load_result loaded = namco::rom_loader::load(argv[1]);
    assert(loaded && loaded.set == namco::rom_set::pacmania);

    namco::system1_machine machine;
    assert(machine.initialize(loaded.pacmania));
    std::atomic_bool audio_running{true};
    std::thread audio_worker([&] {
        std::array<int16_t, 800> samples{};
        while (audio_running.load(std::memory_order_acquire)) {
            g88_ym2151_generate(samples.data(), samples.size(), 100);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });
    const auto started = std::chrono::steady_clock::now();
    input_state state{};
    // Reproduce the live cabinet sequence that originally exposed an MCU
    // failure: three independently edge-latched coin taps followed by Start.
    run(machine, state, 245);
    state.coin1 = true;
    run(machine, state, 2);
    state.coin1 = false;
    run(machine, state, 14);
    state.coin1 = true;
    run(machine, state, 2);
    state.coin1 = false;
    run(machine, state, 9);
    state.coin1 = true;
    run(machine, state, 2);
    state.coin1 = false;
    run(machine, state, 11);

    const unsigned long cycles_before_start = g88_cycles(0);
    state.start = true;
    run(machine, state, 4);
    state.start = false;
    run(machine, state, 1200);
    const unsigned long cycles_after_start = g88_cycles(0);
    audio_running.store(false, std::memory_order_release);
    audio_worker.join();
    const auto elapsed = std::chrono::steady_clock::now() - started;

    assert(cycles_after_start > cycles_before_start + 1100u * 20000u);
    assert(elapsed < std::chrono::seconds(8));
    std::printf(
        "Pac-Mania start transition: main=%04x sub=%04x audio=%04x "
        "mcu=%04x cycles=%lu\n",
        g88_pc(0), g88_pc(1), g88_pc(2), g88_mcu_pc(), cycles_after_start);
    return 0;
}
