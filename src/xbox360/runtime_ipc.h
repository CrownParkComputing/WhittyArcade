#pragma once

#include "xbox360_runtime_api.h"

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#if !defined(_WIN32)
#include <pthread.h>
#endif

namespace whitty::xbox360::ipc {

constexpr std::uint32_t kMagic = 0x58333630u; // "X360"
constexpr std::uint32_t kVersion = 1;
constexpr std::size_t kErrorCapacity = 1024;
constexpr std::size_t kMaximumWidth = 1920;
constexpr std::size_t kMaximumHeight = 1080;
constexpr std::size_t kFrameCapacity =
    kMaximumWidth * kMaximumHeight * 4;
constexpr std::size_t kAudioFrameCapacity = 48000;
constexpr std::size_t kAchievementCapacity = 32;
constexpr std::size_t kAchievementEventCapacity = 32;
constexpr std::size_t kLabelCapacity = 128;
constexpr std::size_t kDescriptionCapacity = 512;

enum class init_state : std::uint32_t {
    pending = 0,
    ready = 1,
    failed = 2,
};

struct achievement {
    std::uint32_t id{};
    std::uint32_t gamerscore{};
    std::uint32_t unlocked{};
    std::uint64_t unlocked_at_filetime{};
    char label[kLabelCapacity]{};
    char description[kDescriptionCapacity]{};
};

struct shared_state {
    std::uint32_t magic{kMagic};
    std::uint32_t version{kVersion};
#if !defined(_WIN32)
    pthread_mutex_t mutex{};
#endif
    std::atomic<std::uint32_t> initialized{
        static_cast<std::uint32_t>(init_state::pending)};
    std::atomic<std::uint32_t> running{};
    std::atomic<std::uint32_t> shutdown_requested{};

    char error[kErrorCapacity]{};
    whitty_xbox360_input input{sizeof(whitty_xbox360_input)};
    std::uint32_t paused{};

    std::uint32_t frame_width{};
    std::uint32_t frame_height{};
    std::uint32_t frame_stride{};
    std::uint64_t frame_sequence{};
    std::uint8_t frame[kFrameCapacity]{};

    std::uint64_t audio_read{};
    std::uint64_t audio_write{};
    std::uint64_t audio_count{};
    std::int16_t audio[kAudioFrameCapacity * 2]{};

    std::uint32_t achievement_count{};
    achievement achievements[kAchievementCapacity]{};
    std::uint32_t event_read{};
    std::uint32_t event_write{};
    std::uint32_t event_count{};
    achievement events[kAchievementEventCapacity]{};
};

#if !defined(_WIN32)
inline bool initialize_mutex(shared_state& state) {
    pthread_mutexattr_t attributes;
    if (pthread_mutexattr_init(&attributes) != 0) return false;
    bool success =
        pthread_mutexattr_setpshared(&attributes, PTHREAD_PROCESS_SHARED) == 0;
#if defined(PTHREAD_MUTEX_ROBUST)
    success = success &&
        pthread_mutexattr_setrobust(&attributes, PTHREAD_MUTEX_ROBUST) == 0;
#endif
    if (success)
        success = pthread_mutex_init(&state.mutex, &attributes) == 0;
    pthread_mutexattr_destroy(&attributes);
    return success;
}

inline bool lock(shared_state& state) {
    const int result = pthread_mutex_lock(&state.mutex);
#if defined(EOWNERDEAD)
    if (result == EOWNERDEAD) {
        pthread_mutex_consistent(&state.mutex);
        return true;
    }
#endif
    return result == 0;
}

inline void unlock(shared_state& state) {
    pthread_mutex_unlock(&state.mutex);
}
#endif

inline void copy_text(char* destination, std::size_t capacity,
                      const std::string& source) {
    if (capacity == 0) return;
    const std::size_t length =
        source.size() < capacity - 1 ? source.size() : capacity - 1;
    std::memcpy(destination, source.data(), length);
    destination[length] = '\0';
}

} // namespace whitty::xbox360::ipc
