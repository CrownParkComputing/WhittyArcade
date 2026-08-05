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

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace {

// A stick byte centred on 0x7f, as -1..1, with a deadzone. Without one a real
// pad's resting position drifts the ship, which reads as the game being broken
// rather than the stick being analogue.
float axis(uint8_t raw) noexcept {
    const float value = (static_cast<float>(raw) - 127.0f) / 127.0f;
    return value > -0.20f && value < 0.20f ? 0.0f : value;
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
        // Seat one is this cabinet's pad. The remaining seats stay
        // disconnected until either a second local pad or the lobby fills
        // them - a game must not be handed four seats it has nobody for.
        inputs[0].connected = 1;
        inputs[0].move_x = axis(state.left_stick_x);
        inputs[0].move_y = -axis(state.left_stick_y);
        inputs[0].aim_x = axis(state.right_stick_x);
        inputs[0].aim_y = -axis(state.right_stick_y);
        if (state.buttons[0]) inputs[0].buttons |= manx_game_button_fire;
        if (state.buttons[1]) inputs[0].buttons |= manx_game_button_bomb;
        if (state.start) inputs[0].buttons |= manx_game_button_start;

        manx_game_frame frame{};
        m_plugin->api()->run_frame(m_instance, inputs, 4, &frame);

        // Drained every frame whether or not there is a device, so a silent
        // host does not let cues pile up inside the plugin.
        manx_game_audio_cue cues[32];
        const uint32_t cue_count =
            m_plugin->api()->take_audio_cues(m_instance, cues, 32);
        if (m_audio)
            for (uint32_t i = 0; i < cue_count; ++i)
                m_audio->play(cues[i].cue, cues[i].gain, cues[i].pan);

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

protected:
    void set_audio_paused(bool paused) override {
        if (m_audio) m_audio->set_paused(paused);
        if (m_instance != nullptr && m_plugin && m_plugin->valid())
            m_plugin->api()->set_paused(m_instance, paused ? 1u : 0u);
    }

private:
    discovered_game m_game;
    std::unique_ptr<loaded_plugin> m_plugin;
    manx_game_instance* m_instance{nullptr};
    std::unique_ptr<arcade_input> m_input;
    std::unique_ptr<plugin_audio> m_audio;
};

} // namespace

std::unique_ptr<emulator_session> make_plugin_session(
    std::shared_ptr<arcade_video_worker> video,
    std::shared_ptr<arcade_cabinet_state> cabinet) {
    return std::make_unique<plugin_session>(std::move(video),
                                            std::move(cabinet));
}
