// arcade_input.cpp - shared, configurable SDL arcade input implementation.
#include "arcade_input.h"
#include "arcade_sdl_guard.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {
constexpr float axis_deadzone = 0.15f;
constexpr float cabinet_press_threshold = 0.5f;
constexpr std::size_t sdl_guid_text_size = 33;

constexpr std::array<input_action, 4> cabinet_actions{{
    input_action::coin1,
    input_action::coin2,
    input_action::start1,
    input_action::start2,
}};

float approach(float current, float target, float amount) {
    if (current < target) return std::min(current + amount, target);
    if (current > target) return std::max(current - amount, target);
    return current;
}

uint8_t cyber_axis(float normalized) {
    constexpr float center = 0x7f;
    constexpr float half_range = 0x38;
    const int value = static_cast<int>(std::lround(
        center + std::clamp(normalized, -1.0f, 1.0f) * half_range));
    return static_cast<uint8_t>(std::clamp(value, 0x47, 0xb7));
}

int watch_code(const input_binding& binding, input_binding_type type) {
    return binding.type == type ? binding.code : -1;
}
} // namespace

arcade_input::~arcade_input() {
    shutdown();
}

int arcade_input::watch_cabinet_button_events(void* userdata,
                                               SDL_Event* event) {
    auto* input = static_cast<arcade_input*>(userdata);
    if (!input || !event) return 0;

    if (event->type == SDL_KEYDOWN && !event->key.repeat) {
        const int code = static_cast<int>(event->key.keysym.scancode);
        for (std::size_t slot = 0; slot < cabinet_actions.size(); ++slot) {
            if (input->m_watch_keyboard_codes[slot].load(
                    std::memory_order_relaxed) == code) {
                input->m_cabinet_events[slot].store(
                    true, std::memory_order_relaxed);
            }
        }
    } else if (event->type == SDL_CONTROLLERBUTTONDOWN &&
               event->cbutton.which == input->m_controller_instance.load(
                                            std::memory_order_relaxed)) {
        const int code = static_cast<int>(event->cbutton.button);
        for (std::size_t slot = 0; slot < cabinet_actions.size(); ++slot) {
            if (input->m_watch_controller_buttons[slot].load(
                    std::memory_order_relaxed) == code) {
                input->m_cabinet_events[slot].store(
                    true, std::memory_order_relaxed);
            }
        }
    }
    return 0;
}

bool arcade_input::initialize() {
    std::lock_guard<std::mutex> sdl_lock(arcade_sdl_mutex());
    if (m_initialized) return true;
    // The persistent video worker owns SDL_INIT_EVENTS and is the only thread
    // that pumps the event queue. This adapter owns controller/joystick
    // references for the lifetime of one emulated board.
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0) {
        std::fprintf(stderr, "SDL controller initialization failed: %s\n",
                     SDL_GetError());
        return false;
    }

    m_mappings = load_input_mappings();
    m_keyboard_bindings = m_mappings.keyboard;
    m_controller_bindings = default_controller_bindings();
    for (std::size_t slot = 0; slot < cabinet_actions.size(); ++slot) {
        const input_binding& binding = m_keyboard_bindings[
            input_action_index(cabinet_actions[slot])];
        m_watch_keyboard_codes[slot].store(
            watch_code(binding, input_binding_type::keyboard),
            std::memory_order_relaxed);
        m_watch_controller_buttons[slot].store(-1,
                                               std::memory_order_relaxed);
        m_cabinet_events[slot].store(false, std::memory_order_relaxed);
    }

    SDL_GameControllerEventState(SDL_ENABLE);
    SDL_AddEventWatch(&arcade_input::watch_cabinet_button_events, this);
    m_event_watch_installed = true;
    m_initialized = true;
    scan_for_controller();
    if (!m_controller)
        std::printf("Input: keyboard active; waiting for an SDL controller\n");
    return true;
}

void arcade_input::shutdown() {
    std::lock_guard<std::mutex> sdl_lock(arcade_sdl_mutex());
    if (m_event_watch_installed) {
        SDL_DelEventWatch(&arcade_input::watch_cabinet_button_events, this);
        m_event_watch_installed = false;
    }
    m_controller_instance.store(-1, std::memory_order_relaxed);
    if (m_controller) {
        SDL_GameControllerClose(m_controller);
        m_controller = nullptr;
    }
    if (m_initialized) {
        SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK);
        m_initialized = false;
    }
}

void arcade_input::set_controller_watch_bindings() {
    for (std::size_t slot = 0; slot < cabinet_actions.size(); ++slot) {
        const input_binding& binding = m_controller_bindings[
            input_action_index(cabinet_actions[slot])];
        m_watch_controller_buttons[slot].store(
            watch_code(binding, input_binding_type::controller_button),
            std::memory_order_relaxed);
    }
}

bool arcade_input::open_controller(int joystick_index) {
    if (joystick_index < 0 || joystick_index >= SDL_NumJoysticks() ||
        !SDL_IsGameController(joystick_index))
        return false;
    SDL_GameController* controller = SDL_GameControllerOpen(joystick_index);
    if (!controller) return false;

    if (m_controller) SDL_GameControllerClose(m_controller);
    m_controller = controller;
    SDL_Joystick* joystick = SDL_GameControllerGetJoystick(m_controller);
    m_controller_instance.store(SDL_JoystickInstanceID(joystick),
                                std::memory_order_relaxed);

    std::array<char, sdl_guid_text_size> guid_text{};
    SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(joystick), guid_text.data(),
                              static_cast<int>(guid_text.size()));
    m_controller_bindings = controller_bindings_for(m_mappings,
                                                     guid_text.data());
    set_controller_watch_bindings();
    for (int axis = 0; axis < SDL_CONTROLLER_AXIS_MAX; ++axis) {
        m_axis_centers[static_cast<std::size_t>(axis)] =
            SDL_GameControllerGetAxis(
                m_controller, static_cast<SDL_GameControllerAxis>(axis));
    }

    std::printf("Input controller: %s [%s]\n",
                SDL_GameControllerName(m_controller) ?
                    SDL_GameControllerName(m_controller) : "unknown",
                guid_text.data());
    return true;
}

void arcade_input::scan_for_controller() {
    if (m_controller) return;
    if (const char* preferred = std::getenv("RRACER_CONTROLLER")) {
        const int index = static_cast<int>(std::strtol(preferred, nullptr, 0));
        if (open_controller(index)) return;
    }
    for (int index = 0; index < SDL_NumJoysticks(); ++index)
        if (open_controller(index)) return;
}

float arcade_input::keyboard_value(input_action action,
                                   const uint8_t* keys) const {
    if (!keys) return 0.0f;
    const input_binding& binding =
        m_keyboard_bindings[input_action_index(action)];
    if (binding.type != input_binding_type::keyboard || binding.code < 0 ||
        binding.code >= SDL_NUM_SCANCODES)
        return 0.0f;
    return keys[binding.code] ? 1.0f : 0.0f;
}

float arcade_input::controller_axis_value(int axis) const {
    if (!m_controller || axis < 0 || axis >= SDL_CONTROLLER_AXIS_MAX)
        return 0.0f;
    const int center = m_axis_centers[static_cast<std::size_t>(axis)];
    const int raw = SDL_GameControllerGetAxis(
        m_controller, static_cast<SDL_GameControllerAxis>(axis));
    const int delta = raw - center;
    const int available = delta < 0 ? center + 32768 : 32767 - center;
    if (available <= 0) return 0.0f;
    float normalized = static_cast<float>(delta) /
                       static_cast<float>(available);
    normalized = std::clamp(normalized, -1.0f, 1.0f);
    if (std::fabs(normalized) <= axis_deadzone) return 0.0f;
    return std::copysign((std::fabs(normalized) - axis_deadzone) /
                         (1.0f - axis_deadzone), normalized);
}

float arcade_input::controller_value(input_action action) const {
    if (!m_controller) return 0.0f;
    const input_binding& binding =
        m_controller_bindings[input_action_index(action)];
    if (binding.type == input_binding_type::controller_button) {
        if (binding.code < 0 || binding.code >= SDL_CONTROLLER_BUTTON_MAX)
            return 0.0f;
        return SDL_GameControllerGetButton(
                   m_controller,
                   static_cast<SDL_GameControllerButton>(binding.code)) ?
            1.0f : 0.0f;
    }
    if (binding.type == input_binding_type::controller_axis) {
        const float value = controller_axis_value(binding.code);
        return std::clamp(binding.direction < 0 ? -value : value,
                          0.0f, 1.0f);
    }
    return 0.0f;
}

float arcade_input::action_value(input_action action,
                                 const uint8_t* keys) const {
    return std::max(keyboard_value(action, keys), controller_value(action));
}

void arcade_input::update() {
    std::lock_guard<std::mutex> sdl_lock(arcade_sdl_mutex());
    if (!m_initialized) return;
    // SDL_PollEvent on the video worker has already pumped host events. Never
    // pump here: doing so concurrently with the renderer corrupted SDL/driver
    // state after several live ROM changes.
    if (m_controller && !SDL_GameControllerGetAttached(m_controller)) {
        std::printf("Input controller disconnected\n");
        m_controller_instance.store(-1, std::memory_order_relaxed);
        SDL_GameControllerClose(m_controller);
        m_controller = nullptr;
        for (auto& code : m_watch_controller_buttons)
            code.store(-1, std::memory_order_relaxed);
    }
    if (!m_controller && (m_update_count % 60) == 0) scan_for_controller();
    ++m_update_count;

    const uint8_t* keys = SDL_GetKeyboardState(nullptr);
    // Cabinet lines are edge-latched briefly. Real cabinets sample dedicated
    // I/O continuously, whereas the adapter transfers host state once per
    // frame; the latch prevents a quick tap being lost between board scans.
    for (std::size_t slot = 0; slot < cabinet_actions.size(); ++slot) {
        const bool pressed = action_value(cabinet_actions[slot], keys) >=
                             cabinet_press_threshold;
        const bool event = m_cabinet_events[slot].exchange(
            false, std::memory_order_relaxed);
        if (!m_suppressed &&
            (event || (pressed && !m_cabinet_pressed[slot]))) {
            m_cabinet_pulse_frames[slot] = slot < 2 ? 2 : 4;
        }
        m_cabinet_pressed[slot] = pressed;
    }

    if (m_suppressed) {
        m_keyboard_steering = 0.0f;
        m_cabinet_pulse_frames.fill(0);
        m_state = input_state{};
        return;
    }

    const float key_left = keyboard_value(input_action::steer_left, keys);
    const float key_right = keyboard_value(input_action::steer_right, keys);
    const float keyboard_target = std::clamp(key_right - key_left,
                                             -1.0f, 1.0f);
    m_keyboard_steering = approach(
        m_keyboard_steering, keyboard_target,
        keyboard_target == 0.0f ? 0.12f : 0.08f);
    float steering = std::clamp(
        controller_value(input_action::steer_right) -
            controller_value(input_action::steer_left),
        -1.0f, 1.0f);
    if (key_left > 0.0f || key_right > 0.0f)
        steering = m_keyboard_steering;
    const int steering_adc = static_cast<int>(std::lround(
        0x800 + steering * 0x580));
    m_state.steering = static_cast<uint16_t>(
        std::clamp(steering_adc, 0x280, 0xd80));

    const float gas = action_value(input_action::gas, keys);
    const float brake = action_value(input_action::brake, keys);
    m_state.gas = static_cast<uint16_t>(std::lround(gas * 0x610));
    m_state.brake = static_cast<uint16_t>(std::lround(brake * 0x610));

    // Cyber Commando and 2D boards consume the same logical P1 directions.
    const float left_x = std::clamp(
        action_value(input_action::p1_right, keys) -
            action_value(input_action::p1_left, keys), -1.0f, 1.0f);
    const float left_y = std::clamp(
        action_value(input_action::p1_down, keys) -
            action_value(input_action::p1_up, keys), -1.0f, 1.0f);
    const float right_x = std::clamp(
        action_value(input_action::right_right, keys) -
            action_value(input_action::right_left, keys), -1.0f, 1.0f);
    const float right_y = std::clamp(
        action_value(input_action::right_down, keys) -
            action_value(input_action::right_up, keys), -1.0f, 1.0f);
    const float p2_x = std::clamp(
        action_value(input_action::p2_right, keys) -
            action_value(input_action::p2_left, keys), -1.0f, 1.0f);
    const float p2_y = std::clamp(
        action_value(input_action::p2_down, keys) -
            action_value(input_action::p2_up, keys), -1.0f, 1.0f);
    m_state.left_stick_x = cyber_axis(left_x);
    m_state.left_stick_y = cyber_axis(left_y);
    m_state.right_stick_x = cyber_axis(right_x);
    m_state.right_stick_y = cyber_axis(right_y);
    m_state.p2_stick_x = cyber_axis(p2_x);
    m_state.p2_stick_y = cyber_axis(p2_y);

    // Coin/start lines are momentary switches rather than held gameplay
    // buttons. Only the edge-latched pulse reaches the emulated I/O board.
    m_state.coin1 = m_cabinet_pulse_frames[0] != 0;
    m_state.coin2 = m_cabinet_pulse_frames[1] != 0;
    m_state.start = m_cabinet_pulse_frames[2] != 0;
    m_state.p2_start = m_cabinet_pulse_frames[3] != 0;
    m_state.service = action_value(input_action::service, keys) >=
                      cabinet_press_threshold;
    m_state.test = m_test_input_enabled.load(std::memory_order_acquire) &&
                   action_value(input_action::test, keys) >=
                       cabinet_press_threshold;
    m_state.shift_down = action_value(input_action::shift_down, keys) >=
                         cabinet_press_threshold;
    m_state.shift_up = action_value(input_action::shift_up, keys) >=
                       cabinet_press_threshold;
    m_state.view = action_value(input_action::view1, keys) >=
                   cabinet_press_threshold;
    m_state.view2 = action_value(input_action::view2, keys) >=
                    cabinet_press_threshold;
    m_state.view3 = action_value(input_action::view3, keys) >=
                    cabinet_press_threshold;
    m_state.view4 = action_value(input_action::view4, keys) >=
                    cabinet_press_threshold;
    m_state.buttons[0] = action_value(input_action::p1_action1, keys) >=
                         cabinet_press_threshold;
    m_state.buttons[1] = action_value(input_action::p1_action2, keys) >=
                         cabinet_press_threshold;
    m_state.buttons[2] = action_value(input_action::p1_action3, keys) >=
                         cabinet_press_threshold;
    m_state.p2_buttons[0] = action_value(input_action::p2_action1, keys) >=
                            cabinet_press_threshold;

    for (uint8_t& frames : m_cabinet_pulse_frames)
        if (frames != 0) --frames;
}
