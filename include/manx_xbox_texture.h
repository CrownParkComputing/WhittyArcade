/**
 * manx_xbox_texture.h — original-Xbox (NV2A) texture decoding for any port.
 *
 * Every original-Xbox title stores its textures in the same handful of
 * NV2A surface formats, in one of two memory orders: Morton/Z-order
 * ("swizzled") for the low format codes, row-major ("linear") for the
 * 0x10-0x20 aliases of the same formats. Block-compressed formats are
 * never swizzled. Which container a title uses — a RenderWare TXD, a
 * bespoke track archive, a raw XPR bundle — is title knowledge; how the
 * bytes inside decode is not, and that is what this module owns.
 *
 * A caller hands over one mip level's source bytes plus the format code
 * and the level's dimensions, and gets back bytes that upload directly
 * into a Vulkan image of manx_xbox_texture_vk_format(). Block formats
 * pass through as BC1/BC2/BC3; everything else is expanded once, at
 * load, to B8G8R8A8_UNORM. Expanding costs a few milliseconds per
 * dictionary and avoids depending on the 16-bit packed Vulkan formats,
 * which are optional features with channel orders that do not all match
 * the NV2A packing anyway.
 *
 * The module needs <vulkan/vulkan.h> for VkFormat but never a device,
 * an instance or an allocator: decoding writes into caller-owned memory
 * (typically a mapped staging buffer or a D3D8-shim LockRect pointer)
 * and allocates nothing itself.
 *
 * See docs/xbox_textures.md for the format table, the swizzle rule and
 * the container quirks the option flags exist for.
 */

#ifndef MANX_XBOX_TEXTURE_H
#define MANX_XBOX_TEXTURE_H

#include <stddef.h>
#include <stdint.h>

#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

/* NV2A surface format codes, as they appear in an Xbox texture
 * descriptor's bits 8..15 and in the D3DFORMAT field of most asset
 * containers. Names follow nv2a-trace; the XDK's own D3DFMT_ spelling
 * is given where it differs. Codes 0x10 and up are the linear
 * (row-major) aliases of the swizzled codes below them. */
enum {
    MANX_XBOX_FMT_Y8           = 0x00, /* XDK D3DFMT_L8  */
    MANX_XBOX_FMT_AY8          = 0x01, /* XDK D3DFMT_AL8 */
    MANX_XBOX_FMT_A1R5G5B5     = 0x02,
    MANX_XBOX_FMT_X1R5G5B5     = 0x03,
    MANX_XBOX_FMT_A4R4G4B4     = 0x04,
    MANX_XBOX_FMT_R5G6B5       = 0x05,
    MANX_XBOX_FMT_A8R8G8B8     = 0x06,
    MANX_XBOX_FMT_X8R8G8B8     = 0x07,
    MANX_XBOX_FMT_P8           = 0x0B,
    MANX_XBOX_FMT_DXT1         = 0x0C,
    MANX_XBOX_FMT_DXT3         = 0x0E,
    MANX_XBOX_FMT_DXT5         = 0x0F,
    MANX_XBOX_FMT_LIN_A1R5G5B5 = 0x10,
    MANX_XBOX_FMT_LIN_R5G6B5   = 0x11,
    MANX_XBOX_FMT_LIN_A8R8G8B8 = 0x12,
    MANX_XBOX_FMT_LIN_Y8       = 0x13,
    MANX_XBOX_FMT_A8           = 0x19,
    MANX_XBOX_FMT_A8Y8         = 0x1A, /* XDK D3DFMT_A8L8 */
    MANX_XBOX_FMT_LIN_AY8      = 0x1B,
    MANX_XBOX_FMT_LIN_X1R5G5B5 = 0x1C,
    MANX_XBOX_FMT_LIN_A4R4G4B4 = 0x1D,
    MANX_XBOX_FMT_LIN_X8R8G8B8 = 0x1E,
    MANX_XBOX_FMT_LIN_A8       = 0x1F,
    MANX_XBOX_FMT_LIN_A8Y8     = 0x20,
};

/* The two halves of a 16-byte DXT3/DXT5 block are stored colour-first,
 * the opposite way round from BC2/BC3. Some containers write them that
 * way; without the swap every surface using one renders as multicoloured
 * block noise. Ignored for DXT1 and for uncompressed formats. */
#define MANX_XBOX_TEXTURE_DXT_COLOUR_FIRST 0x0001u

/* Treat a swizzled format code's bytes as already row-major. For assets
 * a title's own tooling deswizzled ahead of time, or for a container
 * that reuses the swizzled codes to mean "whatever the CPU wrote". */
#define MANX_XBOX_TEXTURE_ASSUME_LINEAR    0x0002u

typedef struct manx_xbox_texture_info {
    VkFormat    format;            /* what a decoded level is in */
    const char *name;              /* "DXT5", "A8R8G8B8", ... for logs */
    int         block_compressed;  /* 4x4 block layout, never swizzled */
    int         swizzled;          /* source is Morton ordered */
    int         paletted;          /* needs a palette to decode */
    uint32_t    block_bytes;       /* bytes per 4x4 block; 0 if not BC */
    uint32_t    source_bits_per_pixel; /* per source pixel; 0 if BC */
} manx_xbox_texture_info;

/* One mip level's source bytes and how to read them. `width`/`height`
 * are THIS level's dimensions, not level 0's. */
typedef struct manx_xbox_texture_source {
    uint32_t    format;        /* MANX_XBOX_FMT_* */
    uint32_t    width;
    uint32_t    height;
    unsigned    options;       /* MANX_XBOX_TEXTURE_* */
    const void *pixels;
    size_t      pixels_size;
    /* Paletted formats only: up to 256 four-byte B,G,R,A entries.
     * A short or absent palette leaves the rest transparent black. */
    const void *palette;
    size_t      palette_size;
} manx_xbox_texture_source;

/* Fill *out for a format code. Returns 1 when the code is one this
 * module decodes, 0 otherwise (out is zeroed). */
int manx_xbox_texture_describe(uint32_t xbox_format,
                                 manx_xbox_texture_info *out);

/* VK_FORMAT_UNDEFINED for a code this module does not decode. */
VkFormat manx_xbox_texture_vk_format(uint32_t xbox_format);

int manx_xbox_texture_is_block_compressed(uint32_t xbox_format);
int manx_xbox_texture_is_swizzled(uint32_t xbox_format);

/* Dimension of mip level `level`, floored at 1 the way the NV2A does. */
uint32_t manx_xbox_texture_level_dim(uint32_t base_dim, uint32_t level);

/* Levels a full chain would have for these dimensions, capped at 16. */
uint32_t manx_xbox_texture_max_levels(uint32_t width, uint32_t height);

/* Mip level count from an Xbox texture descriptor dword (bits 16..19),
 * clamped to at least one and to what the dimensions can support.
 *
 * Take the count from the descriptor rather than assuming a full chain:
 * containers do ship single-level textures next to full chains, and
 * walking a chain that is not there reads the following texture's bytes
 * as the small mips — garbage at exactly the distances mips are sampled. */
uint32_t manx_xbox_texture_descriptor_levels(uint32_t descriptor,
                                               uint32_t width, uint32_t height);

/* Bytes one level occupies in the file. Paletted formats count the
 * index plane only; the palette is passed separately. Returns 0 for an
 * unsupported format. */
size_t manx_xbox_texture_source_bytes(uint32_t xbox_format,
                                        uint32_t width, uint32_t height);

/* Bytes one decoded level occupies, i.e. the minimum dst_size for
 * manx_xbox_texture_decode(). */
size_t manx_xbox_texture_upload_bytes(uint32_t xbox_format,
                                        uint32_t width, uint32_t height);

/* Source bytes for `levels` consecutive levels stored back to back,
 * starting at level 0 of width x height. */
size_t manx_xbox_texture_source_chain_bytes(uint32_t xbox_format,
                                              uint32_t width, uint32_t height,
                                              uint32_t levels);

/* Same, for the decoded side. */
size_t manx_xbox_texture_upload_chain_bytes(uint32_t xbox_format,
                                              uint32_t width, uint32_t height,
                                              uint32_t levels);

/* Decode one level into dst, which must hold at least
 * manx_xbox_texture_upload_bytes() bytes. Returns 1 on success, 0 on
 * an unsupported format or a source/destination buffer too small to
 * hold the level. Writes nothing on failure. */
int manx_xbox_texture_decode(const manx_xbox_texture_source *src,
                               void *dst, size_t dst_size);

#ifdef __cplusplus
}
#endif

#endif /* MANX_XBOX_TEXTURE_H */
