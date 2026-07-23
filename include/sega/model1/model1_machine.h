// Native Sega Model 1 machine state and V60 bus.
#pragma once

#include "sega/model1/model1_rom.h"
#include "arcade_types.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

class v60_device;
class model1_tgp_device;
class model1_video;
class model1_io_board;

class model1_machine {
public:
    model1_machine();
    ~model1_machine();

    model1_machine(const model1_machine&) = delete;
    model1_machine& operator=(const model1_machine&) = delete;

    bool initialize(const std::string& rom_path, std::string* error = nullptr);
    void reset();
    int execute(int cycles);
    int run_frame();
    void set_inputs(const input_state& state);
    bool attract_sound_enabled() const;
    bool set_attract_sound_enabled(bool enabled);
    void set_sound_uart_handler(
            std::function<void(uint8_t, uint64_t)> handler) {
        m_sound_uart_handler = std::move(handler);
    }

    uint32_t program_counter() const;
    std::string disassemble(uint32_t address, std::size_t* length = nullptr) const;
    uint8_t rom_o_bank() const { return m_rom_o_bank; }
    uint16_t list_control(unsigned index) const {
        return index < 2 ? m_list_control[index] : 0;
    }
    std::size_t display_list_nonzero_bytes(unsigned index) const;
    std::size_t palette_nonzero_bytes() const;
    uint16_t display_list_word(unsigned index, std::size_t word) const;
    std::size_t rendered_quad_count() const;
    uint32_t unknown_display_list_type() const;
    const model1_roms& roms() const { return m_roms; }
    model1_rom_set rom_set() const { return m_rom_set; }
    uint64_t tgp_command_count() const { return m_tgp_command_count; }
    bool io_board_active() const;
    uint16_t io_board_pc() const;
    uint64_t io_board_clocks() const;
    uint16_t tgp_geometry_pc() const;
    bool tgp_uses_lle() const;
    uint64_t tgp_unimplemented_count() const;
    uint32_t tgp_last_unimplemented_function() const;
    uint32_t tgp_last_unimplemented_pc() const;
    uint64_t completed_video_frames() const {
        return m_completed_video_frames.load(std::memory_order_relaxed);
    }
    uint64_t changed_video_frames() const {
        return m_changed_video_frames.load(std::memory_order_relaxed);
    }
    const std::vector<uint8_t>& frame_pixels();
    void wait_for_video();
    static constexpr int native_width() { return 496; }
    static constexpr int native_height() { return 384; }
    // Native board raster: 16 MHz / (656 total clocks * 424 total lines).
    static constexpr double hardware_refresh_rate() {
        return 16000000.0 / (656.0 * 424.0);
    }
    // User-selected smooth presentation mode. The emulated processors retain
    // their 16 MHz clocks while vblank is delivered at an even host 60 Hz.
    static constexpr double refresh_rate() { return 60.0; }

private:
    struct video_job {
        std::vector<uint8_t> display_list_0;
        std::vector<uint8_t> display_list_1;
        std::vector<uint8_t> tile_ram;
        std::vector<uint8_t> character_ram;
        std::vector<uint8_t> palette_ram;
        std::vector<uint8_t> color_translation;
        uint16_t list_control_0{};
        uint16_t list_control_1{};
    };
    struct video_result {
        std::vector<uint8_t> pixels;
        std::size_t quad_count{};
        uint32_t unknown_list_type{};
    };

    void queue_video_frame();
    void harvest_video_result();
    void video_worker_loop();
    void stop_video_worker();
    void load_operator_nvram();
    void commit_operator_nvram();
    bool save_operator_nvram() const;

    uint8_t read_program_byte(uint32_t address);
    void write_program_byte(uint32_t address, uint8_t value);
    uint8_t read_io_byte(uint32_t address);
    void write_io_byte(uint32_t address, uint8_t value);

    static uint8_t read_region(const std::vector<uint8_t>& region,
                               uint32_t base, uint32_t address);
    static bool write_region(std::vector<uint8_t>& region, uint32_t base,
                             uint32_t address, uint8_t value);

    model1_roms m_roms;
    model1_rom_set m_rom_set{model1_rom_set::unknown};
    std::unique_ptr<v60_device> m_main_cpu;
    std::unique_ptr<model1_tgp_device> m_tgp;
    std::unique_ptr<model1_video> m_video;
    std::unique_ptr<model1_io_board> m_io_board;

    std::vector<uint8_t> m_ram_a;
    std::vector<uint8_t> m_ram_b;
    std::vector<uint8_t> m_display_list_0;
    std::vector<uint8_t> m_display_list_1;
    std::vector<uint8_t> m_tile_ram;
    std::vector<uint8_t> m_character_ram;
    std::vector<uint8_t> m_palette_ram;
    std::vector<uint8_t> m_color_translation;
    std::vector<uint8_t> m_io_dual_port_ram;
    std::vector<uint8_t> m_communication_ram;
    std::array<uint8_t, 0x80> m_operator_nvram{};
    std::string m_operator_nvram_path;
    bool m_operator_nvram_dirty{};

    uint8_t m_rom_o_bank{0};
    uint16_t m_list_control[2]{};
    uint16_t m_copro_address{0};
    uint32_t m_copro_write_latch{0};
    uint16_t m_copro_read_half_latch{0xffff};
    uint32_t m_copro_read_latch{0xffffffff};
    uint32_t m_fifo_write_latch{0};
    uint32_t m_fifo_read_latch{0xffffffff};
    unsigned m_bus_trace_remaining{0};
    uint8_t m_irq_status{0};
    uint8_t m_irq_mask{0xff};
    uint8_t m_last_irq{0};
    uint16_t m_timer_mode{};
    uint16_t m_timer_period[2]{};
    int64_t m_timer_remaining[2]{};
    double m_cycle_remainder{0.0};
    double m_io_cycle_remainder{0.0};
    uint64_t m_tgp_command_count{0};
    uint64_t m_total_v60_cycles{0};
    uint64_t m_uart_busy_until{0};
    std::function<void(uint8_t, uint64_t)> m_sound_uart_handler;

    // CPU/TGP produces immutable display snapshots. The worker exclusively
    // owns Model 1 display-list decode and software rasterization. Finished
    // RGBA frames then cross into the board-neutral presentation worker.
    std::mutex m_video_mutex;
    std::condition_variable m_video_ready;
    std::optional<video_job> m_pending_video_job;
    std::optional<video_result> m_completed_video_result;
    std::thread m_video_thread;
    std::vector<uint8_t> m_frame_pixels;
    std::size_t m_rendered_quad_count{};
    uint32_t m_unknown_display_list_type{};
    uint64_t m_submitted_video_frames{};
    std::atomic<uint64_t> m_completed_video_frames{};
    std::atomic<uint64_t> m_changed_video_frames{};
    uint64_t m_last_video_hash{};
    bool m_video_worker_active{};
    bool m_video_stop{};
};
