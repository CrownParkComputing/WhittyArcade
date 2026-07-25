#include "sega/model2/model2_machine.h"

#include "sega/model2/model2_bus.h"
#include "sega/model2/model2_video.h"
#include "i960.h"
#include "mb86233_native.h"

#include <algorithm>
#include <cstdio>
#include <array>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <utility>

struct model2_machine::implementation {
    model2_bus bus;
    i80960kb_device cpu;
    mb86233_native_core tgp;
    model2_geometry geometry;
    model2_video video;
    bool tgp_running{};
    uint64_t rendered_video_generation{};
    double i960_work_ms{};
    double tgp_work_ms{};
    int i960_cycle_debt{};
    int rival_write_trace_start{-1};
    int host_fifo_trace_start{-1};
};

model2_machine::model2_machine()
    : m_impl(std::make_unique<implementation>()) {}

model2_machine::~model2_machine() = default;

bool model2_machine::initialize(const std::string& rom_path) {
    if (const char* trace_start = std::getenv("MODEL2_RIVAL_WRITE_TRACE"))
        m_impl->rival_write_trace_start = std::atoi(trace_start);
    if (const char* trace_start = std::getenv("MODEL2_HOST_FIFO_TRACE"))
        m_impl->host_fifo_trace_start = std::atoi(trace_start);
    model2_rom_load_result loaded = model2_rom_loader::load(rom_path);
    if (!loaded) {
        std::fputs(loaded.error.c_str(), stderr);
        return false;
    }
    m_set = loaded.set;
    m_roms = std::move(loaded.roms);
    m_impl->bus.attach(m_roms, model2_rom_loader::profile_for(m_roms.set));
    m_impl->bus.set_unmapped_callback(
        [this](bool write, uint32_t address) {
            std::printf("Model 2 unmapped %s %08x at i960 %08x: %s\n",
                        write ? "write" : "read", address,
                        m_impl->cpu.previous_program_counter(),
                        m_impl->cpu.disassemble(
                            m_impl->cpu.previous_program_counter()).c_str());
        });
    const auto cpu_read8 = [this](uint32_t address) {
            // Video-control polls intentionally execute normally. Sega
            // Rally retains their iteration count as part of its render
            // budget; shortcutting the loop prevents rivals from entering.
            if (address >= 0x00884000 && address <= 0x00887fff &&
                (address & 3) == 0 &&
                !m_impl->bus.tgp_output_available())
                m_impl->cpu.i960_stall();
            return m_impl->bus.read8(address);
        };
    m_impl->cpu.set_program_callbacks(
        cpu_read8,
        [this](uint32_t address, uint8_t value) {
            if (m_impl->rival_write_trace_start >= 0 &&
                static_cast<int>(m_frame_number) >=
                    m_impl->rival_write_trace_start &&
                address >= 0x002139f0 && address < 0x00213a60) {
                std::printf("RIVAL-WRITE frame=%llu address=%08x "
                            "value=%02x size=1 pc=%08x\n",
                            static_cast<unsigned long long>(m_frame_number),
                            address, value,
                            m_impl->cpu.previous_program_counter());
            }
            if (m_impl->rival_write_trace_start >= 0 &&
                static_cast<int>(m_frame_number) >=
                    m_impl->rival_write_trace_start &&
                ((address >= 0x00500110 && address < 0x00500114) ||
                 (address >= 0x00500210 && address < 0x00500214) ||
                 (address >= 0x005001e8 && address < 0x005001ec) ||
                 (address >= 0x005002e8 && address < 0x005002ec) ||
                 (address >= 0x00213be8 && address < 0x00213bec) ||
                 (address >= 0x00213c40 && address < 0x00213c44))) {
                std::printf("RIVAL-ENTITY frame=%llu address=%08x "
                            "value=%02x size=1 pc=%08x "
                            "g0=%08x g2=%08x ac=%08x\n",
                            static_cast<unsigned long long>(m_frame_number),
                            address, value,
                            m_impl->cpu.previous_program_counter(),
                            m_impl->cpu.register_value(16),
                            m_impl->cpu.register_value(18),
                            m_impl->cpu.arithmetic_controls());
            }
            if (m_impl->rival_write_trace_start >= 0 &&
                static_cast<int>(m_frame_number) >=
                    m_impl->rival_write_trace_start &&
                ((address >= 0x00500114 && address < 0x0050011c) ||
                 (address >= 0x00500190 && address < 0x00500198) ||
                 (address >= 0x00500214 && address < 0x0050021c) ||
                 (address >= 0x00500290 && address < 0x00500298))) {
                std::printf("RIVAL-POS-WRITE frame=%llu address=%08x "
                            "value=%02x size=1 pc=%08x\n",
                            static_cast<unsigned long long>(m_frame_number),
                            address, value,
                            m_impl->cpu.previous_program_counter());
            }
            if (m_impl->rival_write_trace_start >= 0 &&
                static_cast<int>(m_frame_number) >=
                    m_impl->rival_write_trace_start &&
                address >= 0x00f00008 && address < 0x00f0000c) {
                std::printf("RIVAL-TIMER-WRITE frame=%llu address=%08x "
                            "value=%02x size=1 pc=%08x\n",
                            static_cast<unsigned long long>(m_frame_number),
                            address, value,
                            m_impl->cpu.previous_program_counter());
            }
            m_impl->bus.write8(address, value);
            if (m_impl->bus.tgp_input_words() >
                model2_bus::tgp_fifo_capacity)
                m_impl->cpu.i960_yield();
            // Model 2 multiplexes timer and device sources onto four i960
            // lines.  An acknowledge can briefly lower a shared line before
            // another source raises it. Propagate that edge during the bus
            // transaction; waiting until the scheduler slice ends loses it
            // and permanently starves the timer service (including sound).
            update_interrupt_lines();
        },
        [this](uint32_t address) {
            return m_impl->bus.access_flags(address);
        },
        [this, cpu_read8](uint32_t address) {
            const uint64_t end = static_cast<uint64_t>(address) + 3;
            const bool video_wait =
                address <= 0x0098000c && end >= 0x0098000c;
            const bool tgp_wait =
                address <= 0x00887fff && end >= 0x00884000;
            if (video_wait || tgp_wait) {
                const uint32_t value =
                    static_cast<uint32_t>(cpu_read8(address)) |
                    (static_cast<uint32_t>(cpu_read8(address + 1)) << 8) |
                    (static_cast<uint32_t>(cpu_read8(address + 2)) << 16) |
                    (static_cast<uint32_t>(cpu_read8(address + 3)) << 24);
                if (tgp_wait && m_impl->host_fifo_trace_start >= 0 &&
                    static_cast<int>(m_frame_number) >=
                        m_impl->host_fifo_trace_start)
                    std::fprintf(stderr,
                                 "%llu OUT %08x %08x pc=%08x\n",
                                 static_cast<unsigned long long>(
                                     m_frame_number),
                                 address, value,
                                 m_impl->cpu.previous_program_counter());
                return value;
            }
            const uint32_t value = m_impl->bus.read32(address);
            if (m_impl->rival_write_trace_start >= 0 &&
                static_cast<int>(m_frame_number) >=
                    m_impl->rival_write_trace_start &&
                address <= 0x00f0000b && address + 3 >= 0x00f00008) {
                std::printf("RIVAL-TIMER frame=%llu address=%08x "
                            "value=%08x pc=%08x g4=%08x g5=%08x\n",
                            static_cast<unsigned long long>(m_frame_number),
                            address, value,
                            m_impl->cpu.previous_program_counter(),
                            m_impl->cpu.register_value(20),
                            m_impl->cpu.register_value(21));
            }
            return value;
        },
        [this](uint32_t address, uint32_t value) {
            if (m_impl->rival_write_trace_start >= 0 &&
                static_cast<int>(m_frame_number) >=
                    m_impl->rival_write_trace_start &&
                address < 0x00213a60 && address + 3 >= 0x002139f0) {
                std::printf("RIVAL-WRITE frame=%llu address=%08x "
                            "value=%08x size=4 pc=%08x\n",
                            static_cast<unsigned long long>(m_frame_number),
                            address, value,
                            m_impl->cpu.previous_program_counter());
            }
            if (m_impl->rival_write_trace_start >= 0 &&
                static_cast<int>(m_frame_number) >=
                    m_impl->rival_write_trace_start &&
                ((address <= 0x00500113 && address + 3 >= 0x00500110) ||
                 (address <= 0x00500213 && address + 3 >= 0x00500210) ||
                 (address <= 0x005001eb && address + 3 >= 0x005001e8) ||
                 (address <= 0x005002eb && address + 3 >= 0x005002e8) ||
                 (address <= 0x00213beb && address + 3 >= 0x00213be8) ||
                 (address <= 0x00213c43 && address + 3 >= 0x00213c40))) {
                std::printf("RIVAL-ENTITY frame=%llu address=%08x "
                            "value=%08x size=4 pc=%08x "
                            "g0=%08x g2=%08x ac=%08x\n",
                            static_cast<unsigned long long>(m_frame_number),
                            address, value,
                            m_impl->cpu.previous_program_counter(),
                            m_impl->cpu.register_value(16),
                            m_impl->cpu.register_value(18),
                            m_impl->cpu.arithmetic_controls());
            }
            if (m_impl->rival_write_trace_start >= 0 &&
                static_cast<int>(m_frame_number) >=
                    m_impl->rival_write_trace_start &&
                ((address <= 0x0050011b && address + 3 >= 0x00500114) ||
                 (address <= 0x00500197 && address + 3 >= 0x00500190) ||
                 (address <= 0x0050021b && address + 3 >= 0x00500214) ||
                 (address <= 0x00500297 && address + 3 >= 0x00500290))) {
                std::printf("RIVAL-POS-WRITE frame=%llu address=%08x "
                            "value=%08x size=4 pc=%08x\n",
                            static_cast<unsigned long long>(m_frame_number),
                            address, value,
                            m_impl->cpu.previous_program_counter());
            }
            if (m_impl->rival_write_trace_start >= 0 &&
                static_cast<int>(m_frame_number) >=
                    m_impl->rival_write_trace_start &&
                address <= 0x00f0000b && address + 3 >= 0x00f00008) {
                std::printf("RIVAL-TIMER-WRITE frame=%llu address=%08x "
                            "value=%08x size=4 pc=%08x\n",
                            static_cast<unsigned long long>(m_frame_number),
                            address, value,
                            m_impl->cpu.previous_program_counter());
            }
            m_impl->bus.write32(address, value);
            if (m_impl->host_fifo_trace_start >= 0 &&
                static_cast<int>(m_frame_number) >=
                    m_impl->host_fifo_trace_start &&
                address >= 0x00880000 && address <= 0x00887fff) {
                uint32_t command = value;
                if (address < 0x00884000) {
                    const uint32_t function =
                        (((address & ~3U) - 0x00880000) >> 4) & 0xff;
                    command = (value & 0x800fffffU) | (function << 23);
                }
                std::fprintf(stderr,
                             "%llu IN %08x %08x pc=%08x\n",
                             static_cast<unsigned long long>(m_frame_number),
                             address, command,
                             m_impl->cpu.previous_program_counter());
            }
            if (m_impl->bus.tgp_input_words() >
                model2_bus::tgp_fifo_capacity)
                m_impl->cpu.i960_yield();
            update_interrupt_lines();
        });
    m_impl->bus.set_program_counter_probe(
        [this]() { return m_impl->cpu.previous_program_counter(); });
    m_impl->tgp.set_program_callbacks(
        [this](uint16_t address) {
            return m_impl->bus.tgp_program_read(address);
        });
    m_impl->tgp.set_data_callbacks(
        [this](uint16_t address) {
            return m_impl->bus.tgp_data_read(address);
        },
        [this](uint16_t address, uint32_t value) {
            m_impl->bus.tgp_data_write(address, value);
        });
    m_impl->tgp.set_io_callbacks(
        [this](uint16_t address) {
            return m_impl->bus.tgp_io_read(address);
        },
        [this](uint16_t address, uint32_t value) {
            m_impl->bus.tgp_io_write(address, value);
            m_impl->tgp.set_gpio0(m_impl->bus.tgp_gpio0());
        });
    m_impl->tgp.set_rf_callbacks(
        [this](uint16_t address) {
            if (address == 1 && !m_impl->bus.tgp_input_available())
                m_impl->tgp.stall();
            return m_impl->bus.tgp_rf_read(address);
        },
        [this](uint16_t address, uint32_t value) {
            m_impl->bus.tgp_rf_write(address, value);
            if (m_impl->bus.tgp_output_words() >
                model2_bus::tgp_fifo_capacity)
                m_impl->tgp.request_yield();
        });
    reset();
    return true;
}

void model2_machine::reset() {
    m_frame_number = 0;
    m_executed_cycles = 0;
    m_cycle_remainder = 0;
    m_impl->i960_cycle_debt = 0;
    m_cpu_error.clear();
    m_exported_texture_generation = 0;
    m_exported_color_generation = 0;
    m_impl->bus.reset();
    m_impl->geometry.reset();
    m_impl->tgp_running = false;
    try {
        m_impl->cpu.standalone_start();
        std::printf("Model 2 i960 reset: IP=%08x %s\n",
                    m_impl->cpu.program_counter(),
                    m_impl->cpu.disassemble(
                        m_impl->cpu.program_counter()).c_str());
    } catch (const std::exception& error) {
        m_cpu_error = error.what();
    }
    decode_geometry();
    render_video_frame();
}

void model2_machine::reset_preserving_nvram() {
    // A service-mode reset is a cabinet reset, not a power loss. Persist the
    // serial EEPROM and battery RAM before reset() reloads their disk images.
    if (!m_impl->bus.flush_nvram())
        std::fprintf(stderr, "Could not save Model 2 NVRAM before reset\n");
    reset();
}

bool model2_machine::flush_nvram() {
    return m_impl->bus.flush_nvram();
}

bool model2_machine::communication_peer_mode() const {
    return m_impl && m_impl->bus.communication_peer_mode();
}

bool model2_machine::communication_linked() const {
    return m_impl && m_impl->bus.communication_linked();
}

uint8_t model2_machine::communication_node_id() const {
    return m_impl ? m_impl->bus.communication_node_id() : 0;
}

uint8_t model2_machine::srally_link_type() const {
    return m_impl ? m_impl->bus.srally_link_type() : 0;
}

bool model2_machine::set_srally_link_type(uint8_t type) {
    return m_impl && m_impl->bus.set_srally_link_type(type);
}

void model2_machine::run_frame(bool render_frame) {
    ++m_frame_number;
    m_impl->i960_work_ms = 0.0;
    m_impl->tgp_work_ms = 0.0;
    auto cpu_end = std::chrono::steady_clock::now();
    auto geometry_end = cpu_end;
    if (m_cpu_error.empty()) {
        // Keep physical refresh timing independent from host performance.
        constexpr double i960_clock = 25000000.0;
        m_cycle_remainder += i960_clock / hardware_refresh_rate;
        const int frame_cycles = static_cast<int>(m_cycle_remainder);
        m_cycle_remainder -= frame_cycles;
        // Race logic exchanges many short FIFO messages with the MB86234.
        // Four scheduler turns per scanline reproduce the board's observable
        // CPU/TGP ordering without making host rendering determine game time.
        // A Model 2 output frame begins on visible line zero. Vblank rises
        // after line 383, then the board continues executing for another 40
        // lines before the completed frame is presented. Keeping those phases
        // distinct is observable: Sega Rally can finish a second object-state
        // update during blanking. Ending run_frame() at vblank instead made
        // scene transitions straddle host frames and eventually sent rivals
        // outside the TGP road collision map.
        constexpr int total_lines = 424;
        constexpr int active_lines = 384;
        const int active_cycles = frame_cycles * active_lines / total_lines;
        const int blank_cycles = frame_cycles - active_cycles;
        const auto run_phase = [this](int cycles, int slices) {
            int previous = 0;
            for (int slice = 1; slice <= slices; ++slice) {
                const int target = cycles * slice / slices;
                run_cycles(target - previous);
                previous = target;
                if (!m_cpu_error.empty()) break;
            }
        };
        const char* slices_text = std::getenv("MODEL2_SLICES_PER_LINE");
        const int slices_per_line = std::max(
            1, slices_text ? std::atoi(slices_text) : 4);
        run_phase(active_cycles, active_lines * slices_per_line);
        m_impl->bus.vblank();
        update_interrupt_lines();
        cpu_end = std::chrono::steady_clock::now();
        // The geometrizer latches its completed command buffer on the rising
        // edge of vblank, before the CPUs begin preparing the next frame.
        // In 30 Hz geometry mode the odd display frame reuses the preceding
        // polygon image. Decoding on that odd frame would read the buffer
        // while the TGP is replacing it and produce a progressively partial
        // scene. Sega Rally selects 60 Hz; several other Model 2 games do not.
        if (m_impl->bus.geometry_frame_due()) decode_geometry();
        geometry_end = std::chrono::steady_clock::now();
        if (m_cpu_error.empty())
            run_phase(blank_cycles,
                      (total_lines - active_lines) * slices_per_line);
    } else {
        if (m_impl->bus.geometry_frame_due()) decode_geometry();
        geometry_end = std::chrono::steady_clock::now();
    }
    if (render_frame) render_video_frame();
    if ((m_frame_number % 60) == 0) {
        std::printf("Model 2 core frame=%llu cpu=%.2fms tgp=%.2fms "
                    "geometry=%.2fms render=%08x video=%08x\n",
                    static_cast<unsigned long long>(m_frame_number),
                    m_impl->i960_work_ms, m_impl->tgp_work_ms,
                    std::chrono::duration<double, std::milli>(
                        geometry_end - cpu_end).count(),
                    m_impl->bus.render_control(),
                    m_impl->bus.video_control());
        std::array<uint32_t, 16> window_counts{};
        for (const model2_geometry_polygon& polygon :
             m_impl->geometry.polygons()) {
            if (polygon.window < window_counts.size())
                ++window_counts[polygon.window];
        }
        if (!m_impl->geometry.polygons().empty()) {
            std::printf("Model 2 windows");
            for (std::size_t window = 0; window < window_counts.size();
                 ++window) {
                if (window_counts[window] != 0)
                    std::printf(" %zu=%u", window, window_counts[window]);
            }
            std::printf("\n");
        }
    }
}

void model2_machine::set_inputs(const input_state& state) {
    m_impl->bus.set_inputs(state);
}

void model2_machine::set_sound_uart_callback(
        std::function<void(uint8_t)> callback) {
    m_impl->bus.set_sound_uart_callback(std::move(callback));
}

void model2_machine::sound_midi_receive(uint8_t data) {
    m_impl->bus.sound_midi_receive(data);
}

void model2_machine::run_cycles(int cycles) {
    if (cycles <= 0 || !m_cpu_error.empty()) return;
    try {
        const auto cpu_begin = std::chrono::steady_clock::now();
        // Each Model 2 TGP FIFO holds eight words plus one synchronized
        // overflow transfer.  Once that ninth transfer commits, the source
        // is halted until the destination consumes it. Let emulated time
        // advance while a source is blocked, but do not execute it ahead.
        const bool cpu_fifo_blocked = m_impl->bus.tgp_input_words() >
                                      model2_bus::tgp_fifo_capacity;
        int executed = 0;
        if (cpu_fifo_blocked) {
            // Time spent halted by FIFO back-pressure also pays down an
            // instruction-boundary overrun from an earlier quantum.
            m_impl->i960_cycle_debt = std::max(
                0, m_impl->i960_cycle_debt - cycles);
        } else if (m_impl->i960_cycle_debt >= cycles) {
            m_impl->i960_cycle_debt -= cycles;
        } else {
            const int cpu_budget = cycles - m_impl->i960_cycle_debt;
            m_impl->i960_cycle_debt = 0;
            executed = m_impl->cpu.standalone_execute(cpu_budget);
            // The i960 core deliberately finishes the current instruction
            // when it crosses a quantum boundary. Carry those extra cycles
            // into the next quantum instead of letting thousands of small
            // scheduler slices overclock the CPU.
            if (executed > cpu_budget)
                m_impl->i960_cycle_debt = executed - cpu_budget;
        }
        const auto cpu_end = std::chrono::steady_clock::now();
        m_impl->i960_work_ms += std::chrono::duration<double, std::milli>(
            cpu_end - cpu_begin).count();
        m_executed_cycles += static_cast<uint64_t>(executed);
        // Device timers follow the 25 MHz board clock, not the number of CPU
        // cycles reported after an instruction crosses a scheduling boundary.
        // The i960 core intentionally completes that instruction and can
        // therefore report more cycles than requested. Feeding the overshoot
        // into the timers made their effective clock depend on slice size and
        // caused Sega Rally's opponent visibility timer to expire early.
        m_impl->bus.tick(static_cast<uint32_t>(cycles));
        if (m_impl->bus.take_tgp_boot_request()) {
            m_impl->tgp.reset();
            m_impl->tgp_running = true;
        }
        if (m_impl->tgp_running &&
            m_impl->bus.tgp_output_words() <=
                model2_bus::tgp_fifo_capacity) {
            const auto tgp_begin = std::chrono::steady_clock::now();
            m_impl->tgp.execute(cycles * 2);
            m_impl->tgp_work_ms += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - tgp_begin).count();
        }
        update_interrupt_lines();
    } catch (const std::exception& error) {
        m_cpu_error = error.what();
        std::fprintf(stderr, "Model 2 i960 fault at %08x: %s\n",
                     m_impl->cpu.previous_program_counter(),
                     m_cpu_error.c_str());
    }
}

void model2_machine::update_interrupt_lines() {
    for (unsigned line = 0; line < 4; ++line)
        m_impl->cpu.set_input_line(
            line, m_impl->bus.irq_asserted(line) ? 1 : 0);
}

uint32_t model2_machine::program_counter() const {
    return m_impl->cpu.program_counter();
}

const uint32_t* model2_machine::frame_buffer() const {
    return m_impl->video.rgba_pixels();
}

void model2_machine::render_video_layers() {
    const uint64_t generation = m_impl->bus.video_generation();
    if (m_impl->rendered_video_generation == generation) return;
    m_impl->video.render_layers(
        m_impl->bus.tile_ram(), m_impl->bus.character_ram(),
        m_impl->bus.palette_ram(), m_impl->bus.color_translation());
    m_impl->rendered_video_generation = generation;
}

const uint32_t* model2_machine::base_layer() const {
    return m_impl->video.base_rgba_pixels();
}

const uint32_t* model2_machine::foreground_layer() const {
    return m_impl->video.foreground_rgba_pixels();
}

model2_gpu_frame model2_machine::make_gpu_frame() const {
    model2_gpu_frame frame;
    frame.polygons = m_impl->geometry.polygons();
    const std::size_t pixel_count = static_cast<std::size_t>(
        screen_width * screen_height);
    frame.base_rgba.assign(base_layer(), base_layer() + pixel_count);
    frame.foreground_rgba.assign(foreground_layer(),
                                 foreground_layer() + pixel_count);
    frame.texture_generation = m_impl->bus.texture_generation();
    if (frame.texture_generation != m_exported_texture_generation) {
        frame.texture_sheet_0 = m_impl->bus.texture_sheet_0();
        frame.texture_sheet_1 = m_impl->bus.texture_sheet_1();
        m_exported_texture_generation = frame.texture_generation;
    }
    frame.color_generation = m_impl->bus.color_generation();
    if (frame.color_generation != m_exported_color_generation) {
        frame.luma = m_impl->bus.luma_ram();
        frame.palette = m_impl->bus.palette_ram();
        frame.color_translation = m_impl->bus.color_translation();
        m_exported_color_generation = frame.color_generation;
    }
    frame.horizontal_offset = m_impl->bus.horizontal_offset();
    frame.vertical_offset = m_impl->bus.vertical_offset();
    return frame;
}

const model2_geometry_summary& model2_machine::geometry_summary() const {
    return m_impl->geometry.summary();
}

const std::vector<model2_geometry_polygon>&
model2_machine::geometry_polygons() const {
    return m_impl->geometry.polygons();
}

uint32_t model2_machine::cpu_register(unsigned index) const {
    return m_impl->cpu.register_value(index);
}

std::string model2_machine::current_instruction() const {
    return m_impl->cpu.disassemble(m_impl->cpu.program_counter());
}

std::string model2_machine::instruction_at(uint32_t address) const {
    return m_impl->cpu.disassemble(address);
}

uint8_t model2_machine::debug_read8(uint32_t address) {
    return m_impl->bus.read8(address);
}

bool model2_machine::tgp_running() const {
    return m_impl->tgp_running;
}

uint16_t model2_machine::tgp_program_counter() const {
    return m_impl->tgp.program_counter();
}

uint32_t model2_machine::tgp_data_word(uint16_t address) const {
    return m_impl->bus.tgp_data_read(address);
}

uint32_t model2_machine::tgp_uploaded_words() const {
    return m_impl->bus.tgp_uploaded_words();
}

uint32_t model2_machine::tgp_program_word(uint16_t address) const {
    return m_impl->bus.tgp_program_read(address);
}

std::size_t model2_machine::tgp_input_words() const {
    return m_impl->bus.tgp_input_words();
}

std::size_t model2_machine::tgp_output_words() const {
    return m_impl->bus.tgp_output_words();
}

uint32_t model2_machine::geometry_read_address() const {
    return m_impl->bus.geometry_read_address();
}

uint32_t model2_machine::geometry_write_address() const {
    return m_impl->bus.geometry_write_address();
}

uint32_t model2_machine::geometry_buffer_word(uint32_t byte_address) const {
    return m_impl->bus.geometry_buffer_word(byte_address);
}

std::size_t model2_machine::non_black_pixels() const {
    return m_impl->video.non_black_pixels();
}

uint64_t model2_machine::video_frame_hash() const {
    return m_impl->video.frame_hash();
}

uint64_t model2_machine::unmapped_reads() const {
    return m_impl->bus.unmapped_read_count();
}

uint64_t model2_machine::unmapped_writes() const {
    return m_impl->bus.unmapped_write_count();
}

uint32_t model2_machine::last_unmapped_read() const {
    return m_impl->bus.last_unmapped_read();
}

uint32_t model2_machine::last_unmapped_write() const {
    return m_impl->bus.last_unmapped_write();
}

void model2_machine::decode_geometry() {
    m_impl->geometry.parse(m_impl->bus.geometry_buffer(),
                           m_impl->bus.geometry_read_address(),
                           m_roms.polygon_data, m_roms.texture_data,
                           m_impl->bus.master_z_clip());
}

void model2_machine::render_video_frame() {
    m_impl->video.render(
        m_impl->bus.tile_ram(), m_impl->bus.character_ram(),
        m_impl->bus.palette_ram(), m_impl->bus.color_translation(),
        m_impl->bus.framebuffer_a(), m_impl->bus.framebuffer_b(),
        m_impl->bus.texture_sheet_0(), m_impl->bus.texture_sheet_1(),
        m_impl->bus.luma_ram(),
        m_impl->geometry.polygons(), m_impl->bus.frame_number(),
        m_impl->bus.render_control(), m_impl->bus.horizontal_offset(),
        m_impl->bus.vertical_offset());
    m_impl->rendered_video_generation = m_impl->bus.video_generation();
}
