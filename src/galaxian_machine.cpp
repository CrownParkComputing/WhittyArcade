// galaxian_machine.cpp - shared Z80 board base for the Galaxian family.
//
// Owns the chips Z80 core, the per-frame clock counter, the sound-write
// handler, the sound-write / IO / memory-dispatch scaffolding that is
// byte-for-byte identical between the supported boards, and the RGBA
// frame buffer the boards render into. Per-game state (memory map,
// palette, render, IRQ/flip/star wiring) lives in the board_interface
// subclass.

#include "galaxian_machine.h"

#include "z80.h"

#include <cstdio>
#include <filesystem>
#include <memory>
#include <vector>

#include <minizip/unzip.h>

struct galaxian_machine::impl {
    std::unique_ptr<galaxian_board_interface> board;

    z80_t cpu{};
    uint64_t pins{0};

    int width{0};
    int height{0};
    double refresh_hz{0.0};
    int clocks_per_frame{0};

    // Frame buffer the boards render into. Sized in set_dimensions().
    // Indexed [y * width + x].
    std::vector<uint32_t> frame_buffer;

    std::function<void(unsigned, uint8_t)> sound_write_handler;
    bool loaded{false};

    void run_frame_impl();
};

galaxian_machine::galaxian_machine(
    std::unique_ptr<galaxian_board_interface> board)
    : m_impl(std::make_unique<impl>()) {
    m_impl->board = std::move(board);
    m_impl->board->configure(this);
    m_impl->pins = z80_init(&m_impl->cpu);
}

galaxian_machine::~galaxian_machine() {
    if (m_impl && m_impl->board) m_impl->board->flush_high_scores();
}

void galaxian_machine::set_dimensions(int width, int height,
                                      double refresh_hz,
                                      int clocks_per_frame) {
    m_impl->width = width;
    m_impl->height = height;
    m_impl->refresh_hz = refresh_hz;
    m_impl->clocks_per_frame = clocks_per_frame;
    m_impl->frame_buffer.assign(static_cast<std::size_t>(width) *
                                     static_cast<std::size_t>(height),
                                 0xff000000u);
}

int galaxian_machine::screen_width() const noexcept {
    return m_impl->width;
}

int galaxian_machine::screen_height() const noexcept {
    return m_impl->height;
}

double galaxian_machine::refresh_rate() const noexcept {
    return m_impl->refresh_hz;
}

int galaxian_machine::clocks_per_frame() const noexcept {
    return m_impl->clocks_per_frame;
}

const std::function<void(unsigned, uint8_t)>&
galaxian_machine::sound_write_handler() const {
    return m_impl->sound_write_handler;
}

bool galaxian_machine::load_roms(const std::string& path) {
    std::string err;
    bool ok = false;
    namespace fs = std::filesystem;
    std::error_code ec;
    if (fs::is_directory(path, ec)) {
        ok = m_impl->board->load_roms_from_dir(path, &err);
    } else {
        unzFile archive = unzOpen(path.c_str());
        if (!archive) {
            std::fprintf(stderr, "Galaxian: could not open ROM archive: %s\n",
                         path.c_str());
            return false;
        }
        ok = m_impl->board->load_roms_into(archive, &err);
        unzClose(archive);
    }
    if (!ok) {
        if (!err.empty()) std::fprintf(stderr, "Galaxian: %s\n", err.c_str());
        return false;
    }
    m_impl->loaded = true;
    return true;
}

void galaxian_machine::reset() {
    m_impl->pins = z80_reset(&m_impl->cpu);
    m_impl->board->reset_extras();
}

void galaxian_machine::set_input(const input_state& state) {
    uint8_t ports[8] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    m_impl->board->apply_input(state, ports, sizeof(ports));
}

void galaxian_machine::set_sound_write_handler(
    std::function<void(unsigned, uint8_t)> handler) {
    m_impl->sound_write_handler = std::move(handler);
}

const uint32_t* galaxian_machine::frame_buffer() const {
    return m_impl->frame_buffer.data();
}

void galaxian_machine::run_frame() {
    if (!m_impl->loaded) return;
    m_impl->board->on_frame_start();
    m_impl->run_frame_impl();
}

void galaxian_machine::impl::run_frame_impl() {
    for (int clock = 0; clock < clocks_per_frame; ++clock) {
        // Z80_NMI is edge-triggered. Boards return true only for the single
        // clock that begins their pulse, so explicitly release the host pin
        // on every other clock. Merely OR-ing the bit leaves NMI asserted in
        // the chips pin mask forever and Moon Cresta receives only its first
        // vertical interrupt (therefore never scans later coin/start edges).
        if (board->wants_nmi())
            pins |= Z80_NMI;
        else
            pins &= ~Z80_NMI;
        pins = z80_tick(&cpu, pins);
        if ((pins & Z80_MREQ) == 0) continue;
        const uint16_t address = Z80_GET_ADDR(pins);
        if (pins & Z80_RD) {
            Z80_SET_DATA(pins, board->cpu_read(address, clock));
        } else if (pins & Z80_WR) {
            board->cpu_write(address, Z80_GET_DATA(pins));
        }
    }
    board->render_frame(frame_buffer.data());
    board->service_high_scores();
}
