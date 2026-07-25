// arcade_input.h - shared SDL keyboard and controller input adapter.
#pragma once

#include "arcade_types.h"
#include "input_mapping.h"

#include <SDL3/SDL.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

// True after the board-neutral two-computer input link has received a packet
// from the other WhittyArcade instance. Native cabinet links (for example
// Sega Rally's Model 2 board) report their own state instead.
bool arcade_input_network_peer_seen();
uint32_t arcade_input_network_peer_ipv4();
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
    void update();
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

    SDL_Gamepad* m_controller{nullptr};
    input_mapping_config m_mappings{};
    std::string m_game_short_name;
    input_binding_table m_keyboard_bindings{};
    input_binding_table m_controller_bindings{};
    std::array<int, SDL_GAMEPAD_AXIS_COUNT> m_axis_centers{};
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
    uint8_t m_network_node{};
    uint32_t m_network_session{};
    uint32_t m_network_peer_session{};
    uint32_t m_network_sequence{};
    uint32_t m_network_peer_sequence{};
    uint16_t m_network_peer_age{0xffff};
    input_state m_network_peer_state{};
};
