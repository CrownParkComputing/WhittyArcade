#include "model1_audio.h"

#include "arcade_audio_output.h"
#include "model1_dsb.h"
#include "model1_multipcm.h"
#include "ymfm.h"
#include "ymfm_opn.h"

#include <AL/al.h>
#include <AL/alc.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "m68k.h"
}

namespace {
constexpr uint32_t address_mask = 0x00ffffff;
constexpr uint32_t ym_ring_mask = 8192 - 1;

class model1_ym_bus final : public ymfm::ymfm_interface {
public:
    void ymfm_set_timer(uint32_t timer, int32_t clocks) override {
        if (timer < 2) m_timer_clocks[timer] = clocks;
    }

    void ymfm_set_busy_end(uint32_t clocks) override {
        m_busy_until = m_clock + clocks;
    }

    bool ymfm_is_busy() override {
        return m_clock < m_busy_until;
    }

    void reset_host_timing() {
        m_clock = 0;
        m_busy_until = 0;
        m_timer_clocks[0] = -1;
        m_timer_clocks[1] = -1;
    }

    void advance_clocks(uint32_t clocks) {
        m_clock += clocks;
        for (uint32_t timer = 0; timer < 2; ++timer) {
            if (m_timer_clocks[timer] < 0) continue;
            m_timer_clocks[timer] -= static_cast<int64_t>(clocks);
            if (m_timer_clocks[timer] <= 0) {
                m_timer_clocks[timer] = -1;
                if (m_engine) m_engine->engine_timer_expired(timer);
            }
        }
    }

private:
    uint64_t m_clock{};
    uint64_t m_busy_until{};
    int64_t m_timer_clocks[2]{-1, -1};
};
}

struct model1_audio_system::ym3438_state {
    model1_ym_bus bus;
    ymfm::ym3438 chip;
    uint64_t clock_fraction{};

    ym3438_state() : chip(bus) {}

    void reset() {
        bus.reset_host_timing();
        clock_fraction = 0;
        chip.reset();
    }

    void advance_m68k_cycles(uint32_t cycles) {
        // The sound CPU runs at 10 MHz and the YM3438 at 8 MHz.
        clock_fraction += static_cast<uint64_t>(cycles) * 4;
        const uint32_t clocks = static_cast<uint32_t>(clock_fraction / 5);
        clock_fraction %= 5;
        bus.advance_clocks(clocks);
    }
};

model1_audio_system::model1_audio_system() = default;

model1_audio_system::~model1_audio_system() {
    shutdown();
}

bool model1_audio_system::initialize(const model1_roms& roms) {
    if (m_initialized.load()) return true;
    m_output_normalizer.reset();
    m_rom = roms.sound_cpu;
    m_samples_1 = roms.samples_1;
    m_samples_2 = roms.samples_2;
    m_ram.assign(0x10000, 0);
    m_pcm_1 = multipcm_create(m_samples_1.data(),
                              static_cast<uint32_t>(m_samples_1.size()));
    m_pcm_2 = multipcm_create(m_samples_2.data(),
                              static_cast<uint32_t>(m_samples_2.size()));
    if (!m_pcm_1 || !m_pcm_2) {
        std::fprintf(stderr, "Model 1: MultiPCM allocation failed\n");
        shutdown();
        return false;
    }

    if (!roms.dsb_cpu.empty() || !roms.dsb_mpeg.empty()) {
        m_dsb = std::make_unique<model1_dsb>();
        if (!m_dsb->initialize(roms.dsb_cpu, roms.dsb_mpeg)) {
            std::fprintf(stderr, "Model 1: DSB initialization failed\n");
            shutdown();
            return false;
        }
    }

    m_ym = std::make_unique<ym3438_state>();
    m_ym->reset();
    const uint32_t ym_sample_rate = m_ym->chip.sample_rate(8'000'000);
    m_ym_resampler.step =
        (static_cast<uint64_t>(ym_sample_rate) << 32) / OUTPUT_RATE;
    m_pcm_1_resampler.step =
        (uint64_t{MULTIPCM_NATIVE_RATE} << 32) / OUTPUT_RATE;
    m_pcm_2_resampler.step = m_pcm_1_resampler.step;
    set_active_musashi_memory(this);
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_pulse_reset();

    m_device = alcOpenDevice(nullptr);
    if (!m_device) {
        std::fprintf(stderr, "Model 1 OpenAL: no output device available\n");
        shutdown();
        return false;
    }
    m_context = alcCreateContext(static_cast<ALCdevice*>(m_device), nullptr);
    if (!m_context ||
        !alcMakeContextCurrent(static_cast<ALCcontext*>(m_context))) {
        std::fprintf(stderr, "Model 1 OpenAL: context creation failed\n");
        shutdown();
        return false;
    }
    alGenSources(1, &m_source);
    alGenBuffers(OPENAL_BUFFER_COUNT, m_buffers.data());
    const bool al_ok = alGetError() == AL_NO_ERROR;
    alcMakeContextCurrent(nullptr);
    if (!al_ok) {
        std::fprintf(stderr, "Model 1 OpenAL: buffer allocation failed\n");
        shutdown();
        return false;
    }
    set_active_musashi_memory(nullptr);
    m_initialized = true;
    std::printf("Model 1 audio initialized: 68000 10MHz, YM3438 8MHz, "
                "2x MultiPCM 10MHz\n");
    return true;
}

void model1_audio_system::shutdown() {
    stop();
    if (m_context)
        alcMakeContextCurrent(static_cast<ALCcontext*>(m_context));
    if (m_source) alDeleteSources(1, &m_source);
    if (m_context) alDeleteBuffers(OPENAL_BUFFER_COUNT, m_buffers.data());
    alcMakeContextCurrent(nullptr);
    if (m_context) alcDestroyContext(static_cast<ALCcontext*>(m_context));
    if (m_device) alcCloseDevice(static_cast<ALCdevice*>(m_device));
    m_source = 0;
    m_buffers = {};
    m_context = nullptr;
    m_device = nullptr;
    if (m_pcm_1) multipcm_destroy(m_pcm_1);
    if (m_pcm_2) multipcm_destroy(m_pcm_2);
    m_pcm_1 = nullptr;
    m_pcm_2 = nullptr;
    m_dsb.reset();
    m_ym.reset();
    m_initialized = false;
}

bool model1_audio_system::dsb_active() const {
    return m_dsb && m_dsb->active();
}

uint64_t model1_audio_system::dsb_triggers() const {
    return m_dsb ? m_dsb->trigger_count() : 0;
}

void model1_audio_system::set_mix_levels(int master, int music, int effects) {
    m_master_volume.store(std::clamp(master, 0, 200));
    m_music_volume.store(std::clamp(music, 0, 100));
    m_effects_volume.store(std::clamp(effects, 0, 100));
}

bool model1_audio_system::enqueue_uart(uint8_t data,
                                       uint64_t v60_timestamp) {
    if (std::getenv("MODEL1_AUDIO_TRACE"))
        std::fprintf(stderr, "Model 1 sound UART @%llu: %02x\n",
                     static_cast<unsigned long long>(v60_timestamp), data);
    const uint32_t write = m_uart_write.load(std::memory_order_relaxed);
    const uint32_t next = (write + 1) % UART_CAPACITY;
    if (next == m_uart_read.load(std::memory_order_acquire)) {
        m_dropped_uart.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    m_uart_transport[write] = data;
    m_uart_timestamp[write] = v60_timestamp;
    m_uart_write.store(next, std::memory_order_release);
    m_uart_bytes.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void model1_audio_system::deliver_uart(uint64_t v60_time) {
    uint32_t read = m_uart_read.load(std::memory_order_relaxed);
    const uint32_t write = m_uart_write.load(std::memory_order_acquire);
    while (read != write && m_uart_rx_count < UART_RX_CAPACITY &&
           m_uart_timestamp[read] <= v60_time) {
        m_uart_rx[m_uart_rx_write] = m_uart_transport[read];
        m_uart_rx_write = (m_uart_rx_write + 1) % UART_RX_CAPACITY;
        ++m_uart_rx_count;
        read = (read + 1) % UART_CAPACITY;
    }
    if (read != write && m_uart_rx_count == UART_RX_CAPACITY)
        m_uart_overrun = true;
    m_uart_read.store(read, std::memory_order_release);
    m68k_set_irq(m_uart_rx_count ? 2 : 0);
}

int model1_audio_system::execute_sound_cpu(int requested_cycles) {
    int total = 0;
    while (total < requested_cycles) {
        // 16 MHz V60 time = 10 MHz sound-68000 time * 8/5.
        deliver_uart(m_sound_frame_base +
                     static_cast<uint64_t>(total) * 8 / 5);
        const int chunk = std::min(64, requested_cycles - total);
        const int executed = m68k_execute(chunk);
        if (executed <= 0) break;
        tick_chips(executed);
        total += executed;
    }
    m_program_counter.store(m68k_get_reg(nullptr, M68K_REG_PC),
                            std::memory_order_relaxed);
    m_executed_cycles.fetch_add(total, std::memory_order_relaxed);
    deliver_uart(m_sound_frame_base + 656u * 424u);
    m_sound_frame_base += 656u * 424u;
    return total;
}

void model1_audio_system::signal_frame() {
    if (!m_running.load(std::memory_order_acquire)) return;
    std::unique_lock<std::mutex> lock(m_frame_mutex);
    m_frame_ready.wait(lock, [this] {
        return !m_running.load(std::memory_order_acquire) ||
               m_pending_frames < 8;
    });
    if (!m_running.load(std::memory_order_relaxed)) return;
    ++m_pending_frames;
    lock.unlock();
    m_frame_ready.notify_all();
}

void model1_audio_system::push_ym_sample(int32_t left, int32_t right) {
    const uint32_t next = (m_ym_ring_write + 1) & ym_ring_mask;
    if (next == m_ym_ring_read)
        m_ym_ring_read = (m_ym_ring_read + 1) & ym_ring_mask;
    m_ym_ring[m_ym_ring_write * 2] = left;
    m_ym_ring[m_ym_ring_write * 2 + 1] = right;
    m_ym_ring_write = next;
}

void model1_audio_system::tick_chips(int cycles) {
    multipcm_tick_m68k_cycles(m_pcm_1, static_cast<uint32_t>(cycles));
    multipcm_tick_m68k_cycles(m_pcm_2, static_cast<uint32_t>(cycles));

    if (!m_ym) return;
    m_ym->advance_m68k_cycles(static_cast<uint32_t>(cycles));

    // YMFM renders one sample per 144 chip clocks. Tick it from actual 68000
    // cycles so firmware status polling and generated audio share a timeline.
    const uint64_t native_rate = m_ym->chip.sample_rate(8'000'000);
    const uint64_t step = (native_rate << 32) / 10'000'000;
    m_ym_clock_fraction += step * static_cast<uint64_t>(cycles);
    uint32_t samples = static_cast<uint32_t>(m_ym_clock_fraction >> 32);
    m_ym_clock_fraction &= 0xffffffffu;
    std::array<ymfm::ym3438::output_data, 256> scratch{};
    while (samples) {
        const uint32_t count = std::min<uint32_t>(samples, 256);
        m_ym->chip.generate(scratch.data(), count);
        for (uint32_t frame = 0; frame < count; ++frame)
            push_ym_sample(scratch[frame].data[0], scratch[frame].data[1]);
        samples -= count;
    }
}

std::vector<int32_t> model1_audio_system::take_ym_samples(std::size_t count) {
    std::vector<int32_t> result(count * 2, 0);
    for (std::size_t frame = 0; frame < count; ++frame) {
        if (m_ym_ring_read == m_ym_ring_write) break;
        result[frame * 2] = m_ym_ring[m_ym_ring_read * 2];
        result[frame * 2 + 1] = m_ym_ring[m_ym_ring_read * 2 + 1];
        m_ym_ring_read = (m_ym_ring_read + 1) & ym_ring_mask;
    }
    return result;
}

std::size_t model1_audio_system::source_frames_needed(
        const resampler_state& state, int output_frames) {
    const uint64_t end_phase =
        state.phase + state.step * static_cast<uint64_t>(output_frames);
    return static_cast<std::size_t>(end_phase >> 32) + 1 +
        (state.primed ? 0 : 1);
}

void model1_audio_system::resample_add(resampler_state& state,
                                        const std::vector<int32_t>& source,
                                        int32_t* output, int output_frames,
                                        int gain_percent) {
    if (source.size() < 4 || output_frames <= 0) return;
    std::size_t cursor = 0;
    if (!state.primed) {
        state.last_left = source[0];
        state.last_right = source[1];
        state.next_left = source[2];
        state.next_right = source[3];
        cursor = 2;
        state.primed = true;
    }
    for (int frame = 0; frame < output_frames; ++frame) {
        while (state.phase >> 32) {
            state.phase -= uint64_t{1} << 32;
            state.last_left = state.next_left;
            state.last_right = state.next_right;
            if (cursor * 2 + 1 < source.size()) {
                state.next_left = source[cursor * 2];
                state.next_right = source[cursor * 2 + 1];
                ++cursor;
            }
        }
        const uint32_t fraction = static_cast<uint32_t>(state.phase >> 16);
        const int32_t left = state.last_left + static_cast<int32_t>(
            (static_cast<int64_t>(state.next_left - state.last_left) *
             fraction) >> 16);
        const int32_t right = state.last_right + static_cast<int32_t>(
            (static_cast<int64_t>(state.next_right - state.last_right) *
             fraction) >> 16);
        output[frame * 2] += left * gain_percent / 100;
        output[frame * 2 + 1] += right * gain_percent / 100;
        state.phase += state.step;
    }
}

void model1_audio_system::fill_buffer(uint32_t buffer) {
    // One video frame is 278,144 V60 clocks. The board's 10 MHz 68000 runs
    // exactly 5/8 as many cycles: 173,840. This preserves the sequencer and
    // UART timing regardless of host/OpenAL scheduling jitter.
    execute_sound_cpu(173840);
    // The DSB Z80 is clocked at 4 MHz, exactly one quarter of the Model 1
    // V60 clock. Bytes written by the 68000 UART above are now visible to it.
    if (m_dsb) m_dsb->execute(69536);
    const std::size_t pcm_1_needed =
        source_frames_needed(m_pcm_1_resampler, BUFFER_FRAMES);
    const std::size_t pcm_2_needed =
        source_frames_needed(m_pcm_2_resampler, BUFFER_FRAMES);
    const std::size_t ym_needed =
        source_frames_needed(m_ym_resampler, BUFFER_FRAMES);
    std::vector<int32_t> pcm_1(pcm_1_needed * 2, 0);
    std::vector<int32_t> pcm_2(pcm_2_needed * 2, 0);
    multipcm_render(m_pcm_1, pcm_1.data(),
                    static_cast<uint32_t>(pcm_1_needed));
    multipcm_render(m_pcm_2, pcm_2.data(),
                    static_cast<uint32_t>(pcm_2_needed));
    std::vector<int32_t> ym = take_ym_samples(ym_needed);
    std::array<int32_t, OUTPUT_SAMPLES> mixed{};
    const int music = m_music_volume.load(std::memory_order_relaxed);
    const int effects = m_effects_volume.load(std::memory_order_relaxed);
    // Sega's analogue board routes YM at 0.30 and each MultiPCM at 0.50.
    // The PCM chips carry both sequences and one-shots, so both UI buses
    // contribute instead of incorrectly labelling a whole chip as effects.
    const int pcm_gain = (music + effects) / 4;
    resample_add(m_ym_resampler, ym, mixed.data(), BUFFER_FRAMES,
                 music * 30 / 100);
    resample_add(m_pcm_1_resampler, pcm_1, mixed.data(), BUFFER_FRAMES,
                 pcm_gain);
    resample_add(m_pcm_2_resampler, pcm_2, mixed.data(), BUFFER_FRAMES,
                 pcm_gain);
    if (m_dsb)
        m_dsb->render(mixed.data(), BUFFER_FRAMES, OUTPUT_RATE, music);

    if (std::getenv("MODEL1_AUDIO_TRACE")) {
        static unsigned trace_buffer = 0;
        if ((++trace_buffer % 60) == 0) {
            const auto vector_peak = [](const auto& samples) {
                int32_t peak = 0;
                for (const int32_t sample : samples)
                    peak = std::max(peak, std::abs(sample));
                return peak;
            };
            std::fprintf(stderr,
                         "Model 1 raw audio peak: pcm1=%d pcm2=%d ym=%d "
                         "mixed=%d voices=%u/%u\n",
                         vector_peak(pcm_1), vector_peak(pcm_2),
                         vector_peak(ym), vector_peak(mixed),
                         multipcm_active_voices(m_pcm_1),
                         multipcm_active_voices(m_pcm_2));
        }
    }

    const int master = m_master_volume.load(std::memory_order_relaxed);
    const arcade_audio_output::metrics output =
        m_output_normalizer.process_from_int32(
            mixed.data(), m_output.data(), m_output.size(), 2, OUTPUT_RATE,
            master, (music + effects) / 2);
    m_peak_sample.store(output.peak, std::memory_order_relaxed);
    m_active_pcm_voices.store(
        static_cast<int>(multipcm_active_voices(m_pcm_1) +
                         multipcm_active_voices(m_pcm_2)),
        std::memory_order_relaxed);
    alBufferData(buffer, AL_FORMAT_STEREO16, m_output.data(),
                 static_cast<ALsizei>(m_output.size() * sizeof(int16_t)),
                 OUTPUT_RATE);
}

void model1_audio_system::audio_loop() {
    set_active_musashi_memory(this);
    if (!alcMakeContextCurrent(static_cast<ALCcontext*>(m_context))) {
        m_running = false;
        set_active_musashi_memory(nullptr);
        return;
    }
    unsigned buffers_used = 0;
    while (m_running.load(std::memory_order_acquire)) {
        {
            std::unique_lock<std::mutex> lock(m_frame_mutex);
            m_frame_ready.wait(lock, [this] {
                return !m_running.load(std::memory_order_acquire) ||
                       m_pending_frames != 0;
            });
            if (!m_running.load(std::memory_order_relaxed)) break;
            --m_pending_frames;
        }
        m_frame_ready.notify_all();

        ALuint buffer = 0;
        ALint processed = 0;
        alGetSourcei(m_source, AL_BUFFERS_PROCESSED, &processed);
        if (processed > 0) {
            alSourceUnqueueBuffers(m_source, 1, &buffer);
        } else if (buffers_used < OPENAL_BUFFER_COUNT) {
            buffer = m_buffers[buffers_used++];
        } else {
            while (m_running.load(std::memory_order_acquire) &&
                   processed == 0) {
                alGetSourcei(m_source, AL_BUFFERS_PROCESSED, &processed);
                if (!processed)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (!m_running.load(std::memory_order_relaxed)) break;
            alSourceUnqueueBuffers(m_source, 1, &buffer);
        }
        fill_buffer(buffer);
        alSourceQueueBuffers(m_source, 1, &buffer);
        ALint state = AL_STOPPED;
        alGetSourcei(m_source, AL_SOURCE_STATE, &state);
        // Prime three whole video frames before starting. This gives the
        // frame-driven producer 50 ms of jitter margin without ever replaying
        // an already-processed OpenAL buffer.
        if (buffers_used >= 3 && state != AL_PLAYING) alSourcePlay(m_source);
    }
    alSourceStop(m_source);
    ALint queued = 0;
    alGetSourcei(m_source, AL_BUFFERS_QUEUED, &queued);
    while (queued-- > 0) {
        ALuint buffer = 0;
        alSourceUnqueueBuffers(m_source, 1, &buffer);
    }
    alcMakeContextCurrent(nullptr);
    set_active_musashi_memory(nullptr);
}

void model1_audio_system::start() {
    if (!m_initialized.load() || m_running.exchange(true)) return;
    m_thread = std::thread(&model1_audio_system::audio_loop, this);
}

void model1_audio_system::stop() {
    m_running = false;
    m_frame_ready.notify_all();
    if (m_thread.joinable()) m_thread.join();
}

uint8_t model1_audio_system::read8(uint32_t raw_address) {
    const uint32_t address = raw_address & address_mask;
    if (address < 0x40000)
        return address < m_rom.size() ? m_rom[address] : 0xff;
    if (address >= 0x080000 && address < 0x0a0000) {
        const uint32_t offset = 0x20000 + address - 0x080000;
        return offset < m_rom.size() ? m_rom[offset] : 0xff;
    }
    if (address >= 0xf00000 && address < 0xf10000)
        return m_ram[address - 0xf00000];
    if (address >= 0xd00000 && address < 0xd00008)
        return (address & 1) && m_ym ? m_ym->chip.read_status() : 0xff;
    if (address >= 0xc20000 && address < 0xc20004) {
        // The 8-bit sound latch occupies the lower byte of the 68000's
        // 16-bit bus. An even-address byte access sees the unused upper lane.
        if ((address & 1) == 0) return 0;
        return (address & 2) == 0 ? read_uart_data() : read_uart_status();
    }
    if ((address >= 0xc40000 && address < 0xc40008) ||
        (address >= 0xc60000 && address < 0xc60008))
        return 0;
    return 0xff;
}

uint16_t model1_audio_system::read16(uint32_t address) {
    const uint32_t masked = address & address_mask;
    // A word read of either latch register is one peripheral transaction.
    // Building it from two read8 calls consumed two FIFO bytes, corrupting
    // short sound-command packets such as the attract-mode cue.
    if (masked == 0xc20000) return read_uart_data();
    if (masked == 0xc20002) return read_uart_status();
    return static_cast<uint16_t>((read8(address) << 8) | read8(address + 1));
}

uint8_t model1_audio_system::read_uart_data() {
    uint8_t data = 0;
    if (m_uart_rx_count) {
        data = m_uart_rx[m_uart_rx_read];
        m_uart_rx_read = (m_uart_rx_read + 1) % UART_RX_CAPACITY;
        --m_uart_rx_count;
    }
    m68k_set_irq(m_uart_rx_count ? 2 : 0);
    return data;
}

uint8_t model1_audio_system::read_uart_status() {
    // T82C51 status: transmitter ready/empty are always available in this
    // one-way hookup, while RXRDY follows the serial receive register. The
    // sound program polls RXRDY as well as servicing its level-2 interrupt.
    uint8_t status = 0x05; // TXRDY | TXEMPTY
    if (m_uart_rx_count) status |= 0x02; // RXRDY
    if (m_uart_overrun) status |= 0x10;  // OE
    return status;
}

uint32_t model1_audio_system::read32(uint32_t address) {
    return (static_cast<uint32_t>(read16(address)) << 16) |
           read16(address + 2);
}

void model1_audio_system::write8(uint32_t raw_address, uint8_t value) {
    const uint32_t address = raw_address & address_mask;
    if (address >= 0xf00000 && address < 0xf10000) {
        m_ram[address - 0xf00000] = value;
        return;
    }
    if (address >= 0xd00000 && address < 0xd00008) {
        if ((address & 1) && m_ym) {
            m_ym->chip.write((address >> 1) & 3, value);
            m_ym_writes.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }
    if (address >= 0xc20000 && address < 0xc20004) {
        if ((address & 1) && (address & 2) == 0 && m_dsb)
            m_dsb->receive_uart(value);
        // Control writes configure the 8251. TX ready/empty are modelled by
        // read_uart_status(); serial framing is not observable by firmware.
        return;
    }
    if (address >= 0xc40000 && address < 0xc40008) {
        if (address & 1) {
            multipcm_write(m_pcm_1, (address >> 1) & 3, value);
            m_pcm_writes.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }
    if (address >= 0xc60000 && address < 0xc60008) {
        if (address & 1) {
            multipcm_write(m_pcm_2, (address >> 1) & 3, value);
            m_pcm_writes.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }
    if (address >= 0xc50000 && address < 0xc50002) {
        if (address & 1) multipcm_set_bank(m_pcm_1, value & 3);
        return;
    }
    if (address >= 0xc70000 && address < 0xc70002) {
        if (address & 1) multipcm_set_bank(m_pcm_2, value & 3);
    }
}

void model1_audio_system::write16(uint32_t address, uint16_t value) {
    write8(address, static_cast<uint8_t>(value >> 8));
    write8(address + 1, static_cast<uint8_t>(value));
}

void model1_audio_system::write32(uint32_t address, uint32_t value) {
    write16(address, static_cast<uint16_t>(value >> 16));
    write16(address + 2, static_cast<uint16_t>(value));
}
