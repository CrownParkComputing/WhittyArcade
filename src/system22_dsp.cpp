// system22_dsp.cpp - standalone Namco C71 integration.
#include "system22_dsp.h"

#include "system22_cpu.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace {
int32_t sign_extend(uint32_t value, unsigned bits) {
    const unsigned shift = 32 - bits;
    return static_cast<int32_t>(value << shift) >> shift;
}
}

system22_dsp_system::system22_dsp_system(system22_bus& bus) : m_bus(bus) {
    m_master.set_program_callbacks(
        [this](uint16_t address) { return master_program_read(address); });
    m_master.set_data_callbacks(
        [this](uint16_t address) { return master_data_read(address); },
        [this](uint16_t address, uint16_t data) { master_data_write(address, data); });
    m_master.set_io_callbacks(
        [this](uint16_t port) { return master_io_read(port); },
        [this](uint16_t port, uint16_t data) { master_io_write(port, data); });
    m_master.set_bio_callback([this] { return m_master_bio; });

    m_slave.set_program_callbacks(
        [this](uint16_t address) { return slave_program_read(address); });
    m_slave.set_data_callbacks(
        [this](uint16_t address) { return slave_data_read(address); },
        [this](uint16_t address, uint16_t data) { slave_data_write(address, data); });
    m_slave.set_io_callbacks(
        [this](uint16_t port) { return slave_io_read(port); },
        [this](uint16_t port, uint16_t data) { slave_io_write(port, data); });
    m_slave.set_bio_callback([] { return uint16_t{1}; });
}

bool system22_dsp_system::initialize(const uint8_t* c71_firmware,
                                     std::size_t firmware_size,
                                     const uint8_t* point_rom,
                                     std::size_t point_rom_size) {
    if (!m_master.load_internal_rom(c71_firmware, firmware_size) ||
        !m_slave.load_internal_rom(c71_firmware, firmware_size) ||
        !point_rom || point_rom_size == 0 || point_rom_size % 3 != 0)
        return false;

    const std::size_t word_count = point_rom_size / 3;
    m_point_rom.resize(word_count);
    const uint8_t* low = point_rom;
    const uint8_t* middle = point_rom + word_count;
    const uint8_t* high = point_rom + word_count * 2;
    for (std::size_t i = 0; i < word_count; ++i) {
        uint32_t value = (static_cast<uint32_t>(high[i]) << 16) |
                         (static_cast<uint32_t>(middle[i]) << 8) | low[i];
        if ((value & 0x800000) != 0) value |= 0xff000000;
        m_point_rom[i] = static_cast<int32_t>(value);
    }
    identity_matrix(m_view_matrix);
    return true;
}

void system22_dsp_system::control(uint8_t value) {
    if (value == 0) {
        m_master_enabled = false;
        m_slave_enabled = false;
        m_irq_enabled = false;
    } else if (value == 1) {
        if (!m_master_enabled) m_master.reset();
        if (!m_slave_enabled) m_slave.reset();
        m_master_enabled = true;
        m_slave_enabled = true;
        m_irq_enabled = true;
    } else if (value == 0xff) {
        if (!m_master_enabled) m_master.reset();
        m_master_enabled = true;
        m_irq_enabled = false;
    }
}

int system22_dsp_system::execute(int clocks) {
    int executed = 0;
    if (m_master_enabled) executed = std::max(executed, m_master.execute(clocks));
    if (m_slave_enabled) executed = std::max(executed, m_slave.execute(clocks));
    return executed;
}

void system22_dsp_system::signal_vblank() {
    if (m_master_enabled && m_irq_enabled)
        m_master.set_input(TMS320C2X_INT0, true);
}

std::vector<polygon_object> system22_dsp_system::take_rendered_polygons() {
    if (m_pdp_pending) {
        m_pdp_pending = false;
        simulate_slave_dsp();
    }
    std::vector<polygon_object> result;
    result.swap(m_rendered_polygons);
    return result;
}

uint16_t system22_dsp_system::master_program_read(uint16_t address) const {
    if (address >= 0x4000 && address <= 0x7fff)
        return m_master_extram[address - 0x4000];
    return 0xffff;
}

uint16_t system22_dsp_system::master_data_read(uint16_t address) {
    if (address >= 0x1000 && address <= 0x3fff)
        return m_master_data[address - 0x1000];
    if (address >= 0x4000 && address <= 0x7fff)
        return m_master_extram[address - 0x4000];
    if (address >= 0x8000)
        return dsp_ram_read(address - 0x8000);
    return 0xffff;
}

void system22_dsp_system::master_data_write(uint16_t address, uint16_t data) {
    if (address >= 0x1000 && address <= 0x3fff)
        m_master_data[address - 0x1000] = data;
    else if (address >= 0x4000 && address <= 0x7fff)
        m_master_extram[address - 0x4000] = data;
    else if (address >= 0x8000)
        dsp_ram_write(address - 0x8000, data);
}

uint16_t system22_dsp_system::master_io_read(uint16_t port) {
    switch (port & 0x0f) {
        case 0x0:
            m_point_data = static_cast<uint32_t>(point_read(m_point_address++));
            return static_cast<uint16_t>(m_point_data);
        case 0x1:
            return static_cast<uint16_t>(0x8000 | ((m_point_data >> 16) & 0xff));
        case 0x2:
            m_master_bio = 1;
            m_pdp_pending = true;
            ++m_pdp_begin_count;
            return 0;
        case 0x3:
            m_master_bio = 0;
            m_upload_state = upload_state::ready;
            return 0;
        case 0x8: return 0;
        case 0x9: return 0x0063;
        case 0xf: return 0;
        default: return 0xffff;
    }
}

void system22_dsp_system::master_io_write(uint16_t port, uint16_t data) {
    switch (port & 0x0f) {
        case 0x0:
            m_point_data = (m_point_data & 0xffff0000) | data;
            point_write(m_point_address++, m_point_data);
            break;
        case 0x1:
            m_point_data = (m_point_data & 0x0000ffff) |
                           (static_cast<uint32_t>(data) << 16);
            break;
        case 0x2: m_pdp_base = data; break;
        case 0x3: m_point_address = (m_point_address << 16) | data; break;
        case 0x7: upload_slave_word(data); break;
        case 0x8: m_render_word_count = 0; break;
        case 0xc: write_render_device(data); break;
        case 0xd: m_dsp_ram_bank = data; break;
        default: break;
    }
}

uint16_t system22_dsp_system::slave_program_read(uint16_t address) const {
    if (address >= 0x8000 && address <= 0x9fff)
        return m_slave_extram[address - 0x8000];
    return 0xffff;
}

uint16_t system22_dsp_system::slave_data_read(uint16_t address) const {
    if (address >= 0x8000 && address <= 0x9fff)
        return m_slave_extram[address - 0x8000];
    return 0xffff;
}

void system22_dsp_system::slave_data_write(uint16_t address, uint16_t data) {
    if (address >= 0x8000 && address <= 0x9fff)
        m_slave_extram[address - 0x8000] = data;
}

uint16_t system22_dsp_system::slave_io_read(uint16_t port) const {
    switch (port & 0x0f) {
        case 0x3: return 0x0010;
        case 0x4: case 0x5: case 0x6: case 0x8: case 0xb: return 0;
        default: return 0xffff;
    }
}

void system22_dsp_system::slave_io_write(uint16_t, uint16_t) {
    // Render-device command transport is connected when the slave display
    // protocol is promoted from the existing polygon decoder.
}

uint16_t system22_dsp_system::dsp_ram_read(uint16_t offset) {
    const uint32_t value = m_bus.read_polygon_word(offset & 0x7fff);
    switch (m_dsp_ram_bank & 3) {
        case 0: return static_cast<uint16_t>(value);
        case 1: return static_cast<uint16_t>(value >> 16);
        case 2:
            m_dsp_ram_latch = static_cast<uint16_t>(value >> 16);
            return static_cast<uint16_t>(value);
        default: return 0;
    }
}

void system22_dsp_system::dsp_ram_write(uint16_t offset, uint16_t data) {
    const std::size_t index = offset & 0x7fff;
    const uint32_t old = m_bus.read_polygon_word(index);
    uint16_t low = static_cast<uint16_t>(old);
    uint16_t high = static_cast<uint16_t>(old >> 16);
    switch (m_dsp_ram_bank & 3) {
        case 0: low = data; break;
        case 1: high = data; break;
        case 2: low = data; high = m_dsp_ram_latch; break;
        default: return;
    }
    m_bus.write_polygon_word(index, (static_cast<uint32_t>(high) << 16) | low);
}

int32_t system22_dsp_system::point_read(uint32_t address) const {
    address &= 0x00ffffff;
    if (address < m_point_rom.size()) return m_point_rom[address];
    if (address >= 0xf00000 && address < 0xf20000) {
        uint32_t value = m_point_ram[address - 0xf00000] & 0x00ffffff;
        if ((value & 0x800000) != 0) value |= 0xff000000;
        return static_cast<int32_t>(value);
    }
    return -1;
}

void system22_dsp_system::point_write(uint32_t address, uint32_t data) {
    address &= 0x00ffffff;
    if (address >= 0xf00000 && address < 0xf20000)
        m_point_ram[address - 0xf00000] = data & 0x00ffffff;
}

void system22_dsp_system::upload_slave_word(uint16_t data) {
    switch (m_upload_state) {
        case upload_state::ready:
            if (data == 0) {
                m_slave_enabled = false;
            } else if (data == 1) {
                m_upload_state = upload_state::destination;
            } else if (data == 3 || data == 0x10) {
                if (!m_slave_enabled) m_slave.reset();
                m_slave_enabled = true;
            }
            break;
        case upload_state::destination:
            m_upload_destination = data;
            m_upload_state = upload_state::data;
            break;
        case upload_state::data:
            m_slave_extram[m_upload_destination & 0x1fff] = data;
            ++m_upload_destination;
            break;
    }
}

void system22_dsp_system::write_render_device(uint16_t data) {
    if (m_render_word_count >= m_render_words.size()) return;
    m_render_words[m_render_word_count++] = data;
    if (m_render_word_count != m_render_words.size()) return;

    const uint16_t* source = m_render_words.data();
    polygon_object polygon{};
    polygon.zsort = ((source[1] & 0x0fff) << 12) | (source[0] & 0x0fff);
    polygon.cmode = (source[4] >> 12) & 0x0f;
    polygon.texturebank = (source[5] >> 12) & 0x0f;
    polygon.color = source[2] >> 8;
    polygon.cz_value = (source[3] >> 2) & 0x1fff;
    polygon.cz_type = source[3] & 3;
    polygon.direct = true;
    polygon.clip_left = 0.0f;
    polygon.clip_right = 639.0f;
    polygon.clip_top = 0.0f;
    polygon.clip_bottom = 479.0f;

    // Non-Super System 22 selects one of four CZ tables and fog colors. Color
    // bit 7 disables fog for the polygon.
    apply_polygon_fog(polygon);

    source += 4;
    for (poly_vertex& vertex : polygon.vertices) {
        vertex.u = source[0] & 0x0fff;
        vertex.v = source[1] & 0x0fff;
        vertex.x = static_cast<int16_t>(source[2]);
        vertex.y = static_cast<float>(-static_cast<int16_t>(source[3]));
        vertex.bri = source[4] >> 8;

        const uint16_t mantissa = source[5];
        int exponent = source[4] & 0x3f;
        vertex.z = mantissa ? static_cast<float>(mantissa) : 65536.0f;
        while (mantissa && exponent++ < 0x2e) vertex.z *= 0.5f;
        source += 6;
    }

    if (m_rendered_polygons.size() < SYSTEM22_MAX_POLYGONS_PER_FRAME)
        m_rendered_polygons.push_back(polygon);
    ++m_direct_polygon_count;
}

float system22_dsp_system::dsp_float(uint32_t value) {
    float result = static_cast<float>(static_cast<int16_t>(value));
    int exponent = static_cast<int>((value >> 16) & 0x3f);
    while (exponent++ < 0x2e) result *= 0.5f;
    return result;
}

void system22_dsp_system::identity_matrix(float matrix[4][4]) {
    std::memset(matrix, 0, sizeof(float) * 16);
    for (int index = 0; index < 4; ++index) matrix[index][index] = 1.0f;
}

void system22_dsp_system::multiply_matrix(float left[4][4],
                                           const float right[4][4]) {
    float result[4][4]{};
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column)
            for (int index = 0; index < 4; ++index)
                result[row][column] += left[row][index] * right[index][column];
    std::memcpy(left, result, sizeof(result));
}

void system22_dsp_system::apply_reflection(float matrix[4][4]) const {
    if (m_reflection == 0) return;
    float reflection[4][4];
    identity_matrix(reflection);
    if ((m_reflection & 0x10) != 0) reflection[0][0] = -1.0f;
    if ((m_reflection & 0x20) != 0) reflection[1][1] = -1.0f;
    multiply_matrix(matrix, reflection);
}

void system22_dsp_system::transform_point(float& x, float& y, float& z,
                                           const float matrix[4][4]) {
    const float old_x = x;
    const float old_y = y;
    const float old_z = z;
    x = matrix[0][0] * old_x + matrix[1][0] * old_y +
        matrix[2][0] * old_z + matrix[3][0];
    y = matrix[0][1] * old_x + matrix[1][1] * old_y +
        matrix[2][1] * old_z + matrix[3][1];
    z = matrix[0][2] * old_x + matrix[1][2] * old_y +
        matrix[2][2] * old_z + matrix[3][2];
}

void system22_dsp_system::transform_normal(float& x, float& y, float& z,
                                            const float matrix[4][4]) {
    const float old_x = x;
    const float old_y = y;
    const float old_z = z;
    x = matrix[0][0] * old_x + matrix[1][0] * old_y + matrix[2][0] * old_z;
    y = matrix[0][1] * old_x + matrix[1][1] * old_y + matrix[2][1] * old_z;
    z = matrix[0][2] * old_x + matrix[1][2] * old_y + matrix[2][2] * old_z;
}

void system22_dsp_system::apply_polygon_fog(polygon_object& polygon) const {
    if ((polygon.color & 0x80) != 0) return;
    const std::size_t type = polygon.cz_type & 3;
    const std::size_t fog_color = type & m_bus.read_mixer_byte(0x84 + type);
    polygon.fog_r = m_bus.read_mixer_byte(0x100 + fog_color);
    polygon.fog_g = m_bus.read_mixer_byte(0x180 + fog_color);
    polygon.fog_b = m_bus.read_mixer_byte(0x200 + fog_color);
    polygon.fog_factor = m_bus.read_cz_byte(type * 0x2000 +
                                            (polygon.cz_value & 0x1fff));
}

void system22_dsp_system::handle_viewport(const uint32_t* source) {
    m_camera_ambient = static_cast<int>((source[1] >> 16) & 0xffff);
    m_camera_power = static_cast<int>(source[1] & 0xffff);
    m_camera_lx = dspfixed_to_nativefloat(source[2]);
    m_camera_ly = dspfixed_to_nativefloat(source[3]);
    m_camera_lz = dspfixed_to_nativefloat(source[4]);
    m_absolute_priority = static_cast<int>((source[3] >> 16) & 0xffff);
    m_camera_vx = sign_extend(source[5] >> 16, 12);
    m_camera_vy = sign_extend(source[5], 12);
    m_camera_zoom = dsp_float(source[6]);
    m_camera_vr = static_cast<int>(dsp_float(source[7]) * m_camera_zoom - 0.5f);
    m_camera_vl = static_cast<int>(dsp_float(source[8]) * m_camera_zoom - 0.5f);
    m_camera_vu = static_cast<int>(dsp_float(source[9]) * m_camera_zoom - 0.5f);
    m_camera_vd = static_cast<int>(dsp_float(source[10]) * m_camera_zoom - 0.5f);

    m_reflection = static_cast<int>((source[2] >> 16) & 0x30);
    m_cullflip = m_reflection == 0x10 || m_reflection == 0x20;
    if ((m_reflection & 0x10) != 0) std::swap(m_camera_vl, m_camera_vr);
    if ((m_reflection & 0x20) != 0) std::swap(m_camera_vu, m_camera_vd);

    m_view_matrix[0][0] = dspfixed_to_nativefloat(source[0x0c]);
    m_view_matrix[1][0] = dspfixed_to_nativefloat(source[0x0d]);
    m_view_matrix[2][0] = dspfixed_to_nativefloat(source[0x0e]);
    m_view_matrix[0][1] = dspfixed_to_nativefloat(source[0x0f]);
    m_view_matrix[1][1] = dspfixed_to_nativefloat(source[0x10]);
    m_view_matrix[2][1] = dspfixed_to_nativefloat(source[0x11]);
    m_view_matrix[0][2] = dspfixed_to_nativefloat(source[0x12]);
    m_view_matrix[1][2] = dspfixed_to_nativefloat(source[0x13]);
    m_view_matrix[2][2] = dspfixed_to_nativefloat(source[0x14]);
    apply_reflection(m_view_matrix);

    m_cz_adjust = 0;
    m_object_shift = 0;
    m_object_flags = 0;
}

void system22_dsp_system::handle_render_options(const uint32_t* source) {
    m_cz_adjust = source[1] & 0x00ffffff;
    m_object_shift = source[2] & 0x00ffffff;
    m_object_flags = (source[3] >> 21) & 7;
}

void system22_dsp_system::handle_view_transform(const uint32_t* source) {
    m_view_matrix[0][0] = dspfixed_to_nativefloat(source[1]);
    m_view_matrix[1][0] = dspfixed_to_nativefloat(source[2]);
    m_view_matrix[2][0] = dspfixed_to_nativefloat(source[3]);
    m_view_matrix[0][1] = dspfixed_to_nativefloat(source[4]);
    m_view_matrix[1][1] = dspfixed_to_nativefloat(source[5]);
    m_view_matrix[2][1] = dspfixed_to_nativefloat(source[6]);
    m_view_matrix[0][2] = dspfixed_to_nativefloat(source[7]);
    m_view_matrix[1][2] = dspfixed_to_nativefloat(source[8]);
    m_view_matrix[2][2] = dspfixed_to_nativefloat(source[9]);
    apply_reflection(m_view_matrix);
}

void system22_dsp_system::handle_primitive(const uint32_t* source, uint16_t code) {
    if (code != 5 && code < 0x45) return;
    float matrix[4][4];
    identity_matrix(matrix);
    matrix[0][0] = dspfixed_to_nativefloat(source[1]);
    matrix[1][0] = dspfixed_to_nativefloat(source[2]);
    matrix[2][0] = dspfixed_to_nativefloat(source[3]);
    matrix[0][1] = dspfixed_to_nativefloat(source[4]);
    matrix[1][1] = dspfixed_to_nativefloat(source[5]);
    matrix[2][1] = dspfixed_to_nativefloat(source[6]);
    matrix[0][2] = dspfixed_to_nativefloat(source[7]);
    matrix[1][2] = dspfixed_to_nativefloat(source[8]);
    matrix[2][2] = dspfixed_to_nativefloat(source[9]);
    matrix[3][0] = static_cast<float>(sign_extend(source[0x0a], 24));
    matrix[3][1] = static_cast<float>(sign_extend(source[0x0b], 24));
    matrix[3][2] = static_cast<float>(sign_extend(source[0x0c], 24));
    multiply_matrix(matrix, m_view_matrix);
    render_poly_object(code, matrix);
}

void system22_dsp_system::register_normals(uint32_t address,
                                            float matrix[4][4]) {
    for (int index = 0; index < 4 && m_lit_surface_count < m_lit_surfaces.size();
         ++index) {
        float nx = dspfixed_to_nativefloat(point_read(address + index * 3));
        float ny = dspfixed_to_nativefloat(point_read(address + index * 3 + 1));
        float nz = dspfixed_to_nativefloat(point_read(address + index * 3 + 2));
        transform_normal(nx, ny, nz, matrix);
        float lx = m_camera_lx;
        float ly = m_camera_ly;
        float lz = m_camera_lz;
        transform_normal(lx, ly, lz, m_view_matrix);
        const float dot = std::max(0.0f, nx * lx + ny * ly + nz * lz);
        const float brightness_value = static_cast<float>(m_camera_ambient) +
            static_cast<float>(m_camera_power) * dot;
        const int brightness = static_cast<int>(brightness_value);
        m_lit_surfaces[m_lit_surface_count++] =
            static_cast<uint8_t>(std::clamp(brightness, 0, 255));
    }
}

void system22_dsp_system::render_single_quad(uint32_t color, uint32_t address,
                                              float matrix[4][4],
                                              uint32_t polyshift, uint32_t flags,
                                              uint32_t packet_format) {
    polygon_object polygon{};
    float zmin = 0.0f;
    float zmax = 0.0f;
    for (int index = 0; index < 4; ++index) {
        poly_vertex& vertex = polygon.vertices[index];
        vertex.x = static_cast<float>(point_read(address + 8 + index * 3));
        vertex.y = static_cast<float>(point_read(address + 9 + index * 3));
        vertex.z = static_cast<float>(point_read(address + 10 + index * 3));
        transform_point(vertex.x, vertex.y, vertex.z, matrix);
        if (index == 0 || vertex.z < zmin) zmin = vertex.z;
        if (index == 0 || vertex.z > zmax) zmax = vertex.z;
    }
    if (zmax < 0.0f) return;

    if ((flags & 0x20) != 0) {
        const poly_vertex* v = polygon.vertices;
        const float c1 = v[2].x * (v[0].z * v[1].y - v[0].y * v[1].z) +
                         v[2].y * (v[0].x * v[1].z - v[0].z * v[1].x) +
                         v[2].z * (v[0].y * v[1].x - v[0].x * v[1].y);
        const float c2 = v[0].x * (v[2].z * v[3].y - v[2].y * v[3].z) +
                         v[0].y * (v[2].x * v[3].z - v[2].z * v[3].x) +
                         v[0].z * (v[2].y * v[3].x - v[2].x * v[3].y);
        if ((m_cullflip && c1 <= 0.0f && c2 <= 0.0f) ||
            (!m_cullflip && c1 >= 0.0f && c2 >= 0.0f))
            return;
    }

    zmin = std::max(zmin, 0.0f);
    int zsort = 0;
    switch (flags & 0x300) {
        case 0x000: zsort = static_cast<int>(std::lround(zmin)); break;
        case 0x100: zsort = static_cast<int>(std::lround(zmax)); break;
        default: zsort = static_cast<int>(std::lround((zmin + zmax) * 0.5f)); break;
    }
    zsort = std::min(zsort, 0x1fffff);
    int priority = m_absolute_priority & 7;
    if ((polyshift & 0x200000) != 0)
        zsort = static_cast<int>(polyshift & 0x1fffff);
    else {
        zsort += sign_extend(polyshift, 18);
        priority += static_cast<int>((polyshift & 0x1c0000) >> 18);
    }
    if ((m_object_shift & 0x200000) != 0)
        zsort = static_cast<int>(m_object_shift & 0x1fffff);
    else {
        zsort += sign_extend(m_object_shift, 18);
        priority += static_cast<int>((m_object_shift & 0x1c0000) >> 18);
    }
    zsort = std::clamp(zsort, 0, 0x1fffff);
    polygon.zsort = static_cast<uint32_t>(zsort | ((priority & 7) << 21));

    for (int index = 0; index < 4; ++index) {
        poly_vertex& vertex = polygon.vertices[index];
        vertex.u = point_read(address + index * 2);
        vertex.v = point_read(address + index * 2 + 1);
        if (m_lit_surface_count != 0) {
            std::size_t light = m_lit_surface_index++;
            if (m_lit_surface_count > 4) light >>= 2;
            vertex.bri = m_lit_surfaces[light % m_lit_surface_count];
        } else if ((packet_format & 0x40) != 0) {
            vertex.bri = (point_read(address + index) >> 16) & 0xff;
        } else {
            vertex.bri = static_cast<int>((color >> 16) & 0xff);
        }
        vertex.x *= m_camera_zoom;
        vertex.y *= m_camera_zoom;
        vertex.u &= 0x0fff;
        vertex.v &= 0x0fff;
    }

    polygon.cmode = (point_read(address) >> 12) & 0x0f;
    polygon.texturebank = (point_read(address + 1) >> 12) & 0x0f;
    polygon.color = (color >> 8) & 0xff;
    polygon.cz_value = static_cast<uint32_t>(std::lround(
        std::clamp(zmax, 0.0f, static_cast<float>(0x1fffff)))) >> 8;
    polygon.cz_type = (flags >> 10) & 3;
    polygon.cz_adjust = m_cz_adjust;
    polygon.objectflags = m_object_flags;
    polygon.viewport_x = static_cast<float>(m_camera_vx);
    polygon.viewport_y = static_cast<float>(m_camera_vy);
    const int clip_center_x = 320 + m_camera_vx;
    const int clip_center_y = 240 + m_camera_vy;
    polygon.clip_left = static_cast<float>(
        std::max(0, clip_center_x + m_camera_vl));
    polygon.clip_right = static_cast<float>(
        std::min(639, clip_center_x - m_camera_vr - 1));
    polygon.clip_top = static_cast<float>(
        std::max(0, clip_center_y + m_camera_vu));
    polygon.clip_bottom = static_cast<float>(
        std::min(479, clip_center_y - m_camera_vd - 1));
    polygon.direct = false;
    apply_polygon_fog(polygon);

    if (m_rendered_polygons.size() < SYSTEM22_MAX_POLYGONS_PER_FRAME)
        m_rendered_polygons.push_back(polygon);
    ++m_display_polygon_count;
}

bool system22_dsp_system::render_quad_packets(uint32_t address, uint32_t length,
                                               float matrix[4][4]) {
    const uint32_t finish = address + length;
    while (address < finish) {
        const int32_t packet_length = point_read(address++);
        if (packet_length <= 0 || packet_length > 0x100 ||
            address + static_cast<uint32_t>(packet_length) > finish)
            return false;
        const uint32_t packet_format = static_cast<uint32_t>(point_read(address));
        switch (packet_length) {
            case 0x17:
                render_single_quad(point_read(address + 2), address + 3, matrix, 0,
                                   point_read(address + 1), packet_format);
                break;
            case 0x18:
                render_single_quad(point_read(address + 2), address + 4, matrix,
                                   point_read(address + 3),
                                   point_read(address + 1), packet_format);
                break;
            case 0x10:
                m_lit_surface_count = 0;
                m_lit_surface_index = 0;
                register_normals(address + 4, matrix);
                break;
            case 0x0d:
                register_normals(address + 1, matrix);
                break;
            default:
                return false;
        }
        address += static_cast<uint32_t>(packet_length);
        if ((packet_format & 0x800000) != 0 && address != finish) return false;
    }
    return address == finish;
}

void system22_dsp_system::render_poly_object(uint16_t code, float matrix[4][4]) {
    const bool point_ram = code == 5;
    uint32_t list_address = point_ram ? 0xf00000 :
        (static_cast<uint32_t>(point_read(code)) & 0x00ffffff);
    m_lit_surface_count = 0;
    m_lit_surface_index = 0;

    for (std::size_t entries = 0; entries < 0x20000; ++entries) {
        int32_t object = point_read(list_address++);
        if (object < 0) {
            if (object == -1) break;
            if (!point_ram) {
                ++m_display_parse_errors;
                break;
            }
            object &= 0x00ffffff;
        }
        uint32_t object_address = static_cast<uint32_t>(object) & 0x00ffffff;
        const int32_t chunk_length = point_read(object_address++);
        if (chunk_length < 0 || chunk_length > 0x100 ||
            !render_quad_packets(object_address,
                                 static_cast<uint32_t>(chunk_length), matrix)) {
            ++m_display_parse_errors;
            break;
        }
    }
    m_object_flags &= ~2u;
}

void system22_dsp_system::simulate_slave_dsp() {
    uint32_t cursor = 0x2ff;
    for (std::size_t records = 0; records < 0x1000; ++records) {
        const uint16_t code = static_cast<uint16_t>(m_bus.read_polygon_word(cursor++));
        const uint16_t length = static_cast<uint16_t>(m_bus.read_polygon_word(cursor++));
        const uint32_t index = cursor;
        if (index + length + 2 >= 0x8000) {
            ++m_display_parse_errors;
            return;
        }
        const uint32_t* source = m_bus.polygon_ram_data() + index;
        switch (length) {
            case 0x15: handle_viewport(source); break;
            case 0x10: handle_render_options(source); break;
            case 0x0a: handle_view_transform(source); break;
            case 0x0d: handle_primitive(source, code); break;
            default:
                ++m_display_parse_errors;
                return;
        }

        cursor += length;
        ++cursor; // 0xffff GOTO marker
        const uint16_t next = static_cast<uint16_t>(m_bus.read_polygon_word(cursor++)) &
                              0x7fff;
        const uint32_t sequential = index + length + 2;
        if (next != sequential) {
            if (next != index + length) ++m_display_parse_errors;
            return;
        }
        if (next == 0x7fff) {
            ++m_display_parse_errors;
            return;
        }
    }
    ++m_display_parse_errors;
}
