// Sega Model 2A-CRX byte-addressed i960 bus and early device storage.
#pragma once

#include "model2_rom.h"
#include "arcade_types.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

class model1_io_board;

class model2_bus {
public:
    static constexpr std::size_t tgp_fifo_capacity = 8;
    using unmapped_callback =
        std::function<void(bool write, uint32_t address)>;
    using sound_uart_callback = std::function<void(uint8_t data)>;

    // Constructor and destructor are defined out of line so translation units
    // that only see the forward-declared model1_io_board do not instantiate
    // the m_iob unique_ptr deleter against an incomplete type.
    model2_bus();
    ~model2_bus();

    // The profile is supplied by the machine (which links the ROM loader);
    // it defaults so I/O-only tests can attach without pulling in the loader.
    void attach(const model2_roms& roms,
                const model2_game_profile& profile = {});
    void reset();
    bool flush_nvram() { return save_nvram(); }

    uint8_t read8(uint32_t address);
    void write8(uint32_t address, uint8_t value);
    uint32_t read32(uint32_t address);
    void write32(uint32_t address, uint32_t value);
    uint16_t access_flags(uint32_t address) const;
    void tick(uint32_t cycles);
    void vblank();
    void set_inputs(const input_state& state);
    bool irq_asserted(unsigned line) const;

    // MB86234 geometry coprocessor buses and host FIFO handshake.
    uint32_t tgp_program_read(uint16_t address) const;
    uint32_t tgp_data_read(uint16_t address) const;
    void tgp_data_write(uint16_t address, uint32_t value);
    uint32_t tgp_io_read(uint16_t address);
    void tgp_io_write(uint16_t address, uint32_t value);
    uint32_t tgp_rf_read(uint16_t address);
    void tgp_rf_write(uint16_t address, uint32_t value);
    bool tgp_input_available() const { return !m_tgp_fifo_in.empty(); }
    bool tgp_output_available() const { return !m_tgp_fifo_out.empty(); }
    bool take_tgp_boot_request();
    bool tgp_gpio0() const { return m_tgp_gpio0; }
    void set_unmapped_callback(unmapped_callback callback) {
        m_unmapped_callback = std::move(callback);
    }
    void set_sound_uart_callback(sound_uart_callback callback) {
        m_sound_uart_callback = std::move(callback);
    }
    // Byte arriving from the sound board's SCSP MIDI-out (audio thread).
    void sound_midi_receive(uint8_t data);
    uint32_t tgp_uploaded_words() const { return m_tgp_program_index; }
    std::size_t tgp_input_words() const { return m_tgp_fifo_in.size(); }
    std::size_t tgp_output_words() const { return m_tgp_fifo_out.size(); }
    const std::vector<uint8_t>& tile_ram() const { return m_tile_ram; }
    const std::vector<uint8_t>& character_ram() const {
        return m_character_ram;
    }
    const std::vector<uint8_t>& palette_ram() const { return m_palette_ram; }
    const std::vector<uint8_t>& color_translation() const {
        return m_color_translation;
    }
    const std::vector<uint8_t>& framebuffer_a() const {
        return m_framebuffer_a;
    }
    const std::vector<uint8_t>& framebuffer_b() const {
        return m_framebuffer_b;
    }
    const std::vector<uint8_t>& texture_sheet_0() const {
        return m_texture_sheet_0;
    }
    const std::vector<uint8_t>& texture_sheet_1() const {
        return m_texture_sheet_1;
    }
    const std::vector<uint8_t>& luma_ram() const { return m_luma_table; }
    const std::vector<uint8_t>& geometry_buffer() const {
        return m_buffer_ram;
    }
    uint32_t frame_number() const { return m_frame_number; }
    uint32_t render_control() const { return m_render_control; }
    // Raw low control bits written at 0x0098000c. Bit zero selects the
    // geometrizer's 30 Hz latch mode; the frame-status bits returned to the
    // i960 are generated separately by read8().
    uint32_t video_control() const;
    bool geometry_frame_due() const;
    uint32_t master_z_clip() const;
    int16_t horizontal_offset() const { return m_horizontal_offset; }
    int16_t vertical_offset() const { return m_vertical_offset; }
    uint64_t video_generation() const { return m_video_generation; }
    uint64_t texture_generation() const { return m_texture_generation; }
    uint64_t color_generation() const { return m_color_generation; }
    uint32_t geometry_read_address() const { return m_geo_read_address; }
    uint32_t geometry_write_address() const { return m_geo_write_address; }
    uint32_t geometry_buffer_word(uint32_t byte_address) const;

    uint64_t unmapped_read_count() const { return m_unmapped_reads; }
    uint64_t unmapped_write_count() const { return m_unmapped_writes; }
    uint32_t last_unmapped_read() const { return m_last_unmapped_read; }
    uint32_t last_unmapped_write() const { return m_last_unmapped_write; }

private:
    void load_nvram();
    bool save_nvram();

    static bool in_range(uint32_t address, uint32_t base,
                         std::size_t size);
    static uint8_t read_region(const std::vector<uint8_t>& region,
                               uint32_t address, uint32_t base,
                               uint8_t fallback = 0xff);
    static bool write_region(std::vector<uint8_t>& region, uint32_t address,
                             uint32_t base, uint8_t value);
    static bool write_video_region(std::vector<uint8_t>& region,
                                   uint32_t address, uint32_t base,
                                   uint8_t value, uint64_t& generation);

    const model2_roms* m_roms{};
    // Declarative hardware config for the loaded game (sound board, I/O kind,
    // NVRAM leaf). Cached from model2_rom_loader::profile_for at attach; read
    // in place of switching on the rom set.
    model2_game_profile m_profile{};
    std::vector<uint8_t> m_local_ram;
    std::vector<uint8_t> m_work_ram;
    std::vector<uint8_t> m_geometry_ram;
    std::vector<uint8_t> m_geometry_program;
    std::vector<uint8_t> m_buffer_ram;
    std::vector<uint8_t> m_control_registers;
    std::vector<uint8_t> m_cpu_control;
    std::vector<uint8_t> m_tile_ram;
    std::vector<uint8_t> m_character_ram;
    std::vector<uint8_t> m_palette_ram;
    std::vector<uint8_t> m_color_translation;
    std::vector<uint8_t> m_z_clip;
    std::vector<uint8_t> m_communication_ram;
    std::vector<uint8_t> m_communication_control;
    std::vector<uint8_t> m_backup_ram;
    std::vector<uint8_t> m_io_registers;
    std::vector<uint8_t> m_uart_registers;
    std::vector<uint8_t> m_texture_ram_0;
    std::vector<uint8_t> m_texture_ram_1;
    std::vector<uint8_t> m_texture_sheet_0;
    std::vector<uint8_t> m_texture_sheet_1;
    std::vector<uint8_t> m_luma_ram;
    std::vector<uint8_t> m_luma_table;
    std::vector<uint8_t> m_framebuffer_a;
    std::vector<uint8_t> m_framebuffer_b;

    uint64_t m_unmapped_reads{};
    uint64_t m_unmapped_writes{};
    uint32_t m_last_unmapped_read{};
    uint32_t m_last_unmapped_write{};
    std::array<uint32_t, 4> m_timer_values{};
    std::array<uint32_t, 4> m_timer_original{};
    std::array<bool, 4> m_timer_running{};
    uint32_t m_irq_request{};
    uint32_t m_irq_enable{};
    uint32_t m_irq_ack_latch{0xffffffffU};
    uint32_t m_frame_number{};
    bool m_nvram_dirty{};
    uint32_t m_last_nvram_save_frame{};
    uint64_t m_video_generation{1};
    uint64_t m_texture_generation{1};
    uint64_t m_color_generation{1};
    uint8_t m_comm_cn{};
    uint8_t m_comm_fg{};
    uint8_t m_comm_zfg{};
    bool m_comm_loopback{};
    bool m_comm_link_alive{};
    uint16_t m_comm_link_timer{};
    std::array<uint8_t, 0x0e00> m_comm_loopback_frame{};
    bool m_comm_loopback_frame_valid{};
    input_state m_inputs{};
    std::array<uint8_t, 7> m_io_port_values{};
    uint8_t m_io_port_config{0xff};
    uint8_t m_io_mode{};
    uint8_t m_io_analog_channel{};
    // Virtua Cop 2 (Model 2A) reads its light guns through the 315-5649 serial
    // channel 2: the i960 writes an axis selector here and reads the selected
    // 10-bit coordinate byte back. 0..7 pick P1_Y/P1_X/P2_Y/P2_X lo/hi bytes;
    // >=8 returns the off-screen status.
    uint8_t m_lightgun_mux{};
    bool m_uart_tx_ready{true};
    // The 8251 has separate data and shift registers.  A byte reaches the
    // SCSP only after its full 10-bit MIDI frame, while TXRDY may reassert as
    // soon as the data register has moved into the shifter.
    uint32_t m_uart_tx_empty_cycles{};
    bool m_uart_tx_empty{true};
    bool m_uart_tx_shift_active{};
    bool m_uart_tx_holding_full{};
    uint8_t m_uart_tx_shift_data{};
    uint8_t m_uart_tx_holding_data{};
    // uPD71051C receive channel, written by the audio thread and consumed by
    // the i960 on the CPU worker thread.
    std::atomic<uint8_t> m_uart_rx_data{};
    std::atomic<bool> m_uart_rx_pending{};
    bool m_eeprom_control_mode{};
    bool m_eeprom_data_out{true};
    bool m_eeprom_chip_select{};
    bool m_eeprom_clock{};
    bool m_eeprom_write_enabled{};
    bool m_eeprom_write_pending{};
    uint8_t m_eeprom_address{};
    uint8_t m_eeprom_command_bits{};
    uint8_t m_eeprom_output_bits{};
    uint8_t m_eeprom_write_bits{};
    uint32_t m_eeprom_shift{};
    uint16_t m_eeprom_output_shift{};
    uint16_t m_eeprom_write_shift{};
    std::array<uint16_t, 64> m_eeprom_words{};
    uint8_t m_gear{1};
    bool m_shift_down_previous{};
    bool m_shift_up_previous{};

    // Original Model 2 games (Virtua Cop) read their guns and switches through
    // a Sega model1io2 cabinet-I/O board (TMPZ84C015) that shares dual-port RAM
    // with the i960 at 0x01c00000. Present only for that set; the Model 2A sets
    // read the 315-5296 chip mapped at the same window.
    std::unique_ptr<model1_io_board> m_iob;
    std::vector<uint8_t> m_iob_dpram;
    std::array<uint8_t, 0x80> m_iob_eeprom{};
    std::array<uint8_t, 0x80> m_iob_eeprom_saved{};
    uint64_t m_iob_clock_accum{};
    uint32_t m_render_control{};
    uint16_t m_horizontal_sync{};
    uint16_t m_vertical_sync{};
    int16_t m_horizontal_offset{90};
    int16_t m_vertical_offset{-8};
    uint32_t m_geo_host_write_latch{};
    uint32_t m_geo_read_address{};
    uint32_t m_geo_write_address{};
    uint32_t m_geo_upload_count{};
    std::vector<uint32_t> m_tgp_data_ram;
    std::deque<uint32_t> m_tgp_fifo_in;
    std::deque<uint32_t> m_tgp_fifo_out;
    uint32_t m_tgp_host_write_latch{};
    uint32_t m_tgp_host_read_latch{0xffffffffU};
    uint32_t m_tgp_program_index{};
    uint32_t m_tgp_bank{};
    uint32_t m_tgp_sincos_base{};
    std::array<uint32_t, 4> m_tgp_atan_base{};
    uint32_t m_tgp_inv_base{};
    uint32_t m_tgp_isqrt_base{};
    bool m_tgp_boot_request{};
    bool m_tgp_gpio0{};
    unmapped_callback m_unmapped_callback;
    sound_uart_callback m_sound_uart_callback;

    void push_geometry_word(uint32_t value);
    uint8_t io_read(uint8_t offset);
    void io_write(uint8_t offset, uint8_t value);
    void eeprom_write_lines(uint8_t value);
    void eeprom_clock_rising(bool data_in);
};
