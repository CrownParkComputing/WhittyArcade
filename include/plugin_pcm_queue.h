#pragma once

#include <cstdint>

// Maximum tolerated PCM latency for a frame-driven game plugin. Normal audio
// remains below this while the device consumes one frame's samples during the
// next frame. Crossing it means presentation stalled; retaining the queue
// would preserve a permanent A/V offset after recovery or restart.
inline constexpr uint32_t kPluginPcmMaximumLatencyMs = 150;

inline bool plugin_pcm_queue_needs_realign(int queued_bytes,
                                          uint32_t sample_rate,
                                          uint32_t channels) noexcept {
    if (queued_bytes <= 0 || sample_rate == 0 || channels == 0) return false;
    const uint64_t maximum_bytes =
        (uint64_t(sample_rate) * channels * sizeof(int16_t) *
         kPluginPcmMaximumLatencyMs) /
        1000u;
    return static_cast<uint64_t>(queued_bytes) > maximum_bytes;
}
