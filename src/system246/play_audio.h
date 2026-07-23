#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

class CSoundHandler;

// RRV's System 246 programme runs the SPU2 voices at very conservative
// digital levels and relies on the cabinet analogue line stage. Runtime
// traces show quiet engine voices well below the decoded disc programme, but
// louder coin/impact one-shots still need headroom. A 16x cabinet stage makes
// the former audible without forcing the latter into the limiter.
constexpr int system246_spu_line_gain_percent = 1600;

// Fold the ordinary 0-100 master range into every source before the buses are
// combined. This makes it impossible for either the CHD music path or SPU2
// effects path to bypass master volume. Values above 100 remain a final
// cabinet boost so WhittyArcade's established 0-200 master range still works.
inline int system246_effective_bus_percent(
        int master_percent, int bus_percent) {
    const int master_ceiling =
        std::min(std::clamp(master_percent, 0, 200), 100);
    return std::clamp(bus_percent, 0, 100) * master_ceiling / 100;
}

inline int system246_output_master_percent(int master_percent) {
    const int master = std::clamp(master_percent, 0, 200);
    return master > 100 ? master : 100;
}

// Keep the calibrated SPU2 bus wide until the final shared output stage.  The
// previous int16 conditioner could limit the effects before disc music was
// added, leaving the final mix impossible to normalize cleanly.
inline std::int32_t system246_effect_bus_sample(
        std::int16_t sample, int effects_percent) {
    const std::int64_t level = std::clamp(effects_percent, 0, 100);
    return static_cast<std::int32_t>(
        static_cast<std::int64_t>(sample) *
        system246_spu_line_gain_percent * level / 10000);
}

inline std::int32_t system246_effect_bus_sample(
        std::int16_t sample, int master_percent, int effects_percent) {
    return system246_effect_bus_sample(
        sample,
        system246_effective_bus_percent(
            master_percent, effects_percent));
}

inline int system246_mix_reference_percent(int music_percent,
                                           int effects_percent) {
    return (std::clamp(music_percent, 0, 100) +
            std::clamp(effects_percent, 0, 100)) / 2;
}

namespace system246::rrv_music {
class player;
}

// Cross-thread controls for the Play! SPU output. The VM owns the actual
// CSoundHandler; Whitty retains this small shared control plane.
class system246_audio_control {
public:
    system246_audio_control();
    ~system246_audio_control();

    void set_mix_levels(int master_percent, int music_percent,
                        int effects_percent);
    int master_volume() const;
    int music_volume() const;
    int effects_volume() const;
    void set_paused(bool paused);
    bool paused() const;

    bool configure_music(const std::string& media_path, std::string& error);
    void observe_game_bgm(std::int32_t game_bgm_number);
    void observe_disc_read(std::uint32_t first_sector,
                           std::uint32_t sector_count);
    void mix_music(std::int32_t* interleaved_stereo,
                   std::size_t sample_count, std::uint32_t sample_rate);

private:
    std::atomic<int> m_master_volume{100};
    std::atomic<int> m_music_volume{100};
    std::atomic<int> m_effects_volume{100};
    std::atomic<bool> m_paused{false};
    std::unique_ptr<system246::rrv_music::player> m_music;
};

// Ownership is transferred to CPS2VM.
CSoundHandler* make_system246_sound_handler(
    std::shared_ptr<system246_audio_control> control);
