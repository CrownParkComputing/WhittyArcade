#include "gng_audio.h"

#include "ymfm.h"
#include "ymfm_opn.h"

extern "C" {
#include "z80.h"
}

#include <AL/al.h>
#include <AL/alc.h>

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace gng {
namespace {

class ym_bus final : public ymfm::ymfm_interface {
public:
    void reset_time() {
        clocks = busy_until = 0;
        timers[0] = timers[1] = -1;
    }
    void advance(uint32_t count) {
        clocks += count;
        for (unsigned index = 0; index < 2; ++index) {
            if (timers[index] < 0) continue;
            timers[index] -= count;
            if (timers[index] <= 0) {
                timers[index] = -1;
                if (m_engine) m_engine->engine_timer_expired(index);
            }
        }
    }
    void ymfm_set_timer(uint32_t number, int32_t duration) override {
        if (number < 2) timers[number] = duration;
    }
    void ymfm_set_busy_end(uint32_t duration) override {
        busy_until = clocks + duration;
    }
    bool ymfm_is_busy() override { return clocks < busy_until; }

private:
    uint64_t clocks{};
    uint64_t busy_until{};
    int64_t timers[2]{-1, -1};
};

} // namespace

struct audio_system::synth {
    std::array<uint8_t, 0x8000> rom{};
    std::array<uint8_t, 0x800> ram{};
    z80_t cpu{};
    uint64_t pins{};
    ym_bus bus1, bus2;
    ymfm::ym2203 ym1;
    ymfm::ym2203 ym2;
    uint32_t irq_clock{};
    bool irq_asserted{};
    bool reset_applied{};
    uint32_t z80_fraction{};
    uint32_t ym_fraction{};
    uint32_t native_fraction{};
    int32_t current_sample{};

    synth() : ym1(bus1), ym2(bus2) {
        ym1.set_fidelity(ymfm::OPN_FIDELITY_MIN);
        ym2.set_fidelity(ymfm::OPN_FIDELITY_MIN);
    }

    void reset() {
        ram.fill(0);
        pins = z80_init(&cpu);
        pins = z80_reset(&cpu);
        bus1.reset_time();
        bus2.reset_time();
        ym1.reset();
        ym2.reset();
        irq_clock = z80_fraction = ym_fraction = native_fraction = 0;
        irq_asserted = false;
        current_sample = 0;
        reset_applied = true;
    }

    void tick(uint8_t command) {
        if (++irq_clock >= 12500) {
            irq_clock -= 12500;
            irq_asserted = true;
        }
        if (irq_asserted) pins |= Z80_INT;
        pins = z80_tick(&cpu, pins);
        const uint16_t address = Z80_GET_ADDR(pins);
        if (pins & Z80_MREQ) {
            if (pins & Z80_RD) {
                uint8_t value = 0xff;
                if (address < 0x8000) value = rom[address];
                else if (address >= 0xc000 && address < 0xc800)
                    value = ram[address - 0xc000];
                else if (address == 0xc800) value = command;
                Z80_SET_DATA(pins, value);
            } else if ((pins & Z80_WR) && address >= 0xc000 &&
                       address < 0xc800) {
                ram[address - 0xc000] = Z80_GET_DATA(pins);
            } else if (pins & Z80_WR) {
                const uint8_t value = Z80_GET_DATA(pins);
                if (address >= 0xe000 && address <= 0xe001)
                    ym1.write(address & 1, value);
                else if (address >= 0xe002 && address <= 0xe003)
                    ym2.write(address & 1, value);
            }
        }
        if ((pins & (Z80_IORQ | Z80_M1)) == (Z80_IORQ | Z80_M1)) {
            irq_asserted = false;
            pins &= ~static_cast<uint64_t>(Z80_INT);
        }
        if (++ym_fraction == 2) {
            ym_fraction = 0;
            bus1.advance(1);
            bus2.advance(1);
        }
    }

    int16_t generate_sample(uint8_t command) {
        // 3 MHz / 48 kHz = 62.5 sound-CPU clocks per host sample.
        z80_fraction += 3'000'000;
        const uint32_t clocks = z80_fraction / 48'000;
        z80_fraction %= 48'000;
        for (uint32_t clock = 0; clock < clocks; ++clock) tick(command);

        // At minimum fidelity ymfm renders the 1.5 MHz YM2203 at 62.5 kHz.
        native_fraction += 62'500;
        while (native_fraction >= 48'000) {
            native_fraction -= 48'000;
            ymfm::ym2203::output_data a{}, b{};
            ym1.generate(&a);
            ym2.generate(&b);
            const int64_t mixed =
                (a.data[0] + b.data[0]) * 20LL +
                (a.data[1] + a.data[2] + a.data[3] +
                 b.data[1] + b.data[2] + b.data[3]) * 40LL;
            current_sample = static_cast<int32_t>(mixed / 280);
        }
        return static_cast<int16_t>(
            std::clamp(current_sample, -32768, 32767));
    }
};

audio_system::audio_system() : m_synth(std::make_unique<synth>()) {}
audio_system::~audio_system() { shutdown(); }

bool audio_system::initialize(const roms& rom_data) {
    if (!rom_data.complete()) return false;
    m_synth->rom = rom_data.sound;
    m_synth->reset();
    m_device = alcOpenDevice(nullptr);
    if (!m_device) return false;
    m_context = alcCreateContext(static_cast<ALCdevice*>(m_device), nullptr);
    if (!m_context ||
        !alcMakeContextCurrent(static_cast<ALCcontext*>(m_context))) {
        if (m_context) alcDestroyContext(static_cast<ALCcontext*>(m_context));
        alcCloseDevice(static_cast<ALCdevice*>(m_device));
        m_context = m_device = nullptr;
        return false;
    }
    alGenBuffers(buffer_count, m_buffers.data());
    alGenSources(1, &m_source);
    const bool okay = alGetError() == AL_NO_ERROR;
    alcMakeContextCurrent(nullptr);
    if (!okay) {
        shutdown();
        return false;
    }
    m_normalizer.reset();
    m_initialized.store(true);
    std::printf("Ghosts'n Goblins audio: Z80 + dual YM2203 enabled\n");
    return true;
}

void audio_system::start() {
    if (!m_initialized.load() || m_running.exchange(true)) return;
    m_thread = std::thread([this] { audio_loop(); });
}

void audio_system::stop() {
    m_running.store(false);
    if (m_thread.joinable()) m_thread.join();
}

void audio_system::shutdown() {
    stop();
    if (!m_device) return;
    alcMakeContextCurrent(static_cast<ALCcontext*>(m_context));
    if (m_source) alDeleteSources(1, &m_source);
    alDeleteBuffers(buffer_count, m_buffers.data());
    alcMakeContextCurrent(nullptr);
    if (m_context) alcDestroyContext(static_cast<ALCcontext*>(m_context));
    alcCloseDevice(static_cast<ALCdevice*>(m_device));
    m_source = 0;
    m_buffers.fill(0);
    m_context = m_device = nullptr;
    m_initialized.store(false);
}

void audio_system::write_command(uint8_t value) { m_command.store(value); }
void audio_system::set_reset_line(bool asserted) { m_reset.store(asserted); }

void audio_system::set_mix_levels(int master, int music, int effects) {
    m_master.store(std::clamp(master, 0, 200));
    m_music.store(std::clamp(music, 0, 100));
    m_effects.store(std::clamp(effects, 0, 100));
}

void audio_system::fill_buffer(uint32_t buffer) {
    const bool reset = m_reset.load(std::memory_order_relaxed);
    if (reset && !m_synth->reset_applied) m_synth->reset();
    if (!reset) m_synth->reset_applied = false;
    for (int index = 0; index < buffer_frames; ++index) {
        m_samples[index] = reset ? 0 :
            m_synth->generate_sample(m_command.load(std::memory_order_relaxed));
    }
    const int mix = (m_music.load() + m_effects.load()) / 2;
    for (int16_t& sample : m_samples)
        sample = static_cast<int16_t>(static_cast<int32_t>(sample) * mix / 100);
    const auto metrics = m_normalizer.process_in_place(
        m_samples.data(), m_samples.size(), 1, sample_rate, m_master.load(), mix);
    m_peak.store(metrics.peak);
    alBufferData(buffer, AL_FORMAT_MONO16, m_samples.data(),
                 static_cast<ALsizei>(m_samples.size() * sizeof(int16_t)),
                 sample_rate);
}

void audio_system::audio_loop() {
    if (!alcMakeContextCurrent(static_cast<ALCcontext*>(m_context))) {
        m_running.store(false);
        return;
    }
    for (uint32_t buffer : m_buffers) fill_buffer(buffer);
    alSourceQueueBuffers(m_source, buffer_count, m_buffers.data());
    alSourcePlay(m_source);
    bool source_paused = false;
    while (m_running.load(std::memory_order_acquire)) {
        if (m_paused.load(std::memory_order_acquire)) {
            if (!source_paused) alSourcePause(m_source);
            source_paused = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        if (source_paused) alSourcePlay(m_source);
        source_paused = false;
        ALint processed = 0;
        alGetSourcei(m_source, AL_BUFFERS_PROCESSED, &processed);
        while (processed-- > 0) {
            ALuint buffer{};
            alSourceUnqueueBuffers(m_source, 1, &buffer);
            fill_buffer(buffer);
            alSourceQueueBuffers(m_source, 1, &buffer);
        }
        ALint state{};
        alGetSourcei(m_source, AL_SOURCE_STATE, &state);
        if (state != AL_PLAYING) alSourcePlay(m_source);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    alSourceStop(m_source);
    ALint queued{};
    alGetSourcei(m_source, AL_BUFFERS_QUEUED, &queued);
    while (queued-- > 0) {
        ALuint buffer{};
        alSourceUnqueueBuffers(m_source, 1, &buffer);
    }
    alcMakeContextCurrent(nullptr);
}

} // namespace gng
