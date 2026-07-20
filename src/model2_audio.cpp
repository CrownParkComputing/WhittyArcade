#include "model2_audio.h"
#include "arcade_audio_output.h"

#include <AL/al.h>
#include <AL/alc.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

extern "C" {
#include "m68k.h"
}

namespace {
constexpr uint32_t address_mask = 0x00ffffff;
constexpr double scsp_clock = 45158400.0 / 2.0;
constexpr double sound_cpu_clock = 45158400.0 / 4.0;
constexpr double model2_refresh = 60.0;
// MAME/ElSemi's SCSP pan table carries a 4x mixer-line gain, but the 16-bit
// DAC stage divides the accumulated mix by four again ("smpl >> 2"), so the
// net slot gain is exactly TL x DISDL x pan.  Reapplying the 4x here pushed
// any multi-voice Sega Rally passage into the int16 clamp and distorted the
// music mix; loudness preference belongs to the master volume (up to 200%).
constexpr double host_output_gain = 1.0;
// The SCSP transmits MOBUF over a 31.25 kbaud serial MIDI link: ten bits
// per byte, 3125 bytes per second against the 44.1 kHz sample clock.
constexpr double midi_out_bytes_per_native_sample = 3125.0 / 44100.0;

void write_le16(std::FILE* file, uint16_t value) {
    const uint8_t bytes[]{static_cast<uint8_t>(value),
                          static_cast<uint8_t>(value >> 8)};
    std::fwrite(bytes, 1, sizeof(bytes), file);
}

void write_le32(std::FILE* file, uint32_t value) {
    const uint8_t bytes[]{static_cast<uint8_t>(value),
                          static_cast<uint8_t>(value >> 8),
                          static_cast<uint8_t>(value >> 16),
                          static_cast<uint8_t>(value >> 24)};
    std::fwrite(bytes, 1, sizeof(bytes), file);
}

void write_wave_header(std::FILE* file, uint32_t data_bytes) {
    std::rewind(file);
    std::fwrite("RIFF", 1, 4, file);
    write_le32(file, 36 + data_bytes);
    std::fwrite("WAVEfmt ", 1, 8, file);
    write_le32(file, 16);
    write_le16(file, 1);
    write_le16(file, 2);
    write_le32(file, 48000);
    write_le32(file, 48000 * 2 * sizeof(int16_t));
    write_le16(file, 2 * sizeof(int16_t));
    write_le16(file, 16);
    std::fwrite("data", 1, 4, file);
    write_le32(file, data_bytes);
}

constexpr std::array<double, 64> attack_ms{{
    100000,100000,8100,6900,6000,4800,4000,3400,3000,2400,2000,1700,
    1500,1200,1000,860,760,600,500,430,380,300,250,220,190,150,130,
    110,95,76,63,55,47,38,31,27,24,19,15,13,12,9.4,7.9,6.8,6.0,
    4.7,3.8,3.4,3.0,2.4,2.0,1.8,1.6,1.3,1.1,.93,.85,.65,.53,
    .44,.40,.35,.0,.0
}};
constexpr std::array<double, 64> decay_ms{{
    100000,100000,118200,101300,88600,70900,59100,50700,44300,35500,
    29600,25300,22200,17700,14800,12700,11100,8900,7400,6300,5500,
    4400,3700,3200,2800,2200,1800,1600,1400,1100,920,790,690,550,
    460,390,340,270,230,200,170,140,110,98,85,68,57,49,43,34,28,
    25,22,18,14,12,11,8.5,7.1,6.1,5.4,4.3,3.6,3.1
}};
constexpr std::array<double, 32> lfo_frequency{{
    .17,.19,.23,.27,.34,.39,.45,.55,.68,.78,.92,1.10,1.39,1.60,1.87,2.27,
    2.87,3.31,3.92,4.79,6.15,7.18,8.60,10.8,14.4,17.2,21.5,28.7,
    43.1,57.4,86.1,172.3
}};
constexpr std::array<double, 8> amplitude_lfo_scale{{
    0.0,.4,.8,1.5,3.0,6.0,12.0,24.0
}};
constexpr std::array<double, 8> pitch_lfo_scale{{
    0.0,7.0,13.5,27.0,55.0,112.0,230.0,494.0
}};

double attenuation(uint8_t value) {
    double db = 0.0;
    if (value & 0x01) db -= 0.4;
    if (value & 0x02) db -= 0.8;
    if (value & 0x04) db -= 1.5;
    if (value & 0x08) db -= 3.0;
    if (value & 0x10) db -= 6.0;
    if (value & 0x20) db -= 12.0;
    if (value & 0x40) db -= 24.0;
    if (value & 0x80) db -= 48.0;
    return std::pow(10.0, db / 20.0);
}

double send_level(unsigned value) {
    constexpr std::array<double, 8> db{{-120.0, -36.0, -30.0, -24.0,
                                        -18.0, -12.0, -6.0, 0.0}};
    return value ? std::pow(10.0, db[value & 7] / 20.0) : 0.0;
}

double pan_level(unsigned pan) {
    const unsigned amount = pan & 15;
    if (amount == 15) return 0.0;
    double db = 0.0;
    if (amount & 1) db -= 3.0;
    if (amount & 2) db -= 6.0;
    if (amount & 4) db -= 12.0;
    if (amount & 8) db -= 24.0;
    return std::pow(10.0, db / 20.0);
}
}

model2_audio_system::model2_audio_system() = default;
model2_audio_system::~model2_audio_system() { shutdown(); }

bool model2_audio_system::initialize(const model2_roms& roms) {
    if (m_initialized.load()) return true;
    m_output_normalizer.reset();
    if (roms.sound_cpu.empty() || roms.samples.empty()) return false;
    m_program_rom = roms.sound_cpu;
    m_sample_rom = roms.samples;
    m_sound_ram.assign(0x80000, 0);
    std::copy_n(m_program_rom.begin(),
                std::min<std::size_t>(16, m_program_rom.size()),
                m_sound_ram.begin());
    m_scsp_registers.fill(0);
    m_voices = {};
    m_fm_ring.fill(0);
    m_fm_ring_position = 0;
    m_dsp.reset(&m_sound_ram);
    m_timer_counter = {0xffff, 0xffff, 0xffff};
    m_timer_fraction = {};
    m_native_sample_fraction = 0.0;
    m_sample_bank_control = 0x20;
    m_monitor_slot = 0;
    m_dma_memory_address = 0;
    m_dma_register_address = 0;
    m_dma_length = 0;
    m_dma_to_ram = false;
    m_dma_gate = false;
    m_midi_out_read = 0;
    m_midi_out_write = 0;
    m_midi_out_count = 0;
    m_midi_out_fraction = 0.0;
    if (const char* capture = std::getenv("MODEL2_WAVE_CAPTURE")) {
        m_wave_capture = std::fopen(capture, "wb+");
        if (m_wave_capture) write_wave_header(m_wave_capture, 0);
    }

    set_active_musashi_memory(this);
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_pulse_reset();
    set_active_musashi_memory(nullptr);

    m_device = alcOpenDevice(nullptr);
    if (!m_device) {
        std::fprintf(stderr, "Model 2 OpenAL: no output device available\n");
        shutdown();
        return false;
    }
    m_context = alcCreateContext(static_cast<ALCdevice*>(m_device), nullptr);
    if (!m_context ||
        !alcMakeContextCurrent(static_cast<ALCcontext*>(m_context))) {
        std::fprintf(stderr, "Model 2 OpenAL: context creation failed\n");
        shutdown();
        return false;
    }
    alGenSources(1, &m_source);
    alGenBuffers(OPENAL_BUFFER_COUNT, m_buffers.data());
    const bool ok = alGetError() == AL_NO_ERROR;
    alcMakeContextCurrent(nullptr);
    if (!ok) {
        std::fprintf(stderr, "Model 2 OpenAL: buffer allocation failed\n");
        shutdown();
        return false;
    }
    m_initialized = true;
    std::printf("Model 2 audio initialized: 68000 %.4f MHz, SCSP %.4f MHz\n",
                sound_cpu_clock / 1000000.0, scsp_clock / 1000000.0);
    return true;
}

void model2_audio_system::shutdown() {
    stop();
    if (m_wave_capture) {
        write_wave_header(m_wave_capture, m_wave_capture_bytes);
        std::fclose(m_wave_capture);
        m_wave_capture = nullptr;
        m_wave_capture_bytes = 0;
    }
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
    m_initialized = false;
}

void model2_audio_system::set_mix_levels(int master, int music, int effects) {
    m_master_volume.store(std::clamp(master, 0, 200));
    m_music_volume.store(std::clamp(music, 0, 100));
    m_effects_volume.store(std::clamp(effects, 0, 100));
}

bool model2_audio_system::enqueue_midi(uint8_t data) {
    const uint32_t write = m_midi_write.load(std::memory_order_relaxed);
    const uint32_t next = (write + 1) % MIDI_CAPACITY;
    if (next == m_midi_read.load(std::memory_order_acquire)) return false;
    m_midi_transport[write] = data;
    m_midi_write.store(next, std::memory_order_release);
    m_midi_bytes.fetch_add(1, std::memory_order_relaxed);
    if (std::getenv("MODEL2_AUDIO_TRACE"))
        std::fprintf(stderr, "Model 2 MIDI enqueue %02x\n", data);
    return true;
}

void model2_audio_system::transport_midi() {
    uint32_t read = m_midi_read.load(std::memory_order_relaxed);
    const uint32_t write = m_midi_write.load(std::memory_order_acquire);
    while (read != write && m_midi_fifo_count < m_midi_fifo.size()) {
        m_midi_fifo[m_midi_fifo_write] = m_midi_transport[read];
        m_midi_fifo_write = (m_midi_fifo_write + 1) & 31;
        ++m_midi_fifo_count;
        read = (read + 1) % MIDI_CAPACITY;
    }
    m_midi_read.store(read, std::memory_order_release);
    if (m_midi_fifo_count)
        set_scsp_word(0x420, scsp_word(0x420) | 0x0008);
    static bool midi_pending_logged = false;
    if (m_midi_fifo_count && std::getenv("MODEL2_AUDIO_TRACE") &&
        !midi_pending_logged) {
        midi_pending_logged = true;
        std::fprintf(stderr,
                     "Model 2 MIDI pending count=%u pend=%04x en=%04x "
                     "levels=%04x/%04x/%04x\n",
                     m_midi_fifo_count, scsp_word(0x420), scsp_word(0x41e),
                     scsp_word(0x424), scsp_word(0x426), scsp_word(0x428));
    }
    update_irq();
}

uint8_t model2_audio_system::pop_midi() {
    uint8_t value = 0xff;
    if (m_midi_fifo_count) {
        value = m_midi_fifo[m_midi_fifo_read];
        m_midi_fifo_read = (m_midi_fifo_read + 1) & 31;
        --m_midi_fifo_count;
    }
    if (!m_midi_fifo_count)
        set_scsp_word(0x420, scsp_word(0x420) & ~uint16_t{0x0008});
    update_irq();
    if (std::getenv("MODEL2_AUDIO_TRACE"))
        std::fprintf(stderr, "Model 2 MIDI pop %02x remaining=%u\n",
                     value, m_midi_fifo_count);
    return value;
}

int model2_audio_system::next_frame_samples() {
    m_output_frame_fraction +=
        static_cast<uint64_t>((OUTPUT_RATE / model2_refresh) * (uint64_t{1} << 32));
    const int frames = static_cast<int>(m_output_frame_fraction >> 32);
    m_output_frame_fraction &= 0xffffffffu;
    return std::clamp(frames, 1, MAX_BUFFER_FRAMES);
}

void model2_audio_system::execute_sound_cpu(int cycles) {
    int completed = 0;
    while (completed < cycles) {
        transport_midi();
        const int amount = std::min(256, cycles - completed);
        const int executed = m68k_execute(amount);
        if (executed <= 0) break;
        completed += executed;
    }
    m_program_counter.store(m68k_get_reg(nullptr, M68K_REG_PC),
                            std::memory_order_relaxed);
    m_executed_cycles.fetch_add(completed, std::memory_order_relaxed);
}

uint16_t model2_audio_system::scsp_word(unsigned offset) const {
    offset &= 0xffe;
    return static_cast<uint16_t>((m_scsp_registers[offset] << 8) |
                                 m_scsp_registers[offset + 1]);
}

uint16_t model2_audio_system::monitor_status() const {
    // CA/SGC/EG must reflect the MSLC-selected slot at the moment of the
    // read.  Sega Rally's stream feeder writes MSLC then immediately reads
    // this word for each of its music channels; serving a value computed
    // during the previous render pass returned the prior slot's position,
    // so every stream appeared derailed and the driver keyed them all off.
    const voice_state& monitored = m_voices[m_monitor_slot & 31];
    const unsigned current_address =
        static_cast<unsigned>(monitored.phase >> (32 + 12)) & 15;
    const unsigned state = static_cast<unsigned>(monitored.stage) & 3;
    const unsigned envelope =
        (31u - (static_cast<unsigned>(std::clamp(monitored.envelope,
                                                 0.0, 1.0) * 1023.0) >> 5)) & 31;
    return static_cast<uint16_t>((current_address << 7) |
                                 (state << 5) | envelope);
}

void model2_audio_system::set_scsp_word(unsigned offset, uint16_t value) {
    offset &= 0xffe;
    m_scsp_registers[offset] = static_cast<uint8_t>(value >> 8);
    m_scsp_registers[offset + 1] = static_cast<uint8_t>(value);
}

void model2_audio_system::update_slot_step(unsigned slot) {
    const uint16_t pitch = scsp_word(slot * 0x20 + 0x10);
    int octave = static_cast<int>((pitch >> 11) & 15);
    octave = (octave ^ 8) - 8;
    double step = (1024.0 + (pitch & 0x3ff)) / 1024.0;
    step = std::ldexp(step, octave);
    step *= static_cast<double>(NATIVE_RATE) / OUTPUT_RATE;
    m_voices[slot].step = static_cast<uint64_t>(
        std::max(0.0, step) * (uint64_t{1} << 32));
}

void model2_audio_system::start_slot(unsigned slot) {
    voice_state& voice = m_voices[slot];
    if (std::getenv("MODEL2_AUDIO_TRACE")) {
        const unsigned base = slot * 0x20;
        std::fprintf(stderr,
                     "Model 2 SCSP key-on slot=%u ctl=%04x sa=%05x "
                     "loop=%04x-%04x pitch=%04x tl=%02x mix=%04x\n",
                     slot, scsp_word(base),
                     ((scsp_word(base) & 15) << 16) | scsp_word(base + 2),
                     scsp_word(base + 4), scsp_word(base + 6),
                     scsp_word(base + 16), scsp_word(base + 12) & 0xff,
                     scsp_word(base + 22));
    }
    update_slot_step(slot);
    voice.phase = 0;
    // The SCSP begins attack at 0x17f rather than digital silence.
    voice.envelope = 383.0 / 1023.0;
    voice.pitch_lfo_phase = 0.0;
    voice.amplitude_lfo_phase = 0.0;
    voice.stage = envelope_stage::attack;
    voice.backwards = false;
    voice.noise_lfsr = 1u | (slot << 1);
    voice.active = true;
}

void model2_audio_system::stop_slot(unsigned slot, bool release) {
    voice_state& voice = m_voices[slot];
    if (release && voice.active)
        voice.stage = envelope_stage::release;
    else
        voice.active = false;
    set_scsp_word(slot * 0x20,
                  scsp_word(slot * 0x20) & ~uint16_t{0x0800});
}

void model2_audio_system::update_slot(unsigned slot, unsigned byte_offset) {
    if (byte_offset > 1) {
        // Pitch writes alter playback rate in place. Re-running key-on here
        // reset the phase and envelope, so continuously modulated engine and
        // music voices replayed their first few milliseconds forever.
        if ((byte_offset & ~1U) == 0x10)
            update_slot_step(slot);
        else if ((byte_offset & ~1U) == 0x12 &&
                 (scsp_word(slot * 0x20 + 0x12) & 0x8000)) {
            m_voices[slot].pitch_lfo_phase = 0.0;
            m_voices[slot].amplitude_lfo_phase = 0.0;
        }
        return;
    }
    uint16_t control = scsp_word(slot * 0x20);
    if (!(control & 0x1000)) return;
    if (std::getenv("MODEL2_AUDIO_TRACE"))
        std::fprintf(stderr, "Model 2 SCSP key execute slot=%u ctl=%04x\n",
                     slot, control);
    for (unsigned index = 0; index < m_voices.size(); ++index) {
        const bool key_on = scsp_word(index * 0x20) & 0x0800;
        // Key-off only moves a slot to the release stage; it keeps rendering
        // until the envelope reaches zero. A key-on landing inside that
        // release tail must retrigger the slot from attack. Testing only
        // !active dropped those key-ons, so rapidly retriggered Sega Rally
        // music/speech notes faded out and never sounded again.
        if (key_on && (!m_voices[index].active ||
                       m_voices[index].stage == envelope_stage::release))
            start_slot(index);
        else if (!key_on && m_voices[index].active) {
            if (std::getenv("MODEL2_AUDIO_TRACE") &&
                m_voices[index].stage != envelope_stage::release)
                std::fprintf(stderr,
                             "Model 2 SCSP key-off slot=%u pos=%04x "
                             "stage=%u\n",
                             index,
                             static_cast<unsigned>(
                                 m_voices[index].phase >> 32),
                             static_cast<unsigned>(m_voices[index].stage));
            stop_slot(index, true);
        }
    }
    set_scsp_word(slot * 0x20, control & ~uint16_t{0x1000});
}

void model2_audio_system::update_global(unsigned byte_offset) {
    const unsigned reg = byte_offset & 0x3e;
    if (reg == 0x02) {
        const uint16_t value = scsp_word(0x402);
        m_dsp.set_ring_buffer(value & 0x3f,
                              8u * 1024u << ((value >> 7) & 3));
    } else if (reg == 0x12) {
        m_dma_memory_address =
            (m_dma_memory_address & 0xf0000) | (scsp_word(0x412) & 0xfffe);
    } else if (reg == 0x14) {
        const uint16_t value = scsp_word(0x414);
        m_dma_memory_address =
            ((static_cast<uint32_t>(value) & 0xf000) << 4) |
            (m_dma_memory_address & 0x0fffe);
        m_dma_register_address = value & 0x0ffe;
    } else if (reg == 0x16) {
        const uint16_t value = scsp_word(0x416);
        m_dma_length = value & 0x0ffe;
        m_dma_to_ram = value & 0x2000;
        m_dma_gate = value & 0x4000;
        if (value & 0x1000) execute_scsp_dma();
    } else if (reg == 0x08) {
        // MSLC is write-only. Reads return the selected slot's latched
        // current-address and envelope state, refreshed by the sample clock.
        m_monitor_slot = static_cast<uint8_t>((scsp_word(0x408) >> 11) & 31);
    } else if (reg == 0x18 || reg == 0x1a || reg == 0x1c) {
        const unsigned timer = (reg - 0x18) / 2;
        const uint16_t value = scsp_word(0x400 + reg);
        m_timer_prescale[timer] = 1u << ((value >> 8) & 7);
        m_timer_counter[timer] = (value & 0xff) << 8;
        m_timer_fraction[timer] = 0;
        if (std::getenv("MODEL2_AUDIO_TRACE"))
            std::fprintf(stderr,
                         "Model 2 SCSP timer%c arm start=%02x prescale=%u "
                         "en=%04x lv=%04x/%04x/%04x\n",
                         'A' + timer, value & 0xff, m_timer_prescale[timer],
                         scsp_word(0x41e), scsp_word(0x424), scsp_word(0x426),
                         scsp_word(0x428));
    } else if (reg == 0x22) {
        const uint16_t reset = scsp_word(0x422);
        set_scsp_word(0x420, scsp_word(0x420) & ~reset);
        // Expired SCSP timers remain pending until software reloads them.
        // Clearing SCIPD alone immediately reasserts their pending bits.
        for (unsigned timer = 0; timer < 3; ++timer) {
            const uint16_t bit = static_cast<uint16_t>(0x40 << timer);
            if ((reset & bit) && m_timer_counter[timer] == 0xffff)
                set_scsp_word(0x420, scsp_word(0x420) | bit);
        }
    }
    update_irq();
}

void model2_audio_system::execute_scsp_dma() {
    // The sound driver uploads the SCSP DSP program and coefficients through
    // this engine. DTLG is a byte count, while both buses transfer words.
    const std::array<uint16_t, 3> saved{{
        scsp_word(0x412), scsp_word(0x414), scsp_word(0x416)}};
    uint32_t memory = m_dma_memory_address;
    uint16_t reg = m_dma_register_address;

    for (unsigned offset = 0; offset < m_dma_length; offset += 2) {
        if (m_dma_to_ram) {
            uint16_t value = 0;
            if (!m_dma_gate)
                value = read16(0x100000u + reg);
            if (memory + 1 < m_sound_ram.size()) {
                m_sound_ram[memory] = static_cast<uint8_t>(value >> 8);
                m_sound_ram[memory + 1] = static_cast<uint8_t>(value);
            }
        } else {
            uint16_t value = 0;
            if (!m_dma_gate && memory + 1 < m_sound_ram.size())
                value = static_cast<uint16_t>((m_sound_ram[memory] << 8) |
                                              m_sound_ram[memory + 1]);
            write16(0x100000u + reg, value);
        }
        memory = (memory + 2) & 0xffffe;
        reg = static_cast<uint16_t>((reg + 2) & 0x0ffe);
    }

    // SCSP-register-bound DMA cannot overwrite its own live parameter words.
    if (!m_dma_to_ram) {
        set_scsp_word(0x412, saved[0]);
        set_scsp_word(0x414, saved[1]);
        set_scsp_word(0x416, saved[2]);
    }
    set_scsp_word(0x416, scsp_word(0x416) & ~uint16_t{0x1000});

    if (std::getenv("MODEL2_AUDIO_TRACE"))
        std::fprintf(stderr,
                     "Model 2 SCSP DMA %s mem=%05x reg=%03x bytes=%u gate=%u\n",
                     m_dma_to_ram ? "reg->ram" : "ram->reg",
                     m_dma_memory_address, m_dma_register_address,
                     m_dma_length, m_dma_gate ? 1u : 0u);
}

void model2_audio_system::update_irq() {
    const uint16_t pending = scsp_word(0x420);
    const uint16_t enabled = scsp_word(0x41e);
    const uint16_t active = pending & enabled;
    unsigned source = 0;
    if (active & 0x40) source = 6;
    else if (active & 0x180) source = 7;
    else if (active & 0x08) source = 3;
    if (!source) {
        m68k_set_irq(0);
        return;
    }
    const uint16_t bit = uint16_t{1} << source;
    const unsigned level = ((scsp_word(0x424) & bit) ? 1 : 0) |
                           ((scsp_word(0x426) & bit) ? 2 : 0) |
                           ((scsp_word(0x428) & bit) ? 4 : 0);
    if (std::getenv("MODEL2_AUDIO_TRACE")) {
        static unsigned last_source = ~0u;
        if (source != last_source) {
            last_source = source;
            std::fprintf(stderr,
                         "Model 2 SCSP IRQ source=%u level=%u pend=%04x "
                         "en=%04x\n",
                         source, level ? level : 1, pending, enabled);
        }
    }
    m68k_set_irq(level ? level : 1);
}

void model2_audio_system::tick_timers(int native_samples) {
    for (unsigned timer = 0; timer < 3; ++timer) {
        if (m_timer_counter[timer] >= 0xffff) continue;
        // SCSP timers are 8-bit counters exposed in the high byte. One tick
        // occurs every programmed-prescaler samples at the 44.1 kHz clock;
        // the former multiply/raw-16-bit implementation could be orders of
        // magnitude slow or fast depending on TACTL.
        const uint32_t elapsed = m_timer_fraction[timer] +
            static_cast<uint32_t>(std::max(native_samples, 0));
        const uint32_t ticks = elapsed / m_timer_prescale[timer];
        m_timer_fraction[timer] = elapsed % m_timer_prescale[timer];
        const uint32_t count = (m_timer_counter[timer] >> 8) + ticks;
        if (count < 0xff) {
            m_timer_counter[timer] = count << 8;
            const unsigned reg = 0x418 + timer * 2;
            set_scsp_word(reg, (scsp_word(reg) & 0xff00) |
                                  static_cast<uint16_t>(count));
            continue;
        }
        m_timer_counter[timer] = 0xffff;
        const unsigned reg = 0x418 + timer * 2;
        set_scsp_word(reg, (scsp_word(reg) & 0xff00) | 0xff);
        set_scsp_word(0x420, scsp_word(0x420) |
                      static_cast<uint16_t>(0x40 << timer));
    }
    update_irq();
}

void model2_audio_system::queue_midi_out(uint8_t value) {
    // The SCSP transmits MOBUF from a small FIFO, serially at the MIDI rate.
    // Delivering bytes to the uPD71051C immediately let the second byte of a
    // driver reply overwrite the i960's one-byte receive latch before the
    // first was read, so command handshakes lost data and the main program
    // stopped streaming music and speech commands.
    if (m_midi_out_count < m_midi_out_fifo.size()) {
        m_midi_out_fifo[m_midi_out_write] = value;
        m_midi_out_write = (m_midi_out_write + 1) & 31;
        ++m_midi_out_count;
    }
}

void model2_audio_system::tick_midi_out(int native_samples) {
    if (!m_midi_out_count) {
        // An idle transmitter starts the next character from a fresh start
        // bit, so the first queued byte always takes one full frame time.
        m_midi_out_fraction = 0.0;
        return;
    }
    m_midi_out_fraction += native_samples * midi_out_bytes_per_native_sample;
    while (m_midi_out_fraction >= 1.0 && m_midi_out_count) {
        m_midi_out_fraction -= 1.0;
        const uint8_t value = m_midi_out_fifo[m_midi_out_read];
        m_midi_out_read = (m_midi_out_read + 1) & 31;
        --m_midi_out_count;
        if (m_midi_out_callback) m_midi_out_callback(value);
    }
}

void model2_audio_system::render_scsp(int16_t* output, int frames) {
    if (!output || frames <= 0) return;
    std::vector<int32_t> wide(static_cast<std::size_t>(frames) * 2);
    const int ui_mix =
        (m_music_volume.load(std::memory_order_relaxed) +
         m_effects_volume.load(std::memory_order_relaxed)) / 2;
    render_scsp_wide(wide.data(), frames, ui_mix);
    arcade_audio_output::condition_from_int32(
        wide.data(), output, wide.size(),
        m_master_volume.load(std::memory_order_relaxed));
}

void model2_audio_system::render_scsp_wide(int32_t* output, int frames,
                                            int ui_mix) {
    int active = 0;
    const double master = (scsp_word(0x400) & 15) / 15.0;
    ui_mix = std::clamp(ui_mix, 0, 100);
    for (int frame = 0; frame < frames; ++frame) {
        double left = 0.0;
        double right = 0.0;
        for (unsigned slot = 0; slot < m_voices.size(); ++slot) {
            voice_state& voice = m_voices[slot];
            const unsigned ring_destination = m_fm_ring_position;
            m_fm_ring_position = (m_fm_ring_position + 1) & 63;
            if (!voice.active) continue;
            ++active;
            const unsigned base = slot * 0x20;
            const uint16_t control = scsp_word(base);
            const uint32_t start = ((control & 15) << 16) |
                                   scsp_word(base + 2);
            const uint32_t loop_start = scsp_word(base + 4);
            const uint32_t loop_end = scsp_word(base + 6);
            // A zero LEA is not a stop condition on hardware: MAME keeps the
            // slot running (a looping voice simply wraps straight back to
            // LSA). Killing the voice here silenced Sega Rally's streamed
            // music channels whenever the driver rewrote loop bounds.
            int64_t position = static_cast<int64_t>(voice.phase >> 32);
            const uint32_t fraction = static_cast<uint32_t>(voice.phase);
            auto read_sample = [&](int64_t index) -> int16_t {
                if (index < 0) return 0;
                if (control & 0x10) {
                    // Sound RAM is stored in bus-address order. The SCSP's
                    // sample bus is big-endian, just like the 68000 view, so
                    // an 8-bit voice reads the addressed byte directly. The
                    // former XOR belonged to word-backed little-endian cores
                    // and swapped every adjacent pair of Sega Rally samples.
                    const uint64_t address = start + static_cast<uint64_t>(index);
                    if (address >= m_sound_ram.size()) return 0;
                    return static_cast<int16_t>(
                        static_cast<int8_t>(m_sound_ram[address]) << 8);
                }
                const uint64_t address = start + static_cast<uint64_t>(index) * 2;
                if (address + 1 >= m_sound_ram.size()) return 0;
                return static_cast<int16_t>((m_sound_ram[address] << 8) |
                                            m_sound_ram[address + 1]);
            };

            // The modulation ring is addressed in slot clocks. MDXSL/MDYSL
            // select two prior slot outputs and MDL controls their phase
            // modulation depth.
            const uint16_t modulation = scsp_word(base + 14);
            const unsigned modulation_level = modulation >> 12;
            if (modulation_level || (modulation & 0x0fff)) {
                const unsigned x = (modulation >> 6) & 63;
                const unsigned y = modulation & 63;
                int32_t offset = (m_fm_ring[(ring_destination + x) & 63] +
                                  m_fm_ring[(ring_destination + y) & 63]) / 2;
                offset <<= 10;
                offset >>= 26 - modulation_level;
                // The hardware's extra left shift for 16-bit voices converts
                // this sample-count offset to its byte-addressed fetch; the
                // read helper below already multiplies by the sample width,
                // so the offset applies equally to both PCM formats here.
                position += offset;
            }

            const unsigned source_control = (control >> 7) & 3;
            double sample = 0.0;
            if (source_control == 0) {
                // The interpolation neighbour follows the loop: on a normal
                // forward loop the sample after LEA-1 is the one at LSA.
                // Reading straight past LEA blended one word of unrelated
                // RAM into every loop pass of sustained instrument notes.
                int64_t neighbor = position + 1;
                if (((control >> 5) & 3) == 1 && loop_end &&
                    neighbor >= loop_end)
                    neighbor = loop_start + (neighbor - loop_end);
                const int32_t first = read_sample(position);
                const int32_t second = read_sample(neighbor);
                sample = first + (second - first) *
                    (static_cast<double>(fraction) / 4294967296.0);
            } else if (source_control == 1) {
                // Internally generated SCSP noise. Slots with SSCTL 2/3
                // produce zero; treating any of these modes as PCM exposes
                // scratch/control pages as a loud 8 KiB repeating sample.
                const uint32_t feedback =
                    ((voice.noise_lfsr >> 0) ^ (voice.noise_lfsr >> 1)) & 1;
                voice.noise_lfsr = (voice.noise_lfsr >> 1) |
                                   (feedback << 15);
                sample = static_cast<int16_t>(voice.noise_lfsr);
            }
            if (control & 0x0200)
                sample = static_cast<int16_t>(static_cast<int16_t>(sample) ^
                                              0x7fff);
            if (control & 0x0400)
                sample = static_cast<int16_t>(static_cast<int16_t>(sample) ^
                                              0x8000);

            const uint16_t envelope_a = scsp_word(base + 8);
            const uint16_t envelope_b = scsp_word(base + 10);
            const uint16_t pitch = scsp_word(base + 16);
            const int octave = static_cast<int>((pitch >> 11) & 15);
            const int signed_octave = (octave ^ 8) - 8;
            const unsigned key_rate = (envelope_b >> 10) & 15;
            const int rate_base = key_rate == 15 ? 0 : signed_octave +
                2 * static_cast<int>(key_rate) + ((pitch >> 9) & 1);
            auto rate_index = [&](unsigned value) {
                return std::clamp(rate_base + static_cast<int>(value * 2),
                                  0, 63);
            };
            const unsigned attack = envelope_a & 31;
            const unsigned decay1 = (envelope_a >> 6) & 31;
            const unsigned decay2 = (envelope_a >> 11) & 31;
            const unsigned release = envelope_b & 31;
            const double decay_level =
                ((31 - ((envelope_b >> 5) & 31)) * 32.0) / 1023.0;
            auto delta = [](double milliseconds) {
                if (milliseconds <= 0.0) return 1.0;
                return 1000.0 / (OUTPUT_RATE * milliseconds);
            };
            switch (voice.stage) {
            case envelope_stage::attack:
                voice.envelope += delta(attack_ms[rate_index(attack)]);
                if (voice.envelope >= 1.0) {
                    voice.envelope = 1.0;
                    if (!(envelope_b & 0x4000))
                        voice.stage = envelope_stage::decay1;
                }
                break;
            case envelope_stage::decay1:
                voice.envelope -= delta(decay_ms[rate_index(decay1)]);
                if (voice.envelope <= decay_level) {
                    voice.envelope = decay_level;
                    voice.stage = envelope_stage::decay2;
                }
                break;
            case envelope_stage::decay2:
                if (decay2)
                    voice.envelope = std::max(0.0, voice.envelope -
                        delta(decay_ms[rate_index(decay2)]));
                break;
            case envelope_stage::release:
                voice.envelope -= delta(decay_ms[rate_index(release)]);
                if (voice.envelope <= 0.0) {
                    voice.envelope = 0.0;
                    voice.active = false;
                    static const bool trace =
                        std::getenv("MODEL2_AUDIO_TRACE") != nullptr;
                    if (trace)
                        std::fprintf(stderr,
                                     "Model 2 SCSP release-done slot=%u "
                                     "pos=%04x rr=%u rate=%u\n",
                                     slot,
                                     static_cast<unsigned>(voice.phase >> 32),
                                     release,
                                     rate_index(release));
                }
                break;
            }

            const uint16_t level_control = scsp_word(base + 12);
            const bool sound_direct = level_control & 0x0100;
            const bool ring_write_inhibit = level_control & 0x0200;
            const uint8_t total_level = level_control & 0xff;
            const uint16_t lfo = scsp_word(base + 18);
            const unsigned lfo_rate = (lfo >> 10) & 31;
            auto waveform = [&](unsigned shape, double phase,
                                bool amplitude) {
                const int index = static_cast<int>(phase) & 255;
                if (amplitude) {
                    if (shape == 0) return 255 - index;
                    if (shape == 1) return index < 128 ? 255 : 0;
                    if (shape == 2)
                        return index < 128 ? 255 - index * 2 :
                                             index * 2 - 256;
                    return static_cast<int>(voice.noise_lfsr & 0xff);
                }
                if (shape == 0) return index < 128 ? index : index - 256;
                if (shape == 1) return index < 128 ? 127 : -128;
                if (shape == 2) {
                    if (index < 64) return index * 2;
                    if (index < 128) return 255 - index * 2;
                    if (index < 192) return 256 - index * 2;
                    return index * 2 - 511;
                }
                return 128 - static_cast<int>(voice.noise_lfsr & 0xff);
            };
            double pitch_lfo = 1.0;
            const unsigned pitch_depth = (lfo >> 5) & 7;
            if (pitch_depth) {
                voice.pitch_lfo_phase +=
                    lfo_frequency[lfo_rate] * 256.0 / OUTPUT_RATE;
                if (voice.pitch_lfo_phase >= 256.0)
                    voice.pitch_lfo_phase -= 256.0;
                const int value = waveform((lfo >> 8) & 3,
                                           voice.pitch_lfo_phase, false);
                pitch_lfo = std::pow(2.0,
                    pitch_lfo_scale[pitch_depth] * value / (128.0 * 1200.0));
            }
            double amplitude_lfo = 1.0;
            const unsigned amplitude_depth = lfo & 7;
            if (amplitude_depth && !sound_direct) {
                voice.amplitude_lfo_phase +=
                    lfo_frequency[lfo_rate] * 256.0 / OUTPUT_RATE;
                if (voice.amplitude_lfo_phase >= 256.0)
                    voice.amplitude_lfo_phase -= 256.0;
                const int value = waveform((lfo >> 3) & 3,
                                           voice.amplitude_lfo_phase, true);
                amplitude_lfo = std::pow(10.0,
                    -amplitude_lfo_scale[amplitude_depth] * value /
                    (256.0 * 20.0));
            }

            double envelope_gain = 1.0;
            if (!sound_direct) {
                if (voice.stage == envelope_stage::attack)
                    envelope_gain = (envelope_a & 0x20) ? 1.0 : voice.envelope;
                else
                    envelope_gain = std::pow(10.0,
                        3.0 * (voice.envelope * 1023.0 - 1023.0) /
                        (32.0 * 20.0));
            }
            sample *= envelope_gain * amplitude_lfo;

            if (!ring_write_inhibit) {
                // The ring write carries the pan table's 4x line gain into a
                // half-scale store: a net 2x on the modulator, twice the
                // 16-bit sample range, which is why the ring holds 32-bit
                // values rather than clamping.
                const double ring_level = sound_direct ? 1.0 :
                                                   attenuation(total_level);
                m_fm_ring[ring_destination] = static_cast<int32_t>(
                    sample * ring_level * 2.0);
            }

            const uint16_t dsp_input = scsp_word(base + 20);
            const unsigned input_send = dsp_input & 7;
            if (input_send) {
                const unsigned input_select = (dsp_input >> 3) & 15;
                const double input_level = sound_direct ? 1.0 :
                    attenuation(total_level);
                m_dsp.set_sample(static_cast<int32_t>(sample * input_level *
                    send_level(input_send) * 16.0), input_select);
            }

            const uint16_t mix = scsp_word(base + 22);
            const unsigned send = (mix >> 13) & 7;
            const unsigned pan = (mix >> 8) & 31;
            const double gain = (sound_direct ? 1.0 : attenuation(total_level)) *
                                send_level(send);
            const double pan_gain = pan_level(pan);
            left += sample * gain * (pan < 16 ? pan_gain : 1.0);
            right += sample * gain * (pan < 16 ? 1.0 : pan_gain);

            uint64_t increment = static_cast<uint64_t>(voice.step * pitch_lfo);
            if (voice.backwards) {
                voice.phase = voice.phase > increment ?
                    voice.phase - increment : 0;
            } else {
                voice.phase += increment;
            }
            position = static_cast<int64_t>(voice.phase >> 32);
            if ((envelope_b & 0x4000) &&
                voice.stage == envelope_stage::attack &&
                position >= loop_start)
                voice.stage = envelope_stage::decay1;
            const unsigned loop = (control >> 5) & 3;
            if (loop == 0 && position >= loop_end) {
                stop_slot(slot, false);
            } else if (loop == 1 && position >= loop_end) {
                const uint64_t overshoot = voice.phase -
                    (uint64_t{loop_end} << 32);
                voice.phase = (uint64_t{loop_start} << 32) + overshoot;
            } else if (loop == 2) {
                // Reverse loop starts forward, turns around at LSA, and then
                // repeatedly wraps backwards from LSA to LEA. It does not
                // behave like ping-pong at LEA.
                if (!voice.backwards && position >= loop_start) {
                    const uint64_t overshoot = voice.phase -
                        (uint64_t{loop_start} << 32);
                    voice.backwards = true;
                    const uint64_t end = uint64_t{loop_end} << 32;
                    voice.phase = overshoot < end ? end - overshoot : 0;
                } else if (voice.backwards && position < loop_start) {
                    const uint64_t start = uint64_t{loop_start} << 32;
                    const uint64_t overshoot = start > voice.phase ?
                        start - voice.phase : 0;
                    const uint64_t end = uint64_t{loop_end} << 32;
                    voice.phase = overshoot < end ? end - overshoot : 0;
                }
            } else if (loop == 3) {
                if (!voice.backwards && position >= loop_end) {
                    const uint64_t end = uint64_t{loop_end} << 32;
                    const uint64_t overshoot = voice.phase - end;
                    voice.backwards = true;
                    voice.phase = overshoot < end ? end - overshoot : 0;
                } else if (voice.backwards && position < loop_start) {
                    const uint64_t start = uint64_t{loop_start} << 32;
                    const uint64_t overshoot = start > voice.phase ?
                        start - voice.phase : 0;
                    voice.backwards = false;
                    voice.phase = start + overshoot;
                }
            }
        }

        m_dsp.step();
        const auto& effects = m_dsp.effects();
        for (unsigned effect = 0; effect < effects.size(); ++effect) {
            const uint16_t mix = scsp_word(effect * 0x20 + 22);
            const unsigned send = (mix >> 5) & 7;
            if (!send) continue;
            const unsigned pan = mix & 31;
            const double gain = send_level(send);
            const double pan_gain = pan_level(pan);
            left += effects[effect] * gain * (pan < 16 ? pan_gain : 1.0);
            right += effects[effect] * gain * (pan < 16 ? 1.0 : pan_gain);
        }
        const double board_mix = host_output_gain * master * ui_mix / 100.0;
        const auto to_wide_sample = [board_mix](double sample) {
            const double scaled = std::clamp(
                sample * board_mix,
                static_cast<double>(std::numeric_limits<int32_t>::min()),
                static_cast<double>(std::numeric_limits<int32_t>::max()));
            return static_cast<int32_t>(std::lround(scaled));
        };
        output[frame * 2] = to_wide_sample(left);
        output[frame * 2 + 1] = to_wide_sample(right);
    }
    m_native_sample_fraction +=
        frames * (static_cast<double>(NATIVE_RATE) / OUTPUT_RATE);
    const int native_samples = static_cast<int>(m_native_sample_fraction);
    m_native_sample_fraction -= native_samples;
    tick_timers(native_samples);
    tick_midi_out(native_samples);
    m_active_voice_count.store(active / std::max(frames, 1),
                               std::memory_order_relaxed);
}

void model2_audio_system::fill_buffer(int frames) {
    const int cpu_cycles = static_cast<int>(sound_cpu_clock / model2_refresh);
    std::fill_n(m_mix_output.begin(), frames * 2, int32_t{});
    const int music = m_music_volume.load(std::memory_order_relaxed);
    const int effects = m_effects_volume.load(std::memory_order_relaxed);
    const int ui_mix = (music + effects) / 2;
    // Timer IRQs and the 68000 run concurrently with the SCSP on hardware.
    // Interleave them at sub-millisecond intervals so the sequencer can
    // reload a timer and advance music/SFX within one video frame instead of
    // being artificially limited to a single 60 Hz service opportunity.
    constexpr int audio_slice_frames = 32;
    int previous_cycles = 0;
    for (int offset = 0; offset < frames; offset += audio_slice_frames) {
        const int count = std::min(audio_slice_frames, frames - offset);
        const int target_cycles = cpu_cycles * (offset + count) / frames;
        execute_sound_cpu(target_cycles - previous_cycles);
        previous_cycles = target_cycles;
        render_scsp_wide(m_mix_output.data() + offset * 2, count, ui_mix);
    }
    const arcade_audio_output::metrics output =
        m_output_normalizer.process_from_int32(
            m_mix_output.data(), m_output.data(),
            static_cast<std::size_t>(frames) * 2, 2, OUTPUT_RATE,
            m_master_volume.load(std::memory_order_relaxed), ui_mix);
    m_peak_sample.store(output.peak, std::memory_order_relaxed);
    if (std::getenv("MODEL2_VOICE_TRACE") && (++m_rendered_buffers % 60) == 0) {
        uint64_t hash = 1469598103934665603ULL;
        for (int index = 0; index < frames * 2; ++index) {
            hash ^= static_cast<uint16_t>(m_output[index]);
            hash *= 1099511628211ULL;
        }
        std::fprintf(stderr, "Model 2 PCM hash=%016llx voices:",
                     static_cast<unsigned long long>(hash));
        for (unsigned slot = 0; slot < m_voices.size(); ++slot) {
            const voice_state& voice = m_voices[slot];
            if (!voice.active) continue;
            const unsigned base = slot * 0x20;
            const uint16_t control = scsp_word(base);
            const uint32_t start = ((control & 15) << 16) |
                                   scsp_word(base + 2);
            // Content probe: peak magnitude of the first 1024 source bytes.
            // Streamed music buffers that the 68000 failed to fill show up
            // as tiny values here despite healthy envelopes and mixes.
            int content_peak = 0;
            for (uint32_t probe = 0; probe < 1024; ++probe) {
                const uint64_t address = start + probe;
                if (address >= m_sound_ram.size()) break;
                content_peak = std::max(content_peak, std::abs(
                    static_cast<int>(static_cast<int8_t>(
                        m_sound_ram[address]))));
            }
            std::fprintf(stderr, " %u{src%d}", slot, content_peak);
            std::fprintf(stderr,
                         " %u[ctl=%04x src=%u lp=%u sa=%05x %04x-%04x "
                         "pos=%04x%c pitch=%04x step=%llx eg=%u/%.3f "
                         "tl=%02x mix=%04x]",
                         slot, control, (control >> 7) & 3,
                         (control >> 5) & 3, start,
                         scsp_word(base + 4), scsp_word(base + 6),
                         static_cast<unsigned>(voice.phase >> 32),
                         voice.backwards ? '-' : '+',
                         scsp_word(base + 16),
                         static_cast<unsigned long long>(voice.step),
                         static_cast<unsigned>(voice.stage), voice.envelope,
                         scsp_word(base + 12) & 0xff,
                         scsp_word(base + 22));
        }
        std::fputc('\n', stderr);
    }
    if (m_wave_capture) {
        const std::size_t samples = static_cast<std::size_t>(frames) * 2;
        std::fwrite(m_output.data(), sizeof(int16_t), samples, m_wave_capture);
        m_wave_capture_bytes += static_cast<uint32_t>(samples * sizeof(int16_t));
    }
}

void model2_audio_system::audio_loop() {
    set_active_musashi_memory(this);
    if (!alcMakeContextCurrent(static_cast<ALCcontext*>(m_context))) {
        m_running = false;
        set_active_musashi_memory(nullptr);
        return;
    }
    // The audio device is the clock.  The sound board on hardware runs in
    // real time no matter how the geometry side performs, so the 68000 and
    // SCSP advance whenever the stream needs another buffer rather than when
    // the machine thread completes a video frame.  Tying them to the frame
    // signal made every dropped i960/TGP frame stretch and gap the music,
    // speech and SFX; the MIDI transport queue absorbs the residual drift
    // between the video and audio clocks.
    unsigned buffers_used = 0;
    while (m_running.load(std::memory_order_acquire)) {
        ALuint buffer = 0;
        ALint processed = 0;
        alGetSourcei(m_source, AL_BUFFERS_PROCESSED, &processed);
        if (processed > 0)
            alSourceUnqueueBuffers(m_source, 1, &buffer);
        else if (buffers_used < OPENAL_BUFFER_COUNT)
            buffer = m_buffers[buffers_used++];
        else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        const int frames = next_frame_samples();
        if (m_paused.load(std::memory_order_relaxed))
            std::fill_n(m_output.begin(), frames * 2, int16_t{});
        else
            fill_buffer(frames);
        alBufferData(buffer, AL_FORMAT_STEREO16, m_output.data(),
                     frames * 2 * static_cast<int>(sizeof(int16_t)),
                     OUTPUT_RATE);
        alSourceQueueBuffers(m_source, 1, &buffer);
        ALint state = AL_STOPPED;
        alGetSourcei(m_source, AL_SOURCE_STATE, &state);
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

void model2_audio_system::start() {
    if (!m_initialized.load() || m_running.exchange(true)) return;
    m_thread = std::thread(&model2_audio_system::audio_loop, this);
}

void model2_audio_system::stop() {
    m_running = false;
    if (m_thread.joinable()) m_thread.join();
}

uint8_t model2_audio_system::sample_rom_read(uint32_t address) const {
    uint32_t offset = 0xffffffffu;
    if (address >= 0x800000 && address < 0xa00000)
        offset = address - 0x800000;
    else if (address >= 0xa00000 && address < 0xe00000) {
        const uint32_t bank = m_sample_rom.size() > 0x800000 &&
                              !(m_sample_bank_control & 0x20) ?
                                  0x800000 : 0x200000;
        offset = bank + address - 0xa00000;
    } else if (address >= 0xe00000) {
        const uint32_t bank = m_sample_rom.size() > 0x800000 &&
                              !(m_sample_bank_control & 0x20) ?
                                  0xa00000 : 0x600000;
        offset = bank + address - 0xe00000;
    }
    const uint8_t value =
        offset < m_sample_rom.size() ? m_sample_rom[offset] : 0xff;
    static const bool trace = std::getenv("MODEL2_SAMPLE_TRACE") != nullptr;
    if (trace) {
        static uint64_t reads = 0;
        if ((reads++ & 0xffff) == 0)
            std::fprintf(stderr,
                         "Model 2 sample read #%llu bus=%06x rom=%06x "
                         "value=%02x bank=%04x\n",
                         static_cast<unsigned long long>(reads - 1), address,
                         offset, value, m_sample_bank_control);
    }
    return value;
}

uint8_t model2_audio_system::read8(uint32_t raw_address) {
    const uint32_t address = raw_address & address_mask;
    if (address < m_sound_ram.size()) return m_sound_ram[address];
    if (address >= 0x100000 && address < 0x101000) {
        const unsigned offset = address - 0x100000;
        if (offset == 0x405) return pop_midi();
        if (offset == 0x408) return static_cast<uint8_t>(monitor_status() >> 8);
        if (offset == 0x409) return static_cast<uint8_t>(monitor_status());
        if (offset >= 0x600 && offset < 0x700) {
            const uint16_t value = static_cast<uint16_t>(
                m_fm_ring[(offset - 0x600) >> 1]);
            return (offset & 1) ? static_cast<uint8_t>(value) :
                                  static_cast<uint8_t>(value >> 8);
        }
        if (offset >= 0x700) {
            const uint16_t value = m_dsp.read_word(offset & 0xffe);
            return (offset & 1) ? static_cast<uint8_t>(value) :
                                  static_cast<uint8_t>(value >> 8);
        }
        return m_scsp_registers[offset];
    }
    if (address >= 0x600000 && address < 0x680000) {
        const uint32_t offset = address - 0x600000;
        return offset < m_program_rom.size() ? m_program_rom[offset] : 0xff;
    }
    if (address >= 0x800000) return sample_rom_read(address);
    return 0xff;
}

uint16_t model2_audio_system::read16(uint32_t address) {
    const uint32_t masked = address & address_mask;
    if (masked == 0x100404)
        return static_cast<uint16_t>((m_scsp_registers[0x404] << 8) |
                                     pop_midi());
    if (masked == 0x100408) return monitor_status();
    return static_cast<uint16_t>((read8(address) << 8) | read8(address + 1));
}

uint32_t model2_audio_system::read32(uint32_t address) {
    return (static_cast<uint32_t>(read16(address)) << 16) |
           read16(address + 2);
}

void model2_audio_system::write8(uint32_t raw_address, uint8_t value) {
    const uint32_t address = raw_address & address_mask;
    if (address < m_sound_ram.size()) {
        m_sound_ram[address] = value;
        return;
    }
    if (address >= 0x100000 && address < 0x101000) {
        const unsigned offset = address - 0x100000;
        m_scsp_registers[offset] = value;
        if (offset < 0x400)
            update_slot(offset / 0x20, offset & 0x1f);
        else if (offset < 0x600)
            update_global(offset);
        else if (offset < 0x700)
            m_fm_ring[(offset - 0x600) >> 1] =
                static_cast<int16_t>(scsp_word(offset & 0xffe));
        else
            m_dsp.write_word(offset & 0xffe, scsp_word(offset & 0xffe));
        if ((offset == 0x406 || offset == 0x407) &&
            std::getenv("MODEL2_UART_TRACE"))
            std::fprintf(stderr, "Model 2 SCSP MIDI-out byte write %03x=%02x\n",
                         offset, value);
        if (offset == 0x407) {
            // SCSP MIDI-out data byte. On the sound board this serial stream
            // feeds the main CPU's uPD71051C RXD; Sega Rally's sound driver
            // answers command bytes here and the i960 will not stream music
            // until those replies arrive.
            queue_midi_out(value);
        }
        m_scsp_writes.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (address == 0x400000 || address == 0x400001) {
        if ((address & 1) == 0)
            m_sample_bank_control =
                static_cast<uint16_t>((m_sample_bank_control & 0x00ff) |
                                      (value << 8));
        else
            m_sample_bank_control =
                static_cast<uint16_t>((m_sample_bank_control & 0xff00) | value);
    }
}

void model2_audio_system::write16(uint32_t address, uint16_t value) {
    const uint32_t masked = address & address_mask;

    // A 68000 word transfer reaches the SCSP as one register transaction.
    // Routing it through write8() executed KEYONEX after the high byte while
    // the old low byte was still present.  Sega Rally consequently retriggered
    // voices with stale loop/format bits, which made speech and SFX repeat.
    if (!(masked & 1) && masked + 1 < m_sound_ram.size()) {
        m_sound_ram[masked] = static_cast<uint8_t>(value >> 8);
        m_sound_ram[masked + 1] = static_cast<uint8_t>(value);
        return;
    }
    if (!(masked & 1) && masked >= 0x100000 && masked < 0x101000) {
        const unsigned offset = masked - 0x100000;
        m_scsp_registers[offset] = static_cast<uint8_t>(value >> 8);
        m_scsp_registers[offset + 1] = static_cast<uint8_t>(value);
        if (offset < 0x400)
            update_slot(offset / 0x20, offset & 0x1f);
        else if (offset < 0x600)
            update_global(offset);
        else if (offset < 0x700)
            m_fm_ring[(offset - 0x600) >> 1] =
                static_cast<int16_t>(value);
        else
            m_dsp.write_word(static_cast<uint16_t>(offset), value);
        if (offset == 0x406 && std::getenv("MODEL2_UART_TRACE"))
            std::fprintf(stderr, "Model 2 SCSP MIDI-out word write 406=%04x\n",
                         value);
        if (offset == 0x406)
            queue_midi_out(static_cast<uint8_t>(value));
        m_scsp_writes.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!(masked & 1) && masked == 0x400000) {
        m_sample_bank_control = value;
        return;
    }

    // Retain byte-lane behavior for unusual unaligned accesses.
    write8(address, static_cast<uint8_t>(value >> 8));
    write8(address + 1, static_cast<uint8_t>(value));
}

void model2_audio_system::write32(uint32_t address, uint32_t value) {
    write16(address, static_cast<uint16_t>(value >> 16));
    write16(address + 2, static_cast<uint16_t>(value));
}
