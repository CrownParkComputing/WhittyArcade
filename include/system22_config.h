// system22_config.h - Namco System 22 configuration
#pragma once

#include "arcade_config.h"

#include <cstddef>

// Screen dimensions (System22 native)
inline constexpr int SYSTEM22_SCREEN_WIDTH = 640;
inline constexpr int SYSTEM22_SCREEN_HEIGHT = 480;

// Host presentation scale. The emulated framebuffer remains 640x480.
inline constexpr int SYSTEM22_WINDOW_WIDTH = SYSTEM22_SCREEN_WIDTH * 2;
inline constexpr int SYSTEM22_WINDOW_HEIGHT = SYSTEM22_SCREEN_HEIGHT * 2;

// Polygon rendering settings
inline constexpr int SYSTEM22_MAX_POLYGONS_PER_FRAME = 10000;
inline constexpr int SYSTEM22_TEXTURE_BANK_WIDTH = 2048;
inline constexpr int SYSTEM22_TEXTURE_BANK_HEIGHT = 1024;

// Audio settings
inline constexpr int SYSTEM22_AUDIO_SAMPLE_RATE = 85333;
inline constexpr int SYSTEM22_AUDIO_CHANNELS = 2;
inline constexpr int SYSTEM22_C352_VOICE_COUNT = 32;
inline constexpr int SYSTEM22_AUDIO_BUFFER_FRAMES = 512;
