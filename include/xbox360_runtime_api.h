// Stable C boundary between WhittyArcade (SDL3/OpenGL) and the isolated
// ReXGlue runtime (SDL3/Vulkan). No ReXGlue or C++ types cross this boundary.
#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#define WHITTY_XBOX360_EXPORT __declspec(dllexport)
#else
#define WHITTY_XBOX360_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define WHITTY_XBOX360_RUNTIME_ABI 0x00010000u

typedef struct whitty_xbox360_runtime whitty_xbox360_runtime;

enum whitty_xbox360_button {
    WHITTY_X360_DPAD_UP = 0x0001,
    WHITTY_X360_DPAD_DOWN = 0x0002,
    WHITTY_X360_DPAD_LEFT = 0x0004,
    WHITTY_X360_DPAD_RIGHT = 0x0008,
    WHITTY_X360_START = 0x0010,
    WHITTY_X360_BACK = 0x0020,
    WHITTY_X360_LEFT_THUMB = 0x0040,
    WHITTY_X360_RIGHT_THUMB = 0x0080,
    WHITTY_X360_LEFT_SHOULDER = 0x0100,
    WHITTY_X360_RIGHT_SHOULDER = 0x0200,
    WHITTY_X360_A = 0x1000,
    WHITTY_X360_B = 0x2000,
    WHITTY_X360_X = 0x4000,
    WHITTY_X360_Y = 0x8000,
};

typedef struct whitty_xbox360_config {
    uint32_t struct_size;
    const char* game_root;
    const char* user_root;
    const char* cache_root;
} whitty_xbox360_config;

typedef struct whitty_xbox360_input {
    uint32_t struct_size;
    uint32_t packet_number;
    uint16_t buttons;
    uint8_t left_trigger;
    uint8_t right_trigger;
    int16_t left_x;
    int16_t left_y;
    int16_t right_x;
    int16_t right_y;
} whitty_xbox360_input;

typedef struct whitty_xbox360_frame {
    uint32_t struct_size;
    const uint8_t* rgba;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint64_t sequence;
} whitty_xbox360_frame;

typedef struct whitty_xbox360_achievement {
    uint32_t struct_size;
    uint32_t id;
    uint32_t gamerscore;
    uint8_t unlocked;
    uint8_t reserved[7];
    uint64_t unlocked_at_filetime;
    const char* label;
    const char* description;
} whitty_xbox360_achievement;

typedef struct whitty_xbox360_api {
    uint32_t struct_size;
    uint32_t abi_version;
    whitty_xbox360_runtime* (*create)(const whitty_xbox360_config*,
                                      char* error, size_t error_capacity);
    void (*destroy)(whitty_xbox360_runtime*);
    void (*set_input)(whitty_xbox360_runtime*,
                      const whitty_xbox360_input*);
    int (*capture_frame)(whitty_xbox360_runtime*, whitty_xbox360_frame*);
    size_t (*read_audio)(whitty_xbox360_runtime*, int16_t* interleaved_stereo,
                         size_t frame_capacity);
    void (*set_paused)(whitty_xbox360_runtime*, int paused);
    int (*is_running)(whitty_xbox360_runtime*);
    size_t (*achievement_count)(whitty_xbox360_runtime*);
    int (*achievement_at)(whitty_xbox360_runtime*, size_t index,
                          whitty_xbox360_achievement*);
    int (*take_unlocked_achievement)(whitty_xbox360_runtime*,
                                    whitty_xbox360_achievement*);
} whitty_xbox360_api;

typedef const whitty_xbox360_api* (*whitty_xbox360_get_api_fn)(
    uint32_t requested_abi);

WHITTY_XBOX360_EXPORT const whitty_xbox360_api* whitty_xbox360_get_api(
    uint32_t requested_abi);

#ifdef __cplusplus
}
#endif
