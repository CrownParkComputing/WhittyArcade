// Optional continuous-PCM extension for MANX game plugins.
//
// Arcade-native plugins often emit discrete cue events. Recompiled console
// games already own a complete mixer, so reducing their music, speech and
// effects to cues would throw the real soundtrack away. This separate symbol
// keeps ABI-2 plugins compatible while allowing those games to hand the host
// already-mixed signed-16 PCM.
#pragma once

#include "manx_game_plugin.h"

#define MANX_GAME_PCM_ABI_VERSION 1u
#define MANX_GAME_PCM_ENTRY_SYMBOL "manx_game_pcm_entry"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct manx_game_pcm_block {
    const int16_t* samples; // interleaved, valid until the next plugin call
    uint32_t frames;
    uint32_t sample_rate;
    uint32_t channels;
} manx_game_pcm_block;

typedef struct manx_game_pcm_api {
    uint32_t abi_version;
    uint32_t (*take_blocks)(manx_game_instance* instance,
                            manx_game_pcm_block* out,
                            uint32_t max_blocks);
} manx_game_pcm_api;

typedef const manx_game_pcm_api* (*manx_game_pcm_entry_fn)(void);

#ifdef __cplusplus
} // extern "C"
#endif
