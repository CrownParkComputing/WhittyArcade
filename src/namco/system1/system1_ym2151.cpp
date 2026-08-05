#include <ymfm.h>
#include <ymfm_opm.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>

namespace {

class system1_ym_interface final : public ymfm::ymfm_interface {
public:
    void reset_runtime() {
        timers[0] = timers[1] = -1;
        irq.store(false, std::memory_order_release);
    }
    void ymfm_set_timer(uint32_t number, int32_t clocks) override {
        if (number < 2) timers[number] = clocks < 0 ? -1 : clocks;
    }
    void ymfm_update_irq(bool asserted) override {
        irq.store(asserted, std::memory_order_release);
    }
    void advance(uint32_t clocks) {
        for (unsigned timer = 0; timer < 2; ++timer) {
            if (timers[timer] < 0) continue;
            timers[timer] -= clocks;
            if (timers[timer] <= 0) {
                timers[timer] = -1;
                if (m_engine) m_engine->engine_timer_expired(timer);
            }
        }
    }
    std::atomic_bool irq{false};

private:
    int64_t timers[2]{-1, -1};
};

system1_ym_interface bus;
ymfm::ym2151 chip(bus);
std::mutex chip_mutex;
uint64_t sample_fraction{};
uint64_t timer_clock_fraction{};

int generate_sample_unlocked() {
    // YM2151 native sample clock is 3.579545 MHz / 64.
    sample_fraction += 3'579'545;
    const unsigned native_samples =
        static_cast<unsigned>(sample_fraction / (64 * 48'000));
    sample_fraction %= 64 * 48'000;
    ymfm::ym2151::output_data output{};
    int64_t mixed = 0;
    const unsigned count = std::max(1u, native_samples);
    for (unsigned index = 0; index < count; ++index) {
        chip.generate(&output, 1);
        mixed += (output.data[0] + output.data[1]) / 2;
    }
    return static_cast<int>(mixed / count);
}

} // namespace

extern "C" {

void g88_ym2151_reset(void) {
    std::lock_guard<std::mutex> lock(chip_mutex);
    bus.reset_runtime();
    chip.reset();
    sample_fraction = 0;
    timer_clock_fraction = 0;
}

void g88_ym2151_write_addr(uint8_t value) {
    std::lock_guard<std::mutex> lock(chip_mutex);
    chip.write_address(value);
}

void g88_ym2151_write_data(uint8_t value) {
    std::lock_guard<std::mutex> lock(chip_mutex);
    chip.write_data(value);
}

uint8_t g88_ym2151_read_status(void) {
    std::lock_guard<std::mutex> lock(chip_mutex);
    return chip.read_status();
}

int g88_ym2151_irq_active(void) {
    return bus.irq.load(std::memory_order_acquire) ? 1 : 0;
}

void g88_ym2151_advance_cpu_cycles(unsigned cycles) {
    if (cycles == 0) return;
    std::lock_guard<std::mutex> lock(chip_mutex);
    // The 6809 runs at 49.152 MHz / 32 (1.536 MHz), while the YM2151 timer
    // domain runs at 3.579545 MHz. Keep timer/IRQ state tied to emulated CPU
    // time: the host audio worker is deliberately not a hardware clock.
    timer_clock_fraction += static_cast<uint64_t>(cycles) * 3'579'545u;
    const uint32_t clocks = static_cast<uint32_t>(
        timer_clock_fraction / 1'536'000u);
    timer_clock_fraction %= 1'536'000u;
    if (clocks != 0) bus.advance(clocks);
}

int g88_ym2151_sample(void) {
    std::lock_guard<std::mutex> lock(chip_mutex);
    return generate_sample_unlocked();
}

void g88_ym2151_generate(int16_t* output, int frames, int level) {
    if (!output || frames <= 0) return;
    level = std::clamp(level, 0, 100);
    std::lock_guard<std::mutex> lock(chip_mutex);
    for (int frame = 0; frame < frames; ++frame) {
        const int sample = generate_sample_unlocked() * level / 100;
        output[frame] = static_cast<int16_t>(
            std::clamp(sample, -32768, 32767));
    }
}

}
