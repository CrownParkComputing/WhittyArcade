// plugin_audio.h - the host end of a game plugin's sound.
//
// A plugin never opens an audio device. It reports cues - "a shot happened,
// this loud, over there" - and this turns them into sound. The split exists so
// audio cannot influence the simulation: in lockstep, two machines whose sound
// devices behave differently must still simulate identically, and a game that
// cannot hear its own audio layer is structurally unable to diverge because of
// it.
//
// Samples come from the bundle, matched by cue NAME rather than index, so
// installing a game's assets is a matter of dropping files in beside it:
//
//     <bundle>/sfx/<cue name>.wav
//
// A cue with no file is SYNTHESISED rather than silent. That is not a
// placeholder for its own sake: it means a freshly built plugin makes the right
// noises at the right moments before any asset extraction has run, so the cue
// wiring can be judged on its own, and a bundle that later supplies a real file
// simply overrides it.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

class plugin_audio {
public:
    plugin_audio() = default;
    ~plugin_audio();

    plugin_audio(const plugin_audio&) = delete;
    plugin_audio& operator=(const plugin_audio&) = delete;

    // `cue_names` is the plugin's own list, in index order - index IS the cue
    // id. Returns false only when no audio device could be opened; a missing
    // sample is not a failure.
    bool initialize(const std::vector<std::string>& cue_names,
                    const std::string& bundle_path, int master_volume,
                    int effects_volume);

    // Plays one cue. `gain` 0..1 from the game, `pan` -1..1. Cheap enough to
    // call several times a frame; a cue with no free voice is dropped rather
    // than queued, because a sound that arrives late is worse than one that
    // never arrives.
    void play(uint32_t cue, float gain, float pan);

    void set_volume(int master_volume, int effects_volume);
    void set_paused(bool paused);

    // What was loaded, for the launcher and for tests: how many cues came from
    // real files rather than the synthesiser.
    std::size_t cue_count() const noexcept { return m_cues.size(); }
    std::size_t sampled_cue_count() const noexcept { return m_sampled; }
    const std::string& source_of(uint32_t cue) const;

private:
    struct cue_sound {
        std::string name;
        std::string source; // file path, or "synthesised"
        uint32_t buffer{0};
        float feedback_strength{0.0f};
    };

    std::vector<cue_sound> m_cues;
    std::vector<uint32_t> m_sources;
    std::size_t m_next_source{0};
    std::size_t m_sampled{0};
    void* m_device{nullptr};
    void* m_context{nullptr};
    float m_volume{1.0f};
    bool m_paused{false};
};

// 16-bit mono PCM, the one shape everything below deals in. Exposed so the WAV
// reader and the synthesiser can be tested without an audio device - the parts
// most likely to be wrong are the ones a speaker cannot check.
struct pcm_sound {
    std::vector<int16_t> samples;
    uint32_t sample_rate{44100};
    bool valid() const noexcept { return !samples.empty(); }
};

// Reads a RIFF/WAVE file. Accepts 8- and 16-bit PCM at any rate, mono or
// stereo, and downmixes stereo to mono because OpenAL will not pan a stereo
// source - a stereo sample would silently ignore the cue's position.
pcm_sound load_wav_file(const std::string& path);
pcm_sound parse_wav(const uint8_t* data, std::size_t size);

// Where a cue's sound came from, and what it is. Separated from the class above
// so the part that MATTERS to the asset pipeline - does a file in the bundle
// actually override the placeholder? - can be tested on a machine with no audio
// device at all, which is most build machines.
struct resolved_cue {
    pcm_sound sound;
    std::string source;   // the file used, or "synthesised"
    bool from_bundle{false};
};

// Looks for <bundle>/sfx/<name>.wav (either case), falling back to synthesis
// when there is no readable file.
resolved_cue resolve_cue(const std::string& bundle_path,
                         const std::string& name);

// The fallback voice for a cue with no file, chosen by NAME so an unfamiliar
// game still gets something appropriate: names containing "shot", "death",
// "explode" and so on pick different shapes. Deterministic - the same name
// always synthesises the same sound.
pcm_sound synthesise_cue(const std::string& name);
