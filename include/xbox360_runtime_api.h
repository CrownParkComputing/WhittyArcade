// Stable C boundary between MANX (SDL3/OpenGL) and the isolated
// ReXGlue runtime (SDL3/Vulkan). No ReXGlue or C++ types cross this boundary.
#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#define MANX_XBOX360_EXPORT __declspec(dllexport)
#else
#define MANX_XBOX360_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define MANX_XBOX360_RUNTIME_ABI 0x00010001u

typedef struct manx_xbox360_runtime manx_xbox360_runtime;

enum manx_xbox360_button {
    MANX_X360_DPAD_UP = 0x0001,
    MANX_X360_DPAD_DOWN = 0x0002,
    MANX_X360_DPAD_LEFT = 0x0004,
    MANX_X360_DPAD_RIGHT = 0x0008,
    MANX_X360_START = 0x0010,
    MANX_X360_BACK = 0x0020,
    MANX_X360_LEFT_THUMB = 0x0040,
    MANX_X360_RIGHT_THUMB = 0x0080,
    MANX_X360_LEFT_SHOULDER = 0x0100,
    MANX_X360_RIGHT_SHOULDER = 0x0200,
    MANX_X360_A = 0x1000,
    MANX_X360_B = 0x2000,
    MANX_X360_X = 0x4000,
    MANX_X360_Y = 0x8000,
};

typedef struct manx_xbox360_config {
    uint32_t struct_size;
    const char* game_root;
    const char* user_root;
    const char* cache_root;
} manx_xbox360_config;

typedef struct manx_xbox360_input {
    uint32_t struct_size;
    uint32_t packet_number;
    uint16_t buttons;
    uint8_t left_trigger;
    uint8_t right_trigger;
    int16_t left_x;
    int16_t left_y;
    int16_t right_x;
    int16_t right_y;
} manx_xbox360_input;

typedef struct manx_xbox360_vibration {
    uint32_t struct_size;
    uint16_t left_motor_speed;
    uint16_t right_motor_speed;
} manx_xbox360_vibration;

typedef struct manx_xbox360_frame {
    uint32_t struct_size;
    const uint8_t* rgba;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint64_t sequence;
} manx_xbox360_frame;

typedef struct manx_xbox360_achievement {
    uint32_t struct_size;
    uint32_t id;
    uint32_t gamerscore;
    uint8_t unlocked;
    uint8_t reserved[7];
    uint64_t unlocked_at_filetime;
    const char* label;
    const char* description;
} manx_xbox360_achievement;

typedef struct manx_xbox360_api {
    uint32_t struct_size;
    uint32_t abi_version;
    manx_xbox360_runtime* (*create)(const manx_xbox360_config*,
                                      char* error, size_t error_capacity);
    void (*destroy)(manx_xbox360_runtime*);
    void (*set_input)(manx_xbox360_runtime*,
                      const manx_xbox360_input*);
    int (*get_vibration)(manx_xbox360_runtime*,
                         manx_xbox360_vibration*);
    int (*capture_frame)(manx_xbox360_runtime*, manx_xbox360_frame*);
    size_t (*read_audio)(manx_xbox360_runtime*, int16_t* interleaved_stereo,
                         size_t frame_capacity);
    void (*set_paused)(manx_xbox360_runtime*, int paused);
    int (*is_running)(manx_xbox360_runtime*);
    size_t (*achievement_count)(manx_xbox360_runtime*);
    int (*achievement_at)(manx_xbox360_runtime*, size_t index,
                          manx_xbox360_achievement*);
    int (*take_unlocked_achievement)(manx_xbox360_runtime*,
                                    manx_xbox360_achievement*);
} manx_xbox360_api;

typedef const manx_xbox360_api* (*manx_xbox360_get_api_fn)(
    uint32_t requested_abi);

MANX_XBOX360_EXPORT const manx_xbox360_api* manx_xbox360_get_api(
    uint32_t requested_abi);

#ifdef __cplusplus
}
#endif
