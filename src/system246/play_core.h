#pragma once

#include "system246_rom.h"

#include <memory>
#include <string>

class CGSHandler;
class system246_audio_control;
class system246_input_channel;
class system246_video_bridge;

// CPU/HLE boundary for the pinned Play! core. No Play! type other than the
// opaque GS pointer escapes this adapter.
class system246_play_core {
public:
    system246_play_core();
    ~system246_play_core();

    system246_play_core(const system246_play_core&) = delete;
    system246_play_core& operator=(const system246_play_core&) = delete;

    bool initialize(
        const system246_rom_load_result& roms,
        const std::string& optical_path,
        std::shared_ptr<system246_input_channel> input,
        std::shared_ptr<system246_audio_control> audio,
        std::shared_ptr<system246_video_bridge> video,
        std::string& error);
    void shutdown();
    void set_paused(bool paused);
    bool paused() const;
    CGSHandler* gs_handler() const;

private:
    struct implementation;
    std::unique_ptr<implementation> m_impl;
};
