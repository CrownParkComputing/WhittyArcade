#pragma once

#include "arcade_types.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

class CPadHandler;

// Thread-safe handoff from Whitty's SDL cabinet adapter to Play!'s VM thread.
class system246_input_channel {
public:
    void set_state(const input_state& state);
    input_state state() const;
    void hold_test_for_frames(uint32_t frames);
    bool take_forced_test_frame();

private:
    mutable std::mutex m_mutex;
    input_state m_state{};
    std::atomic<uint32_t> m_forced_test_frames{0};
};

// Ownership is transferred to CPS2VM.
CPadHandler* make_system246_pad_handler(
    std::shared_ptr<system246_input_channel> channel);
