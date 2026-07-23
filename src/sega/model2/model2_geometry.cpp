#include "sega/model2/model2_geometry.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace {
constexpr uint32_t buffer_mask = 0x1ffff;

uint32_t read_word(const std::vector<uint8_t>& buffer, uint32_t address) {
    if (buffer.empty()) return 0;
    address &= buffer_mask;
    if (address + 3 >= buffer.size()) return 0;
    return static_cast<uint32_t>(buffer[address]) |
           (static_cast<uint32_t>(buffer[address + 1]) << 8) |
           (static_cast<uint32_t>(buffer[address + 2]) << 16) |
           (static_cast<uint32_t>(buffer[address + 3]) << 24);
}

uint32_t read_linear_word(const std::vector<uint8_t>& memory,
                          uint32_t address) {
    if (address + 3 >= memory.size()) return 0;
    return static_cast<uint32_t>(memory[address]) |
           (static_cast<uint32_t>(memory[address + 1]) << 8) |
           (static_cast<uint32_t>(memory[address + 2]) << 16) |
           (static_cast<uint32_t>(memory[address + 3]) << 24);
}

float bits_to_float(uint32_t bits) {
    float value{};
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

uint32_t float_to_bits(float value) {
    uint32_t bits{};
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

uint16_t model2_z_value(float value, uint32_t adjustment) {
    const uint32_t bits = float_to_bits(value);
    int exponent = static_cast<int>((bits >> 23) & 0xff) -
                   static_cast<int>((adjustment >> 23) & 0xff);
    uint32_t mantissa = bits & 0x7fffff;
    mantissa += 0x400;
    if (mantissa > 0x7fffff) {
        ++exponent;
        mantissa = (mantissa & 0x7fffff) >> 1;
    }
    mantissa >>= 11;
    if (bits & 0x80000000U) return 0;
    if (exponent < -12) return 0;
    if (exponent < 0)
        return static_cast<uint16_t>((mantissa | 0x1000) >> -exponent);
    if (exponent < 15)
        return static_cast<uint16_t>(((exponent + 1) << 12) | mantissa);
    return 0xffff;
}

int16_t signed_12(uint32_t value) {
    value &= 0xfff;
    return static_cast<int16_t>((value & 0x800) ? value - 0x1000 : value);
}
} // namespace

model2_clipped_polygon model2_clip_polygon(
        const model2_geometry_polygon& polygon) {
    model2_clipped_polygon input{};
    if (polygon.vertex_count < 3 ||
        polygon.vertex_count > polygon.vertices.size())
        return input;

    float maximum_z = -std::numeric_limits<float>::infinity();
    for (unsigned index = 0; index < polygon.vertex_count; ++index) {
        input.vertices[index] = polygon.vertices[index];
        if (std::isfinite(polygon.vertices[index].z))
            maximum_z = std::max(maximum_z, polygon.vertices[index].z);
    }
    // Model 2 rejects only polygons wholly behind the eye.  Polygons which
    // cross it must reach the frustum clipper; dropping one because a single
    // vertex is behind removes the road, cars and scenery at close range.
    if (maximum_z < 0.0f) return {};
    input.vertex_count = polygon.vertex_count;

    struct clip_plane {
        float x;
        float y;
        float z;
    };
    const float left = static_cast<float>(
        polygon.center[0] - polygon.viewport[0]);
    const float right = static_cast<float>(
        polygon.viewport[2] - polygon.center[0]);
    const float top = static_cast<float>(
        polygon.viewport[3] - polygon.center[1]);
    const float bottom = static_cast<float>(
        polygon.center[1] - polygon.viewport[1]);
    const std::array<clip_plane, 4> planes{{
        {1.0f, 0.0f, left},
        {-1.0f, 0.0f, right},
        {0.0f, -1.0f, top},
        {0.0f, 1.0f, bottom},
    }};

    const auto dot = [](const model2_geometry_vertex& vertex,
                        const clip_plane& plane) {
        return vertex.x * plane.x + vertex.y * plane.y +
               vertex.z * plane.z;
    };
    const auto interpolate = [](const model2_geometry_vertex& first,
                                const model2_geometry_vertex& second,
                                float amount) {
        return model2_geometry_vertex{
            first.x + (second.x - first.x) * amount,
            first.y + (second.y - first.y) * amount,
            first.z + (second.z - first.z) * amount,
            first.u + (second.u - first.u) * amount,
            first.v + (second.v - first.v) * amount};
    };

    for (const clip_plane& plane : planes) {
        if (input.vertex_count < 3) return {};
        model2_clipped_polygon output{};
        for (unsigned index = 0; index < input.vertex_count; ++index) {
            const model2_geometry_vertex& current = input.vertices[index];
            const model2_geometry_vertex& next =
                input.vertices[(index + 1) % input.vertex_count];
            const float current_dot = dot(current, plane);
            const float next_dot = dot(next, plane);
            const bool current_inside = current_dot >= 0.0f;
            const bool next_inside = next_dot >= 0.0f;
            if (current_inside &&
                output.vertex_count < output.vertices.size())
                output.vertices[output.vertex_count++] = current;
            if (current_inside != next_inside &&
                std::isfinite(current_dot) && std::isfinite(next_dot) &&
                output.vertex_count < output.vertices.size()) {
                const float amount = -current_dot /
                    (next_dot - current_dot);
                output.vertices[output.vertex_count++] =
                    interpolate(current, next, amount);
            }
        }
        input = output;
    }
    return input.vertex_count >= 3 ? input : model2_clipped_polygon{};
}

std::vector<std::size_t> model2_polygon_draw_order(
        const std::vector<model2_geometry_polygon>& polygons) {
    std::vector<std::size_t> order;
    order.reserve(polygons.size());
    for (std::size_t index = 0; index < polygons.size(); ++index) {
        const model2_geometry_polygon& polygon = polygons[index];
        if (polygon.vertex_count < 3 || polygon.renderer == 1 ||
            polygon.master_z_clipped ||
            ((polygon.attributes >> 8) & 3) == 0 ||
            (((polygon.attributes >> 17) & 1) == 0 && polygon.backface))
            continue;
        order.push_back(index);
    }
    std::sort(order.begin(), order.end(),
              [&polygons](std::size_t first, std::size_t second) {
        const model2_geometry_polygon& a = polygons[first];
        const model2_geometry_polygon& b = polygons[second];
        if (a.window != b.window) return a.window > b.window;
        if (a.z_value != b.z_value) return a.z_value < b.z_value;
        return first > second;
    });
    return order;
}

void model2_geometry::reset() {
    m_summary = {};
    m_mode = 0;
    m_matrix.fill(0.0f);
    m_matrix[0] = m_matrix[4] = m_matrix[8] = 1.0f;
    m_focus = {1.0f, 1.0f};
    m_light = {};
    m_lod = 0.0f;
    m_distance_coefficients.fill(0.0f);
    m_texture_parameters = {};
    m_viewport = {0, 0, 495, 383};
    m_centers = {};
    m_polygon_ram0.fill(0);
    m_polygon_ram1.fill(0);
    m_texture_ram.fill(0);
    m_log_ram.fill(0);
    m_z_adjust = 0;
    m_polygon_z = 1.0e10f;
    m_current_window = 0;
    m_center_select = 0;
    m_master_z_clip = 0xff;
    m_polygons.clear();
}

void model2_geometry::parse(const std::vector<uint8_t>& buffer,
                            uint32_t start_address,
                            const std::vector<uint8_t>& polygon_rom,
                            const std::vector<uint8_t>& texture_rom,
                            uint32_t master_z_clip) {
    m_summary = {};
    m_polygons.clear();
    m_polygon_z = 1.0e10f;
    m_current_window = 0;
    m_master_z_clip = master_z_clip;
    uint32_t cursor = start_address & buffer_mask;
    uint16_t object_instance = 0;
    constexpr uint32_t maximum_commands = 0x8000;
    constexpr uint32_t maximum_words = 0x20000 / 4;

    const auto take = [&](uint32_t& value) {
        if (m_summary.words_consumed >= maximum_words) {
            m_summary.truncated = true;
            return false;
        }
        value = read_word(buffer, cursor);
        cursor = (cursor + 4) & buffer_mask;
        ++m_summary.words_consumed;
        return true;
    };
    const auto skip = [&](uint64_t count) {
        if (count > maximum_words - m_summary.words_consumed) {
            m_summary.truncated = true;
            return false;
        }
        cursor = (cursor + static_cast<uint32_t>(count * 4)) & buffer_mask;
        m_summary.words_consumed += static_cast<uint32_t>(count);
        return true;
    };
    const auto transform = [this](model2_geometry_vertex point) {
        const float x = point.x * m_matrix[0] +
                        point.y * m_matrix[3] +
                        point.z * m_matrix[6] + m_matrix[9];
        const float y = point.x * m_matrix[1] +
                        point.y * m_matrix[4] +
                        point.z * m_matrix[7] + m_matrix[10];
        const float z = point.x * m_matrix[2] +
                        point.y * m_matrix[5] +
                        point.z * m_matrix[8] + m_matrix[11];
        return model2_geometry_vertex{x * m_focus[0], y * m_focus[1], z};
    };
    const auto transform_vector = [this](model2_geometry_vertex vector) {
        return model2_geometry_vertex{
            vector.x * m_matrix[0] + vector.y * m_matrix[3] +
                vector.z * m_matrix[6],
            vector.x * m_matrix[1] + vector.y * m_matrix[4] +
                vector.z * m_matrix[7],
            vector.x * m_matrix[2] + vector.y * m_matrix[5] +
                vector.z * m_matrix[8]};
    };
    const auto polygon_rom_word = [&polygon_rom](uint32_t index) {
        if (polygon_rom.size() < 4) return 0U;
        const uint32_t words = static_cast<uint32_t>(polygon_rom.size() / 4);
        return read_linear_word(polygon_rom, (index % words) * 4);
    };
    const auto texture_word = [this, &texture_rom](uint32_t address) {
        if (address & 0x00800000U)
            return m_texture_ram[address & 0xffff];
        if (texture_rom.size() < 2) return static_cast<uint16_t>(0);
        const uint32_t words = static_cast<uint32_t>(texture_rom.size() / 2);
        const uint32_t offset = (address % words) * 2;
        return static_cast<uint16_t>(
            static_cast<uint16_t>(texture_rom[offset]) |
            (static_cast<uint16_t>(texture_rom[offset + 1]) << 8));
    };

    while (!m_summary.ended && !m_summary.truncated &&
           m_summary.commands < maximum_commands) {
        uint32_t opcode{};
        if (!take(opcode)) break;
        ++m_summary.commands;

        if (opcode & 0x80000000U) {
            cursor = opcode & buffer_mask;
            continue;
        }

        const unsigned command = (opcode >> 23) & 0x1f;
        ++m_summary.command_counts[command];
        uint32_t count{};
        uint32_t ignored{};
        switch (command) {
        case 0x00:
            break;
        case 0x01:
        case 0x11: {
            uint32_t texture_point{};
            uint32_t texture_header{};
            uint32_t object_address{};
            if (!take(texture_point) || !take(texture_header) ||
                !take(object_address) || !take(count))
                break;
            ++m_summary.object_mode_counts[m_mode & 3];
            const unsigned source = (object_address & 0x01000000U) ? 2 :
                                    (object_address & 0x00800000U) ? 1 : 0;
            ++m_summary.object_source_counts[source];
            const uint16_t current_object_instance = object_instance++;
            // Only an object command changes the rasterizer's eye/center
            // selection. Direct-data commands inherit it; interpreting their
            // otherwise-unused high opcode bits made transient polygons use
            // the main camera instead of the mirror camera.
            m_center_select = static_cast<uint8_t>((opcode >> 29) & 3);
            if (count == 0)
                ++m_summary.zero_length_objects;
            else
                m_summary.object_words += count;

            // All four object formats reserve the normal words. Modes 0/1
            // consume the supplied normal; modes 2/3 derive it from points.
            // Odd modes additionally enable the material's specular term.
            const uint32_t polygon_words = static_cast<uint32_t>(
                std::max<std::size_t>(1, polygon_rom.size() / 4));
            uint32_t object_cursor = (object_address & 0x00800000U) ?
                (object_address % polygon_words) : (object_address & 0x7fff);
            const auto object_word = [&]() {
                uint32_t value{};
                if (object_address & 0x01000000U)
                    value = m_polygon_ram1[object_cursor & 0x7fff];
                else if (object_address & 0x00800000U)
                    value = polygon_rom_word(object_cursor);
                else
                    value = m_polygon_ram0[object_cursor & 0x7fff];
                ++object_cursor;
                return value;
            };
            const auto object_point = [&]() {
                model2_geometry_vertex point{
                    bits_to_float(object_word()),
                    bits_to_float(object_word()),
                    bits_to_float(object_word())};
                return transform(point);
            };

            model2_geometry_vertex previous0 = object_point();
            model2_geometry_vertex previous1 = object_point();
            // A zero object count wraps on the hardware; the rope's link
            // terminator ends it. Virtual On and Gunblade rely on this, and
            // dynamic Model 2 objects are allowed to use the same encoding.
            // Keep a defensive ceiling for malformed display lists.
            const uint32_t links = count == 0 ? 0x1000 :
                std::min<uint32_t>(count, 0x1000);
            for (uint32_t link = 0; link < links; ++link) {
                const uint32_t attributes = object_word();
                const model2_geometry_vertex supplied_normal = transform_vector({
                    bits_to_float(object_word()),
                    bits_to_float(object_word()),
                    bits_to_float(object_word())});
                if ((attributes & 3) == 0) break;

                const model2_geometry_vertex next0 = object_point();
                // The unused fourth point is still present for triangles.
                const model2_geometry_vertex fourth = object_point();
                const model2_geometry_vertex next1 =
                    (attributes & 1) ? fourth : next0;
                if (m_polygons.size() < 32768) {
                    const auto unfocused = [this](model2_geometry_vertex point) {
                        if (m_focus[0] != 0.0f) point.x /= m_focus[0];
                        if (m_focus[1] != 0.0f) point.y /= m_focus[1];
                        return point;
                    };
                    model2_geometry_vertex normal = supplied_normal;
                    if ((m_mode & 2) != 0) {
                        const model2_geometry_vertex p0 = unfocused(previous0);
                        const model2_geometry_vertex p1 = unfocused(previous1);
                        const model2_geometry_vertex p2 = unfocused(next0);
                        const model2_geometry_vertex a{
                            p1.x - p0.x, p1.y - p0.y, p1.z - p0.z};
                        const model2_geometry_vertex b{
                            p2.x - p0.x, p2.y - p0.y, p2.z - p0.z};
                        normal = {a.y * b.z - a.z * b.y,
                                  a.z * b.x - a.x * b.z,
                                  a.x * b.y - a.y * b.x};
                        const float length = std::sqrt(
                            normal.x * normal.x + normal.y * normal.y +
                            normal.z * normal.z);
                        if (length > 0.0f) {
                            normal.x /= length;
                            normal.y /= length;
                            normal.z /= length;
                        }
                    }
                    model2_geometry_polygon polygon{};
                    polygon.vertex_count = (attributes & 1) ? 4 : 3;
                    polygon.vertices[0] = previous1;
                    polygon.vertices[1] = previous0;
                    polygon.vertices[2] = next0;
                    polygon.vertices[3] = next1;
                    polygon.attributes = attributes;
                    polygon.texture_point_address = texture_point;
                    polygon.texture_header_address = texture_header;
                    polygon.object_address = object_address;
                    polygon.object_instance = current_object_instance;
                    for (unsigned header = 0; header < 4; ++header)
                        polygon.texture_header[header] =
                            texture_word(texture_header + header);
                    const uint16_t header0 = polygon.texture_header[0];
                    const uint16_t header3 = polygon.texture_header[3];
                    polygon.renderer = static_cast<uint8_t>((header0 >> 13) & 3);
                    ++m_summary.renderer_counts[polygon.renderer];
                    ++m_summary.link_type_counts[(attributes >> 8) & 3];
                    ++m_summary.z_sort_mode_counts[(attributes >> 10) & 3];
                    if (header0 & 0x8000) ++m_summary.checker_polygons;
                    polygon.color_base = static_cast<uint16_t>((header3 >> 6) & 0x3ff);
                    for (unsigned vertex = 0;
                         vertex < polygon.vertex_count; ++vertex) {
                        const uint16_t pv = texture_word(
                            texture_point + vertex * 2);
                        const uint16_t pu = texture_word(
                            texture_point + vertex * 2 + 1);
                        polygon.vertices[vertex].u =
                            static_cast<float>(pu) * (1.0f / 8.0f);
                        polygon.vertices[vertex].v =
                            static_cast<float>(pv) * (1.0f / 8.0f);
                    }
                    const float unfocused_x = m_focus[0] != 0.0f ?
                        next0.x / m_focus[0] : next0.x;
                    const float unfocused_y = m_focus[1] != 0.0f ?
                        next0.y / m_focus[1] : next0.y;
                    const float dot_light = normal.x * m_light[0] +
                                            normal.y * m_light[1] +
                                            normal.z * m_light[2];
                    const float dot_point = normal.x * unfocused_x +
                                            normal.y * unfocused_y +
                                            normal.z * next0.z;
                    float minimum_z = polygon.vertices[0].z;
                    float maximum_z = polygon.vertices[0].z;
                    for (unsigned vertex = 1;
                         vertex < polygon.vertex_count; ++vertex) {
                        minimum_z = std::min(minimum_z,
                                             polygon.vertices[vertex].z);
                        maximum_z = std::max(maximum_z,
                                             polygon.vertices[vertex].z);
                    }
                    switch ((attributes >> 10) & 3) {
                    case 1: m_polygon_z = minimum_z; break;
                    case 2: m_polygon_z = maximum_z; break;
                    case 3: m_polygon_z = 1.0e10f; break;
                    default: break;
                    }
                    polygon.z_value = model2_z_value(m_polygon_z, m_z_adjust);
                    polygon.window = m_current_window;
                    // The geometry engine determines front/back before focal
                    // distance is applied. Testing the projected X/Y values
                    // incorrectly removes panels near the edge of the view.
                    polygon.backface = dot_point < 0.0f;
                    if (polygon.backface) ++m_summary.backface_polygons;
                    polygon.master_z_clipped =
                        m_master_z_clip != 0xff && minimum_z > 0.0f &&
                        static_cast<int32_t>(1.0f / minimum_z) >
                            static_cast<int32_t>(m_master_z_clip);
                    if (((attributes >> 8) & 3) == 0 ||
                        (((attributes >> 17) & 1) == 0 && polygon.backface) ||
                        polygon.master_z_clipped)
                        ++m_summary.culled_polygons;
                    const texture_parameter& material =
                        m_texture_parameters[(attributes >> 18) & 0x1f];
                    float luminance = dot_light * dot_point < 0.0f ?
                        0.0f : std::fabs(dot_light);
                    float specular = (2.0f * dot_light) * normal.z -
                                     m_light[2];
                    if ((m_mode & 1) == 0 || specular < 0.0f ||
                        material.specular_control == 0)
                        specular = 0.0f;
                    if ((material.specular_control >> 1) != 0)
                        specular *= specular;
                    if ((material.specular_control >> 2) != 0)
                        specular *= specular;
                    if (((material.specular_control + 1) >> 3) != 0)
                        specular *= specular;
                    luminance = luminance * material.diffuse +
                                material.ambient +
                                specular * material.specular_scale;
                    polygon.luma = static_cast<uint8_t>(
                        std::clamp(luminance, 0.0f, 255.0f));
                    const float distance =
                        m_distance_coefficients[attributes >> 27] *
                        std::fabs(dot_point) * m_lod;
                    const uint32_t distance_word = float_to_bits(distance) >> 8;
                    polygon.texture_lod = static_cast<int32_t>(
                        ((distance_word >> 8) & 0x7f80) - 0x3f80) +
                        m_log_ram[distance_word & 0x7fff];
                    polygon.center_select = m_center_select;
                    polygon.viewport = m_viewport;
                    polygon.center = m_centers[polygon.center_select];
                    m_polygons.push_back(polygon);
                }

                texture_point += ((attributes & 1) ? 8 : 6);
                int header_delta = static_cast<int>((attributes >> 12) & 0x1f);
                if (header_delta & 0x10) header_delta -= 0x20;
                texture_header += static_cast<uint32_t>(header_delta * 4);

                switch ((attributes >> 8) & 3) {
                case 0:
                case 2:
                    previous0 = next0;
                    previous1 = next1;
                    break;
                case 1:
                    previous1 = next0;
                    break;
                case 3:
                    previous0 = next1;
                    break;
                }
            }
            break;
        }
        case 0x02:
        case 0x12: {
            uint32_t texture_point{};
            uint32_t texture_header{};
            if (!take(texture_point) || !take(texture_header)) break;
            const auto direct_point = [&]() {
                uint32_t x{}, y{}, z{};
                if (!take(x) || !take(y) || !take(z))
                    return model2_geometry_vertex{};
                // The geometrizer-to-rasterizer bus carries the upper 24
                // bits of each direct-data float.
                return model2_geometry_vertex{
                    bits_to_float(x & 0xffffff00U),
                    bits_to_float(y & 0xffffff00U),
                    bits_to_float(z & 0xffffff00U)};
            };
            model2_geometry_vertex previous0 = direct_point();
            model2_geometry_vertex previous1 = direct_point();
            const uint16_t current_object_instance = object_instance++;
            while (!m_summary.truncated) {
                uint32_t raw_attributes{};
                if (!take(raw_attributes) || (raw_attributes & 3) == 0) break;
                ++m_summary.direct_polygons;
                const uint32_t attributes = raw_attributes & 0x00ffffffU;
                uint32_t luma_word{}, distance_word{};
                if (!take(luma_word) || !take(distance_word)) break;
                const model2_geometry_vertex next0 = direct_point();
                const model2_geometry_vertex next1 =
                    (attributes & 1) ? direct_point() : next0;
                if (m_summary.truncated) break;

                // Direct polygons share the same Z buckets, window priority
                // and LIFO links as ROM/RAM-backed objects.  Keeping them in
                // this display-list position restores transient geometry
                // such as spray and impact effects without camera-Z overlays.
                if (m_polygons.size() < 32768) {
                    model2_geometry_polygon polygon{};
                    polygon.vertex_count = (attributes & 1) ? 4 : 3;
                    polygon.vertices[0] = previous1;
                    polygon.vertices[1] = previous0;
                    polygon.vertices[2] = next0;
                    polygon.vertices[3] = next1;
                    polygon.attributes = attributes;
                    polygon.texture_point_address = texture_point;
                    polygon.texture_header_address = texture_header;
                    polygon.object_address = 0xffffffffU;
                    polygon.object_instance = current_object_instance;
                    for (unsigned header = 0; header < 4; ++header)
                        polygon.texture_header[header] =
                            texture_word(texture_header + header);
                    const uint16_t header0 = polygon.texture_header[0];
                    const uint16_t header3 = polygon.texture_header[3];
                    polygon.renderer =
                        static_cast<uint8_t>((header0 >> 13) & 3);
                    polygon.color_base =
                        static_cast<uint16_t>((header3 >> 6) & 0x3ff);
                    polygon.luma =
                        static_cast<uint8_t>((luma_word >> 23) & 0xff);
                    polygon.backface = (luma_word & 0x80000000U) != 0;
                    ++m_summary.renderer_counts[polygon.renderer];
                    ++m_summary.link_type_counts[(attributes >> 8) & 3];
                    ++m_summary.z_sort_mode_counts[(attributes >> 10) & 3];
                    if (header0 & 0x8000) ++m_summary.checker_polygons;
                    if (polygon.backface) ++m_summary.backface_polygons;
                    for (unsigned vertex = 0;
                         vertex < polygon.vertex_count; ++vertex) {
                        const uint16_t pv = texture_word(
                            texture_point + vertex * 2);
                        const uint16_t pu = texture_word(
                            texture_point + vertex * 2 + 1);
                        polygon.vertices[vertex].u =
                            static_cast<float>(pu) * (1.0f / 8.0f);
                        polygon.vertices[vertex].v =
                            static_cast<float>(pv) * (1.0f / 8.0f);
                    }
                    float minimum_z = polygon.vertices[0].z;
                    float maximum_z = polygon.vertices[0].z;
                    for (unsigned vertex = 1;
                         vertex < polygon.vertex_count; ++vertex) {
                        minimum_z = std::min(minimum_z,
                                             polygon.vertices[vertex].z);
                        maximum_z = std::max(maximum_z,
                                             polygon.vertices[vertex].z);
                    }
                    polygon.master_z_clipped =
                        m_master_z_clip != 0xff && minimum_z > 0.0f &&
                        static_cast<int32_t>(1.0f / minimum_z) >
                            static_cast<int32_t>(m_master_z_clip);
                    if (((attributes >> 8) & 3) == 0 ||
                        (((attributes >> 17) & 1) == 0 && polygon.backface) ||
                        polygon.master_z_clipped || maximum_z < 0.0f)
                        ++m_summary.culled_polygons;
                    switch ((attributes >> 10) & 3) {
                    case 1: m_polygon_z = minimum_z; break;
                    case 2: m_polygon_z = maximum_z; break;
                    case 3: m_polygon_z = 1.0e10f; break;
                    default: break;
                    }
                    polygon.z_value = model2_z_value(m_polygon_z, m_z_adjust);
                    const uint32_t raster_distance = distance_word >> 8;
                    polygon.texture_lod = static_cast<int32_t>(
                        ((raster_distance >> 8) & 0x7f80) - 0x3f80) +
                        m_log_ram[raster_distance & 0x7fff];
                    polygon.window = m_current_window;
                    polygon.center_select = m_center_select;
                    polygon.viewport = m_viewport;
                    polygon.center = m_centers[polygon.center_select];
                    m_polygons.push_back(polygon);
                }

                texture_point += (attributes & 1) ? 8 : 6;
                int header_delta = static_cast<int>(
                    (attributes >> 12) & 0x1f);
                if (header_delta & 0x10) header_delta -= 0x20;
                texture_header += static_cast<uint32_t>(header_delta * 4);
                switch ((attributes >> 8) & 3) {
                case 0:
                case 2:
                    previous0 = next0;
                    previous1 = next1;
                    break;
                case 1:
                    previous1 = next0;
                    break;
                case 3:
                    previous0 = next1;
                    break;
                }
            }
            break;
        }
        case 0x03:
        case 0x13: {
            ++m_current_window;
            std::array<uint32_t, 6> coordinates{};
            for (uint32_t& coordinate : coordinates) {
                if (!take(coordinate)) break;
            }
            m_viewport = {signed_12(coordinates[0] >> 16),
                          signed_12(coordinates[0]),
                          signed_12(coordinates[1] >> 16),
                          signed_12(coordinates[1])};
            for (unsigned center = 0; center < 4; ++center) {
                m_centers[center] = {
                    signed_12(coordinates[center + 2] >> 16),
                    signed_12(coordinates[center + 2])};
            }
            break;
        }
        case 0x04:
        case 0x14: {
            uint32_t destination{};
            if (!take(destination) || !take(count)) break;
            for (uint32_t word = 0; word < count; ++word) {
                uint32_t value{};
                if (!take(value)) break;
                if (command == 0x14) {
                    for (unsigned byte = 0; byte < 4; ++byte)
                        m_log_ram[destination++ & 0x7fff] =
                            static_cast<uint16_t>((value >> (byte * 8)) & 0xff);
                } else {
                    if (destination & 0x00800000U)
                        m_texture_ram[destination & 0xffff] =
                            static_cast<uint16_t>(value);
                    else
                        m_log_ram[destination & 0x7fff] =
                            static_cast<uint16_t>(value);
                    ++destination;
                }
            }
            break;
        }
        case 0x05:
        case 0x15: {
            uint32_t destination{};
            if (!take(destination) || !take(count)) break;
            std::array<uint32_t, 0x8000>& memory =
                (destination & 0x01000000U) ? m_polygon_ram1 : m_polygon_ram0;
            uint32_t index = destination & 0x7fff;
            for (uint32_t word = 0; word < count; ++word) {
                uint32_t value{};
                if (!take(value)) break;
                memory[index++ & 0x7fff] = value;
            }
            break;
        }
        case 0x06:
        {
            uint32_t index_word{};
            if (!take(index_word) || !take(count)) break;
            unsigned index = (index_word >> 2) & 0x1f;
            for (uint32_t entry = 0; entry < count; ++entry) {
                uint32_t packed{}, coefficient{};
                if (!take(packed) || !take(coefficient)) break;
                texture_parameter& material = m_texture_parameters[index];
                material.diffuse = static_cast<float>(packed & 0xff);
                material.ambient = static_cast<float>((packed >> 8) & 0xff);
                material.specular_scale =
                    static_cast<float>((packed >> 16) & 0xff);
                material.specular_control =
                    static_cast<uint8_t>(packed >> 24);
                m_distance_coefficients[index] = bits_to_float(coefficient);
                index = (index + 1) & 0x1f;
            }
            break;
        }
        case 0x07:
            take(m_mode);
            break;
        case 0x08:
        case 0x18: {
            uint32_t value{};
            // The geometry-to-rasterizer link transfers the upper 24 bits,
            // then the rasterizer restores them to their original position.
            // This clears the low byte; shifting the original word instead
            // destroys the float exponent and collapses most polygons into
            // Z bucket ffff, allowing the road to hide cars and scenery.
            if (take(value)) m_z_adjust = value & 0xffffff00U;
            break;
        }
        case 0x10:
        case 0x1e:
            skip(1);
            break;
        case 0x17:
            take(m_mode);
            break;
        case 0x09:
        case 0x19: {
            uint32_t x{}, y{};
            if (take(x) && take(y))
                m_focus = {bits_to_float(x), bits_to_float(y)};
            break;
        }
        case 0x0d:
            skip(2);
            break;
        case 0x0a:
        case 0x1a:
            for (float& component : m_light) {
                uint32_t value{};
                if (!take(value)) break;
                component = bits_to_float(value);
            }
            break;
        case 0x16: {
            uint32_t value{};
            if (take(value)) m_lod = bits_to_float(value);
            break;
        }
        case 0x0b:
        case 0x1b:
            for (float& element : m_matrix) {
                uint32_t value{};
                if (!take(value)) break;
                element = bits_to_float(value);
            }
            break;
        case 0x0c:
        case 0x1c:
            for (unsigned element = 9; element < 12; ++element) {
                uint32_t value{};
                if (!take(value)) break;
                m_matrix[element] = bits_to_float(value);
            }
            break;
        case 0x0e:
            if (!skip(32) || !take(count)) break;
            skip(static_cast<uint64_t>(count) * 3);
            break;
        case 0x0f:
        case 0x1f:
            m_summary.ended = true;
            break;
        case 0x1d:
            if (!take(ignored) || !take(count)) break;
            skip(static_cast<uint64_t>(count) * 3);
            break;
        default:
            m_summary.truncated = true;
            break;
        }
    }
    if (!m_summary.ended && m_summary.commands >= maximum_commands)
        m_summary.truncated = true;
    m_summary.decoded_polygons = static_cast<uint32_t>(m_polygons.size());
}
