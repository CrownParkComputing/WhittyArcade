// shinobi_machine.cpp -- Shinobi (Sega System 16-B) machine runtime.
//
// Provides:
//   1. `bus_adapter`: an m68000_bus implementation that routes 8/16-bit
//      bus reads/writes into `system16b::board`'s per-board memory map.
//   2. `system16b_machine_t`: the runtime that drives the 68000 each frame
//      and exposes the frame buffer + audio to the host session.
// The 68000 is Moira-backed (m68000_cpu), a fully instance-based core:
// each machine owns its CPU object, so dual-68000 boards (main + sound,
// or multi-board sessions) need no process-global dispatcher like the
// old Musashi integration did.

#include "sega/system16b/system16b_machine.h"
#include "m68000_cpu.h"

#include "z80.h"     // vendored BSD/MIT Z80 core from chips-SDL.

#include <cstdio>
#include <cstring>
#include <memory>
#include <algorithm>
#include <string>

namespace system16b {

// =====================================================================
// bus_adapter -- routes 8/16-bit accesses into board.
// =====================================================================
class bus_adapter final : public m68000_bus {
public:
    explicit bus_adapter(board* b) : m_board(b) {}
    uint8_t read8(uint32_t address) override {
        return m_board->byte_read(address);
    }
    uint16_t read16(uint32_t address) override {
        return static_cast<uint16_t>(m_board->cpu_read_16(address));
    }
    void write8(uint32_t address, uint8_t value) override {
        m_board->byte_write(address, value);
    }
    void write16(uint32_t address, uint16_t value) override {
        m_board->cpu_write_16(address, value);
    }
private:
    board* m_board;
};

// =====================================================================
// system16b_machine_t
// =====================================================================

// Z80 sound CPU state (chips/z80.h pin-loop core, same as galaxian).
struct system16b_machine_t::sound_cpu {
    z80_t    cpu{};
    uint64_t pins{0};
    std::function<void(unsigned, uint8_t)> write_cb;
    std::function<void(uint32_t)> timer_cb;
    uint64_t ym_clock_fraction{0};
};

void system16b_machine_t::set_sound_write_callback(
    std::function<void(unsigned, uint8_t)> cb) {
    if (!m_sound) m_sound = std::make_unique<sound_cpu>();
    m_sound->write_cb = std::move(cb);
}

void system16b_machine_t::set_sound_timer_callback(
    std::function<void(uint32_t)> cb) {
    if (!m_sound) m_sound = std::make_unique<sound_cpu>();
    m_sound->timer_cb = std::move(cb);
}

system16b_machine_t::system16b_machine_t()
    : m_board(std::make_unique<board>()) {}

system16b_machine_t::~system16b_machine_t() {
    if (m_board) m_board->flush_high_scores();
}

bool system16b_machine_t::load_roms(const std::string& path) {
    auto result = system16b_rom_loader::load(path);
    if (!result) {
        std::fprintf(stderr, "shinobi: failed to load %s: %s\n",
                     path.c_str(), result.error.c_str());
        return false;
    }
    std::memcpy(m_board->program_rom().data(),   result.roms.program.data(),
                result.roms.program.size());
    std::memcpy(m_board->tile_plane0().data(),   result.roms.tile_plane0.data(),
                result.roms.tile_plane0.size());
    std::memcpy(m_board->tile_plane1().data(),   result.roms.tile_plane1.data(),
                result.roms.tile_plane1.size());
    std::memcpy(m_board->tile_plane2().data(),   result.roms.tile_plane2.data(),
                result.roms.tile_plane2.size());
    std::memcpy(m_board->sprite_gfx().data(),    result.roms.sprite_gfx.data(),
                result.roms.sprite_gfx.size());

    // Sound CPU ROM stage. Programs of mostly-0xFF (the loader default)
    // are flagged as "no sound CPU ROM" so we skip running the Z80.
    const bool program_looks_present =
        std::any_of(result.roms.sound_prog.begin(), result.roms.sound_prog.end(),
                    [](uint8_t b) { return b != 0xff; });
    if (program_looks_present) {
        std::memcpy(m_board->sound_prog_rom_.data(), result.roms.sound_prog.data(),
                    result.roms.sound_prog.size());
        std::memcpy(m_board->sound_data_rom_.data(), result.roms.sound_data.data(),
                    result.roms.sound_data.size());
        m_board->have_sound_rom_ = true;
        if (!m_sound) m_sound = std::make_unique<sound_cpu>();
        m_sound->pins = z80_init(&m_sound->cpu);
    } else {
        m_board->have_sound_rom_ = false;
    }

    m_board->set_game(result.set);
    m_board->reset_extras();
    m_ready = true;
    return true;
}

void system16b_machine_t::reset() {
    if (!m_ready) return;
    m_board->reset_extras();

    // Create (or re-create) this machine's private 68000 and pulse reset
    // so it reads the M68K reset vectors from the program image
    // (address 0..7 for SP and PC, big-endian). Each instance is
    // independent; no global core state to clobber.
    if (!m_main_bus)
        m_main_bus = std::make_unique<bus_adapter>(m_board.get());
    m_main_cpu = std::make_unique<m68000_cpu>(*m_main_bus);
    m_main_cpu->reset();
}

void system16b_machine_t::run_frame() {
    if (!m_ready || !m_main_cpu) return;

    int remaining = kMainCyclesPerFrame;
    while (remaining > 0) {
        const int chunk = remaining > 20000 ? 20000 : remaining;
        remaining -= m_main_cpu->execute(chunk);
    }
    // Render BEFORE the vblank handler runs: the visible frame was
    // scanned out with the scroll/page register values held during the
    // active period. The level-4 handler copies next-frame scroll values
    // into text RAM, so rendering after it would show one frame ahead
    // (and expose not-yet-filled tile pages at scene boundaries).
    m_board->render_frame(const_cast<uint32_t*>(frame_buffer()));

    // VBLANK: assert level-4 long enough for the handler to run to its
    // RTE. Exception entry alone costs ~44 cycles, and Shinobi's handler
    // does the per-frame vblank bookkeeping the main loop waits on, so a
    // token slice would leave the game stuck in its boot wait.
    m_main_cpu->set_irq(4);
    m_main_cpu->execute(4000);
    m_main_cpu->set_irq(0);

    // Z80 sound CPU @ 5 MHz (main 68000 is 10 MHz, so half the cycles).
    // Same chips/z80.h pin loop as galaxian_machine, plus IORQ handling
    // for the YM2151 / uPD7759 / sound-latch ports.
    if (m_sound && m_board->have_sound_rom_) {
        uint64_t pins = m_sound->pins;
        unsigned timer_slice_clocks = 0;
        auto advance_ym_timer = [this](unsigned z80_clocks) {
            if (!m_sound->timer_cb || z80_clocks == 0) return;
            m_sound->ym_clock_fraction +=
                static_cast<uint64_t>(z80_clocks) * 4'000'000u;
            const uint32_t ym_clocks = static_cast<uint32_t>(
                m_sound->ym_clock_fraction / 5'000'000u);
            m_sound->ym_clock_fraction %= 5'000'000u;
            if (ym_clocks != 0) m_sound->timer_cb(ym_clocks);
        };
        if (std::getenv("SHINOBI_TRACE_Z80")) {
            static int frames = 0;
            if (++frames % 30 == 0)
                std::fprintf(stderr, "z80 pc=%04x iff=%02x sp=%04x\n",
                             m_sound->cpu.pc,
                             (unsigned)(m_sound->cpu.iff1 |
                                        (m_sound->cpu.iff2 << 1)),
                             m_sound->cpu.sp);
        }
        for (int clock = 0; clock < kSoundCyclesPerFrame; ++clock) {
            if (m_board->sound_irq_pending_) pins |= Z80_INT;
            pins = z80_tick(&m_sound->cpu, pins);
            const uint16_t address = Z80_GET_ADDR(pins);
            if (pins & Z80_MREQ) {
                if (pins & Z80_RD) {
                    Z80_SET_DATA(pins, m_board->co_cpu_read(address));
                } else if (pins & Z80_WR) {
                    m_board->co_cpu_write(address, Z80_GET_DATA(pins));
                }
            } else if (pins & Z80_IORQ) {
                const unsigned port = address & 0xFFu;
                if (pins & Z80_RD) {
                    Z80_SET_DATA(pins, m_board->co_cpu_port_read(port));
                } else if (pins & Z80_WR) {
                    const uint8_t data = Z80_GET_DATA(pins);
                    m_board->co_cpu_port_write(port, data);
                    if (m_sound->write_cb &&
                        (port == 0x00u || port == 0x01u || port == 0x80u)) {
                        m_sound->write_cb(port, data);
                    }
                }
            }
            if (++timer_slice_clocks == 512) {
                advance_ym_timer(timer_slice_clocks);
                timer_slice_clocks = 0;
            }
        }
        advance_ym_timer(timer_slice_clocks);
        m_sound->pins = pins & ~(uint64_t)Z80_INT;
    }
    m_board->service_high_scores();
}

uint32_t system16b_machine_t::main_pc() const {
    return m_main_cpu ? m_main_cpu->program_counter() : 0;
}

void system16b_machine_t::set_input(const input_state& state) {
    if (!m_ready) return;
    constexpr std::size_t kPorts = 5;
    uint8_t ports[kPorts] = {};
    m_board->apply_input(state, ports, kPorts);
    // Map Shinobi's per-port bytes into the per-board mirror that
    // byte_read() decodes (System 16-B I/O at 0xC40000).
    m_board->p1_input_      = ports[0];
    m_board->p2_input_      = ports[1];
    m_board->service_input_ = ports[2];
    m_board->dsw1_          = ports[3];
    m_board->dsw2_          = ports[4];
}

}  // namespace system16b
