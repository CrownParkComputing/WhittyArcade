#include "plugin_pcm_queue.h"

#include <cassert>
#include <cstdio>

int main() {
    constexpr uint32_t rate = 48000;
    constexpr uint32_t channels = 2;
    constexpr int boundary =
        rate * channels * int(sizeof(int16_t)) *
        int(kPluginPcmMaximumLatencyMs) / 1000;

    assert(!plugin_pcm_queue_needs_realign(0, rate, channels));
    assert(!plugin_pcm_queue_needs_realign(boundary, rate, channels));
    assert(plugin_pcm_queue_needs_realign(boundary + 1, rate, channels));
    assert(!plugin_pcm_queue_needs_realign(boundary + 1, 0, channels));
    assert(!plugin_pcm_queue_needs_realign(boundary + 1, rate, 0));
    std::printf("plugin_pcm_queue_test: stale PCM is cleared above %u ms, "
                "while current audio remains queued\n",
                kPluginPcmMaximumLatencyMs);
    return 0;
}
