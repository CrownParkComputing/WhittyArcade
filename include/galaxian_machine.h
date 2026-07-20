// Galaxian-family Z80 board emulation. A common Z80 hot loop,
// frame buffer, and sound-write wiring live in galaxian_machine; the
// per-game memory map, video layout, IRQ/flip/star wiring, and input
// translation live in a board_interface subclass.
//
// The two shipped boards are:
//   - phoenix_board_interface  in galaxian_machine_phoenix.cpp
//   - mooncrst_board_interface in galaxian_machine_mooncrst.cpp
//
// Out of scope: a non-Z80 CPU (e.g. 6502) would need a different base;
// the hot loop assumes Z80 timing.

#pragma once

#include "arcade_types.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

// minizip declares unzFile as `void*`; forward-declare to match so the
// board_interface methods can take an unzFile without forcing every
// user to include <minizip/unzip.h>.
typedef void* unzFile;

class galaxian_machine;

// Per-board hook surface. Each board's impl defines its own memory
// map, palette, video layout, and input translation. The base class
// owns the Z80 state, the RGBA frame buffer, and the sound-write
// handler; the impl receives a back-pointer to the base in
// configure() and writes pixels into the base's frame buffer through
// render_frame(rgba).
class galaxian_board_interface {
public:
    virtual ~galaxian_board_interface() = default;

    // Called once by the base after construction and before
    // load_roms(). The impl uses the back-pointer to register its
    // screen dimensions, refresh rate, and clocks-per-frame via
    // set_dimensions(), and stores the back-pointer for use inside
    // cpu_write() (e.g. to fire the sound-write handler).
    virtual void configure(galaxian_machine* self) = 0;

    // Address-space access. Called by the base inside the Z80 hot loop
    // for every memory transaction. frame_clock is the 0-based clock
    // within the current frame; the impl may use it to compute vblank
    // timing (e.g. Phoenix's 0x7800 DIP+vblank port).
    virtual uint8_t cpu_read(uint16_t address, int frame_clock) = 0;
    virtual void cpu_write(uint16_t address, uint8_t data) = 0;

    // Optional M68K bus access. The default implementations compose two
    // 8-bit reads/writes from cpu_read/cpu_write; M68K boards override
    // these with their own 16-bit-bus-aware versions when they need
    // big-endian word access (e.g. Sega System 16-B).
    virtual unsigned cpu_read_16(unsigned address) {
        return (static_cast<unsigned>(cpu_read(static_cast<uint16_t>(address),
                                              0)) << 8) |
                static_cast<unsigned>(cpu_read(static_cast<uint16_t>(address + 1),
                                              0));
    }
    virtual void cpu_write_16(unsigned address, unsigned data) {
        cpu_write(static_cast<uint16_t>(address),   static_cast<uint8_t>(data >> 8));
        cpu_write(static_cast<uint16_t>(address+1), static_cast<uint8_t>(data));
    }

    // Optional sound CPU (co-CPU) bus access. Shinobi uses a second
    // Musashi 68000 running at 5 MHz as a sound CPU. The default is
    // no-op so existing Z80 boards (Moon Cresta / Phoenix) compile
    // unchanged.
    virtual uint8_t  co_cpu_read(unsigned /*address*/)      { return 0xff; }
    virtual void     co_cpu_write(unsigned /*address*/,
                                  uint8_t /*data*/)        {}
    virtual void     co_cpu_pulse_reset()                    {}
    virtual unsigned co_cpu_read_16(unsigned /*address*/)    { return 0xffff; }
    virtual void     co_cpu_write_16(unsigned address,
                                     unsigned data) {
        co_cpu_write(address,   static_cast<uint8_t>(data >> 8));
        co_cpu_write(address+1, static_cast<uint8_t>(data));
    }
    virtual unsigned co_cpu_clock_hz()                      { return 0; }

    // Optional per-frame-start hook. The base calls this once at the
    // start of run_frame. The default is a no-op; Moon Cresta uses it
    // to arm its NMI pulse for the first clock of frames where NMI is
    // enabled.
    virtual void on_frame_start() {}

    // Optional per-clock NMI check. The base calls this before each
    // z80_tick; returning true asserts Z80_NMI for that tick. Default
    // is false (no NMI). Moon Cresta returns true for the first tick
    // of every frame where NMI is enabled, then clears its arm flag.
    virtual bool wants_nmi() { return false; }

    // Optional MAME-compatible high-score lifecycle. Boards with a verified
    // score-memory map restore only after the game initializes its table and
    // flush changed bytes when the machine closes.
    virtual void service_high_scores() {}
    virtual void flush_high_scores() {}

    // Per-board state clear, called by the base at the end of reset()
    // after the base's own state has been cleared. The default is a
    // no-op; Phoenix clears its page+palette bank and scroll state
    // here, Moon Cresta clears NMI/flip/stars/gfx_bank state.
    virtual void reset_extras() {}

    // Render the current frame into the supplied RGBA buffer. The
    // base pre-sizes the buffer to the dimensions configure() set.
    // Each pixel is a 32-bit word laid out as
    //   (A << 24) | (R << 16) | (G << 8) | B
    // (native little-endian, with the alpha channel forced to 0xff).
    virtual void render_frame(uint32_t* rgba) = 0;

    // Load ROMs from an already-opened ZIP archive. The base opens
    // the archive in load_roms() and dispatches here once per set.
    // Returns true on success. On failure, *err is set to a
    // human-readable description and the function returns false.
    virtual bool load_roms_into(unzFile archive, std::string* err) = 0;

    // Load ROMs from an on-disk directory of files. The base dispatches
    // here when load_roms() is called with a path that is a directory
    // rather than a zip archive. Default implementation rejects the
    // call with a "unsupported" error; boards that need it override.
    virtual bool load_roms_from_dir(const std::string& dir,
                                    std::string* err) {
        (void)dir;
        *err = "Galaxian board: directory-based ROM loading is not supported";
        return false;
    }

    // Translate the framework's input_state into the per-board
    // input bytes. port_count is the size of the ports[] array; the
    // impl writes 1-2 bytes.
    virtual void apply_input(const input_state& s, uint8_t* ports,
                             std::size_t port_count) = 0;
};

class galaxian_machine {
public:
    explicit galaxian_machine(std::unique_ptr<galaxian_board_interface> board);
    ~galaxian_machine();

    galaxian_machine(const galaxian_machine&) = delete;
    galaxian_machine& operator=(const galaxian_machine&) = delete;

    // Per-board screen dimensions / refresh rate, set by the impl's
    // configure() call. Available before load_roms() so the frontend
    // can size the OpenGL texture.
    int screen_width() const noexcept;
    int screen_height() const noexcept;
    double refresh_rate() const noexcept;
    int clocks_per_frame() const noexcept;

    bool load_roms(const std::string& path);
    void reset();
    void run_frame();
    void set_input(const input_state& state);
    void set_sound_write_handler(
        std::function<void(unsigned, uint8_t)> handler);
    const uint32_t* frame_buffer() const;

    // Called by the board_interface::configure() impl to register its
    // dimensions and clock rate. Public so the impl can call it from
    // configure(); not part of the user-facing API.
    void set_dimensions(int width, int height, double refresh_hz,
                        int clocks_per_frame);

    // Sound-write handler the base forwards to the audio system. The
    // board reaches it through the back-pointer it was given in
    // configure().
    const std::function<void(unsigned, uint8_t)>& sound_write_handler() const;

private:
    struct impl;
    std::unique_ptr<impl> m_impl;
};

// Board implementations are private to their translation units. Sessions
// construct them through these factories instead of redeclaring functions in
// an application source file.
std::unique_ptr<galaxian_board_interface> make_phoenix_board_interface();
std::unique_ptr<galaxian_board_interface> make_mooncrst_board_interface();
