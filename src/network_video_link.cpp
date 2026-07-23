#include "network_video_link.h"

#include "arcade_input.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

#include <zlib.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {
constexpr uint32_t video_magic = 0x32564157; // "WAV2", little endian
constexpr uint32_t maximum_frame_bytes = 4096U * 4096U * 4U;
constexpr uint32_t maximum_compressed_bytes =
    maximum_frame_bytes + 65536U;

struct video_header {
    uint32_t magic{};
    uint32_t sequence{};
    uint32_t width{};
    uint32_t height{};
    uint32_t display_width{};
    uint32_t display_height{};
    uint32_t flags{};
    uint32_t raw_size{};
    uint32_t compressed_size{};
};

#if defined(_WIN32)
using native_socket = SOCKET;
constexpr native_socket invalid_socket = INVALID_SOCKET;
void close_socket(native_socket socket) { closesocket(socket); }
bool socket_retryable() {
    const int error = WSAGetLastError();
    return error == WSAEWOULDBLOCK || error == WSAETIMEDOUT ||
           error == WSAEINTR;
}
#else
using native_socket = int;
constexpr native_socket invalid_socket = -1;
void close_socket(native_socket socket) { ::close(socket); }
bool socket_retryable() {
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}
#endif

uint16_t environment_port() {
    const char* text = std::getenv("WHITTY_VIDEO_PORT");
    if (!text || !*text) return 0;
    const unsigned long value = std::strtoul(text, nullptr, 10);
    return value > 1024 && value <= 65535 ?
        static_cast<uint16_t>(value) : 0;
}

int environment_role() {
    const char* text = std::getenv("WHITTY_VIDEO_ROLE");
    const int value = text ? std::atoi(text) : 0;
    return value == 1 || value == 2 ? value : 0;
}

void set_socket_timeouts(native_socket socket) {
#if defined(_WIN32)
    const DWORD timeout = 250;
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    const timeval timeout{0, 250000};
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
}

bool set_nonblocking(native_socket socket) {
#if defined(_WIN32)
    u_long enabled = 1;
    return ioctlsocket(socket, FIONBIO, &enabled) == 0;
#else
    const int flags = fcntl(socket, F_GETFL, 0);
    return flags >= 0 &&
           fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

bool send_all(native_socket socket, const uint8_t* bytes, std::size_t size,
              const std::atomic_bool& stop) {
    while (size && !stop.load(std::memory_order_acquire)) {
        const int sent = static_cast<int>(send(
            socket, reinterpret_cast<const char*>(bytes),
            static_cast<int>(std::min<std::size_t>(
                size, std::numeric_limits<int>::max())), 0));
        if (sent > 0) {
            bytes += sent;
            size -= static_cast<std::size_t>(sent);
        } else if (sent < 0 && socket_retryable()) {
            continue;
        } else {
            return false;
        }
    }
    return size == 0;
}

bool receive_all(native_socket socket, uint8_t* bytes, std::size_t size,
                 const std::atomic_bool& stop) {
    while (size && !stop.load(std::memory_order_acquire)) {
        const int received = static_cast<int>(recv(
            socket, reinterpret_cast<char*>(bytes),
            static_cast<int>(std::min<std::size_t>(
                size, std::numeric_limits<int>::max())), 0));
        if (received > 0) {
            bytes += received;
            size -= static_cast<std::size_t>(received);
        } else if (received < 0 && socket_retryable()) {
            continue;
        } else {
            return false;
        }
    }
    return size == 0;
}
} // namespace

network_video_link::network_video_link()
    : m_role(environment_role()), m_port(environment_port()) {
    if (!m_role || !m_port) {
        m_role = 0;
        return;
    }
#if defined(_WIN32)
    static const bool winsock_ready = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    if (!winsock_ready) {
        m_role = 0;
        return;
    }
#endif
    m_thread = std::thread(&network_video_link::run, this);
}

network_video_link::~network_video_link() {
    m_stop.store(true, std::memory_order_release);
    m_ready.notify_all();
    if (m_thread.joinable()) m_thread.join();
}

void network_video_link::submit(network_video_frame frame) {
    if (!sender() || frame.rgba.empty()) return;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pending_send = std::move(frame);
        m_send_pending = true;
    }
    m_ready.notify_one();
}

bool network_video_link::take_received(network_video_frame& frame) {
    if (!receiver()) return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_receive_pending) return false;
    frame = std::move(m_received);
    m_received = {};
    m_receive_pending = false;
    return true;
}

void network_video_link::run() {
    if (sender())
        run_sender();
    else
        run_receiver();
}

void network_video_link::run_sender() {
    const native_socket listener =
        ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == invalid_socket) return;
    int reuse = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(m_port);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(listener, reinterpret_cast<const sockaddr*>(&address),
               sizeof(address)) != 0 ||
        listen(listener, 1) != 0 || !set_nonblocking(listener)) {
        close_socket(listener);
        return;
    }
    std::printf("WhittyArcade Player 1 video stream listening on TCP %u\n",
                m_port);

    native_socket client = invalid_socket;
    std::vector<uint8_t> previous_frame;
    uint32_t delta_frames = 0;
    while (!m_stop.load(std::memory_order_acquire)) {
        if (client == invalid_socket) {
            client = accept(listener, nullptr, nullptr);
            if (client == invalid_socket) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            set_socket_timeouts(client);
            previous_frame.clear();
            delta_frames = 0;
            std::printf("WhittyArcade Player 2 video display connected\n");
        }

        network_video_frame frame;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_ready.wait_for(lock, std::chrono::milliseconds(100), [this] {
                return m_stop.load(std::memory_order_acquire) ||
                       m_send_pending;
            });
            if (m_stop.load(std::memory_order_acquire)) break;
            if (!m_send_pending) continue;
            frame = std::move(m_pending_send);
            m_pending_send = {};
            m_send_pending = false;
        }
        if (frame.rgba.size() > maximum_frame_bytes) continue;
        const bool delta = !previous_frame.empty() &&
            previous_frame.size() == frame.rgba.size() &&
            delta_frames < 120;
        std::vector<uint8_t> payload = frame.rgba;
        if (delta) {
            for (std::size_t index = 0; index < payload.size(); ++index)
                payload[index] ^= previous_frame[index];
        }
        uLongf compressed_size = compressBound(payload.size());
        std::vector<uint8_t> compressed(compressed_size);
        if (compress2(compressed.data(), &compressed_size,
                      payload.data(), payload.size(),
                      Z_BEST_SPEED) != Z_OK)
            continue;
        compressed.resize(compressed_size);
        video_header header{};
        header.magic = video_magic;
        header.sequence = frame.sequence;
        header.width = static_cast<uint32_t>(frame.width);
        header.height = static_cast<uint32_t>(frame.height);
        header.display_width = static_cast<uint32_t>(frame.display_width);
        header.display_height = static_cast<uint32_t>(frame.display_height);
        header.flags = (frame.flip_y ? 1U : 0U) |
                       (frame.apply_board_color ? 2U : 0U) |
                       (delta ? 4U : 0U);
        header.raw_size = static_cast<uint32_t>(frame.rgba.size());
        header.compressed_size =
            static_cast<uint32_t>(compressed.size());
        if (!send_all(client, reinterpret_cast<const uint8_t*>(&header),
                      sizeof(header), m_stop) ||
            !send_all(client, compressed.data(), compressed.size(), m_stop)) {
            close_socket(client);
            client = invalid_socket;
            previous_frame.clear();
            delta_frames = 0;
            std::printf("WhittyArcade Player 2 video display disconnected\n");
        } else {
            previous_frame = std::move(frame.rgba);
            delta_frames = delta ? delta_frames + 1 : 0;
        }
    }
    if (client != invalid_socket) close_socket(client);
    close_socket(listener);
}

void network_video_link::run_receiver() {
    while (!m_stop.load(std::memory_order_acquire)) {
        const uint32_t peer_ipv4 = arcade_input_network_peer_ipv4();
        if (!peer_ipv4) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        const native_socket server =
            ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (server == invalid_socket) return;
        set_socket_timeouts(server);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(m_port);
        address.sin_addr.s_addr = peer_ipv4;
        if (connect(server, reinterpret_cast<const sockaddr*>(&address),
                    sizeof(address)) != 0) {
            close_socket(server);
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            continue;
        }
        std::printf("WhittyArcade Player 2 receiving Player 1 video\n");
        std::vector<uint8_t> previous_frame;
        while (!m_stop.load(std::memory_order_acquire)) {
            video_header header{};
            if (!receive_all(server,
                    reinterpret_cast<uint8_t*>(&header),
                    sizeof(header), m_stop) ||
                header.magic != video_magic ||
                header.width == 0 || header.height == 0 ||
                header.width > 4096 || header.height > 4096 ||
                header.raw_size == 0 ||
                header.raw_size > maximum_frame_bytes ||
                header.raw_size !=
                    static_cast<uint64_t>(header.width) *
                        header.height * 4U ||
                header.compressed_size == 0 ||
                header.compressed_size > maximum_compressed_bytes)
                break;
            std::vector<uint8_t> compressed(header.compressed_size);
            if (!receive_all(server, compressed.data(), compressed.size(),
                             m_stop))
                break;
            network_video_frame frame;
            frame.rgba.resize(header.raw_size);
            uLongf raw_size = frame.rgba.size();
            if (uncompress(frame.rgba.data(), &raw_size,
                           compressed.data(), compressed.size()) != Z_OK ||
                raw_size != frame.rgba.size())
                break;
            const bool delta = (header.flags & 4U) != 0;
            if (delta) {
                if (previous_frame.size() != frame.rgba.size())
                    break;
                for (std::size_t index = 0; index < frame.rgba.size();
                     ++index)
                    frame.rgba[index] ^= previous_frame[index];
            }
            previous_frame = frame.rgba;
            frame.width = static_cast<int>(header.width);
            frame.height = static_cast<int>(header.height);
            frame.display_width = static_cast<int>(header.display_width);
            frame.display_height = static_cast<int>(header.display_height);
            frame.flip_y = (header.flags & 1) != 0;
            frame.apply_board_color = (header.flags & 2) != 0;
            frame.sequence = header.sequence;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_received = std::move(frame);
                m_receive_pending = true;
            }
        }
        close_socket(server);
        if (!m_stop.load(std::memory_order_acquire))
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}
