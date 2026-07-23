// system22_types.h - renderer and DSP data types for Namco System 22
#pragma once

#include "arcade_types.h"
#include "namco/system22/system22_config.h"
#include <cstdint>

// Polygon vertex - matches MAME's namcos22_polyvertex
struct poly_vertex {
    float x, y, z;      // Position
    int u, v;          // Texture coordinates (0-0xFFF)
    int bri;           // Brightness (0-0xFF)
};

// View matrix defaults to identity, which is the machine's reset view.
struct view_matrix {
    float m[4][4]{
        {1.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, 0.0f, 1.0f},
    };
};

// Polygon object data for GPU rendering
struct polygon_object {
    uint32_t zsort;
    uint32_t texturebank;
    uint32_t color;
    uint32_t cmode;
    uint32_t cz_value;
    uint32_t cz_type;
    uint32_t cz_adjust;
    uint32_t objectflags;
    uint8_t fog_r;
    uint8_t fog_g;
    uint8_t fog_b;
    uint8_t fog_factor;
    bool sprite{false};
    uint32_t sprite_tile{0};
    uint8_t sprite_alpha{0};
    float viewport_x;
    float viewport_y;
    float clip_left{0.0f};
    float clip_right{639.0f};
    float clip_top{0.0f};
    float clip_bottom{479.0f};

    poly_vertex vertices[4];
    bool direct;
};

// Helper functions
inline float dspfixed_to_nativefloat(uint32_t value) {
    const uint32_t bits = value & 0xffffu;
    const int32_t signed_value = (bits & 0x8000u) != 0 ?
        static_cast<int32_t>(bits) - 0x10000 : static_cast<int32_t>(bits);
    return static_cast<float>(signed_value) / 32767.0f;
}

inline int32_t signed24(int32_t v) {
    const uint32_t bits = static_cast<uint32_t>(v) & 0x00ffffffu;
    return (bits & 0x00800000u) != 0 ?
        static_cast<int32_t>(bits) - 0x01000000 :
        static_cast<int32_t>(bits);
}

inline int system22_direct_texture_coordinate(uint16_t value,
                                               bool super_system22) {
    return super_system22 ? value >> 4 : value & 0x0fff;
}

inline bool system22_temporal_shadow_material(const polygon_object& polygon,
                                               bool super_system22) {
    if (polygon.direct || polygon.sprite || polygon.objectflags != 0)
        return false;

    if (!super_system22) {
        if ((polygon.color & 0x7fu) != 0x42u || polygon.cmode != 0 ||
            polygon.texturebank != 0)
            return false;
        for (const poly_vertex& vertex : polygon.vertices) {
            if (vertex.u != 11 || vertex.v != 15 || vertex.bri != 0)
                return false;
        }
        return true;
    }

    // Dirt Dash's projected vehicle-shadow meshes use this measured
    // palette/mode/bank signature at unity (8.6 fixed-point) shade.
    if ((polygon.color & 0x7fu) != 0x01u || polygon.cmode != 5 ||
        polygon.texturebank != 2)
        return false;
    for (const poly_vertex& vertex : polygon.vertices) {
        if (vertex.bri != 0x40) return false;
    }
    return true;
}
