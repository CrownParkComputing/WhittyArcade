#include "sega/model2/model2_audio.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

class model2_audio_test_peer {
public:
    static void reset(model2_audio_system& audio, std::size_t ram_size) {
        audio.m_sound_ram.assign(ram_size, 0);
        audio.m_scsp_registers.fill(0);
        audio.m_voices = {};
        audio.m_fm_ring.fill(0);
        audio.m_fm_ring_position = 0;
        audio.m_dsp.reset(&audio.m_sound_ram);
        audio.m_timer_counter = {0xffff, 0xffff, 0xffff};
        audio.m_timer_fraction = {};
        audio.m_native_sample_fraction = 0.0;
        audio.m_monitor_slot = 0;
        audio.m_dma_memory_address = 0;
        audio.m_dma_register_address = 0;
        audio.m_dma_length = 0;
        audio.m_dma_to_ram = false;
        audio.m_dma_gate = false;
        audio.m_midi_out_read = 0;
        audio.m_midi_out_write = 0;
        audio.m_midi_out_count = 0;
        audio.m_midi_out_fraction = 0.0;
        audio.set_scsp_word(0x400, 0x000f);
    }

    static void render(model2_audio_system& audio, int16_t* output,
                       int frames) {
        audio.render_scsp(output, frames);
    }

    static bool active(const model2_audio_system& audio, unsigned slot = 0) {
        return audio.m_voices.at(slot).active;
    }

    static uint64_t phase(const model2_audio_system& audio,
                          unsigned slot = 0) {
        return audio.m_voices.at(slot).phase;
    }

    static uint64_t step(const model2_audio_system& audio,
                         unsigned slot = 0) {
        return audio.m_voices.at(slot).step;
    }
};

namespace {

constexpr uint32_t scsp = 0x100000;

void configure_voice(model2_audio_system& audio, uint16_t control,
                     uint16_t end, uint16_t pitch = 0,
                     uint8_t total_level = 0, uint16_t mix = 0xe000,
                     uint16_t envelope_b = 0) {
    audio.write16(scsp + 0x00, control & ~uint16_t{0x1800});
    audio.write16(scsp + 0x02, 0x0000);
    audio.write16(scsp + 0x04, 0x0000);
    audio.write16(scsp + 0x06, end);
    audio.write16(scsp + 0x08, 0x001f); // fastest attack, no decay
    audio.write16(scsp + 0x0a, envelope_b);
    audio.write16(scsp + 0x0c, total_level);
    audio.write16(scsp + 0x10, pitch);
    audio.write16(scsp + 0x16, mix);
    audio.write16(scsp + 0x00, control | 0x1800); // KEYONB + KEYONEX
}

int peak(const std::vector<int16_t>& samples) {
    int result = 0;
    for (int16_t sample : samples)
        result = std::max(result, std::abs(static_cast<int>(sample)));
    return result;
}

void test_pcm_order_and_pitch_continuity() {
    model2_audio_system audio;
    model2_audio_test_peer::reset(audio, 0x1000);
    for (unsigned index = 0; index < 64; ++index)
        audio.write8(index, static_cast<uint8_t>(index < 32 ? 0x20 : 0xe0));

    configure_voice(audio, 0x0010, 64);
    std::vector<int16_t> output(40 * 2);
    model2_audio_test_peer::render(audio, output.data(), 40);
    assert(peak(output) != 0);
    assert(output[20 * 2] > 0);
    assert(output[38 * 2] < 0);

    const uint64_t phase = model2_audio_test_peer::phase(audio);
    const uint64_t old_step = model2_audio_test_peer::step(audio);
    audio.write16(scsp + 0x10, 0x0800); // raise one octave
    assert(model2_audio_test_peer::phase(audio) == phase);
    assert(model2_audio_test_peer::step(audio) > old_step);
}

void test_loop_and_speech_lifetime() {
    model2_audio_system audio;
    model2_audio_test_peer::reset(audio, 0x20000);
    for (unsigned index = 0; index < 0x10000; ++index)
        audio.write8(index, static_cast<uint8_t>((index & 0x7f) + 1));

    configure_voice(audio, 0x0010, 0xffff);
    std::vector<int16_t> first(32000 * 2);
    model2_audio_test_peer::render(audio, first.data(), 32000);
    assert(peak(first) != 0);
    assert(model2_audio_test_peer::active(audio));

    std::vector<int16_t> tail(45000 * 2);
    model2_audio_test_peer::render(audio, tail.data(), 45000);
    assert(!model2_audio_test_peer::active(audio));

    model2_audio_test_peer::reset(audio, 0x1000);
    for (unsigned index = 0; index < 16; ++index)
        audio.write8(index, static_cast<uint8_t>(index * 8));
    configure_voice(audio, 0x0030, 16); // forward loop
    std::vector<int16_t> looped(1000 * 2);
    model2_audio_test_peer::render(audio, looped.data(), 1000);
    assert(model2_audio_test_peer::active(audio));
    assert((model2_audio_test_peer::phase(audio) >> 32) < 16);
}

void test_reference_mix_level_and_key_off() {
    model2_audio_system audio;
    model2_audio_test_peer::reset(audio, 0x4000);
    for (unsigned index = 0; index < 0x2000; ++index)
        audio.write8(index, (index & 32) ? 0xc0 : 0x40);

    // Sega Rally's boot sequence uses TL=0x2a and DISDL=3. The pan table's
    // 4x line-mixer gain is cancelled by the DAC output stage, so the net
    // slot gain is exactly TL x DISDL: a 0x4000-peak source lands near
    // 16384 x -15.8 dB x -24 dB = about 168 counts.
    configure_voice(audio, 0x0030, 0x1fff, 0, 0x2a, 0x6000, 0x001f);
    std::vector<int16_t> output(4096 * 2);
    model2_audio_test_peer::render(audio, output.data(), 4096);
    const int measured_peak = peak(output);
    assert(measured_peak >= 80);
    assert(measured_peak <= 400);

    audio.write16(scsp + 0x00, 0x1010); // KEYONEX with KEYONB clear
    std::vector<int16_t> release(1024 * 2);
    model2_audio_test_peer::render(audio, release.data(), 1024);
    assert(!model2_audio_test_peer::active(audio));
}

void test_dsp_effect_program() {
    std::vector<uint8_t> ram(0x80000);
    model2_scsp_dsp dsp;
    dsp.reset(&ram);
    dsp.write_word(0x700, 0x7fff); // coefficient 0 = approximately 1.0
    // Step 0 multiplies MIXS0 by coefficient 0. Step 1 shifts the prior
    // accumulator into EFREG0, matching the SCSP DSP pipeline delay.
    dsp.write_word(0x802, 0xa800); // X=MIXS0, Y=COEF
    dsp.write_word(0x804, 0x0030);
    dsp.write_word(0x80c, 0x1030); // EWT, EWA=0, SHIFT=3
    dsp.write_word(0xbf0, 0x0000); // hardware start strobe
    dsp.set_sample(4096, 0);
    dsp.step();
    assert(dsp.effects()[0] != 0);
}

void test_retrigger_during_release_tail() {
    model2_audio_system audio;
    model2_audio_test_peer::reset(audio, 0x1000);
    for (unsigned index = 0; index < 64; ++index)
        audio.write8(index, static_cast<uint8_t>(index < 32 ? 0x60 : 0xa0));

    // Long release: envelope_b release rate 1 (slowest audible decay).
    configure_voice(audio, 0x0030, 64, 0, 0, 0xe000, 0x0001);
    std::vector<int16_t> sounding(512 * 2);
    model2_audio_test_peer::render(audio, sounding.data(), 512);
    assert(peak(sounding) != 0);

    // Key off; the slot must keep rendering through its release tail.
    audio.write16(scsp + 0x00, 0x1030); // KEYONEX, KEYONB clear, loop kept
    std::vector<int16_t> tail(256 * 2);
    model2_audio_test_peer::render(audio, tail.data(), 256);
    assert(model2_audio_test_peer::active(audio));

    // Retrigger mid-release (exactly what Sega Rally's driver does on every
    // rapid note repeat). The slot must restart from attack at phase zero.
    audio.write16(scsp + 0x00, 0x1830); // KEYONB + KEYONEX, looped
    assert(model2_audio_test_peer::phase(audio) == 0);
    std::vector<int16_t> retriggered(512 * 2);
    model2_audio_test_peer::render(audio, retriggered.data(), 512);
    assert(peak(retriggered) != 0);
    assert(model2_audio_test_peer::active(audio));
}

void test_scsp_word_write_is_atomic() {
    model2_audio_system audio;
    model2_audio_test_peer::reset(audio, 0x1000);
    audio.write16(scsp + 0x06, 0x0040);
    const uint64_t before = audio.scsp_writes();
    audio.write16(scsp + 0x00, 0x1830);

    // One 68000 word access is one SCSP transaction. In particular KEYONEX
    // must not execute between its high and low bytes with stale slot bits.
    assert(audio.scsp_writes() == before + 1);
    assert(model2_audio_test_peer::active(audio));
}

void test_scsp_dma_both_directions() {
    model2_audio_system audio;
    model2_audio_test_peer::reset(audio, 0x1000);
    audio.write16(0x0100, 0x6a5c);

    // Sound RAM -> SCSP coefficient RAM.
    audio.write16(scsp + 0x412, 0x0100);
    audio.write16(scsp + 0x414, 0x0700);
    audio.write16(scsp + 0x416, 0x1002);
    assert(audio.read16(scsp + 0x700) == 0x6a5c);
    assert((audio.read16(scsp + 0x416) & 0x1000) == 0);

    // SCSP coefficient RAM -> sound RAM.
    audio.write16(scsp + 0x700, 0x35a6);
    audio.write16(scsp + 0x412, 0x0200);
    audio.write16(scsp + 0x414, 0x0700);
    audio.write16(scsp + 0x416, 0x3002);
    assert(audio.read16(0x0200) == 0x35a6);
}

void test_midi_out_serial_pacing() {
    model2_audio_system audio;
    model2_audio_test_peer::reset(audio, 0x1000);
    std::vector<uint8_t> received;
    audio.set_midi_out_callback(
        [&received](uint8_t value) { received.push_back(value); });

    // Three back-to-back MOBUF writes, as the driver's command replies do.
    audio.write8(scsp + 0x407, 0x41);
    audio.write8(scsp + 0x407, 0x42);
    audio.write8(scsp + 0x407, 0x43);
    assert(received.empty()); // nothing before one full character time

    // 31.25 kbaud is one byte per 14.112 samples of the 44.1 kHz clock,
    // about 15.4 output frames. Ten frames must not complete a character.
    std::vector<int16_t> output(64 * 2);
    model2_audio_test_peer::render(audio, output.data(), 10);
    assert(received.empty());
    model2_audio_test_peer::render(audio, output.data(), 10);
    assert(received.size() == 1);
    model2_audio_test_peer::render(audio, output.data(), 64);
    model2_audio_test_peer::render(audio, output.data(), 64);
    assert(received.size() == 3);
    assert(received[0] == 0x41 && received[1] == 0x42 && received[2] == 0x43);
}

void test_scsp_slot_monitor_advances() {
    model2_audio_system audio;
    model2_audio_test_peer::reset(audio, 0x10000);
    for (unsigned index = 0; index < 0x10000; ++index)
        audio.write8(index, static_cast<uint8_t>(index));
    configure_voice(audio, 0x0030, 0xffff, 0x1800);
    audio.write16(scsp + 0x408, 0x0000); // select slot zero

    std::vector<int16_t> output(5000 * 2);
    model2_audio_test_peer::render(audio, output.data(), 5000);
    const uint16_t status = audio.read16(scsp + 0x408);
    assert(((status >> 7) & 15) != 0); // CA follows the playback cursor
    assert(((status >> 5) & 3) != 3);  // keyed slot is not in release
}

void test_scsp_slot_monitor_tracks_mslc_immediately() {
    // Sega Rally's stream feeder writes MSLC and reads CA back-to-back for
    // each music channel, with no render pass in between. The status must
    // describe the newly selected slot, not the previously monitored one.
    model2_audio_system audio;
    model2_audio_test_peer::reset(audio, 0x20000);
    for (unsigned index = 0; index < 0x20000; ++index)
        audio.write8(index, static_cast<uint8_t>(index * 37));

    // Slot 0 looping at 0x0000; slot 1 configured identically.
    configure_voice(audio, 0x0030, 0x1fff);
    audio.write16(scsp + 0x20, 0x0030 & ~uint16_t{0x1800});
    audio.write16(scsp + 0x22, 0x0000);
    audio.write16(scsp + 0x26, 0x1fff);
    audio.write16(scsp + 0x28, 0x001f);
    audio.write16(scsp + 0x36, 0xe000);
    audio.write16(scsp + 0x20, 0x0030 | 0x1800);

    // Advance slot state so slot 0 sits in a nonzero CA page while slot 1
    // was keyed later and lags behind.
    std::vector<int16_t> output(4096 * 2);
    model2_audio_test_peer::render(audio, output.data(), 4096);
    audio.write16(scsp + 0x00,
                  (audio.read16(scsp + 0x00) & ~uint16_t{0x1800}) | 0x1800);
    model2_audio_test_peer::render(audio, output.data(), 4096);

    audio.write16(scsp + 0x408, 0 << 11);
    const uint16_t status_zero = audio.read16(scsp + 0x408);
    audio.write16(scsp + 0x408, 1 << 11);
    const uint16_t status_one = audio.read16(scsp + 0x408);
    const uint64_t phase_zero = model2_audio_test_peer::phase(audio, 0);
    const uint64_t phase_one = model2_audio_test_peer::phase(audio, 1);
    assert(((status_zero >> 7) & 15) ==
           ((phase_zero >> (32 + 12)) & 15));
    assert(((status_one >> 7) & 15) ==
           ((phase_one >> (32 + 12)) & 15));
}

} // namespace

int main() {
    test_pcm_order_and_pitch_continuity();
    test_loop_and_speech_lifetime();
    test_reference_mix_level_and_key_off();
    test_dsp_effect_program();
    test_retrigger_during_release_tail();
    test_scsp_word_write_is_atomic();
    test_scsp_dma_both_directions();
    test_midi_out_serial_pacing();
    test_scsp_slot_monitor_advances();
    test_scsp_slot_monitor_tracks_mslc_immediately();
    std::puts("Model 2 SCSP PCM/pitch/loop/speech/mix/retrigger/DMA/MIDI-out/monitor tests passed");
    return 0;
}
