#include "sega/model2/model2_draw_list.h"

#include "sega/model2/model2_geometry.h"

#include <algorithm>

std::vector<model2_draw_vertex> model2_build_draw_vertices(
    const model2_gpu_frame& frame) {
    std::vector<model2_draw_vertex> vertices;
    vertices.reserve(frame.polygons.size() * 18);
    const std::vector<std::size_t> polygon_order =
        model2_polygon_draw_order(frame.polygons);
    const auto append_vertex = [&vertices, &frame](
        const model2_geometry_polygon& polygon,
        const model2_geometry_vertex& source,
        float priority) {
        vertices.push_back({
            source.x, source.y, source.z, source.u, source.v,
            static_cast<float>(polygon.center[0]),
            static_cast<float>(polygon.center[1]),
            static_cast<float>(polygon.viewport[0] +
                               frame.horizontal_offset),
            static_cast<float>(polygon.viewport[2] +
                               frame.horizontal_offset),
            static_cast<float>(384 - polygon.viewport[3] +
                               frame.vertical_offset),
            static_cast<float>(384 - polygon.viewport[1] +
                               frame.vertical_offset),
            static_cast<float>(polygon.texture_header[0]),
            static_cast<float>(polygon.texture_header[1]),
            static_cast<float>(polygon.texture_header[2]),
            static_cast<float>(polygon.color_base),
            static_cast<float>(polygon.luma),
            static_cast<float>(polygon.renderer),
            static_cast<float>(polygon.texture_lod),
            priority});
    };
    const auto append_triangle = [&append_vertex](
        const model2_geometry_polygon& polygon,
        const model2_geometry_vertex& a,
        const model2_geometry_vertex& b,
        const model2_geometry_vertex& c, float priority) {
        append_vertex(polygon, a, priority);
        append_vertex(polygon, b, priority);
        append_vertex(polygon, c, priority);
    };
    for (std::size_t rank = 0; rank < polygon_order.size(); ++rank) {
        const model2_geometry_polygon& polygon =
            frame.polygons[polygon_order[rank]];
        const model2_clipped_polygon clipped =
            model2_clip_polygon(polygon);
        if (clipped.vertex_count < 3) continue;
        const float priority = static_cast<float>(rank + 1) /
            static_cast<float>(polygon_order.size() + 1);
        for (unsigned vertex = 1; vertex + 1 < clipped.vertex_count;
             ++vertex) {
            append_triangle(polygon, clipped.vertices[0],
                            clipped.vertices[vertex],
                            clipped.vertices[vertex + 1], priority);
        }
    }
    return vertices;
}

std::vector<uint8_t> model2_build_color_table(const model2_gpu_frame& frame) {
    if (frame.palette.size() < 0x4000 ||
        frame.color_translation.size() < 0xc000)
        return {};
    const auto word_at = [](const std::vector<uint8_t>& memory,
                            std::size_t index) {
        const std::size_t offset = index * 2;
        return static_cast<uint16_t>(memory[offset]) |
               (static_cast<uint16_t>(memory[offset + 1]) << 8);
    };
    const auto gamma = [](uint8_t value) {
        if (value <= 64) return static_cast<uint8_t>(0);
        return static_cast<uint8_t>(std::min(
            255, (static_cast<int>(value) - 64) * 255 / 191));
    };
    std::vector<uint8_t> colors(std::size_t{1024} * 64 * 3);
    for (unsigned luma = 0; luma < 64; ++luma) {
        for (unsigned base = 0; base < 1024; ++base) {
            const uint16_t color = word_at(frame.palette, 0x1000 + base);
            const unsigned red_index = luma + ((color & 0x1f) << 8);
            const unsigned green_index =
                0x2000 + luma + (((color >> 5) & 0x1f) << 8);
            const unsigned blue_index =
                0x4000 + luma + (((color >> 10) & 0x1f) << 8);
            const std::size_t output =
                (static_cast<std::size_t>(luma) * 1024 + base) * 3;
            colors[output] = gamma(static_cast<uint8_t>(
                word_at(frame.color_translation, red_index)));
            colors[output + 1] = gamma(static_cast<uint8_t>(
                word_at(frame.color_translation, green_index)));
            colors[output + 2] = gamma(static_cast<uint8_t>(
                word_at(frame.color_translation, blue_index)));
        }
    }
    return colors;
}
