// The cabinet a game plugin runs in.
//
// Everything else in this directory drives an emulated machine. This one drives
// a shared library: it holds the plugin open, hands it the pads as plain
// intent, and presents the picture it gives back. That is the whole of it -
// which is the point, because it means a native game reaches the frontend, the
// input mapper, the high-score table and the multiplayer lobby through exactly
// the same interface every board already uses.
//
// The plugin is kept alive for as long as the session is: every instance it
// handed out points at code inside that library, so unloading it early would
// leave this file calling into unmapped memory.

#include "arcade_session_internal.h"
#include "arcade_frontend.h"
#include "arcade_catalog.h"
#include "game_plugin_host.h"
#include "plugin_audio.h"
#include "plugin_pcm_queue.h"
#include "arcade_input.h"
#include "arcade_audio_output.h"
#include "manx_game_pcm.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <algorithm>
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace {

// arcade_input deliberately stores logical directions in its cabinet ADC range:
// centre 0x7f, full travel 0x47..0xb7 (56 steps each way). Treating it as a
// generic 0..255 byte made full pad travel only 44%, below RR6's menu deadzone
// and below the bridge's digital-direction threshold.
float axis(uint8_t raw) noexcept {
    constexpr float centre = 0x7f;
    constexpr float half_range = 0x38;
    const float value = std::clamp(
        (static_cast<float>(raw) - centre) / half_range, -1.0f, 1.0f);
    return value > -0.20f && value < 0.20f ? 0.0f : value;
}

float trigger(uint16_t raw) noexcept {
    // arcade_input stores driving pedals in the cabinets' 0..0x610 ADC range,
    // not a generic uint16 range. Dividing by 0xffff capped RR6 at 2.4%
    // throttle, which looked exactly like a dead accelerator.
    constexpr float pedal_maximum = 0x610;
    return std::clamp(static_cast<float>(raw) / pedal_maximum, 0.0f, 1.0f);
}

void map_buttons(manx_game_input& output, const uint8_t* buttons,
                 bool start) noexcept {
    if (buttons[0]) output.buttons |= manx_game_button_fire;
    if (buttons[1]) output.buttons |= manx_game_button_secondary;
    if (buttons[2]) output.buttons |= manx_game_button_bomb;
    if (buttons[3]) output.buttons |= manx_game_button_special;
    if (start) output.buttons |= manx_game_button_start;
}

class plugin_session final : public video_emulator_session {
public:
    plugin_session(std::shared_ptr<arcade_video_worker> video,
                   std::shared_ptr<arcade_cabinet_state> cabinet)
        : video_emulator_session(arcade_board_type::game_plugin,
                                 std::move(video), std::move(cabinet)) {}

    ~plugin_session() override {
        // Instance first, then the library it lives in, then the devices this
        // board owns. Any other order calls into something already gone.
        if (m_instance != nullptr && m_plugin && m_plugin->valid())
            m_plugin->api()->destroy(m_instance);
        m_instance = nullptr;
        m_plugin.reset();
        if (m_pcm_stream) SDL_DestroyAudioStream(m_pcm_stream);
        m_pcm_stream = nullptr;
        if (m_gpu_renderer) m_gpu_renderer->reset_session();
        if (m_input) m_input->shutdown();
    }

    bool initialize(const std::string& bundle_path, const std::string&,
                    const emulator_settings& settings) override {
        // The launcher hands over the path it selected; for a plugin that is
        // its folder, which is what identifies it. Looking the record back up
        // rather than carrying a copy means the library and the data can never
        // be paired from two routes that disagree.
        const discovered_game* found = find_plugin_game(bundle_path);
        if (found == nullptr) {
            std::fprintf(stderr, "No game plugin installed at %s\n",
                         bundle_path.c_str());
            return false;
        }
        m_game = *found;
        game_plugin_library library;
        std::string error;
        m_plugin = library.load(m_game, error);
        if (!m_plugin) {
            std::fprintf(stderr, "%s will not load: %s\n",
                         m_game.display_name.c_str(), error.c_str());
            return false;
        }
        m_input = std::make_unique<arcade_input>();
        if (!m_gpu_renderer->initialize(settings) ||
            !m_input->initialize(m_game.short_name.c_str()))
            return false;
        m_instance = m_plugin->api()->create(m_game.bundle_path.c_str());
        if (m_instance == nullptr) {
            std::fprintf(stderr, "%s would not start\n",
                         m_game.display_name.c_str());
            return false;
        }
        m_pcm_master_volume = settings.master_volume;
        m_pcm_music_volume = settings.music_volume;
        // Cue names come from the plugin, so the bundle's sound files are
        // matched to what the game actually emits rather than to a list the
        // host guessed. A game with no audio reports none and stays silent.
        const uint32_t cue_count =
            m_plugin->api()->describe_audio_cues(nullptr, 0);
        if (cue_count > 0) {
            std::vector<const char*> raw(cue_count, nullptr);
            const uint32_t written =
                m_plugin->api()->describe_audio_cues(raw.data(), cue_count);
            std::vector<std::string> names;
            names.reserve(written);
            for (uint32_t i = 0; i < written; ++i)
                names.emplace_back(raw[i] != nullptr ? raw[i] : "");
            m_audio = std::make_unique<plugin_audio>();
            m_audio->initialize(names, m_game.bundle_path,
                                settings.master_volume,
                                settings.effects_volume);
        }
        std::printf("%s (native plugin) initialized, %u seat(s)\n",
                    m_game.display_name.c_str(), m_game.max_players);
        return true;
    }

    void run_frame() override {
        m_input->set_suppressed(m_gpu_renderer->input_suppressed());
        m_input->update();
        const input_state& state = m_input->state();

        manx_game_input inputs[4]{};
        // Seat one is this cabinet's pad. arcade_input has already delayed and
        // merged the peer's frame under lockstep; preserve its complete second
        // driving seat instead of discarding it at the plugin boundary.
        inputs[0].connected = 1;
        inputs[0].move_x = axis(state.left_stick_x);
        inputs[0].move_y = -axis(state.left_stick_y);
        inputs[0].aim_x = axis(state.right_stick_x);
        inputs[0].aim_y = -axis(state.right_stick_y);
        inputs[0].trigger_l = trigger(state.brake);
        inputs[0].trigger_r = trigger(state.gas);
        map_buttons(inputs[0], state.buttons, state.start);
        if (std::getenv("MANX_PLUGIN_INPUT_TRACE")) {
            static int last_x = -999;
            static int last_y = -999;
            static int last_l = -1;
            static int last_r = -1;
            static uint32_t last_buttons = ~0u;
            const int x = static_cast<int>(inputs[0].move_x * 100.0f);
            const int y = static_cast<int>(inputs[0].move_y * 100.0f);
            const int l = static_cast<int>(inputs[0].trigger_l * 100.0f);
            const int r = static_cast<int>(inputs[0].trigger_r * 100.0f);
            if (x != last_x || y != last_y || l != last_l || r != last_r ||
                inputs[0].buttons != last_buttons) {
                std::printf("MANX plugin input: move=%d,%d trigger=%d,%d "
                            "buttons=%02x raw=%02x,%02x gas=%04x\n",
                            x, y, l, r, inputs[0].buttons,
                            state.left_stick_x, state.left_stick_y, state.gas);
                std::fflush(stdout);
                last_x = x;
                last_y = y;
                last_l = l;
                last_r = r;
                last_buttons = inputs[0].buttons;
            }
        }

        if (arcade_input_netplay_active()) {
            inputs[1].connected = 1;
            inputs[1].move_x = axis(state.p2_stick_x);
            inputs[1].move_y = -axis(state.p2_stick_y);
            inputs[1].aim_x = axis(state.p2_right_stick_x);
            inputs[1].aim_y = -axis(state.p2_right_stick_y);
            inputs[1].trigger_l = trigger(state.p2_brake);
            inputs[1].trigger_r = trigger(state.p2_gas);
            map_buttons(inputs[1], state.p2_buttons, state.p2_start);
        }

        manx_game_frame frame{};
        m_plugin->api()->run_frame(m_instance, inputs, 4, &frame);

        // A plugin can hash the actual simulation state much more cheaply and
        // precisely than the host can infer it from an occasional raster.
        // Publish the frame just completed; the input link carries it with the
        // next packet and compares the same numbered frame on both machines.
        if (arcade_input_netplay_active()) {
            const uint32_t next = arcade_input_netplay_frame();
            const uint64_t hash =
                m_plugin->api()->state_checksum(m_instance);
            if (next > 0 && hash != 0)
                arcade_input_netplay_publish_state(next - 1, hash);
        }

        if (const manx_game_stats_api* stats = m_plugin->stats_api()) {
            manx_game_stat_event events[32]{};
            const uint32_t count = std::min<uint32_t>(
                stats->take_events(m_instance, events, 32), 32);
            for (uint32_t i = 0; i < count; ++i) {
                const manx_game_stat_event& event = events[i];
                if (event.title_id == 0 || event.property_id == 0 ||
                    event.value == 0)
                    continue;
                arcade_online_score score;
                score.title_id = event.title_id;
                score.leaderboard_id = event.leaderboard_id;
                score.property_id = event.property_id;
                score.value = event.value;
                score.lower_is_better =
                    (event.flags & manx_game_stat_lower_is_better) != 0;
                const uint32_t metadata_count = std::min<uint32_t>(
                    event.metadata_count, MANX_GAME_STAT_MAX_PROPERTIES);
                score.metadata.reserve(metadata_count);
                for (uint32_t property = 0; property < metadata_count;
                     ++property) {
                    if (event.metadata[property].property_id == 0) continue;
                    score.metadata.push_back({
                        event.metadata[property].property_id,
                        event.metadata[property].value});
                }
                m_scores.push_back(std::move(score));
                if (m_scores.size() > 64) m_scores.pop_front();
            }
        }

        // Drained every frame whether or not there is a device, so a silent
        // host does not let cues pile up inside the plugin.
        manx_game_audio_cue cues[32];
        const uint32_t cue_count =
            m_plugin->api()->take_audio_cues(m_instance, cues, 32);
        if (m_audio)
            for (uint32_t i = 0; i < cue_count; ++i)
                m_audio->play(cues[i].cue, cues[i].gain, cues[i].pan);

        if (const manx_game_pcm_api* pcm = m_plugin->pcm_api()) {
            manx_game_pcm_block blocks[4]{};
            const uint32_t count = std::min<uint32_t>(
                pcm->take_blocks(m_instance, blocks, 4), 4);
            for (uint32_t i = 0; i < count; ++i)
                queue_pcm(blocks[i]);
        }

        if (frame.pixels != nullptr)
            m_gpu_renderer->present_rgba_frame(
                frame.pixels, static_cast<int>(frame.width),
                static_cast<int>(frame.height),
                static_cast<int>(frame.aspect_x),
                static_cast<int>(frame.aspect_y));
    }

    void reload_input_mappings() override {
        if (m_input) m_input->reload_mappings();
    }
    double frame_seconds() const override {
        return m_game.refresh_hz > 0.0 ? 1.0 / m_game.refresh_hz : 1.0 / 60.0;
    }

    bool take_online_score(arcade_online_score& out) override {
        if (m_scores.empty()) return false;
        out = m_scores.front();
        m_scores.pop_front();
        return true;
    }

protected:
    void set_audio_paused(bool paused) override {
        if (m_audio) m_audio->set_paused(paused);
        if (m_pcm_stream) {
            if (paused) {
                SDL_PauseAudioStreamDevice(m_pcm_stream);
                SDL_ClearAudioStream(m_pcm_stream);
            } else {
                SDL_ResumeAudioStreamDevice(m_pcm_stream);
            }
        }
        if (m_instance != nullptr && m_plugin && m_plugin->valid())
            m_plugin->api()->set_paused(m_instance, paused ? 1u : 0u);
    }

private:
    void queue_pcm(const manx_game_pcm_block& block) {
        if (!block.samples || block.frames == 0 || block.sample_rate == 0 ||
            block.channels == 0 || block.channels > 8)
            return;
        if (!m_pcm_stream || m_pcm_rate != block.sample_rate ||
            m_pcm_channels != block.channels) {
            if (m_pcm_stream) SDL_DestroyAudioStream(m_pcm_stream);
            m_pcm_stream = nullptr;
            if (!SDL_WasInit(SDL_INIT_AUDIO) &&
                !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
                std::fprintf(stderr, "plugin PCM: SDL audio init failed: %s\n",
                             SDL_GetError());
                return;
            }
            const SDL_AudioSpec spec{
                SDL_AUDIO_S16, static_cast<int>(block.channels),
                static_cast<int>(block.sample_rate)};
            m_pcm_stream = SDL_OpenAudioDeviceStream(
                SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
            if (!m_pcm_stream) {
                std::fprintf(stderr, "plugin PCM: output failed: %s\n",
                             SDL_GetError());
                return;
            }
            m_pcm_rate = block.sample_rate;
            m_pcm_channels = block.channels;
            const float master =
                static_cast<float>(std::clamp(m_pcm_master_volume, 0, 200)) /
                100.0f;
            const float music =
                static_cast<float>(std::clamp(m_pcm_music_volume, 0, 100)) /
                100.0f;
            SDL_SetAudioStreamGain(m_pcm_stream, master * music);
            SDL_ResumeAudioStreamDevice(m_pcm_stream);
            std::printf("plugin PCM: %u Hz, %u channel(s)\n",
                        block.sample_rate, block.channels);
        }
        if (arcade_audio_output::output_muted()) return;
        const int frame_bytes =
            static_cast<int>(block.channels * sizeof(int16_t));
        // A render/compile spike must not become permanent A/V drift. Once the
        // queued sound is more than 150 ms old, discard it and keep THIS fresh
        // block; dropping the incoming block retained the stale half-second
        // queue indefinitely.
        if (plugin_pcm_queue_needs_realign(
                SDL_GetAudioStreamQueued(m_pcm_stream), block.sample_rate,
                block.channels))
            SDL_ClearAudioStream(m_pcm_stream);
        SDL_PutAudioStreamData(
            m_pcm_stream, block.samples,
            static_cast<int>(block.frames) * frame_bytes);
    }

    discovered_game m_game;
    std::unique_ptr<loaded_plugin> m_plugin;
    manx_game_instance* m_instance{nullptr};
    std::unique_ptr<arcade_input> m_input;
    std::unique_ptr<plugin_audio> m_audio;
    SDL_AudioStream* m_pcm_stream{nullptr};
    uint32_t m_pcm_rate{};
    uint32_t m_pcm_channels{};
    int m_pcm_master_volume{100};
    int m_pcm_music_volume{100};
    std::deque<arcade_online_score> m_scores;
};

} // namespace

std::unique_ptr<emulator_session> make_plugin_session(
    std::shared_ptr<arcade_video_worker> video,
    std::shared_ptr<arcade_cabinet_state> cabinet) {
    return std::make_unique<plugin_session>(std::move(video),
                                            std::move(cabinet));
}
