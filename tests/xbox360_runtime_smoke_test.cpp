#include "xbox360_runtime_loader.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr,
                     "usage: xbox360_runtime_smoke_test GAME USER CACHE\n");
        return 2;
    }

    xbox360_runtime_loader runtime;
    std::string error;
    if (!runtime.initialize(argv[1], argv[2], argv[3], error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }

    manx_xbox360_input input{};
    input.struct_size = sizeof(input);
    std::array<std::int16_t, 4096 * 2> audio{};
    std::vector<std::uint8_t> frame;
    int width = 0;
    int height = 0;
    std::uint64_t sequence = 0;
    std::size_t audio_frames = 0;
    bool captured = false;

    for (std::uint32_t iteration = 0;
         iteration < 1200 && runtime.running(); ++iteration) {
        input.packet_number = iteration + 1;
        runtime.set_input(input);
        captured = runtime.capture_frame(frame, width, height, sequence) ||
                   captured;
        audio_frames += runtime.read_audio(audio.data(), 4096);
        if (captured && audio_frames != 0 && iteration >= 120) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    const auto achievements = runtime.achievements();
    std::printf("running=%d frame=%dx%d sequence=%llu audio_frames=%zu "
                "achievements=%zu\n",
                runtime.running() ? 1 : 0, width, height,
                static_cast<unsigned long long>(sequence), audio_frames,
                achievements.size());
    runtime.shutdown();

    if (!captured) {
        std::fprintf(stderr, "Robotron produced no offscreen frame\n");
        return 1;
    }
    return 0;
}

