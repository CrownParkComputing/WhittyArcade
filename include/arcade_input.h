// arcade_input.h - shared SDL keyboard and controller input adapter.
#pragma once

#include "arcade_feedback.h"
#include "arcade_types.h"
#include "input_mapping.h"
#include "lan_peers.h"

#include <SDL3/SDL.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>

// True after the board-neutral two-computer input link has received a packet
// from the other MANX instance. Native cabinet links (for example
// Sega Rally's Model 2 board) report their own state instead.
bool arcade_input_network_peer_seen();
// True while this cabinet is the one the controls are driving: it holds the
// keyboard, or the pointer is over it. With several cabinets on one computer
// they all see the same gamepad, so without this every press goes into all of
// them at once and no single cabinet can be given a coin.
bool arcade_input_cabinet_active();
// Netplay, read by the host loop: true while this machine is waiting for the
// peer's input for the frame it is about to run. The board must not be
// stepped while it is true, or the two machines diverge.
bool arcade_input_netplay_stalled();
// True when this launch is a lockstep netplay session at all.
bool arcade_input_netplay_active();
// How many frames of the peer's input are buffered ahead of the local frame.
int arcade_input_netplay_lead();
// The frame the local machine has advanced to under lockstep.
uint32_t arcade_input_netplay_frame();
// A hash of the running board's state, published by the session each frame
// and compared with the peer's to notice a divergence.
void arcade_input_netplay_publish_state(uint32_t frame, uint64_t hash);
// Non-zero once the two machines have simulated the same frame differently;
// the value is the frame the divergence was seen on.
uint32_t arcade_input_netplay_desync_frame();
// True once a netplay peer that was connected has gone away. The two
// cabinets are one session: when either app closes, the other has nothing
// left to play against and should close too rather than sit waiting.
bool arcade_input_netplay_peer_lost();
// Sends a cooperative session-closing packet to the linked cabinet. The peer
// leaves through its normal event loop, releasing video, audio and input
// before the owner falls back to process signals.
void arcade_input_netplay_request_shutdown();
// Polls the netplay link without running a frame. A cabinet waiting on its
// partner stops calling update(), so this is what still notices the partner
// leaving. There is exactly one linked input per process, which registers
// itself when the link opens.
void arcade_input_netplay_poll();
uint32_t arcade_input_network_peer_ipv4();

// Multiplayer traffic normally rides the lobby's socket, which is already up,
// already unicast to every machine in the session and already through
// whatever firewalls are in the way. Installing a transport switches the
// in-game link onto it; without one the link opens a socket of its own,
// which is what a cabinet started straight from the command line does.
void arcade_input_set_netplay_transport(
    std::function<void(const void*, std::size_t)> send);
// Handed each payload the transport delivers. Safe to call from any thread.
void arcade_input_netplay_deliver(const void* data, std::size_t bytes);
void arcade_input_set_authoritative_player(uint8_t player);
uint8_t arcade_input_network_authoritative_player();

class arcade_input {
public:
    ~arcade_input();

    bool initialize(std::string_view game_short_name = {});

    // True for the games whose cabinet has a light gun, so a session can show
    // the on-screen sight for exactly the games whose aim comes from the
    // mouse. Shared with the input mapping so the two cannot disagree.
    static bool is_lightgun_game(std::string_view game_short_name);

    // Where the emulated picture is drawn, from the renderer (drawable pixels,
    // origin top-left). The light gun maps the pointer onto this rectangle
    // instead of assuming the picture fills a centred 4:3 area of the window,
    // which is wrong whenever integer scaling or a non-4:3 board is in play.
    void set_picture_rect(int x, int y, int width, int height,
                          int drawable_width, int drawable_height) {
        m_picture_x = x;
        m_picture_y = y;
        m_picture_width = width;
        m_picture_height = height;
        m_picture_drawable_width = drawable_width;
        m_picture_drawable_height = drawable_height;
    }
    void reload_mappings();
    void shutdown();
    void request_peer_shutdown();
    // True when this cabinet can reach the others - either through a socket
    // of its own or through the lobby's channel.
    bool link_open() const;
    // Whole-pad force feedback. Continuous effects are refreshed often
    // enough to survive SDL's finite timeout without flooding a wireless
    // controller; pulses always restart immediately.
    void set_rumble(uint16_t low_frequency, uint16_t high_frequency);
    void pulse_rumble(uint16_t low_frequency, uint16_t high_frequency,
                      uint32_t duration_ms);
    void update();
    // Drains the link and ages the peer without merging or advancing a
    // frame. A stalled cabinet stops calling update(), so this is what lets
    // it still notice its partner leaving.
    void poll_link();
    void set_suppressed(bool suppressed) { m_suppressed = suppressed; }
    void set_test_input_enabled(bool enabled) {
        m_test_input_enabled.store(enabled, std::memory_order_release);
    }

    const input_state& state() const { return m_state; }
    bool controller_connected() const { return m_controller != nullptr; }

private:
    static bool watch_cabinet_button_events(void* userdata, SDL_Event* event);
    bool open_controller(SDL_JoystickID joystick_id);
    void scan_for_controller();
    float keyboard_value(input_action action, const bool* keys) const;
    float controller_value(input_action action) const;
    float action_value(input_action action, const bool* keys) const;
    float controller_axis_value(int axis) const;
    void set_controller_watch_bindings();
    bool initialize_network_link();
    void shutdown_network_link();
    void exchange_network_input(const input_state& local_state);
    void send_link_packet(const input_state& local_state,
                          bool shutting_down = false);
    bool drain_network_input();
    void wait_for_peer_frame(uint32_t frame);
    bool apply_rumble(uint16_t low_frequency, uint16_t high_frequency,
                      uint32_t duration_ms);

    SDL_Gamepad* m_controller{nullptr};
    input_mapping_config m_mappings{};
    std::string m_game_short_name;
    input_binding_table m_keyboard_bindings{};
    input_binding_table m_controller_bindings{};
    std::array<int, SDL_GAMEPAD_AXIS_COUNT> m_axis_centers{};
    arcade_feedback::profile m_feedback_profile{};
    uint16_t m_rumble_low{};
    uint16_t m_rumble_high{};
    uint16_t m_applied_rumble_low{};
    uint16_t m_applied_rumble_high{};
    uint16_t m_audio_impact_low{};
    uint16_t m_audio_impact_high{};
    uint64_t m_audio_impact_until{};
    uint64_t m_rumble_refresh_count{};
    bool m_rumble_supported{};
    bool m_rumble_error_reported{};
    // SDL3 joystick instance ids are unsigned; 0 is the "no controller"
    // sentinel (SDL3 never assigns instance id 0).
    std::atomic<SDL_JoystickID> m_controller_instance{0};
    input_state m_state{};
    float m_keyboard_steering{0.0f};
    uint64_t m_update_count{0};
    // Coin 1, coin 2, start 1 and start 2, in that order. Event-watch
    // bindings are atomic because SDL pumps events on the video worker.
    std::array<uint8_t, 4> m_cabinet_pulse_frames{};
    std::array<bool, 4> m_cabinet_pressed{};
    std::array<std::atomic_bool, 4> m_cabinet_events{};
    std::array<std::atomic_int, 4> m_watch_keyboard_codes{};
    std::array<std::atomic_int, 4> m_watch_controller_buttons{};
    std::atomic_bool m_mouse_activity{false};
    std::atomic_bool m_test_input_enabled{true};
    bool m_test_toggle_active{false};
    bool m_test_key_was_pressed{false};
    bool m_time_crisis_mouse{false};
    // Virtua Cop's model1io2 gun ADC spans a full 10-bit range calibrated in
    // the I/O board, so its cursor maps to the whole 0..255 axis rather than
    // Time Crisis's narrow cyber_axis window.
    bool m_lightgun_full_range{false};
    // Time Crisis cabinets have a foot pedal (Space, as on Time Crisis 1);
    // the other gun games leave that action as a reload shortcut instead.
    bool m_lightgun_pedal{false};
    // Picture rectangle published by the renderer; zero width means "not known
    // yet", in which case the gun falls back to assuming a centred 4:3 image.
    int m_picture_x{0};
    int m_picture_y{0};
    int m_picture_width{0};
    int m_picture_height{0};
    int m_picture_drawable_width{0};
    int m_picture_drawable_height{0};
    bool m_lightgun_mouse_active{false};
    bool m_event_watch_installed{false};
    bool m_initialized{false};
    bool m_suppressed{false};
    std::intptr_t m_network_socket{-1};
    uint16_t m_network_peer_port{};
    // Only used until the peer answers: after that the link is unicast.
    lan::peer_sweep m_network_sweep;
    std::chrono::steady_clock::time_point m_network_search_due{};
    uint8_t m_network_node{};
    uint32_t m_network_session{};
    uint32_t m_network_peer_session{};
    uint32_t m_network_sequence{};
    uint32_t m_network_peer_sequence{};
    uint16_t m_network_peer_age{0xffff};
    input_state m_network_peer_state{};

    // Lockstep netplay. Both machines simulate every frame themselves and
    // exchange only inputs, so a frame may only be simulated once both
    // sides' inputs for it are known. Local input is published for a frame
    // some way ahead - the delay - and each side keeps a small ring of the
    // peer's future inputs, which is what absorbs jitter without either
    // machine waiting on the other in the common case.
    static constexpr int netplay_delay_frames = 3;
    static constexpr int netplay_ring = 64;
    // The launch said this is a two-player session, so the start button is
    // the board's two-player start. Set for a shared cabinet, a split
    // screen, twin monitors and two machines alike.
    bool m_two_player_session{false};
    bool m_netplay{false};
    uint32_t m_netplay_frame{0};
    std::array<input_state, netplay_ring> m_netplay_local{};
    std::array<input_state, netplay_ring> m_netplay_peer{};
    std::array<bool, netplay_ring> m_netplay_peer_known{};
    // Highest frame the peer has published, for the stall report.
    uint32_t m_netplay_peer_frame{0};
    bool m_netplay_stalled{false};
    bool m_netplay_peer_ever_seen{false};
    uint8_t m_netplay_last_cabinet{0};

public:
    // Turns the input link from "newest packet wins" into frame-locked
    // exchange. Both cabinets must agree, so the host decides and the mode
    // travels with the launch.
    void set_netplay(bool enabled) { m_netplay = enabled; }
    bool netplay() const { return m_netplay; }
    // The frame the local machine is about to simulate. Netplay advances it
    // only when the peer's input for that frame has arrived.
    uint32_t netplay_frame() const { return m_netplay_frame; }
    // True while waiting on the peer: the caller must not step the board.
    bool netplay_stalled() const { return m_netplay_stalled; }
    // How far ahead the peer has published - a health reading for the UI.
    int netplay_lead_frames() const {
        return static_cast<int>(m_netplay_peer_frame) -
               static_cast<int>(m_netplay_frame);
    }
};
