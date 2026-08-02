# Original-Xbox textures (MANX)
`include/whitty_xbox_texture.h` + `src/xbox_texture/whitty_xbox_texture.c`
decode original-Xbox (NV2A) texture data into bytes that upload straight
into a Vulkan image. It is a framework module: every original-Xbox title
stores its art in the same handful of NV2A surface formats, so a port
only has to know which container holds them, not how the bytes inside
are arranged.

The module needs `<vulkan/vulkan.h>` for `VkFormat` and nothing else —
no device, no instance, no allocator. Decoding writes into caller-owned
memory and allocates nothing.

## Format codes

The code lives in bits 8..15 of an Xbox texture descriptor, and most
containers also write it into an explicit `D3DFORMAT` field. Names below
follow nv2a-trace; the XDK's own spelling is given where it differs.

| Code | Format | Source bits/pixel | Swizzled | Decodes to |
|------|--------|-------------------|----------|------------|
| 0x00 | Y8 (XDK `L8`) | 8 | yes | `B8G8R8A8_UNORM` |
| 0x01 | AY8 (XDK `AL8`) | 8 | yes | `B8G8R8A8_UNORM` |
| 0x02 | A1R5G5B5 | 16 | yes | `B8G8R8A8_UNORM` |
| 0x03 | X1R5G5B5 | 16 | yes | `B8G8R8A8_UNORM` |
| 0x04 | A4R4G4B4 | 16 | yes | `B8G8R8A8_UNORM` |
| 0x05 | R5G6B5 | 16 | yes | `B8G8R8A8_UNORM` |
| 0x06 | A8R8G8B8 | 32 | yes | `B8G8R8A8_UNORM` |
| 0x07 | X8R8G8B8 | 32 | yes | `B8G8R8A8_UNORM` |
| 0x0B | P8 (paletted) | 8 + palette | yes | `B8G8R8A8_UNORM` |
| 0x0C | DXT1 | 8 bytes / 4x4 block | never | `BC1_RGBA_UNORM_BLOCK` |
| 0x0E | DXT3 | 16 bytes / 4x4 block | never | `BC2_UNORM_BLOCK` |
| 0x0F | DXT5 | 16 bytes / 4x4 block | never | `BC3_UNORM_BLOCK` |
| 0x10 | LIN_A1R5G5B5 | 16 | no | `B8G8R8A8_UNORM` |
| 0x11 | LIN_R5G6B5 | 16 | no | `B8G8R8A8_UNORM` |
| 0x12 | LIN_A8R8G8B8 | 32 | no | `B8G8R8A8_UNORM` |
| 0x13 | LIN_Y8 | 8 | no | `B8G8R8A8_UNORM` |
| 0x19 | A8 | 8 | yes | `B8G8R8A8_UNORM` |
| 0x1A | A8Y8 (XDK `A8L8`) | 16 | yes | `B8G8R8A8_UNORM` |
| 0x1B | LIN_AY8 | 8 | no | `B8G8R8A8_UNORM` |
| 0x1C | LIN_X1R5G5B5 | 16 | no | `B8G8R8A8_UNORM` |
| 0x1D | LIN_A4R4G4B4 | 16 | no | `B8G8R8A8_UNORM` |
| 0x1E | LIN_X8R8G8B8 | 32 | no | `B8G8R8A8_UNORM` |
| 0x1F | LIN_A8 | 8 | no | `B8G8R8A8_UNORM` |
| 0x20 | LIN_A8Y8 | 16 | no | `B8G8R8A8_UNORM` |

Codes 0x10 and up are the linear (row-major) aliases of the swizzled
codes below them: same pixels, same packing, different memory order.
Anything not in the table — the YUV, bump-map and depth/stencil codes —
is rejected by `whitty_xbox_texture_describe()`.

Everything that is not block-compressed decodes to `B8G8R8A8_UNORM`
rather than a packed 16-bit Vulkan format. `VK_FORMAT_A4R4G4B4_*` needs
Vulkan 1.3 or `VK_EXT_4444_formats`, the 5-bit formats are optional
features, and the channel orders do not all match the NV2A packing
anyway. Expanding once at load costs a few milliseconds per dictionary
and removes a whole class of device-dependent failure. It also happens
to be free for the commonest case: an NV2A `A8R8G8B8` texel is already
B, G, R, A in memory.

Channel conventions worth pinning down:

- `A8` samples RGB as zero and carries data only in alpha, per D3D.
  A title that wants white-on-alpha uses `AY8`, where the one byte
  drives luminance and alpha together.
- `A8Y8` is little-endian: byte 0 luminance, byte 1 alpha.
- `X`-channel formats (`X1R5G5B5`, `X8R8G8B8`) come out fully opaque.
- Palette entries are four bytes B, G, R, A — the same order as a
  little-endian `D3DCOLOR`. A short palette leaves the remaining
  indices transparent black rather than failing the texture.

## Swizzling

The low format codes are stored in Morton (Z-order) curve order, which
gives the NV2A 2D locality in its texture cache. The offset of a texel
is built by interleaving the bits of x and y — but only as far as the
**smaller** dimension goes. The larger dimension's remaining high bits
follow sequentially above the interleaved field:

```
256 x 64:  xbits = 8, ybits = 6
offset = [x7 x6] [y5 x5 y4 x4 y3 x3 y2 x2 y1 x1 y0 x0]
          ^ tail  ^ interleaved to min(xbits, ybits)
```

A plain square Morton decode therefore scrambles non-square art into
stripes. `whitty_xbox_texture_decode()` implements the rule as a gather
— it computes a source offset per destination pixel and converts in the
same pass — so no intermediate deswizzle buffer is ever allocated.

Two rules bound it:

- **Block-compressed formats are never swizzled.** The 4x4 block layout
  already provides the locality, and the codes have no linear alias.
- **Swizzling requires power-of-two dimensions.** The NV2A cannot
  address a non-power-of-two surface in Z order, so such a level is
  row-major whatever its format code says. The module detects this and
  falls back rather than producing garbage.

`WHITTY_XBOX_TEXTURE_ASSUME_LINEAR` overrides the format's swizzle bit,
for assets a title's own tooling deswizzled ahead of time.

## Mip level counts

**Take the level count from the texture descriptor, bits 16..19 of the
dword at descriptor offset 0x0C. Do not assume a full chain.**

`whitty_xbox_texture_descriptor_levels()` reads it and clamps to at
least one level and to what the dimensions support.

Containers do ship single-level textures next to full chains — 8 of the
197 entries in Burnout's `US/C1_V1` `static.dat` have one level while
the rest have up to ten. Walking a chain that is not there reads the
*following* texture's bytes as the small mips, which is garbage at
exactly the distances where mips get sampled. Skipping mips entirely is
no better: minified track surfaces alias into multicoloured speckle,
because every screen pixel samples one arbitrary texel of a detailed
256x256 surface.

## Container quirk: colour-first DXT blocks

A DXT3/DXT5 block is sixteen bytes: eight of alpha then eight of colour.
BC2/BC3 expect that order. Some containers write the two halves the
other way round, and without a swap every surface using one renders as
multicoloured block noise. `WHITTY_XBOX_TEXTURE_DXT_COLOUR_FIRST` swaps
the halves per block, per level.

This is a property of the container, not of the console or the title —
Burnout 3 needs it for its track archives and must *not* have it for its
menu dictionaries. Measured by decoding both layouts offline and scoring
mean neighbour continuity (lower is smoother, so more plausible as
photographic art) over the first 40 DXT5 textures of each file:

| Source | as stored | halves swapped | verdict |
|--------|-----------|----------------|---------|
| `Tracks/US/C1_V1/static.dat` | 48.0 | **16.3** | colour-first |
| `Data/Global.txd` | **7.8** | 41.1 | ordinary BC3 |
| `Data/Frontend.txd` | **9.3** | 49.7 | ordinary BC3 |

Individual textures are just as clear-cut: `WF_hoteldoor` scores 55.9
as stored against 12.0 swapped, while `Car_Medalgold` scores 2.1 as
stored against 62.7 swapped. Decide this per container, with a
measurement, before wiring the flag in.

## Using it from another title

Link the target and include the header:

```cmake
target_link_libraries(my_title PRIVATE whitty_xbox_texture)
```

The port keeps the container walk — finding entries, names, dimensions,
format codes and where the pixel data starts — and hands each mip level
to the module:

```c
whitty_xbox_texture_info info;
if (!whitty_xbox_texture_describe(fmt, &info))
    continue;                       /* not a format we decode */

uint32_t levels = whitty_xbox_texture_descriptor_levels(descriptor, w, h);

for (uint32_t l = 0; l < levels; l++) {
    whitty_xbox_texture_source lvl = {0};
    lvl.format      = fmt;
    lvl.width       = whitty_xbox_texture_level_dim(w, l);
    lvl.height      = whitty_xbox_texture_level_dim(h, l);
    lvl.options     = 0;            /* or DXT_COLOUR_FIRST, ASSUME_LINEAR */
    lvl.pixels      = cursor;
    lvl.pixels_size = whitty_xbox_texture_source_bytes(fmt, lvl.width, lvl.height);
    /* paletted formats also set lvl.palette / lvl.palette_size */

    size_t upload = whitty_xbox_texture_upload_bytes(fmt, lvl.width, lvl.height);
    whitty_xbox_texture_decode(&lvl, staging_for_level(l), upload);

    cursor += lvl.pixels_size;
}
```

Create the image with `whitty_xbox_texture_vk_format(fmt)` and
`whitty_xbox_texture_upload_chain_bytes()` worth of staging.
`whitty_xbox_texture_source_chain_bytes()` sizes the read side, which is
also the bounds check to run before trusting a descriptor's level count.

Checklist for a new title:

1. Dump the format codes its containers actually use. Most use three or
   four; the table above is much wider than any one title needs.
2. For each container holding DXT3/DXT5, score both block orders before
   assuming either. See the table above for what a decisive result looks
   like.
3. Confirm the level count field really is at descriptor + 0x0C bits
   16..19 for that container, then use it.
4. If a swizzled code decodes to stripes at the right colours, check for
   a non-square texture first — that is the interleave rule, not a
   broken palette.

## Consumers

- `Burnout3Recomp/src/game/static_textures.c` — track `static.dat`
  archives. DXT1 and DXT5 only (9240 entries over the 37 shipped
  tracks, no third format), full mip chains from the descriptor, and
  `DXT_COLOUR_FIRST`.
- `Burnout3Recomp/src/game/txd_loader.c` — Criterion `.txd` menu
  dictionaries. P8, DXT1 and DXT5; level 0 only, since menu art is drawn
  at its authored size; no `DXT_COLOUR_FIRST`. The P8 entries *are*
  Morton swizzled, and their palettes sit after the index plane, usually
  behind 64 bytes of padding.
