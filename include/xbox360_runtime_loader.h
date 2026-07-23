#pragma once

#include "xbox360_runtime_api.h"

#include <cstdint>
#include <string>
#include <vector>

struct xbox360_runtime_achievement {
    std::uint32_t id{};
    std::uint32_t gamerscore{};
    bool unlocked{};
    std::uint64_t unlocked_at_filetime{};
    std::string label;
    std::string description;
};

class xbox360_runtime_loader {
public:
    xbox360_runtime_loader() = default;
    ~xbox360_runtime_loader();

    xbox360_runtime_loader(const xbox360_runtime_loader&) = delete;
    xbox360_runtime_loader& operator=(const xbox360_runtime_loader&) = delete;

    bool initialize(const std::string& game_root,
                    const std::string& user_root,
                    const std::string& cache_root,
                    std::string& error);
    void shutdown();
    void set_input(const whitty_xbox360_input& input);
    bool capture_frame(std::vector<std::uint8_t>& rgba, int& width,
                       int& height, std::uint64_t& sequence);
    std::size_t read_audio(std::int16_t* stereo, std::size_t frame_capacity);
    void set_paused(bool paused);
    bool running() const;
    std::vector<xbox360_runtime_achievement> achievements() const;
    bool take_unlocked_achievement(xbox360_runtime_achievement& achievement);

private:
    bool initialize_direct(const std::string& game_root,
                           const std::string& user_root,
                           const std::string& cache_root,
                           std::string& error);
    bool initialize_worker(const std::string& game_root,
                           const std::string& user_root,
                           const std::string& cache_root,
                           std::string& error);
    void shutdown_direct();
    void shutdown_worker();

    static xbox360_runtime_achievement copy_achievement(
        const whitty_xbox360_achievement& value);

    void* m_library{};
    const whitty_xbox360_api* m_api{};
    whitty_xbox360_runtime* m_runtime{};
    void* m_worker_state{};
    int m_worker_fd{-1};
    std::int64_t m_worker_pid{-1};
};
