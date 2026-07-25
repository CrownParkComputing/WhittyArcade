// Sega Model 2 display-list decoder boundary.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

struct model2_geometry_vertex {
    float x{};
    float y{};
    float z{};
    float u{};
    float v{};
};

struct model2_geometry_polygon {
    std::array<model2_geometry_vertex, 4> vertices{};
    uint8_t vertex_count{};
    uint8_t luma{255};
    uint8_t center_select{};
    uint32_t attributes{};
    uint32_t texture_point_address{};
    uint32_t texture_header_address{};
    uint32_t object_address{};
    uint16_t object_instance{};
    uint16_t color_base{};
    uint16_t z_value{};
    uint16_t window{};
    int32_t texture_lod{};
    uint8_t renderer{};
    bool backface{};
    bool master_z_clipped{};
    std::array<uint16_t, 4> texture_header{};
    std::array<int16_t, 4> viewport{};
    std::array<int16_t, 2> center{};
};

// The hardware clips a triangle or quad against the four view-frustum
// planes before projection.  A clipped quad can grow to eight vertices.
struct model2_clipped_polygon {
    std::array<model2_geometry_vertex, 8> vertices{};
    uint8_t vertex_count{};
};

struct model2_geometry_summary {
    std::array<uint32_t, 32> command_counts{};
    uint32_t commands{};
    uint32_t words_consumed{};
    uint32_t direct_polygons{};
    uint32_t decoded_polygons{};
    uint32_t object_words{};
    uint32_t zero_length_objects{};
    std::array<uint32_t, 4> object_mode_counts{};
    std::array<uint32_t, 3> object_source_counts{};
    std::array<uint32_t, 4> renderer_counts{};
    std::array<uint32_t, 4> link_type_counts{};
    std::array<uint32_t, 4> z_sort_mode_counts{};
    uint32_t checker_polygons{};
    uint32_t backface_polygons{};
    uint32_t culled_polygons{};
    bool ended{};
    bool truncated{};
};

// Return the rasterizer's front-to-back fill order.  Model 2 draws newer
// windows first, walks Z buckets from near to far, and links polygons in a
// bucket newest-first.  Both the GPU and diagnostic software renderer use
// this order so their visibility decisions cannot drift apart.
std::vector<std::size_t> model2_polygon_draw_order(
    const std::vector<model2_geometry_polygon>& polygons);

model2_clipped_polygon model2_clip_polygon(
    const model2_geometry_polygon& polygon);

class model2_geometry {
public:
    void reset();
    void parse(const std::vector<uint8_t>& buffer, uint32_t start_address,
               const std::vector<uint8_t>& polygon_rom,
               const std::vector<uint8_t>& texture_rom,
               uint32_t master_z_clip = 0xff);
    const model2_geometry_summary& summary() const { return m_summary; }
    const std::vector<model2_geometry_polygon>& polygons() const {
        return m_polygons;
    }

private:
    struct texture_parameter {
        float diffuse{};
        float ambient{};
        uint8_t specular_control{};
        float specular_scale{};
    };

    model2_geometry_summary m_summary;
    uint32_t m_mode{};
    std::array<float, 12> m_matrix{};
    std::array<float, 2> m_focus{};
    std::array<float, 3> m_light{};
    float m_lod{};
    std::array<float, 32> m_distance_coefficients{};
    std::array<texture_parameter, 32> m_texture_parameters{};
    std::array<int16_t, 4> m_viewport{};
    std::array<std::array<int16_t, 2>, 4> m_centers{};
    std::array<uint32_t, 0x8000> m_polygon_ram0{};
    std::array<uint32_t, 0x8000> m_polygon_ram1{};
    std::array<uint16_t, 0x10000> m_texture_ram{};
    std::array<uint16_t, 0x8000> m_log_ram{};
    uint32_t m_z_adjust{};
    float m_polygon_z{1.0e10f};
    uint16_t m_current_window{};
    uint8_t m_center_select{};
    uint32_t m_master_z_clip{0xff};
    // Set once a parsed list produced polygons; gates the MODEL2_TRACE_GEO
    // diagnostic so it captures real display lists, not boot-time stubs.
    bool m_trace_armed{false};
    std::vector<model2_geometry_polygon> m_polygons;
};
