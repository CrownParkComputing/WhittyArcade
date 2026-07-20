/*
 * Geometrizer - Sega Model 1 Emulator
 * audio/gpgx_shared.h
 *
 * Minimal compatibility shim for the vendored Genesis Plus GX YM2612 core
 * (gpgx_ym2612.c). The original file expects a "shared.h" umbrella header
 * from the GPGX project; we only need the integer typedefs and stubs for
 * the save-state macros (we don't expose save/load).
 */

#ifndef GEO_GPGX_SHARED_H
#define GEO_GPGX_SHARED_H

#include <stdint.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* GPGX uses uppercase fixed-width typedefs. */
typedef int8_t   INT8;
typedef uint8_t  UINT8;
typedef int16_t  INT16;
typedef uint16_t UINT16;
typedef int32_t  INT32;
typedef uint32_t UINT32;
typedef int64_t  INT64;
typedef uint64_t UINT64;

typedef uint8_t  uint8;

/* GPGX uses INLINE for static inline hot helpers. */
#ifndef INLINE
#define INLINE static __inline__
#endif

/* The save_param/load_param macros in YM2612{Load,Save}Context expect a
 * `state` pointer in scope. We don't use save states, but the functions
 * exist as part of the API â define no-op shims so they compile. */
#define save_param(addr, size) do { (void)(addr); (void)(size); } while (0)
#define load_param(addr, size) do { (void)(addr); (void)(size); } while (0)

#include "gpgx_ym2612.h"

#endif /* GEO_GPGX_SHARED_H */

