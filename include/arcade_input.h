// arcade_input.h - shared SDL keyboard and controller input adapter.
#pragma once

#include "arcade_types.h"
#include "input_mapping.h"

#include <SDL2/SDL.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

class arcade_input {
public:
    ~arcade_input();

    bool initialize(std::string_view game_short_name = {});
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
    static int watch_cabinet_button_events(void* userdata, SDL_Event* event);
    bool open_controller(int joystick_index);
    void scan_for_controller();
    float keyboard_value(input_action action, const uint8_t* keys) const;
    float controller_value(input_action action) const;
    float action_value(input_action action, const uint8_t* keys) const;
    float controller_axis_value(int axis) const;
    void set_controller_watch_bindings();

    SDL_GameController* m_controller{nullptr};
    input_mapping_config m_mappings{};
    std::string m_game_short_name;
    input_binding_table m_keyboard_bindings{};
    input_binding_table m_controller_bindings{};
    std::array<int, SDL_CONTROLLER_AXIS_MAX> m_axis_centers{};
    std::atomic<SDL_JoystickID> m_controller_instance{-1};
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
    bool m_time_crisis_mouse{false};
    bool m_lightgun_mouse_active{false};
    bool m_event_watch_installed{false};
    bool m_initialized{false};
    bool m_suppressed{false};
};
