// Taito Z sound board implementation: ymfm's YM2610 plus the OpenAL
// streaming worker. The OpenAL half mirrors system16b_audio.cpp.

#include "taito/taitoz/taitoz_audio.h"

#include "ymfm_opn.h"

#include <AL/al.h>
#include <AL/alc.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace {

// ymfm drives timers and the IRQ line through this interface, and reads
// the ADPCM sample ROMs through ymfm_external_read.
class taitoz_ymfm_interface final : public ymfm::ymfm_interface {
public:
    // Timers are scheduled in chip clocks and expire as the machine feeds
    // clocks in, so sequencing stays tied to emulated time rather than to
    // how fast OpenAL happens to drain buffers.
    void ymfm_set_timer(uint32_t tnum, int32_t duration_in_clocks) override {
        if (tnum > 1) return;
        if (duration_in_clocks < 0) {
            m_timer_active[tnum] = false;
        } else {
            m_timer_active[tnum] = true;
            m_timer_remaining[tnum] = duration_in_clocks;
            ++m_scheduled;
        }
    }

    // The YM2610's status register reports a short busy window after a
    // register write. Leaving this unimplemented makes the chip look
    // permanently ready, which a driver that waits on the transition will
    // never accept.
    void ymfm_set_busy_end(uint32_t clocks) override {
        m_busy_remaining = clocks;
    }
    bool ymfm_is_busy() override { return m_busy_remaining > 0; }

    void ymfm_update_irq(bool asserted) override {
        if (asserted && !m_irq) ++m_irqs;
        m_irq = asserted;
    }

    uint8_t ymfm_external_read(ymfm::access_class type,
                               uint32_t address) override {
        ++m_ext_reads;
        if (type == ymfm::ACCESS_ADPCM_A) {
            ++m_ext_a;
            if (address >= m_adpcm_a_size) ++m_ext_a_oob;
        }
        if (type == ymfm::ACCESS_ADPCM_A)
            return (m_adpcm_a && address < m_adpcm_a_size) ? m_adpcm_a[address]
                                                           : 0;
        if (type == ymfm::ACCESS_ADPCM_B)
            return (m_adpcm_b && address < m_adpcm_b_size) ? m_adpcm_b[address]
                                                           : 0;
        return 0;
    }

    // ymfm routes timer expiry back through the engine callbacks the
    // interface already holds.
    void advance(uint32_t clocks) {
        m_busy_remaining = (m_busy_remaining > static_cast<int64_t>(clocks))
            ? m_busy_remaining - clocks : 0;
        for (uint32_t tnum = 0; tnum < 2; ++tnum) {
            if (!m_timer_active[tnum]) continue;
            if (m_timer_remaining[tnum] > static_cast<int64_t>(clocks)) {
                m_timer_remaining[tnum] -= clocks;
                continue;
            }
            m_timer_active[tnum] = false;
            ++m_expired;
            if (m_engine) m_engine->engine_timer_expired(tnum);
        }
    }

    bool irq() const { return m_irq; }
    unsigned m_scheduled{0}, m_expired{0}, m_irqs{0};
    unsigned m_ext_reads{0}, m_ext_a{0}, m_ext_a_oob{0};

    const uint8_t* m_adpcm_a{nullptr};
    std::size_t m_adpcm_a_size{0};
    const uint8_t* m_adpcm_b{nullptr};
    std::size_t m_adpcm_b_size{0};

private:
    bool m_timer_active[2]{false, false};
    int64_t m_timer_remaining[2]{0, 0};
    bool m_irq{false};
    int64_t m_busy_remaining{0};
};

}  // namespace

struct taitoz_sound_synth::implementation {
    taitoz_ymfm_interface intf;
    ymfm::ym2610 chip;
    // The chip runs at its own rate; this accumulator resamples its output
    // up to the host rate.
    uint64_t sample_pos{0};
    ymfm::ym2610::output_data last{};

    implementation() : chip(intf) {}
};

taitoz_sound_synth::taitoz_sound_synth()
    : m_impl(std::make_unique<implementation>()) {
    reset();
}

taitoz_sound_synth::~taitoz_sound_synth() = default;

void taitoz_sound_synth::reset() {
    m_impl->chip.reset();
    m_impl->sample_pos = 0;
}

// Use ymfm's own offset dispatch rather than hand-rolling it: offset 2 is
// the *extended* status (ADPCM end-of-sample flags), not a second copy of
// the FM/timer status. Returning the wrong one there leaves a driver that
// polls for sample-end spinning forever.
void taitoz_sound_synth::write_port(unsigned port, uint8_t data) {
    m_impl->chip.write(port & 3u, data);
}

uint8_t taitoz_sound_synth::read_port(unsigned port) {
    return m_impl->chip.read(port & 3u);
}

void taitoz_sound_synth::set_adpcm_a(const uint8_t* rom, std::size_t bytes) {
    m_impl->intf.m_adpcm_a = rom;
    m_impl->intf.m_adpcm_a_size = bytes;
}

void taitoz_sound_synth::set_adpcm_b(const uint8_t* rom, std::size_t bytes) {
    m_impl->intf.m_adpcm_b = rom;
    m_impl->intf.m_adpcm_b_size = bytes;
}

void taitoz_sound_synth::advance_timer_clocks(uint32_t clocks) {
    m_impl->intf.advance(clocks);
}

bool taitoz_sound_synth::irq_asserted() const { return m_impl->intf.irq(); }

void taitoz_sound_synth::debug_counters(unsigned& scheduled, unsigned& expired,
                                        unsigned& irqs) const {
    scheduled = m_impl->intf.m_scheduled;
    expired = m_impl->intf.m_expired;
    irqs = m_impl->intf.m_irqs;
}

void taitoz_sound_synth::adpcm_counters(unsigned& reads, unsigned& a_reads,
                                        unsigned& a_oob) const {
    reads = m_impl->intf.m_ext_reads;
    a_reads = m_impl->intf.m_ext_a;
    a_oob = m_impl->intf.m_ext_a_oob;
}

void taitoz_sound_synth::generate(int16_t* output, int frames) {
    // The YM2610 produces samples at clock/144; step through its native
    // rate and hold each output for the host frames it spans.
    const uint32_t chip_rate = chip_clock_hz / 144;
    ymfm::ym2610::output_data out;
    for (int i = 0; i < frames; ++i) {
        m_impl->sample_pos += chip_rate;
        while (m_impl->sample_pos >= static_cast<uint64_t>(sample_rate)) {
            m_impl->sample_pos -= sample_rate;
            m_impl->chip.generate(&out, 1);
            m_impl->last = out;
        }
        // FM outputs 0/1 are the stereo pair; output 2 is the SSG, which
        // the board mixes into both sides.
        const int32_t ssg = m_impl->last.data[2];
        int32_t l = m_impl->last.data[0] + ssg;
        int32_t r = m_impl->last.data[1] + ssg;
        output[i * 2]     = static_cast<int16_t>(std::clamp(l, -32768, 32767));
        output[i * 2 + 1] = static_cast<int16_t>(std::clamp(r, -32768, 32767));
    }
}

std::unique_ptr<taitoz_sound_synth> make_taitoz_sound_synth() {
    return std::make_unique<taitoz_sound_synth>();
}

// =====================================================================
// OpenAL streaming worker
// =====================================================================

taitoz_audio_system::taitoz_audio_system(
        std::unique_ptr<taitoz_sound_synth> synth)
    : m_synth(std::move(synth)) {}

taitoz_audio_system::~taitoz_audio_system() { shutdown(); }

bool taitoz_audio_system::initialize() {
    if (m_initialized.load()) return true;
    ALCdevice* device = alcOpenDevice(nullptr);
    if (!device) return false;
    ALCcontext* context = alcCreateContext(device, nullptr);
    if (!context) {
        alcCloseDevice(device);
        return false;
    }
    alcMakeContextCurrent(context);
    m_device = device;
    m_context = context;
    alGenSources(1, &m_source);
    alGenBuffers(buffer_count, m_buffers.data());
    m_initialized.store(true);
    return true;
}

void taitoz_audio_system::fill_buffer(uint32_t buffer) {
    {
        std::lock_guard<std::mutex> lock(m_synth_mutex);
        if (m_paused.load())
            std::memset(m_samples.data(), 0, m_samples.size() * sizeof(int16_t));
        else
            m_synth->generate(m_samples.data(), buffer_frames);
    }
    const arcade_audio_output::metrics metrics =
        m_output_normalizer.process_in_place(
            m_samples.data(), m_samples.size(), 2,
            taitoz_sound_synth::sample_rate,
            m_master_volume.load(), m_music_volume.load());
    m_peak_sample.store(metrics.peak);
    alBufferData(buffer, AL_FORMAT_STEREO16, m_samples.data(),
                 static_cast<ALsizei>(m_samples.size() * sizeof(int16_t)),
                 taitoz_sound_synth::sample_rate);
}

void taitoz_audio_system::audio_loop() {
    for (uint32_t buffer : m_buffers) fill_buffer(buffer);
    alSourceQueueBuffers(m_source, buffer_count, m_buffers.data());
    alSourcePlay(m_source);
    while (m_running.load()) {
        ALint processed = 0;
        alGetSourcei(m_source, AL_BUFFERS_PROCESSED, &processed);
        while (processed-- > 0) {
            ALuint buffer = 0;
            alSourceUnqueueBuffers(m_source, 1, &buffer);
            fill_buffer(buffer);
            alSourceQueueBuffers(m_source, 1, &buffer);
        }
        ALint state = 0;
        alGetSourcei(m_source, AL_SOURCE_STATE, &state);
        if (state != AL_PLAYING) alSourcePlay(m_source);
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
}

void taitoz_audio_system::start() {
    if (!m_initialized.load() || m_running.load()) return;
    m_running.store(true);
    m_thread = std::thread([this] { audio_loop(); });
}

void taitoz_audio_system::stop() {
    if (!m_running.exchange(false)) return;
    if (m_thread.joinable()) m_thread.join();
    if (m_initialized.load()) alSourceStop(m_source);
}

void taitoz_audio_system::shutdown() {
    stop();
    if (!m_initialized.exchange(false)) return;
    alDeleteSources(1, &m_source);
    alDeleteBuffers(buffer_count, m_buffers.data());
    alcMakeContextCurrent(nullptr);
    if (m_context) alcDestroyContext(static_cast<ALCcontext*>(m_context));
    if (m_device) alcCloseDevice(static_cast<ALCdevice*>(m_device));
    m_context = nullptr;
    m_device = nullptr;
}

void taitoz_audio_system::write_port(unsigned port, uint8_t data) {
    std::lock_guard<std::mutex> lock(m_synth_mutex);
    m_synth->write_port(port, data);
}

uint8_t taitoz_audio_system::read_port(unsigned port) {
    std::lock_guard<std::mutex> lock(m_synth_mutex);
    return m_synth->read_port(port);
}

void taitoz_audio_system::advance_timer_clocks(uint32_t clocks) {
    std::lock_guard<std::mutex> lock(m_synth_mutex);
    m_synth->advance_timer_clocks(clocks);
}

bool taitoz_audio_system::irq_asserted() {
    std::lock_guard<std::mutex> lock(m_synth_mutex);
    return m_synth->irq_asserted();
}

void taitoz_audio_system::set_mix_levels(int master, int music, int effects) {
    m_master_volume.store(master);
    m_music_volume.store(music);
    m_effects_volume.store(effects);
}
