// Sega Model 2A-CRX machine bring-up boundary.
#pragma once

#include "sega/model2/model2_rom.h"
#include "sega/model2/model2_geometry.h"
#include "sega/model2/model2_gpu_frame.h"
#include "arcade_types.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

class model2_machine {
public:
    static constexpr int screen_width = 496;
    static constexpr int screen_height = 384;
    // Model 2's 16 MHz video clock spans 656 by 424 total clocks. Keep this
    // physical vblank cadence separate from WhittyArcade's 60 Hz presentation
    // cap: the i960 and TGP still need their full work budget between vblanks.
    static constexpr double hardware_refresh_rate =
        16000000.0 / (656.0 * 424.0);
    static constexpr double refresh_rate = 60.0;

    model2_machine();
    ~model2_machine();

    model2_machine(const model2_machine&) = delete;
    model2_machine& operator=(const model2_machine&) = delete;

    bool initialize(const std::string& rom_path);
    void reset();
    void reset_preserving_nvram();
    bool flush_nvram();
    void run_frame(bool render_frame = true);
    void set_inputs(const input_state& state);
    void set_sound_uart_callback(std::function<void(uint8_t)> callback);
    // Byte sent back by the sound board's SCSP MIDI-out (audio thread safe).
    void sound_midi_receive(uint8_t data);
    void run_cycles(int cycles);

    const uint32_t* frame_buffer() const;
    void render_video_layers();
    const uint32_t* base_layer() const;
    const uint32_t* foreground_layer() const;
    model2_gpu_frame make_gpu_frame() const;
    const model2_roms& roms() const { return m_roms; }
    model2_rom_set rom_set() const { return m_set; }
    uint32_t program_counter() const;
    uint32_t cpu_register(unsigned index) const;
    std::string current_instruction() const;
    std::string instruction_at(uint32_t address) const;
    uint8_t debug_read8(uint32_t address);
    bool tgp_running() const;
    uint16_t tgp_program_counter() const;
    uint32_t tgp_data_word(uint16_t address) const;
    uint32_t tgp_uploaded_words() const;
    std::size_t tgp_input_words() const;
    std::size_t tgp_output_words() const;
    uint32_t geometry_read_address() const;
    uint32_t geometry_write_address() const;
    uint32_t geometry_buffer_word(uint32_t byte_address) const;
    const model2_geometry_summary& geometry_summary() const;
    const std::vector<model2_geometry_polygon>& geometry_polygons() const;
    std::size_t non_black_pixels() const;
    uint64_t video_frame_hash() const;
    uint64_t executed_cycles() const { return m_executed_cycles; }
    bool cpu_faulted() const { return !m_cpu_error.empty(); }
    const std::string& cpu_error() const { return m_cpu_error; }
    uint64_t unmapped_reads() const;
    uint64_t unmapped_writes() const;
    uint32_t last_unmapped_read() const;
    uint32_t last_unmapped_write() const;
    bool communication_peer_mode() const;
    bool communication_linked() const;
    uint8_t communication_node_id() const;
    uint8_t srally_link_type() const;
    bool set_srally_link_type(uint8_t type);

private:
    void update_interrupt_lines();
    void decode_geometry();
    void render_video_frame();

    model2_rom_set m_set{model2_rom_set::unknown};
    model2_roms m_roms;
    struct implementation;
    std::unique_ptr<implementation> m_impl;
    uint64_t m_frame_number{};
    uint64_t m_executed_cycles{};
    double m_cycle_remainder{};
    std::string m_cpu_error;
    mutable uint64_t m_exported_texture_generation{};
    mutable uint64_t m_exported_color_generation{};
};
