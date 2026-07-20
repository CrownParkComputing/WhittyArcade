// license:BSD-3-Clause
// Standalone Yamaha YMW258-F / Sega 315-5560 MultiPCM interface.
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef uint8_t u8;
typedef int8_t s8;
typedef uint16_t u16;
typedef int16_t s16;
typedef uint32_t u32;
typedef int32_t s32;
typedef uint64_t u64;
typedef int64_t s64;

#ifdef __cplusplus
extern "C" {
#endif

// Model 1 drives each 315-5560 at 10 MHz. The measured word clock uses /224.
#define MULTIPCM_NATIVE_RATE (10000000u / 224u)

typedef struct multipcm multipcm_t;

multipcm_t *multipcm_create(const u8 *rom, u32 rom_size);
void multipcm_destroy(multipcm_t *chip);
void multipcm_reset(multipcm_t *chip);
void multipcm_write(multipcm_t *chip, u8 offset, u8 data);
u8 multipcm_read(const multipcm_t *chip);
void multipcm_set_bank(multipcm_t *chip, u8 bank);
void multipcm_tick_m68k_cycles(multipcm_t *chip, u32 cycles);
void multipcm_render(void *chip, s32 *stereo_out, u32 frames);
u32 multipcm_debug_writes(void);
u32 multipcm_active_voices(const multipcm_t *chip);

#ifdef __cplusplus
}
#endif
