#include "model2_geometry.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {
void put(std::vector<uint8_t>& memory, uint32_t& cursor, uint32_t value) {
    memory[cursor++] = static_cast<uint8_t>(value);
    memory[cursor++] = static_cast<uint8_t>(value >> 8);
    memory[cursor++] = static_cast<uint8_t>(value >> 16);
    memory[cursor++] = static_cast<uint8_t>(value >> 24);
}

uint32_t floating(float value) {
    uint32_t bits{};
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}
} // namespace

int main() {
    std::vector<uint8_t> buffer(0x20000);
    std::vector<uint8_t> polygon_rom(0x100);
    std::vector<uint8_t> texture_rom(0x100);
    uint32_t cursor = 0;

    put(buffer, cursor, 0x07U << 23); // mode
    put(buffer, cursor, 1);
    put(buffer, cursor, 0x0bU << 23); // identity matrix
    for (unsigned element = 0; element < 12; ++element)
        put(buffer, cursor, floating(
            (element == 0 || element == 4 || element == 8) ? 1.0f : 0.0f));
    put(buffer, cursor, 0x09U << 23); // focus
    put(buffer, cursor, floating(1.0f));
    put(buffer, cursor, floating(1.0f));
    put(buffer, cursor, 0x08U << 23); // Z-sort reference
    put(buffer, cursor, floating(8.0f));
    put(buffer, cursor, 0x03U << 23); // viewport and four centers
    put(buffer, cursor, 0);
    put(buffer, cursor, (495U << 16) | 383U);
    for (unsigned center = 0; center < 4; ++center)
        put(buffer, cursor, (248U << 16) | 192U);
    put(buffer, cursor, 0x01U << 23); // terminator-delimited ROM-backed quad
    put(buffer, cursor, 0);
    put(buffer, cursor, 0);
    put(buffer, cursor, 0x00800000);
    put(buffer, cursor, 0); // hardware count rollover; rope ends on attr zero
    put(buffer, cursor, 0x0fU << 23);

    uint32_t object = 0;
    put(polygon_rom, object, floating(0.0f));
    put(polygon_rom, object, floating(0.0f));
    put(polygon_rom, object, floating(10.0f));
    put(polygon_rom, object, floating(1.0f));
    put(polygon_rom, object, floating(0.0f));
    put(polygon_rom, object, floating(10.0f));
    put(polygon_rom, object, 0x00020501); // quad, min-Z, double-sided
    put(polygon_rom, object, floating(0.0f));
    put(polygon_rom, object, floating(0.0f));
    put(polygon_rom, object, floating(1.0f));
    put(polygon_rom, object, floating(1.0f));
    put(polygon_rom, object, floating(1.0f));
    put(polygon_rom, object, floating(10.0f));
    put(polygon_rom, object, floating(0.0f));
    put(polygon_rom, object, floating(1.0f));
    put(polygon_rom, object, floating(10.0f));

    model2_geometry geometry;
    geometry.reset();
    geometry.parse(buffer, 0, polygon_rom, texture_rom);
    const model2_geometry_summary& summary = geometry.summary();
    assert(summary.ended);
    assert(!summary.truncated);
    assert(summary.command_counts[0x01] == 1);
    assert(summary.zero_length_objects == 1);
    assert(summary.decoded_polygons == 1);
    assert(geometry.polygons().size() == 1);
    assert(geometry.polygons()[0].vertex_count == 4);
    assert(geometry.polygons()[0].object_address == 0x00800000);
    assert(geometry.polygons()[0].object_instance == 0);
    assert(geometry.polygons()[0].center[0] == 248);
    assert(geometry.polygons()[0].center[1] == 192);
    assert(geometry.polygons()[0].z_value == 0x1400);
    assert(!geometry.polygons()[0].master_z_clipped);

    // The rasterizer's master Z register rejects geometry that is closer than
    // the selected reciprocal-Z threshold.  0xff disables this test.
    std::vector<uint8_t> near_polygon_rom = polygon_rom;
    for (const uint32_t offset : {8U, 20U, 48U, 60U}) {
        uint32_t z_cursor = offset;
        put(near_polygon_rom, z_cursor, floating(0.25f));
    }
    geometry.reset();
    geometry.parse(buffer, 0, near_polygon_rom, texture_rom, 0);
    assert(geometry.polygons().size() == 1);
    assert(geometry.polygons()[0].master_z_clipped);
    assert(model2_polygon_draw_order(geometry.polygons()).empty());

    // Direct-data polygons are generated dynamically by games for moving
    // display elements. They bypass the object ROM and already contain
    // transformed coordinates, packed luma and face state.
    std::vector<uint8_t> direct_buffer(0x20000);
    uint32_t direct = 0;
    put(direct_buffer, direct, 0x02U << 23);
    put(direct_buffer, direct, 0); // texture point address
    put(direct_buffer, direct, 0); // texture header address
    put(direct_buffer, direct, floating(0.0f));
    put(direct_buffer, direct, floating(0.0f));
    put(direct_buffer, direct, floating(10.0f));
    put(direct_buffer, direct, floating(1.0f));
    put(direct_buffer, direct, floating(0.0f));
    put(direct_buffer, direct, floating(10.0f));
    put(direct_buffer, direct, 0x00020501); // quad, min-Z, double-sided
    put(direct_buffer, direct, 0x5aU << 23); // packed luma, front face
    put(direct_buffer, direct, 0); // texture distance
    put(direct_buffer, direct, floating(1.0f));
    put(direct_buffer, direct, floating(1.0f));
    put(direct_buffer, direct, floating(10.0f));
    put(direct_buffer, direct, floating(0.0f));
    put(direct_buffer, direct, floating(1.0f));
    put(direct_buffer, direct, floating(10.0f));
    put(direct_buffer, direct, 0); // end polygon rope
    put(direct_buffer, direct, 0x0fU << 23);

    geometry.reset();
    geometry.parse(direct_buffer, 0, polygon_rom, texture_rom);
    const model2_geometry_summary& direct_summary = geometry.summary();
    assert(direct_summary.ended);
    assert(!direct_summary.truncated);
    assert(direct_summary.command_counts[0x02] == 1);
    assert(direct_summary.direct_polygons == 1);
    assert(direct_summary.decoded_polygons == 1);
    assert(geometry.polygons().size() == 1);
    assert(geometry.polygons()[0].vertex_count == 4);
    assert(geometry.polygons()[0].luma == 0x5a);
    assert(geometry.polygons()[0].object_address == 0xffffffffU);

    // Center/eye selection is latched only by object command 1.  Command 2
    // carries no center bits and must inherit the preceding object's mirror
    // selection so transient polygons land in the same view.
    std::vector<uint8_t> inherited_buffer(0x20000);
    uint32_t inherited = 0;
    put(inherited_buffer, inherited, 0x03U << 23); // viewport/four centers
    put(inherited_buffer, inherited, 0);
    put(inherited_buffer, inherited, (495U << 16) | 383U);
    put(inherited_buffer, inherited, (100U << 16) | 110U);
    put(inherited_buffer, inherited, (200U << 16) | 210U);
    put(inherited_buffer, inherited, (300U << 16) | 310U);
    put(inherited_buffer, inherited, (400U << 16) | 410U);
    put(inherited_buffer, inherited,
        (0x01U << 23) | (2U << 29)); // object selects center 2
    put(inherited_buffer, inherited, 0);
    put(inherited_buffer, inherited, 0);
    put(inherited_buffer, inherited, 0x00800000);
    put(inherited_buffer, inherited, 1);
    put(inherited_buffer, inherited, 0x02U << 23); // direct data
    put(inherited_buffer, inherited, 0);
    put(inherited_buffer, inherited, 0);
    put(inherited_buffer, inherited, floating(0.0f));
    put(inherited_buffer, inherited, floating(0.0f));
    put(inherited_buffer, inherited, floating(10.0f));
    put(inherited_buffer, inherited, floating(1.0f));
    put(inherited_buffer, inherited, floating(0.0f));
    put(inherited_buffer, inherited, floating(10.0f));
    put(inherited_buffer, inherited, 0x00020501);
    put(inherited_buffer, inherited, 0x5aU << 23);
    put(inherited_buffer, inherited, 0);
    put(inherited_buffer, inherited, floating(1.0f));
    put(inherited_buffer, inherited, floating(1.0f));
    put(inherited_buffer, inherited, floating(10.0f));
    put(inherited_buffer, inherited, floating(0.0f));
    put(inherited_buffer, inherited, floating(1.0f));
    put(inherited_buffer, inherited, floating(10.0f));
    put(inherited_buffer, inherited, 0);
    put(inherited_buffer, inherited, 0x0fU << 23);

    geometry.reset();
    geometry.parse(inherited_buffer, 0, polygon_rom, texture_rom);
    assert(geometry.polygons().size() == 2);
    const model2_geometry_polygon& inherited_direct = geometry.polygons()[1];
    assert(inherited_direct.object_address == 0xffffffffU);
    assert(inherited_direct.center_select == 2);
    assert(inherited_direct.center[0] == 300);
    assert(inherited_direct.center[1] == 310);

    // Model 2's one-bit fill buffer draws newer windows first, then walks
    // near-to-far Z buckets. Polygons linked into the same bucket are LIFO.
    // This is intentionally not camera-depth order: Sega Rally uses the
    // object Z bucket to keep opponent cars in front of the road.
    std::vector<model2_geometry_polygon> ordered_polygons(6);
    for (model2_geometry_polygon& polygon : ordered_polygons) {
        polygon.vertex_count = 3;
        polygon.renderer = 2;
        polygon.attributes = 1U << 8;
    }
    ordered_polygons[0].window = 1;
    ordered_polygons[0].z_value = 10;
    ordered_polygons[1].window = 0;
    ordered_polygons[1].z_value = 0;
    ordered_polygons[2].window = 1;
    ordered_polygons[2].z_value = 5;
    ordered_polygons[3].window = 1;
    ordered_polygons[3].z_value = 5;
    ordered_polygons[4].renderer = 1; // translucent solid: no pixels
    ordered_polygons[5].backface = true;
    const std::vector<std::size_t> order =
        model2_polygon_draw_order(ordered_polygons);
    assert((order == std::vector<std::size_t>{3, 2, 0, 1}));

    // A close polygon can straddle the camera origin.  The Model 2
    // rasterizer clips it to the four view planes; rejecting it merely
    // because one vertex is behind the eye removes nearby road and objects.
    model2_geometry_polygon crossing{};
    crossing.vertex_count = 3;
    crossing.viewport = {0, 0, 100, 100};
    crossing.center = {50, 50};
    crossing.vertices[0] = {-10.0f, -10.0f, 10.0f, 0.0f, 0.0f};
    crossing.vertices[1] = {10.0f, -10.0f, 10.0f, 8.0f, 0.0f};
    crossing.vertices[2] = {0.0f, 1.0f, -0.01f, 4.0f, 8.0f};
    const model2_clipped_polygon clipped = model2_clip_polygon(crossing);
    assert(clipped.vertex_count >= 3);
    for (unsigned index = 0; index < clipped.vertex_count; ++index) {
        const model2_geometry_vertex& vertex = clipped.vertices[index];
        assert(vertex.x + 50.0f * vertex.z >= -0.001f);
        assert(-vertex.x + 50.0f * vertex.z >= -0.001f);
        assert(-vertex.y + 50.0f * vertex.z >= -0.001f);
        assert(vertex.y + 50.0f * vertex.z >= -0.001f);
    }

    crossing.vertices[0].z = -1.0f;
    crossing.vertices[1].z = -1.0f;
    crossing.vertices[2].z = -1.0f;
    assert(model2_clip_polygon(crossing).vertex_count == 0);
    return 0;
}
