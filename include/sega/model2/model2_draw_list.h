// The Model 2 GPU draw list: the vertex records and colour table the
// renderer hands to OpenGL and Vulkan, built from a model2_gpu_frame.
//
// This lives outside arcade_renderer.cpp so the Vulkan presenter harness can
// replay a real game frame through the exact transformation the game uses -
// a harness with its own copy of this logic would only prove its copy.
#pragma once

#include "sega/model2/model2_gpu_frame.h"
#include "system22_vulkan_uniforms.h"

#include <cstddef>
#include <cstdint>
#include <vector>

struct model2_draw_vertex {
    float x, y, z;
    float u, v;
    float center_x, center_y;
    float clip_left, clip_right, clip_top, clip_bottom;
    float header0, header1, header2;
    float color_base;
    float luma;
    float renderer;
    float texture_lod;
    float draw_priority;
};

// The OpenGL and Vulkan pipelines both declare their attributes from these
// offsets, so the array uploads unmodified.
static_assert(sizeof(model2_draw_vertex) == model2_vertex_layout::stride);
static_assert(offsetof(model2_draw_vertex, x) ==
              model2_vertex_layout::position);
static_assert(offsetof(model2_draw_vertex, u) == model2_vertex_layout::uv);
static_assert(offsetof(model2_draw_vertex, center_x) ==
              model2_vertex_layout::center);
static_assert(offsetof(model2_draw_vertex, clip_left) ==
              model2_vertex_layout::clip);
static_assert(offsetof(model2_draw_vertex, header0) ==
              model2_vertex_layout::header0);
static_assert(offsetof(model2_draw_vertex, color_base) ==
              model2_vertex_layout::color_base);
static_assert(offsetof(model2_draw_vertex, luma) == model2_vertex_layout::luma);
static_assert(offsetof(model2_draw_vertex, renderer) ==
              model2_vertex_layout::renderer);
static_assert(offsetof(model2_draw_vertex, texture_lod) ==
              model2_vertex_layout::texture_lod);
static_assert(offsetof(model2_draw_vertex, draw_priority) ==
              model2_vertex_layout::draw_priority);

// Clips, orders and triangulates the frame's polygons. Draw priority encodes
// the hardware's Z-sort bucket order as a depth value in (0, 1); drawn with a
// LESS depth test it emulates the one-bit fill buffer - the first polygon to
// claim a pixel keeps it.
std::vector<model2_draw_vertex> model2_build_draw_vertices(
    const model2_gpu_frame& frame);

// The 1024x64 packed-RGB colour table combining the frame's palette with its
// colour-translation (gamma) RAM. Requires the frame's colour packet
// (palette >= 0x4000 bytes, colour translation >= 0xc000 bytes); returns
// empty otherwise.
std::vector<uint8_t> model2_build_color_table(const model2_gpu_frame& frame);
