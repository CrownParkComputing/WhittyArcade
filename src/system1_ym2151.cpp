#include <ymfm.h>
#include <ymfm_opm.h>

#include <algorithm>
#include <cstdint>
#include <mutex>

namespace {

class system1_ym_interface final : public ymfm::ymfm_interface {
public:
    void reset_runtime() {
        timers[0] = timers[1] = -1;
        irq = false;
    }
    void ymfm_set_timer(uint32_t number, int32_t clocks) override {
        if (number < 2) timers[number] = clocks < 0 ? -1 : clocks;
    }
    void ymfm_update_irq(bool asserted) override { irq = asserted; }
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
    bool irq{};

private:
    int64_t timers[2]{-1, -1};
};

system1_ym_interface bus;
ymfm::ym2151 chip(bus);
std::mutex chip_mutex;
uint64_t sample_fraction{};

} // namespace

extern "C" {

void g88_ym2151_reset(void) {
    std::lock_guard<std::mutex> lock(chip_mutex);
    bus.reset_runtime();
    chip.reset();
    sample_fraction = 0;
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
    std::lock_guard<std::mutex> lock(chip_mutex);
    return bus.irq ? 1 : 0;
}

int g88_ym2151_sample(void) {
    std::lock_guard<std::mutex> lock(chip_mutex);
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
        bus.advance(64);
        mixed += (output.data[0] + output.data[1]) / 2;
    }
    return static_cast<int>(mixed / count);
}

}
