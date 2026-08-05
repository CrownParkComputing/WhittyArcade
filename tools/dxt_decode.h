// dxt_decode.h — standalone DXT1/DXT3/DXT5 software decoder.
//
// Decodes a single mip level of BC1/BC2/BC3 (DXT1/3/5) to RGBA8888.
// No GPU, no Vulkan, no allocations — the caller provides dst.
//
// Usage:
//   uint32_t width, height, four_cc;
//   const uint8_t *src = ...;   // compressed blocks (after DDS header)
//   size_t src_size = ...;
//   uint32_t *dst = malloc(width * height * 4);
//   dxt_decode_image(src, src_size, dst, width, height, four_cc);
//   // dst is now RGBA8888, row-major, width*4 bytes per row.

#ifndef DXT_DECODE_H
#define DXT_DECODE_H

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FOURCC_DXT1 0x31545844u
#define FOURCC_DXT3 0x33545844u
#define FOURCC_DXT5 0x35545844u

/* ── DXT1 block decoder ────────────────────────────────────────
 *
 * 8 bytes per 4×4 block: two RGB565 endpoints + 16 two-bit indices.
 * If c0 > c1 the endpoints are interpolated; otherwise only three
 * colours are used and index 3 means transparent black. */

static void dxt1_decode_block(const uint8_t *src, uint32_t *dst,
                              uint32_t dst_row_stride)
{
    uint32_t c0 = (uint32_t)src[0] | ((uint32_t)src[1] << 8);
    uint32_t c1 = (uint32_t)src[2] | ((uint32_t)src[3] << 8);
    uint32_t idx = (uint32_t)src[4] | ((uint32_t)src[5] << 8) |
                   ((uint32_t)src[6] << 16) | ((uint32_t)src[7] << 24);

    /* Expand 5-6-5 to 8-8-8. */
    uint32_t r0 = ((c0 >> 11) & 0x1Fu) * 255u / 31u;
    uint32_t g0 = ((c0 >>  5) & 0x3Fu) * 255u / 63u;
    uint32_t b0 =  (c0        & 0x1Fu) * 255u / 31u;
    uint32_t r1 = ((c1 >> 11) & 0x1Fu) * 255u / 31u;
    uint32_t g1 = ((c1 >>  5) & 0x3Fu) * 255u / 63u;
    uint32_t b1 =  (c1        & 0x1Fu) * 255u / 31u;

    uint32_t col[4];
    col[0] = 0xFF000000u | (b0 << 16) | (g0 << 8) | r0;
    col[1] = 0xFF000000u | (b1 << 16) | (g1 << 8) | r1;

    if (c0 > c1) {
        col[2] = 0xFF000000u |
            (((2u * b0 + b1) / 3u) << 16) |
            (((2u * g0 + g1) / 3u) <<  8) |
             ((2u * r0 + r1) / 3u);
        col[3] = 0xFF000000u |
            (((b0 + 2u * b1) / 3u) << 16) |
            (((g0 + 2u * g1) / 3u) <<  8) |
             ((r0 + 2u * r1) / 3u);
    } else {
        col[2] = 0xFF000000u |
            (((b0 + b1) / 2u) << 16) |
            (((g0 + g1) / 2u) <<  8) |
             ((r0 + r1) / 2u);
        col[3] = 0x00000000u; /* transparent */
    }

    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            uint32_t i = (idx >> (2 * (y * 4 + x))) & 3u;
            dst[y * dst_row_stride + x] = col[i];
        }
    }
}

/* ── DXT5 block decoder ────────────────────────────────────────
 *
 * 16 bytes per 4×4 block:
 *   - 8 bytes alpha: a0, a1 (bytes), then 48 bits of 3-bit indices.
 *   - 8 bytes colour: same as DXT1 but always 4-colour interpolation
 *     (DXT5 never has the transparent-black index 3). */

static void dxt5_decode_block(const uint8_t *src, uint32_t *dst,
                              uint32_t dst_row_stride)
{
    uint32_t a0 = src[0], a1 = src[1];

    /* Alpha indices: 6 bytes, 16 × 3 bits, little-endian packing.
     * bits 0..2 = pixel 0, bits 3..5 = pixel 1, etc. */
    uint64_t aidx = (uint64_t)src[2]        |
                    ((uint64_t)src[3] <<  8) |
                    ((uint64_t)src[4] << 16) |
                    ((uint64_t)src[5] << 24) |
                    ((uint64_t)src[6] << 32) |
                    ((uint64_t)src[7] << 40);

    uint32_t alpha[8];
    alpha[0] = a0;
    alpha[1] = a1;
    if (a0 > a1) {
        for (int i = 2; i < 8; i++)
            alpha[i] = ((8 - (uint32_t)i) * a0 + ((uint32_t)i - 1) * a1) / 7u;
    } else {
        for (int i = 2; i < 6; i++)
            alpha[i] = ((6 - (uint32_t)i) * a0 + ((uint32_t)i - 1) * a1) / 5u;
        alpha[6] = 0;
        alpha[7] = 255;
    }

    /* Colour endpoints (bytes 8..15, same as DXT1). */
    const uint8_t *cb = src + 8;
    uint32_t c0 = (uint32_t)cb[0] | ((uint32_t)cb[1] << 8);
    uint32_t c1 = (uint32_t)cb[2] | ((uint32_t)cb[3] << 8);
    uint32_t cidx = (uint32_t)cb[4] | ((uint32_t)cb[5] << 8) |
                    ((uint32_t)cb[6] << 16) | ((uint32_t)cb[7] << 24);

    uint32_t r0 = ((c0 >> 11) & 0x1Fu) * 255u / 31u;
    uint32_t g0 = ((c0 >>  5) & 0x3Fu) * 255u / 63u;
    uint32_t b0 =  (c0        & 0x1Fu) * 255u / 31u;
    uint32_t r1 = ((c1 >> 11) & 0x1Fu) * 255u / 31u;
    uint32_t g1 = ((c1 >>  5) & 0x3Fu) * 255u / 63u;
    uint32_t b1 =  (c1        & 0x1Fu) * 255u / 31u;

    uint32_t col[4];
    col[0] = (b0 << 16) | (g0 << 8) | r0;
    col[1] = (b1 << 16) | (g1 << 8) | r1;
    col[2] = (((2u * b0 + b1) / 3u) << 16) |
             (((2u * g0 + g1) / 3u) <<  8) |
              ((2u * r0 + r1) / 3u);
    col[3] = (((b0 + 2u * b1) / 3u) << 16) |
             (((g0 + 2u * g1) / 3u) <<  8) |
              ((r0 + 2u * r1) / 3u);

    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            int pi = y * 4 + x;
            uint32_t ai = (uint32_t)((aidx >> (3 * pi)) & 7u);
            uint32_t ci = (cidx >> (2 * pi)) & 3u;
            dst[y * dst_row_stride + x] =
                (alpha[ai] << 24) | col[ci];
        }
    }
}

/* ── DXT3 block decoder ────────────────────────────────────────
 *
 * 16 bytes per 4×4 block:
 *   - 8 bytes explicit alpha (4 bits per pixel, pre-multiplied nibbles).
 *   - 8 bytes colour (same as DXT1, always 4-colour interpolation). */

static void dxt3_decode_block(const uint8_t *src, uint32_t *dst,
                              uint32_t dst_row_stride)
{
    /* Explicit alpha: 4 bits per pixel × 16 pixels = 64 bits = 8 bytes.
     * src[0] = alpha of pixel 0 (low nibble) + pixel 1 (high nibble). */
    const uint8_t *ab = src;
    uint32_t alpha[16];
    for (int i = 0; i < 16; i++) {
        uint32_t nib = ab[i / 2] >> ((i & 1) ? 4 : 0);
        alpha[i] = (nib & 0xFu) * 255u / 15u;
    }

    /* Colour endpoints (bytes 8..15, same as DXT1 with 4-colour interp). */
    const uint8_t *cb = src + 8;
    uint32_t c0 = (uint32_t)cb[0] | ((uint32_t)cb[1] << 8);
    uint32_t c1 = (uint32_t)cb[2] | ((uint32_t)cb[3] << 8);
    uint32_t cidx = (uint32_t)cb[4] | ((uint32_t)cb[5] << 8) |
                    ((uint32_t)cb[6] << 16) | ((uint32_t)cb[7] << 24);

    uint32_t r0 = ((c0 >> 11) & 0x1Fu) * 255u / 31u;
    uint32_t g0 = ((c0 >>  5) & 0x3Fu) * 255u / 63u;
    uint32_t b0 =  (c0        & 0x1Fu) * 255u / 31u;
    uint32_t r1 = ((c1 >> 11) & 0x1Fu) * 255u / 31u;
    uint32_t g1 = ((c1 >>  5) & 0x3Fu) * 255u / 63u;
    uint32_t b1 =  (c1        & 0x1Fu) * 255u / 31u;

    uint32_t col[4];
    col[0] = (b0 << 16) | (g0 << 8) | r0;
    col[1] = (b1 << 16) | (g1 << 8) | r1;
    col[2] = (((2u * b0 + b1) / 3u) << 16) |
             (((2u * g0 + g1) / 3u) <<  8) |
              ((2u * r0 + r1) / 3u);
    col[3] = (((b0 + 2u * b1) / 3u) << 16) |
             (((g0 + 2u * g1) / 3u) <<  8) |
              ((r0 + 2u * r1) / 3u);

    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            int pi = y * 4 + x;
            uint32_t ci = (cidx >> (2 * pi)) & 3u;
            dst[y * dst_row_stride + x] =
                (alpha[pi] << 24) | col[ci];
        }
    }
}

/* ═══════════════════════════════════════════════════════════════
 *  Image-level decoder
 * ═══════════════════════════════════════════════════════════════ */

/* Returns the number of bytes the caller must allocate for `dst`,
 * i.e. width * height * 4. */
static inline size_t dxt_decode_dst_size(uint32_t width, uint32_t height)
{
    return (size_t)width * height * 4;
}

/* Returns the expected number of compressed source bytes, or 0 if the
 * format is unknown. */
static inline size_t dxt_decode_src_size(uint32_t four_cc,
                                         uint32_t width, uint32_t height)
{
    uint32_t bw = (width  + 3u) / 4u;
    uint32_t bh = (height + 3u) / 4u;
    uint32_t blocks = bw * bh;
    switch (four_cc) {
    case FOURCC_DXT1: return (size_t)blocks * 8;
    case FOURCC_DXT3:
    case FOURCC_DXT5: return (size_t)blocks * 16;
    default:          return 0;
    }
}

/* Decode a full DXT1/DXT3/DXT5 image to RGBA8888.
 *
 * `src` points to the first compressed block (NOT the DDS header).
 * `dst` must be at least `width * height * 4` bytes.
 * `dst_row_stride` is the number of uint32_t pixels per row (usually
 * `width`, but can be larger if row-padding is desired).
 *
 * Returns 1 on success, 0 if the format is unknown or `src_size` is
 * too small. */
static int dxt_decode_image(const uint8_t *src, size_t src_size,
                            uint32_t *dst, uint32_t width, uint32_t height,
                            uint32_t four_cc)
{
    uint32_t bw, bh, block_bytes;
    void (*decode)(const uint8_t *, uint32_t *, uint32_t);

    if (!src || !dst || width == 0 || height == 0) return 0;

    switch (four_cc) {
    case FOURCC_DXT1: block_bytes =  8; decode = dxt1_decode_block; break;
    case FOURCC_DXT3: block_bytes = 16; decode = dxt3_decode_block; break;
    case FOURCC_DXT5: block_bytes = 16; decode = dxt5_decode_block; break;
    default: return 0;
    }

    bw = (width  + 3u) / 4u;
    bh = (height + 3u) / 4u;
    if (src_size < (size_t)bw * bh * block_bytes) return 0;

    for (uint32_t by = 0; by < bh; by++) {
        for (uint32_t bx = 0; bx < bw; bx++) {
            uint32_t px = bx * 4;
            uint32_t py = by * 4;
            decode(src, dst + py * width + px, width);
            src += block_bytes;
        }
    }
    return 1;
}

#ifdef __cplusplus
}
#endif

#endif /* DXT_DECODE_H */
