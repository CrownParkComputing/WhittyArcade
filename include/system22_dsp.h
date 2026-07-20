// system22_dsp.h - Namco C71 master/slave DSP subsystem.
#pragma once

#include "system22_tms320c25.h"
#include "system22_types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

class system22_bus;

class system22_dsp_system {
public:
    explicit system22_dsp_system(system22_bus& bus);

    bool initialize(const uint8_t* c71_firmware, std::size_t firmware_size,
                    const uint8_t* point_rom, std::size_t point_rom_size);
    void control(uint8_t value);
    int execute(int clocks);
    void signal_vblank();
    std::vector<polygon_object> take_rendered_polygons();

    bool master_enabled() const { return m_master_enabled; }
    bool slave_enabled() const { return m_slave_enabled; }
    uint16_t master_pc() const { return m_master.program_counter(); }
    uint16_t slave_pc() const { return m_slave.program_counter(); }
    uint64_t direct_polygon_count() const { return m_direct_polygon_count; }
    uint64_t display_polygon_count() const { return m_display_polygon_count; }
    uint64_t pdp_begin_count() const { return m_pdp_begin_count; }
    uint64_t display_parse_errors() const { return m_display_parse_errors; }

private:
    enum class upload_state : uint8_t { ready, destination, data };

    uint16_t master_program_read(uint16_t address) const;
    uint16_t master_data_read(uint16_t address);
    void master_data_write(uint16_t address, uint16_t data);
    uint16_t master_io_read(uint16_t port);
    void master_io_write(uint16_t port, uint16_t data);

    uint16_t slave_program_read(uint16_t address) const;
    uint16_t slave_data_read(uint16_t address) const;
    void slave_data_write(uint16_t address, uint16_t data);
    uint16_t slave_io_read(uint16_t port) const;
    void slave_io_write(uint16_t port, uint16_t data);

    uint16_t dsp_ram_read(uint16_t offset);
    void dsp_ram_write(uint16_t offset, uint16_t data);
    int32_t point_read(uint32_t address) const;
    void point_write(uint32_t address, uint32_t data);
    void upload_slave_word(uint16_t data);
    void write_render_device(uint16_t data);
    void simulate_slave_dsp();
    void handle_viewport(const uint32_t* source);
    void handle_render_options(const uint32_t* source);
    void handle_view_transform(const uint32_t* source);
    void handle_primitive(const uint32_t* source, uint16_t code);
    void render_poly_object(uint16_t code, float matrix[4][4]);
    bool render_quad_packets(uint32_t address, uint32_t length, float matrix[4][4]);
    void render_single_quad(uint32_t color, uint32_t address, float matrix[4][4],
                            uint32_t polyshift, uint32_t flags,
                            uint32_t packet_format);
    void register_normals(uint32_t address, float matrix[4][4]);
    void apply_polygon_fog(polygon_object& polygon) const;

    static float dsp_float(uint32_t value);
    static void identity_matrix(float matrix[4][4]);
    static void multiply_matrix(float left[4][4], const float right[4][4]);
    void apply_reflection(float matrix[4][4]) const;
    static void transform_point(float& x, float& y, float& z,
                                const float matrix[4][4]);
    static void transform_normal(float& x, float& y, float& z,
                                 const float matrix[4][4]);

    system22_bus& m_bus;
    tms320c2x_device m_master;
    tms320c2x_device m_slave;

    std::array<uint16_t, 0x3000> m_master_data{};
    std::array<uint16_t, 0x4000> m_master_extram{};
    std::array<uint16_t, 0x2000> m_slave_extram{};
    std::vector<int32_t> m_point_rom;
    std::array<uint32_t, 0x20000> m_point_ram{};

    uint16_t m_dsp_ram_bank{0};
    uint16_t m_dsp_ram_latch{0};
    uint32_t m_point_address{0};
    uint32_t m_point_data{0};
    uint16_t m_pdp_base{0};
    uint16_t m_master_bio{0};
    upload_state m_upload_state{upload_state::ready};
    uint16_t m_upload_destination{0};
    std::array<uint16_t, 0x1c> m_render_words{};
    std::size_t m_render_word_count{0};
    std::vector<polygon_object> m_rendered_polygons;
    uint64_t m_direct_polygon_count{0};
    uint64_t m_display_polygon_count{0};
    uint64_t m_pdp_begin_count{0};
    uint64_t m_display_parse_errors{0};
    bool m_pdp_pending{false};

    float m_view_matrix[4][4]{};
    int m_camera_ambient{0};
    int m_camera_power{0};
    float m_camera_lx{0.0f};
    float m_camera_ly{0.0f};
    float m_camera_lz{0.0f};
    int m_absolute_priority{0};
    int m_camera_vx{0};
    int m_camera_vy{0};
    float m_camera_zoom{1.0f};
    int m_camera_vr{0};
    int m_camera_vl{0};
    int m_camera_vu{0};
    int m_camera_vd{0};
    int m_reflection{0};
    bool m_cullflip{false};
    uint32_t m_cz_adjust{0};
    uint32_t m_object_shift{0};
    uint32_t m_object_flags{0};
    std::array<uint8_t, 0x80> m_lit_surfaces{};
    std::size_t m_lit_surface_count{0};
    std::size_t m_lit_surface_index{0};
    bool m_master_enabled{false};
    bool m_slave_enabled{false};
    bool m_irq_enabled{false};
};
