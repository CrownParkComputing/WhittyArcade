// arcade_input.cpp - shared, configurable SDL arcade input implementation.
#include "arcade_input.h"
#include "arcade_sdl_guard.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {
constexpr float axis_deadzone = 0.15f;
constexpr float cabinet_press_threshold = 0.5f;
constexpr std::size_t sdl_guid_text_size = 33;
constexpr std::array<uint8_t, 4> input_link_magic{{'W', 'A', 'I', '2'}};

std::atomic_bool network_peer_seen{false};
std::atomic_uint32_t network_peer_ipv4{0};
std::atomic_uint8_t network_authoritative_player{0};

struct network_input_packet {
    std::array<uint8_t, 4> magic{};
    uint8_t version{};
    uint8_t node{};
    uint16_t state_size{};
    uint32_t session{};
    uint32_t sequence{};
    uint8_t active_player{};
    uint8_t reserved[3]{};
    input_state state{};
};
static_assert(std::is_trivially_copyable_v<input_state>);

#if defined(_WIN32)
using input_native_socket = SOCKET;
constexpr input_native_socket invalid_input_socket = INVALID_SOCKET;
input_native_socket to_input_socket(std::intptr_t value) {
    return static_cast<input_native_socket>(value);
}
#else
using input_native_socket = int;
constexpr input_native_socket invalid_input_socket = -1;
input_native_socket to_input_socket(std::intptr_t value) {
    return static_cast<input_native_socket>(value);
}
#endif

uint16_t input_link_port(const char* name) {
    const char* value = std::getenv(name);
    if (!value || !*value) return 0;
    const unsigned long parsed = std::strtoul(value, nullptr, 10);
    return parsed > 1024 && parsed <= 65535 ?
        static_cast<uint16_t>(parsed) : 0;
}

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

bool second_cabinet_process() {
    const char* node = std::getenv("WHITTY_CABINET_NODE");
    return node && std::strcmp(node, "2") == 0;
}

void route_player_two_keyboard_to_cabinet(
        input_binding_table& bindings) {
    if (!second_cabinet_process()) return;
    // A network cabinet is a separate physical machine, so its local user
    // gets the normal P1 keyboard layout; the link maps that contribution to
    // the authoritative game's P2 inputs. Only the second process of a local
    // two-cabinet setup needs the alternate P2 keys.
    const char* network_link = std::getenv("WHITTY_INPUT_LINK");
    if (network_link && *network_link &&
        std::strcmp(network_link, "0") != 0)
        return;
    const auto copy = [&bindings](input_action destination,
                                  input_action source) {
        bindings[input_action_index(destination)] =
            bindings[input_action_index(source)];
    };
    copy(input_action::coin1, input_action::coin2);
    copy(input_action::start1, input_action::start2);
    copy(input_action::p1_left, input_action::p2_left);
    copy(input_action::p1_right, input_action::p2_right);
    copy(input_action::p1_up, input_action::p2_up);
    copy(input_action::p1_down, input_action::p2_down);
    copy(input_action::p1_action1, input_action::p2_action1);
    copy(input_action::p1_action2, input_action::p2_action2);
    copy(input_action::steer_left, input_action::p2_left);
    copy(input_action::steer_right, input_action::p2_right);
    copy(input_action::gas, input_action::p2_up);
    copy(input_action::brake, input_action::p2_down);
    copy(input_action::shift_down, input_action::p2_action1);
    copy(input_action::shift_up, input_action::p2_action2);
}
} // namespace

bool arcade_input_network_peer_seen() {
    return network_peer_seen.load(std::memory_order_acquire);
}

uint32_t arcade_input_network_peer_ipv4() {
    return network_peer_ipv4.load(std::memory_order_acquire);
}

void arcade_input_set_authoritative_player(uint8_t player) {
    network_authoritative_player.store(
        player <= 2 ? player : 0, std::memory_order_release);
}

uint8_t arcade_input_network_authoritative_player() {
    return network_authoritative_player.load(std::memory_order_acquire);
}

arcade_input::~arcade_input() {
    shutdown();
}

bool arcade_input::initialize_network_link() {
    shutdown_network_link();
    const char* enabled = std::getenv("WHITTY_INPUT_LINK");
    if (!enabled || !*enabled || std::strcmp(enabled, "0") == 0)
        return true;
    const char* node_text = std::getenv("WHITTY_CABINET_NODE");
    const int node = node_text ? std::atoi(node_text) : 0;
    const uint16_t local_port = input_link_port("WHITTY_INPUT_LOCAL_PORT");
    const uint16_t peer_port = input_link_port("WHITTY_INPUT_PEER_PORT");
    if ((node != 1 && node != 2) || !local_port || !peer_port)
        return false;

#if defined(_WIN32)
    static const bool winsock_ready = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    if (!winsock_ready) return false;
#endif
    const input_native_socket socket_handle =
        ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_handle == invalid_input_socket) return false;
    int enabled_option = 1;
    setsockopt(socket_handle, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&enabled_option),
               sizeof(enabled_option));
    setsockopt(socket_handle, SOL_SOCKET, SO_BROADCAST,
               reinterpret_cast<const char*>(&enabled_option),
               sizeof(enabled_option));
    // Input is live state, not an event recording. A small receive buffer
    // prevents a stalled frame from accumulating seconds of old commands.
    const int receive_buffer = 32768;
    setsockopt(socket_handle, SOL_SOCKET, SO_RCVBUF,
               reinterpret_cast<const char*>(&receive_buffer),
               sizeof(receive_buffer));
#if defined(_WIN32)
    u_long nonblocking = 1;
    if (ioctlsocket(socket_handle, FIONBIO, &nonblocking) != 0) {
        closesocket(socket_handle);
        return false;
    }
#else
    const int flags = fcntl(socket_handle, F_GETFL, 0);
    if (flags < 0 ||
        fcntl(socket_handle, F_SETFL, flags | O_NONBLOCK) < 0) {
        ::close(socket_handle);
        return false;
    }
#endif
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(local_port);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(socket_handle, reinterpret_cast<const sockaddr*>(&local),
               sizeof(local)) != 0) {
#if defined(_WIN32)
        closesocket(socket_handle);
#else
        ::close(socket_handle);
#endif
        return false;
    }

    m_network_socket = static_cast<std::intptr_t>(socket_handle);
    m_network_peer_port = peer_port;
    m_network_node = static_cast<uint8_t>(node);
    const uint64_t ticks = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    m_network_session = static_cast<uint32_t>(
        ticks ^ (ticks >> 32) ^ reinterpret_cast<std::uintptr_t>(this));
    if (m_network_session == 0) m_network_session = 1;
    m_network_peer_session = 0;
    m_network_sequence = 0;
    m_network_peer_sequence = 0;
    m_network_peer_age = 0xffff;
    m_network_peer_state = {};
    network_peer_seen.store(false, std::memory_order_release);
    network_peer_ipv4.store(0, std::memory_order_release);
    std::printf("WhittyArcade player %u input link UDP %u -> %u\n",
                m_network_node, local_port, peer_port);
    return true;
}

void arcade_input::shutdown_network_link() {
    if (m_network_socket != -1) {
        const input_native_socket socket_handle =
            to_input_socket(m_network_socket);
#if defined(_WIN32)
        closesocket(socket_handle);
#else
        ::close(socket_handle);
#endif
    }
    m_network_socket = -1;
    m_network_peer_port = 0;
    m_network_node = 0;
    m_network_session = 0;
    m_network_peer_session = 0;
    m_network_peer_age = 0xffff;
    m_network_peer_state = {};
    network_peer_seen.store(false, std::memory_order_release);
    network_peer_ipv4.store(0, std::memory_order_release);
}

void arcade_input::exchange_network_input(const input_state& local_state) {
    if (m_network_socket == -1 || m_network_node == 0) return;
    const input_native_socket socket_handle =
        to_input_socket(m_network_socket);
    network_input_packet outgoing{};
    outgoing.magic = input_link_magic;
    outgoing.version = 1;
    outgoing.node = m_network_node;
    outgoing.state_size = static_cast<uint16_t>(sizeof(input_state));
    outgoing.session = m_network_session;
    outgoing.sequence = ++m_network_sequence;
    outgoing.active_player =
        network_authoritative_player.load(std::memory_order_acquire);
    outgoing.state = local_state;

    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(m_network_peer_port);
    peer.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    sendto(socket_handle, reinterpret_cast<const char*>(&outgoing),
           static_cast<int>(sizeof(outgoing)), 0,
           reinterpret_cast<const sockaddr*>(&peer), sizeof(peer));
    // Also loop the LAN packet onto this host so the exact two-computer mode
    // can be tested with two local WhittyArcade instances.
    peer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sendto(socket_handle, reinterpret_cast<const char*>(&outgoing),
           static_cast<int>(sizeof(outgoing)), 0,
           reinterpret_cast<const sockaddr*>(&peer), sizeof(peer));

    bool received_peer = false;
    for (;;) {
        network_input_packet incoming{};
        sockaddr_in sender{};
#if defined(_WIN32)
        int sender_size = sizeof(sender);
#else
        socklen_t sender_size = sizeof(sender);
#endif
        const int received = static_cast<int>(recvfrom(
            socket_handle, reinterpret_cast<char*>(&incoming),
            static_cast<int>(sizeof(incoming)), 0,
            reinterpret_cast<sockaddr*>(&sender), &sender_size));
        if (received != static_cast<int>(sizeof(incoming))) break;
        if (incoming.magic != input_link_magic ||
            incoming.version != 1 ||
            incoming.node == m_network_node ||
            incoming.state_size != sizeof(input_state))
            continue;
        if (incoming.session != m_network_peer_session) {
            m_network_peer_session = incoming.session;
            m_network_peer_sequence = 0;
        }
        // UDP may duplicate and reorder datagrams. Keep only the newest live
        // state; an older button-down packet must never follow a newer
        // button-release and make firing stick.
        if (m_network_peer_sequence != 0 &&
            static_cast<int32_t>(incoming.sequence -
                                 m_network_peer_sequence) <= 0)
            continue;
        m_network_peer_state = incoming.state;
        if (incoming.node == 1)
            network_authoritative_player.store(
                incoming.active_player <= 2 ?
                    incoming.active_player : 0,
                std::memory_order_release);
        m_network_peer_sequence = incoming.sequence;
        m_network_peer_age = 0;
        network_peer_ipv4.store(sender.sin_addr.s_addr,
                                std::memory_order_release);
        received_peer = true;
    }
    if (!received_peer && m_network_peer_age < 0xfffe)
        ++m_network_peer_age;
    const bool connected = m_network_peer_age < 180;
    const bool input_fresh = m_network_peer_age < 6;
    network_peer_seen.store(connected, std::memory_order_release);

    // Both emulators see the same logical cabinet: computer one supplies P1,
    // while computer two's normal controls feed the original board's P2 lines.
    if (!connected) {
        if (m_network_node == 2) m_state = input_state{};
        return;
    }
    const input_state neutral{};
    const input_state& remote =
        input_fresh ? m_network_peer_state : neutral;
    const input_state& player1 =
        m_network_node == 1 ? local_state : remote;
    const input_state& player2 =
        m_network_node == 2 ? local_state : remote;
    input_state combined = player1;
    combined.coin2 = combined.coin2 || player2.coin1 || player2.coin2;
    combined.p2_start =
        combined.p2_start || player2.start || player2.p2_start;
    combined.p2_stick_x = player2.left_stick_x;
    combined.p2_stick_y = player2.left_stick_y;
    for (std::size_t button = 0; button < std::size(combined.p2_buttons);
         ++button) {
        combined.p2_buttons[button] =
            combined.p2_buttons[button] || player2.buttons[button] ||
            player2.p2_buttons[button];
    }
    m_state = combined;
}

bool arcade_input::watch_cabinet_button_events(void* userdata,
                                               SDL_Event* event) {
    auto* input = static_cast<arcade_input*>(userdata);
    if (!input || !event) return true;

    if (event->type == SDL_EVENT_KEY_DOWN && !event->key.repeat) {
        const int code = static_cast<int>(event->key.scancode);
        for (std::size_t slot = 0; slot < cabinet_actions.size(); ++slot) {
            if (input->m_watch_keyboard_codes[slot].load(
                    std::memory_order_relaxed) == code) {
                input->m_cabinet_events[slot].store(
                    true, std::memory_order_relaxed);
            }
        }
    } else if (event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
               event->gbutton.which == input->m_controller_instance.load(
                                            std::memory_order_relaxed)) {
        const int code = static_cast<int>(event->gbutton.button);
        for (std::size_t slot = 0; slot < cabinet_actions.size(); ++slot) {
            if (input->m_watch_controller_buttons[slot].load(
                    std::memory_order_relaxed) == code) {
                input->m_cabinet_events[slot].store(
                    true, std::memory_order_relaxed);
            }
        }
    } else if (input->m_time_crisis_mouse &&
               (event->type == SDL_EVENT_MOUSE_MOTION ||
                event->type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                event->type == SDL_EVENT_MOUSE_BUTTON_UP)) {
        input->m_mouse_activity.store(true, std::memory_order_relaxed);
    }
    return 0;
}

bool arcade_input::initialize(std::string_view game_short_name) {
    std::lock_guard<std::mutex> sdl_lock(arcade_sdl_mutex());
    if (m_initialized) return true;
    // The persistent video worker owns SDL_INIT_EVENTS and is the only thread
    // that pumps the event queue. This adapter owns controller/joystick
    // references for the lifetime of one emulated board.
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK)) {
        std::fprintf(stderr, "SDL controller initialization failed: %s\n",
                     SDL_GetError());
        return false;
    }

    m_game_short_name = game_short_name;
    // Time Crisis (System 22) and the Model 2 light-gun games (Virtua Cop and
    // Virtua Cop 2) all drive the crosshair from the mouse; only their ADC
    // calibration differs. The Model 2 guns use the full 0..255 axis.
    m_lightgun_full_range = m_game_short_name == "vcop" ||
                            m_game_short_name == "vcop2";
    m_time_crisis_mouse = m_game_short_name == "timecris" ||
                          m_lightgun_full_range;
    m_lightgun_mouse_active = false;
    m_mouse_activity.store(false, std::memory_order_relaxed);
    m_mappings = load_input_mappings();
    m_keyboard_bindings = keyboard_bindings_for(m_mappings,
                                                 m_game_short_name);
    route_player_two_keyboard_to_cabinet(m_keyboard_bindings);
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

    SDL_SetGamepadEventsEnabled(true);
    SDL_AddEventWatch(&arcade_input::watch_cabinet_button_events, this);
    m_event_watch_installed = true;
    m_initialized = true;
    scan_for_controller();
    if (!m_controller)
        std::printf("Input: keyboard active; waiting for an SDL controller\n");
    if (m_time_crisis_mouse)
        std::printf("Time Crisis mouse gun: move to aim, left click fires, "
                    "hold Space to stand, release Space or right click to "
                    "take cover/reload\n");
    if (!initialize_network_link())
        std::fprintf(stderr,
                     "Could not open WhittyArcade's network player link\n");
    return true;
}

void arcade_input::reload_mappings() {
    std::lock_guard<std::mutex> sdl_lock(arcade_sdl_mutex());
    if (!m_initialized) return;

    m_mappings = load_input_mappings();
    m_keyboard_bindings = keyboard_bindings_for(m_mappings,
                                                 m_game_short_name);
    route_player_two_keyboard_to_cabinet(m_keyboard_bindings);
    for (std::size_t slot = 0; slot < cabinet_actions.size(); ++slot) {
        const input_binding& binding = m_keyboard_bindings[
            input_action_index(cabinet_actions[slot])];
        m_watch_keyboard_codes[slot].store(
            watch_code(binding, input_binding_type::keyboard),
            std::memory_order_relaxed);
    }

    m_controller_bindings = default_controller_bindings();
    if (m_controller) {
        SDL_Joystick* joystick = SDL_GetGamepadJoystick(m_controller);
        std::array<char, sdl_guid_text_size> guid_text{};
        SDL_GUIDToString(SDL_GetJoystickGUID(joystick),
                                  guid_text.data(),
                                  static_cast<int>(guid_text.size()));
        m_controller_bindings = controller_bindings_for(
            m_mappings, guid_text.data(), m_game_short_name);
    }
    set_controller_watch_bindings();

    // Menu navigation must never leak a stale cabinet edge into gameplay.
    m_cabinet_pulse_frames.fill(0);
    m_cabinet_pressed.fill(false);
    for (auto& event : m_cabinet_events)
        event.store(false, std::memory_order_relaxed);
    m_keyboard_steering = 0.0f;
    m_state = input_state{};
}

void arcade_input::shutdown() {
    std::lock_guard<std::mutex> sdl_lock(arcade_sdl_mutex());
    shutdown_network_link();
    if (m_event_watch_installed) {
        SDL_RemoveEventWatch(&arcade_input::watch_cabinet_button_events, this);
        m_event_watch_installed = false;
    }
    m_controller_instance.store(-1, std::memory_order_relaxed);
    if (m_controller) {
        SDL_CloseGamepad(m_controller);
        m_controller = nullptr;
    }
    if (m_initialized) {
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK);
        m_initialized = false;
    }
    m_time_crisis_mouse = false;
    m_lightgun_mouse_active = false;
    m_mouse_activity.store(false, std::memory_order_relaxed);
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

bool arcade_input::open_controller(SDL_JoystickID joystick_id) {
    if (!SDL_IsGamepad(joystick_id)) return false;
    SDL_Gamepad* controller = SDL_OpenGamepad(joystick_id);
    if (!controller) return false;

    if (m_controller) SDL_CloseGamepad(m_controller);
    m_controller = controller;
    SDL_Joystick* joystick = SDL_GetGamepadJoystick(m_controller);
    m_controller_instance.store(SDL_GetJoystickID(joystick),
                                std::memory_order_relaxed);

    std::array<char, sdl_guid_text_size> guid_text{};
    SDL_GUIDToString(SDL_GetJoystickGUID(joystick), guid_text.data(),
                              static_cast<int>(guid_text.size()));
    m_controller_bindings = controller_bindings_for(
        m_mappings, guid_text.data(), m_game_short_name);
    set_controller_watch_bindings();
    for (int axis = 0; axis < SDL_GAMEPAD_AXIS_COUNT; ++axis) {
        m_axis_centers[static_cast<std::size_t>(axis)] =
            SDL_GetGamepadAxis(
                m_controller, static_cast<SDL_GamepadAxis>(axis));
    }

    std::printf("Input controller: %s [%s]\n",
                SDL_GetGamepadName(m_controller) ?
                    SDL_GetGamepadName(m_controller) : "unknown",
                guid_text.data());
    return true;
}

void arcade_input::scan_for_controller() {
    if (m_controller) return;
    int count = 0;
    SDL_JoystickID* ids = SDL_GetJoysticks(&count);
    if (!ids) return;
    if (const char* preferred = std::getenv("RRACER_CONTROLLER")) {
        const int index = static_cast<int>(std::strtol(preferred, nullptr, 0));
        if (index >= 0 && index < count) open_controller(ids[index]);
        SDL_free(ids);
        return;
    }
    for (int index = 0; index < count; ++index)
        if (open_controller(ids[index])) {
            SDL_free(ids);
            return;
        }
    SDL_free(ids);
}

float arcade_input::keyboard_value(input_action action,
                                   const bool* keys) const {
    if (!keys) return 0.0f;
    const input_binding& binding =
        m_keyboard_bindings[input_action_index(action)];
    if (binding.type != input_binding_type::keyboard || binding.code < 0 ||
        binding.code >= SDL_SCANCODE_COUNT)
        return 0.0f;
    return keys[binding.code] ? 1.0f : 0.0f;
}

float arcade_input::controller_axis_value(int axis) const {
    if (!m_controller || axis < 0 || axis >= SDL_GAMEPAD_AXIS_COUNT)
        return 0.0f;
    const int center = m_axis_centers[static_cast<std::size_t>(axis)];
    const int raw = SDL_GetGamepadAxis(
        m_controller, static_cast<SDL_GamepadAxis>(axis));
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
    const auto binding_value = [this](const input_binding& binding) {
      if (binding.type == input_binding_type::controller_button) {
        if (binding.code < 0 || binding.code >= SDL_GAMEPAD_BUTTON_COUNT)
            return 0.0f;
        return SDL_GetGamepadButton(
                   m_controller,
                   static_cast<SDL_GamepadButton>(binding.code)) ?
            1.0f : 0.0f;
      }
      if (binding.type == input_binding_type::controller_axis) {
        const float value = controller_axis_value(binding.code);
        return std::clamp(binding.direction < 0 ? -value : value,
                          0.0f, 1.0f);
      }
      return 0.0f;
    };
    const input_binding& primary =
        m_controller_bindings[input_action_index(action)];
    return std::max(binding_value(primary),
                    binding_value(default_controller_alias(action, primary)));
}

float arcade_input::action_value(input_action action,
                                 const bool* keys) const {
    return std::max(keyboard_value(action, keys), controller_value(action));
}

void arcade_input::update() {
    std::lock_guard<std::mutex> sdl_lock(arcade_sdl_mutex());
    if (!m_initialized) return;
    // SDL_PollEvent on the video worker has already pumped host events. Never
    // pump here: doing so concurrently with the renderer corrupted SDL/driver
    // state after several live ROM changes.
    if (m_controller && !SDL_GamepadConnected(m_controller)) {
        std::printf("Input controller disconnected\n");
        m_controller_instance.store(-1, std::memory_order_relaxed);
        SDL_CloseGamepad(m_controller);
        m_controller = nullptr;
        for (auto& code : m_watch_controller_buttons)
            code.store(-1, std::memory_order_relaxed);
    }
    if (!m_controller && (m_update_count % 60) == 0) scan_for_controller();
    ++m_update_count;

    const bool* keys = SDL_GetKeyboardState(nullptr);
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
        m_lightgun_mouse_active = false;
        m_mouse_activity.store(false, std::memory_order_relaxed);
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
    const float action1 = action_value(input_action::p1_action1, keys);
    const float action2 = action_value(input_action::p1_action2, keys);
    m_state.buttons[0] = action1 >= cabinet_press_threshold;
    m_state.buttons[1] = action2 >= cabinet_press_threshold;
    m_state.buttons[2] = action_value(input_action::p1_action3, keys) >=
                         cabinet_press_threshold;
    m_state.p2_buttons[0] = action_value(input_action::p2_action1, keys) >=
                            cabinet_press_threshold;
    m_state.p2_buttons[1] = action_value(input_action::p2_action2, keys) >=
                            cabinet_press_threshold;

    if (m_time_crisis_mouse) {
        const bool mouse_activity = m_mouse_activity.exchange(
            false, std::memory_order_relaxed);
        const bool alternate_gun_activity =
            std::fabs(left_x) > 0.0f || std::fabs(left_y) > 0.0f;
        if (mouse_activity)
            m_lightgun_mouse_active = true;
        else if (alternate_gun_activity)
            m_lightgun_mouse_active = false;

        SDL_Window* mouse_window = SDL_GetMouseFocus();
        if (m_lightgun_mouse_active && mouse_window) {
            float mouse_fx = 0.0f;
            float mouse_fy = 0.0f;
            const uint32_t mouse_buttons = SDL_GetMouseState(
                &mouse_fx, &mouse_fy);
            const int mouse_x = static_cast<int>(mouse_fx);
            const int mouse_y = static_cast<int>(mouse_fy);
            int window_width = 0;
            int window_height = 0;
            SDL_GetWindowSize(mouse_window, &window_width, &window_height);
            if (window_width > 1 && window_height > 1) {
                // Host presentation preserves the board's 4:3 image. Remove
                // any pillarbox/letterbox area before mapping the cursor to
                // the calibrated gun ADC range.
                int content_x = 0;
                int content_y = 0;
                int content_width = window_width;
                int content_height = window_height;
                if (window_width * 3 > window_height * 4) {
                    content_width = window_height * 4 / 3;
                    content_x = (window_width - content_width) / 2;
                } else if (window_height * 4 > window_width * 3) {
                    content_height = window_width * 3 / 4;
                    content_y = (window_height - content_height) / 2;
                }
                const float normalized_x = std::clamp(
                    static_cast<float>(mouse_x - content_x) /
                        static_cast<float>(std::max(content_width - 1, 1)),
                    0.0f, 1.0f);
                const float normalized_y = std::clamp(
                    static_cast<float>(mouse_y - content_y) /
                        static_cast<float>(std::max(content_height - 1, 1)),
                    0.0f, 1.0f);
                if (m_lightgun_full_range) {
                    m_state.left_stick_x = static_cast<uint8_t>(
                        std::lround(normalized_x * 255.0f));
                    m_state.left_stick_y = static_cast<uint8_t>(
                        std::lround(normalized_y * 255.0f));
                } else {
                    m_state.left_stick_x =
                        cyber_axis(normalized_x * 2.0f - 1.0f);
                    m_state.left_stick_y =
                        cyber_axis(normalized_y * 2.0f - 1.0f);
                }
                m_state.buttons[0] =
                    (mouse_buttons & SDL_BUTTON_LMASK) != 0 ||
                    action1 >= cabinet_press_threshold;
                // The real pedal is held while Richard is exposed. Releasing
                // it ducks into cover and reloads, so right mouse reverses
                // the held Space/controller pedal for an intuitive reload.
                m_state.buttons[1] =
                    action2 >= cabinet_press_threshold &&
                    (mouse_buttons & SDL_BUTTON_RMASK) == 0;
            }
        }
    }

    exchange_network_input(m_state);
    for (uint8_t& frames : m_cabinet_pulse_frames)
        if (frames != 0) --frames;
}
