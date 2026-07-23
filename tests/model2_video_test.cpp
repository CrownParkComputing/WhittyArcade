#include "sega/model2/model2_video.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <vector>

int main() {
    std::vector<uint8_t> sheet0(0x100000, 0x11);
    std::vector<uint8_t> sheet1(0x100000, 0x55);
    model2_geometry_polygon polygon{};
    polygon.renderer = 2;

    // At Z=1, a zero LOD value selects the base sheet. One 7-bit LOD step
    // selects the first mipmap, which resides in the alternate sheet.
    polygon.texture_lod = 0;
    model2_texture_sample sample = model2_sample_texture(
        polygon, sheet0, sheet1, 0.0f, 0.0f, 1.0f);
    assert(sample.covered);
    assert(sample.luma_index == 8);

    polygon.texture_lod = -128;
    sample = model2_sample_texture(
        polygon, sheet0, sheet1, 0.0f, 0.0f, 1.0f);
    assert(sample.luma_index == 40);

    // The half-level blends adjacent mipmaps before the luma translator.
    polygon.texture_lod = -64;
    sample = model2_sample_texture(
        polygon, sheet0, sheet1, 0.0f, 0.0f, 1.0f);
    assert(sample.luma_index == 24);

    // A magnified material can blend the dedicated 128x128 microtexture
    // from the opposite sheet into the base level, capped just below 50%.
    std::fill(sheet1.begin(), sheet1.end(), 0x77);
    polygon.texture_header[0] = 0x1000;
    polygon.texture_lod = 256;
    sample = model2_sample_texture(
        polygon, sheet0, sheet1, 0.0f, 0.0f, 1.0f);
    assert(sample.luma_index == 31);

    // Translucent texel 15 is a coverage hole. Other texels claim the
    // one-bit fill buffer, even though their final RGB remains opaque.
    polygon.texture_header[0] = 0;
    polygon.texture_lod = 0;
    polygon.renderer = 3;
    std::fill(sheet0.begin(), sheet0.end(), 0xff);
    sample = model2_sample_texture(
        polygon, sheet0, sheet1, 0.0f, 0.0f, 1.0f);
    assert(!sample.covered);
    std::fill(sheet0.begin(), sheet0.end(), 0x22);
    sample = model2_sample_texture(
        polygon, sheet0, sheet1, 0.0f, 0.0f, 1.0f);
    assert(sample.covered);
    assert(sample.luma_index == 16);
    return 0;
}
