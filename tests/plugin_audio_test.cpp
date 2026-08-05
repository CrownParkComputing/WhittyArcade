// The parts of plugin audio that a speaker cannot check.
//
// Everything here runs without an audio device, which is the point: a wrong
// WAV header interpretation or a clicking envelope is audible but not
// diagnosable by ear, and the failure ("it sounds a bit off") is the same for
// half a dozen different causes. Each of these pins one of them.

#include "plugin_audio.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

void put_u32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xffu));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xffu));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xffu));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xffu));
}

void put_u16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xffu));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xffu));
}

void put_tag(std::vector<uint8_t>& out, const char* tag) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(tag[i]));
}

// Builds a WAV. `extra_chunk` inserts a junk chunk between fmt and data, which
// is what real files from real tools look like.
std::vector<uint8_t> make_wav(uint16_t channels, uint16_t bits, uint32_t rate,
                              const std::vector<uint8_t>& pcm,
                              bool extra_chunk = false) {
    std::vector<uint8_t> body;
    put_tag(body, "fmt ");
    put_u32(body, 16);
    put_u16(body, 1);
    put_u16(body, channels);
    put_u32(body, rate);
    put_u32(body, rate * channels * (bits / 8u));
    put_u16(body, static_cast<uint16_t>(channels * (bits / 8u)));
    put_u16(body, bits);
    if (extra_chunk) {
        put_tag(body, "LIST");
        put_u32(body, 5); // odd size, so the pad byte matters
        for (int i = 0; i < 5; ++i) body.push_back('x');
        body.push_back(0);
    }
    put_tag(body, "data");
    put_u32(body, static_cast<uint32_t>(pcm.size()));
    body.insert(body.end(), pcm.begin(), pcm.end());

    std::vector<uint8_t> file;
    put_tag(file, "RIFF");
    put_u32(file, static_cast<uint32_t>(body.size() + 4));
    put_tag(file, "WAVE");
    file.insert(file.end(), body.begin(), body.end());
    return file;
}

void test_rubbish_is_rejected_not_played() {
    // A truncated or non-WAV file must produce nothing. Handing OpenAL a
    // buffer built from a JPEG produces a burst of full-scale noise, which is
    // loud enough to be genuinely unpleasant.
    assert(!parse_wav(nullptr, 0).valid());
    const uint8_t nonsense[64] = {'J', 'F', 'I', 'F'};
    assert(!parse_wav(nonsense, sizeof(nonsense)).valid());
    std::vector<uint8_t> wav = make_wav(1, 16, 44100, {1, 0, 2, 0});
    assert(!parse_wav(wav.data(), 20).valid() &&
           "a truncated file must not be read past its end");
}

void test_sixteen_bit_mono_is_read_exactly() {
    std::vector<uint8_t> pcm;
    put_u16(pcm, 0x0000);
    put_u16(pcm, 0x7fff);
    put_u16(pcm, 0x8000); // -32768
    const std::vector<uint8_t> wav = make_wav(1, 16, 22050, pcm);
    const pcm_sound sound = parse_wav(wav.data(), wav.size());
    assert(sound.valid());
    assert(sound.sample_rate == 22050);
    assert(sound.samples.size() == 3);
    assert(sound.samples[0] == 0);
    assert(sound.samples[1] == 32767);
    assert(sound.samples[2] == -32768);
}

void test_eight_bit_silence_is_silent() {
    // 8-bit WAV is UNSIGNED with 128 as the zero point. Read as signed, a
    // silent file becomes a full-amplitude square wave - the single most
    // common WAV bug and by far the loudest.
    const std::vector<uint8_t> wav = make_wav(1, 8, 11025, {128, 128, 128, 128});
    const pcm_sound sound = parse_wav(wav.data(), wav.size());
    assert(sound.valid() && sound.samples.size() == 4);
    for (const int16_t sample : sound.samples)
        assert(sample == 0 && "8-bit 128 is silence, not full scale");

    const std::vector<uint8_t> loud = make_wav(1, 8, 11025, {255, 0});
    const pcm_sound peaks = parse_wav(loud.data(), loud.size());
    assert(peaks.samples[0] > 30000 && peaks.samples[1] < -30000);
}

void test_stereo_is_downmixed_so_panning_still_works() {
    // OpenAL refuses to position a stereo source, so a stereo sample would
    // silently ignore the cue's pan - every sound arriving dead centre with no
    // error anywhere.
    std::vector<uint8_t> pcm;
    put_u16(pcm, static_cast<uint16_t>(1000));  // left
    put_u16(pcm, static_cast<uint16_t>(3000));  // right
    const std::vector<uint8_t> wav = make_wav(2, 16, 44100, pcm);
    const pcm_sound sound = parse_wav(wav.data(), wav.size());
    assert(sound.valid());
    assert(sound.samples.size() == 1 && "two channels are one frame");
    assert(sound.samples[0] == 2000);
}

void test_a_chunk_between_fmt_and_data_is_stepped_over() {
    // Files from most tools carry LIST/INFO between fmt and data. A reader
    // that assumes data starts at offset 36 reads the metadata as audio.
    std::vector<uint8_t> pcm;
    put_u16(pcm, static_cast<uint16_t>(4321));
    const std::vector<uint8_t> wav = make_wav(1, 16, 8000, pcm, true);
    const pcm_sound sound = parse_wav(wav.data(), wav.size());
    assert(sound.valid());
    assert(sound.sample_rate == 8000);
    assert(sound.samples.size() == 1);
    assert(sound.samples[0] == 4321 && "the LIST chunk was read as audio");
}

void test_synthesis_is_deterministic_and_per_name() {
    // Two machines in a lockstep match must hear the same fallback sounds, or
    // "it sounds different on his machine" becomes a suspected desync.
    const pcm_sound a = synthesise_cue("shot");
    const pcm_sound b = synthesise_cue("shot");
    assert(a.valid() && a.samples == b.samples);

    // And different cues must be distinguishable, or every event sounds the
    // same and the fallback is worthless for judging the cue wiring.
    const pcm_sound death = synthesise_cue("player_death");
    assert(death.samples.size() != a.samples.size());
    assert(synthesise_cue("spawn").samples != synthesise_cue("bomb").samples);

    // An unrecognised name still gets a voice rather than silence.
    assert(synthesise_cue("something_nobody_anticipated").valid());
}

void test_synthesis_neither_clicks_nor_clips() {
    for (const char* name : {"shot", "enemy_death", "player_death", "spawn",
                             "bomb", "black_hole_collapse", "multiplier_up"}) {
        const pcm_sound sound = synthesise_cue(name);
        assert(sound.valid());
        // A sound that starts at full amplitude clicks, and the click survives
        // every later mixing stage - it is the artefact you cannot EQ out.
        assert(std::abs(sound.samples.front()) < 2000);
        // And it must decay to near silence, or one-shots overlap into a drone.
        assert(std::abs(sound.samples.back()) < 2000);
        for (const int16_t sample : sound.samples)
            assert(sample > -32000 && sample < 32000 &&
                   "synthesis must leave headroom for the master mix");
    }
}

void test_a_bundle_sample_overrides_the_placeholder(const fs::path& root) {
    // This is the contract the whole asset pipeline rests on: an import writes
    // <bundle>/sfx/<cue>.wav, and the game then uses it instead of the
    // synthesised placeholder. If it silently did not, everything would still
    // work and still make noise - just never the imported sound.
    const fs::path sfx = root / "sfx";
    fs::create_directories(sfx);

    // No file: synthesised, and flagged as not from the bundle.
    const resolved_cue none = resolve_cue(root.string(), "shot");
    assert(none.sound.valid());
    assert(!none.from_bundle);
    assert(none.source == "synthesised");

    // A real file: used, and reported by path so the launcher can say which.
    std::vector<uint8_t> pcm;
    for (int i = 0; i < 64; ++i) {
        pcm.push_back(0x11);
        pcm.push_back(0x22);
    }
    const std::vector<uint8_t> wav = make_wav(1, 16, 32000, pcm);
    {
        std::ofstream out(sfx / "shot.wav", std::ios::binary);
        out.write(reinterpret_cast<const char*>(wav.data()),
                  static_cast<std::streamsize>(wav.size()));
    }
    const resolved_cue supplied = resolve_cue(root.string(), "shot");
    assert(supplied.from_bundle && "the bundle's sample was ignored");
    assert(supplied.source.find("shot.wav") != std::string::npos);
    assert(supplied.sound.sample_rate == 32000);
    assert(supplied.sound.samples.size() == 64);
    assert(supplied.sound.samples != none.sound.samples &&
           "the file was found but the placeholder was played anyway");

    // A corrupt file must not be played as noise, and must not leave the cue
    // silent either - it falls back, loudly, to the placeholder.
    {
        std::ofstream out(sfx / "bomb.wav", std::ios::binary);
        out << "this is not a wav file at all, not even close";
    }
    const resolved_cue broken = resolve_cue(root.string(), "bomb");
    assert(!broken.from_bundle);
    assert(broken.sound.valid() && "a corrupt sample must not silence the cue");

    // A cue whose name has no file and no keyword still resolves.
    assert(resolve_cue(root.string(), "unheard_of").sound.valid());
}

} // namespace

int main() {
    test_rubbish_is_rejected_not_played();
    test_sixteen_bit_mono_is_read_exactly();
    test_eight_bit_silence_is_silent();
    test_stereo_is_downmixed_so_panning_still_works();
    test_a_chunk_between_fmt_and_data_is_stepped_over();
    test_synthesis_is_deterministic_and_per_name();
    test_synthesis_neither_clicks_nor_clips();

    const fs::path root = fs::temp_directory_path() / "manx_plugin_audio_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);
    test_a_bundle_sample_overrides_the_placeholder(root);
    fs::remove_all(root, ec);

    std::printf("plugin_audio_test: all checks passed\n");
    return 0;
}
