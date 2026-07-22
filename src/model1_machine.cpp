#include "model1_machine.h"

#include "model1_tgp_device.h"
#include "model1_tgp_hle.h"
#include "model1_tgp_lle.h"
#include "model1_io_board.h"
#include "model1_cabinet.h"
#include "model1_video.h"
#include "platform_paths.h"
#include "v60.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <utility>

namespace {
constexpr uint32_t address_mask = 0x00ffffff;
namespace fs = std::filesystem;

fs::path model1_nvram_path(model1_rom_set set) {
    fs::path root = whitty_platform::config_root();
    if (root.empty()) root = ".";
    const char* short_name = model1_rom_loader::set_short_name(set);
    const std::string filename = std::string(
        short_name && *short_name ? short_name : "model1-unknown") + ".nv";
    return root / "WhittyArcade" / "nvram" / filename;
}

uint8_t byte_from_word(uint16_t value, uint32_t address) {
    return static_cast<uint8_t>(value >> ((address & 1) * 8));
}

void merge_word_byte(uint16_t& target, uint32_t address, uint8_t value) {
    const unsigned shift = (address & 1) * 8;
    target = static_cast<uint16_t>((target & ~(uint16_t{0xff} << shift)) |
                                   (uint16_t{value} << shift));
}

uint8_t byte_from_dword(uint32_t value, uint32_t address) {
    return static_cast<uint8_t>(value >> ((address & 3) * 8));
}

void merge_dword_byte(uint32_t& target, uint32_t address, uint8_t value) {
    const unsigned shift = (address & 3) * 8;
    target = (target & ~(uint32_t{0xff} << shift)) |
             (uint32_t{value} << shift);
}

} // namespace

model1_machine::model1_machine() = default;
model1_machine::~model1_machine() {
    save_operator_nvram();
    stop_video_worker();
}

void model1_machine::load_operator_nvram() {
    m_operator_nvram_path = model1_nvram_path(m_rom_set).string();
    bool loaded = false;
    std::ifstream input(m_operator_nvram_path, std::ios::binary);
    if (input) {
        input.read(reinterpret_cast<char*>(m_operator_nvram.data()),
                   static_cast<std::streamsize>(m_operator_nvram.size()));
        loaded = input.gcount() ==
                 static_cast<std::streamsize>(m_operator_nvram.size());
        if (loaded && m_rom_set == model1_rom_set::virtua_formula)
            loaded = m_operator_nvram[0] == 'S' &&
                     m_operator_nvram[1] == 'E' &&
                     m_operator_nvram[2] == 'G' &&
                     m_operator_nvram[3] == 'A';
    }

    if (!loaded) {
        m_operator_nvram.fill(m_rom_set == model1_rom_set::virtua_formula ?
                              0x00 : 0xff);
        if (m_roms.default_nvram.size() >= 0x100) {
            for (std::size_t index = 0; index < m_operator_nvram.size(); ++index)
                m_operator_nvram[index] = m_roms.default_nvram[index * 2];
        }
        save_operator_nvram();
    }

    std::copy(m_operator_nvram.begin(), m_operator_nvram.end(),
              m_io_dual_port_ram.begin() + 0x100);
    std::copy(m_operator_nvram.begin(), m_operator_nvram.end(),
              m_ram_a.begin() + 0xdc80);
    std::copy(m_operator_nvram.begin(), m_operator_nvram.end(),
              m_ram_a.begin() + 0xdd00);
}

bool model1_machine::save_operator_nvram() const {
    if (m_operator_nvram_path.empty()) return false;
    const fs::path path(m_operator_nvram_path);
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    if (error) return false;

    const fs::path temporary = path.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) return false;
        output.write(reinterpret_cast<const char*>(m_operator_nvram.data()),
                     static_cast<std::streamsize>(m_operator_nvram.size()));
        if (!output.good()) return false;
    }
    fs::rename(temporary, path, error);
    if (!error) return true;
    fs::remove(path, error);
    error.clear();
    fs::rename(temporary, path, error);
    return !error;
}

bool model1_machine::attract_sound_enabled() const {
    return model1_cabinet::attract_sound_enabled(m_operator_nvram);
}

bool model1_machine::set_attract_sound_enabled(bool enabled) {
    model1_cabinet::set_attract_sound(m_operator_nvram, enabled);
    std::copy(m_operator_nvram.begin(), m_operator_nvram.end(),
              m_io_dual_port_ram.begin() + 0x100);
    std::copy(m_operator_nvram.begin(), m_operator_nvram.end(),
              m_ram_a.begin() + 0xdc80);
    std::copy(m_operator_nvram.begin(), m_operator_nvram.end(),
              m_ram_a.begin() + 0xdd00);
    m_operator_nvram_dirty = true;
    if (save_operator_nvram()) {
        m_operator_nvram_dirty = false;
        return true;
    }
    return false;
}

void model1_machine::commit_operator_nvram() {
    // The V60 has already copied the edited descriptor, including its new
    // checksum, into I/O DPRAM byte by byte. write_program_byte mirrors those
    // writes into m_operator_nvram; command 2 is the durable commit barrier.
    if (save_operator_nvram()) m_operator_nvram_dirty = false;
}

bool model1_machine::initialize(const std::string& rom_path,
                                std::string* error) {
    model1_rom_load_result loaded = model1_rom_loader::load(rom_path);
    if (!loaded) {
        if (error) *error = loaded.error;
        return false;
    }
    m_rom_set = loaded.set;
    m_roms = std::move(loaded.roms);

    m_ram_a.assign(0x10000, 0);
    m_ram_b.assign(0x40000, 0);
    m_display_list_0.assign(0x10000, 0);
    m_display_list_1.assign(0x10000, 0);
    m_tile_ram.assign(0x10000, 0);
    m_character_ram.assign(0x80000, 0);
    m_palette_ram.assign(0x4000, 0);
    m_color_translation.assign(0xc000, 0);
    m_io_dual_port_ram.assign(0x800, 0);
    // Model 1 I/O-board DPRAM is byte-wide on the low lane of the V60 bus.
    // Seed neutral controls and the firmware's idle-high inputs.
    m_io_dual_port_ram[0x00] = 0x80; // steering centre
    m_io_dual_port_ram[0x01] = 0x30; // accelerator released
    m_io_dual_port_ram[0x02] = 0x30; // brake released
    std::fill(m_io_dual_port_ram.begin() + 0x03,
              m_io_dual_port_ram.begin() + 0x08, 0x80);
    m_io_dual_port_ram[0x08] = 0xff;
    m_io_dual_port_ram[0x09] = 0xff;
    m_io_dual_port_ram[0x0a] = 0xff;
    m_io_dual_port_ram[0x0b] = 0xff;
    m_io_dual_port_ram[0x0c] = 0xff;
    m_io_dual_port_ram[0x0d] = 0xff;
    m_io_dual_port_ram[0x0e] = 0xff;
    m_io_dual_port_ram[0x21] = 0x80; // command interface idle

    // Load the I/O-board EEPROM after RAM exists. Factory defaults are used
    // only for a cabinet with no saved operator settings.
    load_operator_nvram();
    if (!m_roms.io_cpu.empty() &&
        (m_rom_set == model1_rom_set::virtua_fighter ||
         m_rom_set == model1_rom_set::star_wars_arcade)) {
        m_io_board = std::make_unique<model1_io_board>(
            model1_io_board_type::standard_315_5338a, m_roms.io_cpu,
            m_io_dual_port_ram, m_operator_nvram);
    }
    m_communication_ram.assign(0x1004, 0);
    if (m_roms.legacy_tgp.size() == 0x2000) {
        m_tgp = std::make_unique<model1_tgp_lle>(
            m_roms.legacy_tgp, m_roms.copro_data, m_roms.lookup_tables,
            m_rom_set == model1_rom_set::virtua_formula,
            m_rom_set == model1_rom_set::virtua_fighter ||
            m_rom_set == model1_rom_set::star_wars_arcade ||
            m_rom_set == model1_rom_set::wing_war);
    } else {
        auto hle = std::make_unique<model1_tgp_hle>();
        hle->set_copro_data(m_roms.copro_data);
        m_tgp = std::move(hle);
    }
    m_video = std::make_unique<model1_video>();
    m_video->set_polygon_rom(m_roms.polygon_data);

    m_main_cpu = std::make_unique<v60_device>();
    if (std::getenv("MODEL1_BUS_TRACE")) m_bus_trace_remaining = 200;
    m_main_cpu->set_program_callbacks(
        [this](uint32_t address) { return read_program_byte(address); },
        [this](uint32_t address, uint8_t value) {
            write_program_byte(address, value);
        });
    m_main_cpu->set_io_callbacks(
        [this](uint32_t address) { return read_io_byte(address); },
        [this](uint32_t address, uint8_t value) {
            write_io_byte(address, value);
        });
    m_main_cpu->set_standalone_irq_callback([this]() {
        for (uint8_t level = 0; level < 8; ++level) {
            if ((m_irq_status & (uint8_t{1} << level)) == 0) continue;
            m_last_irq = level;
            return static_cast<int>(level);
        }
        return 0;
    });
    m_main_cpu->standalone_start();
    m_video_thread = std::thread(&model1_machine::video_worker_loop, this);
    return true;
}

void model1_machine::reset() {
    m_rom_o_bank = 0;
    m_list_control[0] = 0;
    m_list_control[1] = 0;
    m_copro_address = 0;
    m_copro_write_latch = 0;
    m_copro_read_half_latch = 0xffff;
    m_copro_read_latch = 0xffffffff;
    m_fifo_write_latch = 0;
    m_fifo_read_latch = 0xffffffff;
    m_tgp_command_count = 0;
    m_irq_status = 0;
    m_irq_mask = 0xff;
    m_last_irq = 0;
    m_timer_mode = 0;
    m_timer_period[0] = m_timer_period[1] = 0;
    m_timer_remaining[0] = m_timer_remaining[1] = 0;
    m_cycle_remainder = 0;
    m_io_cycle_remainder = 0;
    m_total_v60_cycles = 0;
    m_uart_busy_until = 0;
    if (m_main_cpu) m_main_cpu->set_input_line(0, CLEAR_LINE);
    if (m_tgp) m_tgp->reset();
    if (m_io_board) m_io_board->reset();
    if (m_main_cpu) m_main_cpu->standalone_reset();
}

int model1_machine::execute(int cycles) {
    const int executed = m_main_cpu ? m_main_cpu->standalone_execute(cycles) : 0;
    if (m_io_board && executed > 0) {
        // Standard I/O Z80 is 4 MHz while the main V60 is 16 MHz.
        m_io_cycle_remainder += static_cast<double>(executed) * 0.25;
        const int io_clocks = static_cast<int>(m_io_cycle_remainder);
        m_io_cycle_remainder -= io_clocks;
        m_io_board->execute(io_clocks);
    }
    return executed;
}

void model1_machine::set_inputs(const input_state& state) {
    // The Model 1 I/O Z80 normally samples the cabinet and publishes these
    // bytes in dual-port RAM. The native backend currently models that
    // transaction directly, while retaining the real mailbox layout used by
    // the V60 program.
    if (m_io_board && m_io_board->active()) {
        m_io_board->set_inputs(m_rom_set, state);
        return;
    }
    if (m_rom_set == model1_rom_set::wing_war) {
        m_io_dual_port_ram[0x00] =
            static_cast<uint8_t>(0xff - state.left_stick_x);
        m_io_dual_port_ram[0x01] = state.left_stick_y;
        m_io_dual_port_ram[0x02] = static_cast<uint8_t>(1 +
            std::clamp<int>(state.gas, 0, 0x610) * 0xfe / 0x610);

        uint8_t in0 = 0xff;
        if (state.coin1) in0 &= ~uint8_t{0x01};
        if (state.test) in0 &= ~uint8_t{0x04};
        if (state.service) in0 &= ~uint8_t{0x08};
        if (state.start) in0 &= ~uint8_t{0x10};
        if (state.view) in0 &= ~uint8_t{0x20};
        if (state.view2) in0 &= ~uint8_t{0x40};
        if (state.view3) in0 &= ~uint8_t{0x80};
        uint8_t in1 = 0xff;
        if (state.view4) in1 &= ~uint8_t{0x01};
        if (state.shift_down) in1 &= ~uint8_t{0x10}; // machine gun
        if (state.shift_up) in1 &= ~uint8_t{0x20};   // missile
        if (state.brake > 0x100) in1 &= ~uint8_t{0x40}; // smoke
        m_io_dual_port_ram[0x08] = in0;
        m_io_dual_port_ram[0x09] = in1;
        m_io_dual_port_ram[0x0a] = 0xff;
        return;
    }

    if (m_rom_set == model1_rom_set::star_wars_arcade) {
        // Star Wars' two-player yoke cabinet publishes five analog channels.
        // Expose player one on the left stick and triggers; player two stays
        // neutral until the shared input state grows a second full yoke.
        m_io_dual_port_ram[0x00] = static_cast<uint8_t>(0xff - state.left_stick_x);
        m_io_dual_port_ram[0x01] = state.left_stick_y;
        m_io_dual_port_ram[0x02] = static_cast<uint8_t>(0xc8 -
            std::clamp<int>(state.gas, 0, 0x610) * (0xc8 - 0x1c) / 0x610);
        m_io_dual_port_ram[0x03] = 0x7f;
        m_io_dual_port_ram[0x04] = 0x7f;

        uint8_t in0 = 0xff;
        if (state.coin1) in0 &= ~uint8_t{0x01};
        if (state.coin2) in0 &= ~uint8_t{0x02};
        if (state.test) in0 &= ~uint8_t{0x04};
        if (state.service) in0 &= ~uint8_t{0x08};
        if (state.start) in0 &= ~uint8_t{0x10};
        uint8_t in1 = 0xff;
        if (state.view) in1 &= ~uint8_t{0x01};
        if (state.view2) in1 &= ~uint8_t{0x02};
        if (state.view3) in1 &= ~uint8_t{0x10};
        m_io_dual_port_ram[0x08] = in0;
        m_io_dual_port_ram[0x09] = in1;
        m_io_dual_port_ram[0x0a] = 0xff;
        return;
    }

    if (m_rom_set == model1_rom_set::virtua_fighter) {
        uint8_t in0 = 0xff;
        if (state.coin1) in0 &= ~uint8_t{0x01};
        if (state.coin2) in0 &= ~uint8_t{0x02};
        if (state.test) in0 &= ~uint8_t{0x04};
        if (state.service) in0 &= ~uint8_t{0x08};
        if (state.start) in0 &= ~uint8_t{0x10};

        uint8_t in1 = 0xff;
        // Existing cabinet buttons become VF Punch, Kick and Guard. This
        // keeps keyboard V/B/N and controller B/X/Y consistent across games.
        if (state.view) in1 &= ~uint8_t{0x01};
        if (state.view2) in1 &= ~uint8_t{0x02};
        if (state.view3) in1 &= ~uint8_t{0x04};
        if (state.left_stick_y > 0xaf) in1 &= ~uint8_t{0x10};
        if (state.left_stick_y < 0x50) in1 &= ~uint8_t{0x20};
        if (state.left_stick_x > 0xaf) in1 &= ~uint8_t{0x40};
        if (state.left_stick_x < 0x50) in1 &= ~uint8_t{0x80};

        m_io_dual_port_ram[0x08] = in0;
        m_io_dual_port_ram[0x09] = in1;
        m_io_dual_port_ram[0x0a] = 0xff; // player two is not yet exposed
        return;
    }

    const int steering = static_cast<int>(state.steering) - 0x280;
    m_io_dual_port_ram[0x00] = static_cast<uint8_t>(std::clamp(
        steering * 255 / (0xd80 - 0x280), 0, 255));
    m_io_dual_port_ram[0x01] = static_cast<uint8_t>(0x30 +
        std::clamp<int>(state.gas, 0, 0x610) * (0xff - 0x30) / 0x610);
    m_io_dual_port_ram[0x02] = static_cast<uint8_t>(0x30 +
        std::clamp<int>(state.brake, 0, 0x610) * (0xff - 0x30) / 0x610);

    uint8_t in0 = 0xff;
    if (state.coin1) in0 &= ~uint8_t{0x01};
    if (state.coin2) in0 &= ~uint8_t{0x02};
    if (state.test) in0 &= ~uint8_t{0x04};
    if (state.service) in0 &= ~uint8_t{0x08};
    if (state.start) in0 &= ~uint8_t{0x10};
    if (state.view) in0 &= ~uint8_t{0x20};
    if (state.view2) in0 &= ~uint8_t{0x40};
    if (state.view3) in0 &= ~uint8_t{0x80};

    uint8_t in1 = 0xff;
    if (state.view4) in1 &= ~uint8_t{0x01};
    if (state.shift_down) in1 &= ~uint8_t{0x10};
    if (state.shift_up) in1 &= ~uint8_t{0x20};
    m_io_dual_port_ram[0x08] = in0;
    m_io_dual_port_ram[0x09] = in1;
}

int model1_machine::run_frame() {
    // Publish a finished render without waiting for the graphics worker. The
    // previous front buffer stays immutable until this producer thread swaps
    // in a newer completed result.
    harvest_video_result();

    // One complete Model 1 raster frame is 656 * 424 V60 clocks. The TGP is
    // clocked at 40 MHz, so it receives 2.5 geometry cycles for every V60
    // cycle (695,360 per frame). This ratio is load-bearing: scheduling both
    // devices at 16 MHz leaves scene construction far behind gameplay.
    m_cycle_remainder += 656.0 * 424.0;
    const int cycles = static_cast<int>(m_cycle_remainder);
    m_cycle_remainder -= cycles;
    // Both processors run at 16 MHz. Interleave them so FIFO producer and
    // consumer wake one another promptly instead of running a whole frame
    // apart. This matters once the driving simulation begins issuing many
    // short geometry requests.
    constexpr int slices = 512;
    int executed = 0;
    int remaining = cycles;
    int tgp_remaining = cycles * 5 / 2;
    for (int slice = 0; slice < slices; ++slice) {
        if (slice == slices / 2 && !(m_irq_mask & (uint8_t{1} << 3))) {
            // Mid-frame sound-board interrupt (level 3).
            m_irq_status |= uint8_t{1} << 3;
            if (m_main_cpu) m_main_cpu->set_input_line(0, ASSERT_LINE);
        }
        const int chunk = remaining / (slices - slice);
        const int tgp_chunk = tgp_remaining / (slices - slice);
        const int cpu_executed = execute(chunk);
        executed += cpu_executed;
        m_total_v60_cycles += static_cast<uint64_t>(
            std::max(cpu_executed, 0));
        for (unsigned timer = 0; timer < 2; ++timer) {
            if (!m_timer_period[timer]) continue;
            m_timer_remaining[timer] -= std::max(cpu_executed, 0);
            const int64_t reload =
                static_cast<int64_t>(0x800) * m_timer_period[timer];
            if (m_timer_remaining[timer] <= 0) {
                do m_timer_remaining[timer] += reload;
                while (m_timer_remaining[timer] <= 0);
                if (!(m_irq_mask & 1)) {
                    m_irq_status |= 1;
                    if (m_main_cpu)
                        m_main_cpu->set_input_line(0, ASSERT_LINE);
                }
            }
        }
        if (m_tgp) m_tgp->execute(tgp_chunk);
        remaining -= chunk;
        tgp_remaining -= tgp_chunk;
    }

    queue_video_frame();

    // Vblank is level 1 and is accepted at the next CPU slice/frame.
    if (!(m_irq_mask & (uint8_t{1} << 1))) {
        m_irq_status |= uint8_t{1} << 1;
        if (m_main_cpu) m_main_cpu->set_input_line(0, ASSERT_LINE);
    }
    // EEPROM updates arrive as several byte writes. Flush once at the frame
    // boundary so the complete game-generated record and checksum survive
    // even if the emulator is not closed through the menu.
    if (m_operator_nvram_dirty && save_operator_nvram())
        m_operator_nvram_dirty = false;
    return executed;
}

void model1_machine::queue_video_frame() {
    if (!m_video || !m_video_thread.joinable()) return;

    // Copy only at the hardware frame boundary. The V60 is then free to
    // construct the next list while the graphics worker decodes this stable
    // snapshot, with no shared-memory races or renderer-side locks.
    video_job job{
        m_display_list_0,
        m_display_list_1,
        m_tile_ram,
        m_character_ram,
        m_palette_ram,
        m_color_translation,
        m_list_control[0],
        m_list_control[1],
    };

    // List-bank selection is live hardware state. Compute the same transition
    // performed by model1_video immediately on the producer side so the V60
    // does not need to wait for rasterization before choosing its next bank.
    uint16_t effective_control = job.list_control_0;
    if ((job.list_control_1 & 0x1f) == 0x1f &&
        !(effective_control & 4)) {
        effective_control = static_cast<uint16_t>(
            (effective_control & ~uint16_t{0x40}) |
            ((effective_control & 8) ? 0x40 : 0));
    }
    if ((effective_control & 4) && (m_submitted_video_frames & 1))
        effective_control ^= 0x40;
    m_list_control[0] = effective_control;
    ++m_submitted_video_frames;

    std::unique_lock<std::mutex> lock(m_video_mutex);
    // One active and one pending snapshot form a bounded pipeline. If an
    // unusually expensive scene exceeds that capacity, apply backpressure
    // instead of dropping a display-list upload required by later frames.
    m_video_ready.wait(lock, [this] {
        return m_video_stop || !m_pending_video_job.has_value();
    });
    if (m_video_stop) return;
    m_pending_video_job.emplace(std::move(job));
    lock.unlock();
    m_video_ready.notify_all();
}

void model1_machine::video_worker_loop() {
    for (;;) {
        video_job job;
        {
            std::unique_lock<std::mutex> lock(m_video_mutex);
            m_video_ready.wait(lock, [this] {
                return m_video_stop || m_pending_video_job.has_value();
            });
            if (m_video_stop && !m_pending_video_job) break;
            job = std::move(*m_pending_video_job);
            m_pending_video_job.reset();
            m_video_worker_active = true;
        }
        m_video_ready.notify_all();

        m_video->render(job.display_list_0, job.display_list_1,
                        job.tile_ram, job.character_ram, job.palette_ram,
                        job.color_translation, job.list_control_0,
                        job.list_control_1);

        video_result result;
        result.pixels = m_video->rgba_pixels();
        result.quad_count = m_video->last_quad_count();
        result.unknown_list_type = m_video->last_unknown_list_type();

        // Track actual output cadence separately from the emulated raster
        // clock. This exposes repeated display-list frames instead of
        // incorrectly reporting CPU scheduler frequency as graphics FPS.
        uint64_t hash = 1469598103934665603ull;
        for (uint8_t byte : result.pixels) {
            hash ^= byte;
            hash *= 1099511628211ull;
        }
        m_completed_video_frames.fetch_add(1, std::memory_order_relaxed);
        if (hash != m_last_video_hash) {
            m_last_video_hash = hash;
            m_changed_video_frames.fetch_add(1, std::memory_order_relaxed);
        }
        {
            std::lock_guard<std::mutex> lock(m_video_mutex);
            m_completed_video_result.emplace(std::move(result));
            m_video_worker_active = false;
        }
        m_video_ready.notify_all();
    }
}

void model1_machine::harvest_video_result() {
    std::lock_guard<std::mutex> lock(m_video_mutex);
    if (!m_completed_video_result) return;
    m_frame_pixels = std::move(m_completed_video_result->pixels);
    m_rendered_quad_count = m_completed_video_result->quad_count;
    m_unknown_display_list_type =
        m_completed_video_result->unknown_list_type;
    m_completed_video_result.reset();
}

void model1_machine::wait_for_video() {
    if (!m_video_thread.joinable()) return;
    {
        std::unique_lock<std::mutex> lock(m_video_mutex);
        m_video_ready.wait(lock, [this] {
            return m_video_stop ||
                (!m_pending_video_job && !m_video_worker_active);
        });
    }
    harvest_video_result();
}

void model1_machine::stop_video_worker() {
    if (!m_video_thread.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(m_video_mutex);
        m_video_stop = true;
        m_pending_video_job.reset();
    }
    m_video_ready.notify_all();
    m_video_thread.join();
}

const std::vector<uint8_t>& model1_machine::frame_pixels() {
    harvest_video_result();
    return m_frame_pixels;
}

std::size_t model1_machine::display_list_nonzero_bytes(unsigned index) const {
    const std::vector<uint8_t>* list = index == 0 ? &m_display_list_0 :
        (index == 1 ? &m_display_list_1 : nullptr);
    return list ? static_cast<std::size_t>(std::count_if(
        list->begin(), list->end(), [](uint8_t value) { return value != 0; })) : 0;
}

std::size_t model1_machine::palette_nonzero_bytes() const {
    return static_cast<std::size_t>(std::count_if(
        m_palette_ram.begin(), m_palette_ram.end(),
        [](uint8_t value) { return value != 0; }));
}

uint16_t model1_machine::display_list_word(unsigned index,
                                           std::size_t word) const {
    const std::vector<uint8_t>* list = index == 0 ? &m_display_list_0 :
        (index == 1 ? &m_display_list_1 : nullptr);
    const std::size_t offset = word * 2;
    if (!list || offset + 1 >= list->size()) return 0;
    return static_cast<uint16_t>((*list)[offset]) |
           (static_cast<uint16_t>((*list)[offset + 1]) << 8);
}

std::size_t model1_machine::rendered_quad_count() const {
    return m_rendered_quad_count;
}

uint32_t model1_machine::unknown_display_list_type() const {
    return m_unknown_display_list_type;
}

uint64_t model1_machine::tgp_unimplemented_count() const {
    return m_tgp ? m_tgp->unimplemented_count() : 0;
}

uint16_t model1_machine::tgp_geometry_pc() const {
    return m_tgp ? m_tgp->geometry_pc() : 0;
}

bool model1_machine::io_board_active() const {
    return m_io_board && m_io_board->active();
}

uint16_t model1_machine::io_board_pc() const {
    return m_io_board ? m_io_board->program_counter() : 0;
}

uint64_t model1_machine::io_board_clocks() const {
    return m_io_board ? m_io_board->executed_clocks() : 0;
}

bool model1_machine::tgp_uses_lle() const {
    return m_tgp && m_tgp->is_lle();
}

uint32_t model1_machine::tgp_last_unimplemented_function() const {
    return m_tgp ? m_tgp->last_unimplemented_function() : 0;
}

uint32_t model1_machine::tgp_last_unimplemented_pc() const {
    return m_tgp ? m_tgp->last_unimplemented_pc() : 0;
}

uint32_t model1_machine::program_counter() const {
    return m_main_cpu ? m_main_cpu->program_counter() : 0;
}

std::string model1_machine::disassemble(uint32_t address,
                                        std::size_t* length) const {
    return m_main_cpu ? m_main_cpu->disassemble(address, length) : std::string{};
}

uint8_t model1_machine::read_region(const std::vector<uint8_t>& region,
                                    uint32_t base, uint32_t address) {
    const uint32_t offset = address - base;
    return offset < region.size() ? region[offset] : 0xff;
}

bool model1_machine::write_region(std::vector<uint8_t>& region, uint32_t base,
                                  uint32_t address, uint8_t value) {
    const uint32_t offset = address - base;
    if (offset >= region.size()) return false;
    region[offset] = value;
    return true;
}

uint8_t model1_machine::read_program_byte(uint32_t raw_address) {
    const uint32_t address = raw_address & address_mask;
    if (m_bus_trace_remaining && m_main_cpu) {
        const uint32_t pc = m_main_cpu->program_counter() & address_mask;
        if (pc >= 0xfe1500 && pc < 0xfe1600) {
            std::fprintf(stderr, "M1 read PC=%06x address=%06x\n", pc,
                         address);
            --m_bus_trace_remaining;
        }
    }
    if (address <= 0x0fffff)
        return m_roms.main_cpu[address];
    if (address >= 0x100000 && address <= 0x1fffff) {
        const uint32_t source = 0x1000000 +
            static_cast<uint32_t>(m_rom_o_bank) * 0x100000 +
            (address - 0x100000);
        return source < m_roms.main_cpu.size() ? m_roms.main_cpu[source] : 0xff;
    }
    if (address >= 0x200000 && address <= 0x2fffff)
        return m_roms.main_cpu[address];
    if (address >= 0x400000 && address <= 0x40ffff)
        return read_region(m_ram_a, 0x400000, address);
    if (address >= 0x500000 && address <= 0x53ffff)
        return read_region(m_ram_b, 0x500000, address);
    if (address >= 0x600000 && address <= 0x60ffff)
        return read_region(m_display_list_0, 0x600000, address);
    if (address >= 0x610000 && address <= 0x61ffff)
        return read_region(m_display_list_1, 0x610000, address);
    if (address >= 0x680000 && address <= 0x680003) {
        const unsigned index = (address >> 1) & 1;
        const uint16_t value = index == 0 ?
            static_cast<uint16_t>(m_list_control[0] | 0x30) :
            m_list_control[1];
        return byte_from_word(value, address);
    }
    if (address >= 0x700000 && address <= 0x70ffff)
        return read_region(m_tile_ram, 0x700000, address);
    if (address >= 0x780000 && address <= 0x7fffff)
        return read_region(m_character_ram, 0x780000, address);
    if (address >= 0x900000 && address <= 0x903fff)
        return read_region(m_palette_ram, 0x900000, address);
    if (address >= 0x910000 && address <= 0x91bfff)
        return read_region(m_color_translation, 0x910000, address);
    if (address >= 0xb00000 && address <= 0xb01003)
        return read_region(m_communication_ram, 0xb00000, address);
    if (address >= 0xc40000 && address <= 0xc40003) {
        // Main-board uPD71051: low byte is on the V60's even lane. At
        // 31.25 kbaud a ten-bit character occupies 5,120 V60 clocks.
        if (address & 1) return 0;
        if (address & 2) {
            return m_total_v60_cycles >= m_uart_busy_until ?
                uint8_t{0x05} : uint8_t{0x00}; // TXRDY | TXEMPTY
        }
        return 0;
    }
    if (address >= 0xc00000 && address <= 0xc00fff)
        return (address & 1) == 0 ?
            m_io_dual_port_ram[(address - 0xc00000) >> 1] : 0x00;
    if (address >= 0xd00000 && address <= 0xd1ffff) {
        // Formula's V60 polls RAM address zero as a geometry-completion
        // barrier.  On the board, and in the original driver, that poll
        // yields roughly 100 us to the TGP when FIFO work is pending.  A
        // plain memory read here allowed the V60 to consume the previous
        // frame's track/car result, which manifested as detached cars and a
        // camera occasionally falling through the road.
        if ((address & 1) == 0 && m_tgp && m_tgp->ram_address() == 0 &&
            m_tgp->input_pending())
            m_tgp->execute(4000);
        return byte_from_word(m_tgp ? m_tgp->ram_address() : 0xffff, address);
    }
    if (address >= 0xd20000 && address <= 0xd3ffff) {
        const unsigned byte = address & 3;
        if ((byte & 1) == 0) {
            const bool completion_word = m_tgp &&
                m_tgp->ram_address() == 0 && (byte >> 1) == 1;
            m_copro_read_half_latch = m_tgp ?
                m_tgp->read_ram_half(byte >> 1) : 0xffff;
            // 0xffffffff in shared RAM word zero is the TGP's busy marker.
            // Give it the same 100 us window before returning that marker to
            // the V60, then sample the non-incrementing address again.
            if (completion_word && m_copro_read_half_latch == 0xffff) {
                m_tgp->execute(4000);
                m_copro_read_half_latch = m_tgp->read_ram_half(byte >> 1);
            }
        }
        return byte_from_word(m_copro_read_half_latch, address);
    }
    if (address >= 0xd80000 && address <= 0xd9ffff) {
        if ((address & 3) == 0 && m_tgp && m_tgp->output_ready())
            m_fifo_read_latch = m_tgp->pop_output();
        const uint8_t result = byte_from_dword(m_fifo_read_latch, address);
        return result;
    }
    if (address >= 0xdc0000 && address <= 0xddffff)
        return 0xff; // Input FIFO is always ready from the V60's perspective.
    if (address == 0xe00002) return m_irq_mask;
    if (address == 0xe00003) return 0;
    if (address >= 0xe0000c && address <= 0xe0000f) {
        const unsigned timer = (address - 0xe0000c) >> 1;
        const uint16_t value = m_timer_period[timer] ?
            static_cast<uint16_t>(std::max<int64_t>(
                m_timer_remaining[timer], 0) / 0x800) : 0;
        return byte_from_word(value, address);
    }
    if (address >= 0xf80000)
        return m_roms.main_cpu[address];
    return 0xff;
}

void model1_machine::write_program_byte(uint32_t raw_address, uint8_t value) {
    const uint32_t address = raw_address & address_mask;
    if (address >= 0x400000 && address <= 0x40ffff) {
        if (std::getenv("MODEL1_CONFIG_TRACE") &&
            address >= 0x40dc80 && address < 0x40dd80 &&
            m_ram_a[address - 0x400000] != value) {
            std::fprintf(stderr, "Model 1 cabinet RAM %06x: %02x -> %02x\n",
                         address, m_ram_a[address - 0x400000], value);
        }
        write_region(m_ram_a, 0x400000, address, value);
        return;
    }
    if (address >= 0x500000 && address <= 0x53ffff) {
        write_region(m_ram_b, 0x500000, address, value);
        return;
    }
    if (address >= 0x600000 && address <= 0x60ffff) {
        write_region(m_display_list_0, 0x600000, address, value);
        return;
    }
    if (address >= 0x610000 && address <= 0x61ffff) {
        write_region(m_display_list_1, 0x610000, address, value);
        return;
    }
    if (address >= 0x680000 && address <= 0x680003) {
        merge_word_byte(m_list_control[(address >> 1) & 1], address, value);
        return;
    }
    if (address >= 0x700000 && address <= 0x70ffff) {
        write_region(m_tile_ram, 0x700000, address, value);
        return;
    }
    if (address >= 0x780000 && address <= 0x7fffff) {
        write_region(m_character_ram, 0x780000, address, value);
        return;
    }
    if (address >= 0x900000 && address <= 0x903fff) {
        write_region(m_palette_ram, 0x900000, address, value);
        return;
    }
    if (address >= 0x910000 && address <= 0x91bfff) {
        write_region(m_color_translation, 0x910000, address, value);
        return;
    }
    if (address >= 0xb00000 && address <= 0xb01003) {
        write_region(m_communication_ram, 0xb00000, address, value);
        return;
    }
    if (address >= 0xc40000 && address <= 0xc40003) {
        if ((address & 1) == 0 && (address & 2) == 0) {
            m_uart_busy_until = m_total_v60_cycles + 5120;
            if (m_sound_uart_handler)
                m_sound_uart_handler(value, m_total_v60_cycles);
        }
        return; // mode/command writes are accepted by the UART.
    }
    if (address >= 0xc00000 && address <= 0xc00fff) {
        if ((address & 1) == 0) {
            const std::size_t offset = (address - 0xc00000) >> 1;
            if (std::getenv("MODEL1_CONFIG_TRACE") && offset >= 0x100 &&
                offset < 0x180 && m_io_dual_port_ram[offset] != value) {
                std::fprintf(stderr,
                             "Model 1 cabinet NVRAM %03zx: %02x -> %02x\n",
                             offset, m_io_dual_port_ram[offset], value);
            }
            m_io_dual_port_ram[offset] = value;
            if (offset >= 0x100 && offset < 0x180) {
                m_operator_nvram[offset - 0x100] = value;
                m_operator_nvram_dirty = true;
            }
            // Host-to-I/O command mailbox. The compatibility backend models
            // the firmware transaction atomically; board timing can be added
            // later without changing the visible mailbox protocol.
            if (!m_io_board && offset == 0x20) {
                if (value != 0) {
                    m_io_dual_port_ram[0x22] = value;
                    m_io_dual_port_ram[0x23] = 0;
                    m_io_dual_port_ram[0x24] = 0;
                    m_io_dual_port_ram[0x25] = 0;
                    m_io_dual_port_ram[0x21] = 0x40;
                    if (value == 0x02) {
                        commit_operator_nvram();
                        m_ram_a[0xdd20] = 1;
                        m_ram_a[0xdd30] = 1;
                        m_ram_a[0xdd31] = 1;
                    }
                    m_io_dual_port_ram[0x20] = 0;
                } else {
                    m_io_dual_port_ram[0x21] = 0x80;
                }
            }
        }
        return;
    }
    if (address >= 0xd00000 && address <= 0xd1ffff) {
        merge_word_byte(m_copro_address, address, value);
        if ((address & 1) != 0 && m_tgp)
            m_tgp->set_ram_address(m_copro_address);
        return;
    }
    if (address >= 0xd20000 && address <= 0xd3ffff) {
        merge_dword_byte(m_copro_write_latch, address, value);
        if ((address & 3) == 1 && m_tgp)
            m_tgp->write_ram_half(0, static_cast<uint16_t>(m_copro_write_latch));
        if ((address & 3) == 3 && m_tgp)
            m_tgp->write_ram_half(1, static_cast<uint16_t>(m_copro_write_latch >> 16));
        return;
    }
    if (address >= 0xd80000 && address <= 0xd9ffff) {
        merge_dword_byte(m_fifo_write_latch, address, value);
        if ((address & 3) == 3) {
            ++m_tgp_command_count;
            if (m_tgp)
                m_tgp->push(m_fifo_write_latch,
                            m_main_cpu ? m_main_cpu->program_counter() : 0);
        }
        return;
    }
    if (address == 0xe00000) {
        if (value == 0x10) {
            m_irq_status = 0;
        } else if (value == 0x20) {
            m_irq_status &= static_cast<uint8_t>(~(uint8_t{1} << m_last_irq));
        }
        if (m_irq_status == 0 && m_main_cpu)
            m_main_cpu->set_input_line(0, CLEAR_LINE);
        return;
    }
    if (address == 0xe00002) {
        m_irq_mask = value;
        if (m_irq_status == 0 && m_main_cpu)
            m_main_cpu->set_input_line(0, CLEAR_LINE);
        return;
    }
    if (address >= 0xe00004 && address <= 0xe00005 &&
        (address & 1) == 0 && (value & 0x0f) == 1) {
        m_rom_o_bank = static_cast<uint8_t>((value >> 4) & 7);
        return;
    }
    if (address >= 0xe00006 && address <= 0xe00007) {
        merge_word_byte(m_timer_mode, address, value);
        return;
    }
    if (address >= 0xe00008 && address <= 0xe0000b) {
        const unsigned timer = (address - 0xe00008) >> 1;
        merge_word_byte(m_timer_period[timer], address, value);
        m_timer_remaining[timer] =
            static_cast<int64_t>(0x800) * m_timer_period[timer];
        return;
    }
}

uint8_t model1_machine::read_io_byte(uint32_t address) {
    const uint32_t masked = address & address_mask;
    if (masked >= 0xd80000 && masked <= 0xd9ffff &&
        (masked & 3) == 0 && m_tgp && !m_tgp->output_ready()) {
        // V60 IN instructions are retryable. Stalling here matches the real
        // FIFO handshake and keeps an empty read from becoming 0xffffffff.
        if (m_main_cpu) m_main_cpu->stall();
        return 0xff;
    }
    return read_program_byte(address);
}

void model1_machine::write_io_byte(uint32_t address, uint8_t value) {
    write_program_byte(address, value);
}
