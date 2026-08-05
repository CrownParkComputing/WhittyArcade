// Killer Instinct (Midway Wolf Unit) session.
//
// Wires the MIPS R4600 interpreter to the Midway Wolf Unit memory map.
// The board has:
//   - MIPS R4600 CPU at ~100 MHz
//   - 8 MB system RAM
//   - Boot ROM (512 KiB)
//   - IDE hard disk (CHD image)
//   - Custom ASIC for 3D graphics, DMA, and I/O
//   - DCS audio system (ADSP-2105 based)
//
// Memory map (physical):
//   0x00000000 - 0x007FFFFF  8 MB RAM
//   0x1FC00000 - 0x1FC7FFFF  Boot ROM (512 KiB, mirrored)
//   0x1F000000 - 0x1FBFFFFF  I/O region (ASIC, IDE, DCS, inputs, etc.)
//
// I/O map (offsets from io_base = 0x1F000000):
//   0x200000: DCS audio (ADSP-2105)
//   0x400000: Video ASIC (framebuffer, display list, control, vsync)
//   0x400100: DMA Blitter (source, dest, size, stride, control, command)
//   0x400400: Player 1 controls (joystick, buttons, start)
//   0x400C00: Player 2 controls
//   0x401000: Coin/Service/DIP switches
//   0x500000: IDE controller (data, registers, command/status)

#include "arcade_session_internal.h"
#include "arcade_types.h"
#include "midway/mips_cpu.h"
#include "midway/midway_rom.h"
#include "midway/midway_chd.h"

#include <cstdio>
#include <cstdlib>
#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// Boot-time tracing to /tmp/ki_boot.log, off unless KI_BOOT_LOG is set - the
// per-frame file opens otherwise dominate the frame and crater the FPS.
const bool s_ki_log = std::getenv("KI_BOOT_LOG") != nullptr;

// ---- Midway Wolf Unit constants -------------------------------------------

// Real Midway Wolf Unit map, decoded on the 29-bit physical address
// (KSEG0/1/KUSEG all fold to the same phys). Taken from MAME
// src/mame/rare/kinst.cpp kinst_map(); the previous map here - a 16 MB RAM
// block, an I/O window at 0x1F000000, IDE at 0x1F500000 and a game-ROM
// window at 0x1F800000 - was invented, which is why the boot ROM polled a
// drive that was not there (first_ide stayed 0) and nothing ever rendered.
constexpr uint32_t ram0_size      = 0x00080000;  // 512 KiB low bank; holds FB
constexpr uint32_t ram1_base      = 0x08000000;
constexpr uint32_t ram1_size      = 0x00800000;  // 8 MiB main work RAM
constexpr uint32_t boot_rom_base  = 0x1FC00000;
constexpr uint32_t boot_rom_size  = 0x80000;     // 512 KiB (u98), mirrored

// I/O ports (dword each) at 0x10000080..0x100000BF. The register meanings
// are shuffled between kinst and kinst2, handled at the access site.
constexpr uint32_t io_ports_base  = 0x10000080;
constexpr uint32_t io_ports_end   = 0x100000C0;

// IDE: cs0 registers at 0x10000100..0x1000013F on EIGHT-byte spacing
// (ide_r does cs0_r(offset/2), offset being the dword index), and the cs1
// alternate-status / device-control byte at 0x10000170.
constexpr uint32_t ide_cs0_base   = 0x10000100;
constexpr uint32_t ide_cs0_end    = 0x10000140;
constexpr uint32_t ide_cs1_base   = 0x10000170;

// Framebuffer lives inside the low RAM bank; vram_control bit 2 flips the
// page. 320x240, packed two BGR-555 pixels per 32-bit word, 640 bytes/row.
constexpr uint32_t video_fb_page0 = 0x30000;
constexpr uint32_t video_fb_page1 = 0x58000;
constexpr int      video_width    = 320;
constexpr int      video_height   = 240;

// IDE status bits
constexpr uint8_t ide_status_err  = 0x01;
constexpr uint8_t ide_status_drq  = 0x08;
constexpr uint8_t ide_status_rdy  = 0x40;
constexpr uint8_t ide_status_bsy  = 0x80;

// Input bit definitions (KI joystick is 8-way digital + 6 buttons)
// Bits are active-low (0 = pressed) on the real hardware.
constexpr uint32_t p1_joy_up     = (1u << 0);
constexpr uint32_t p1_joy_down   = (1u << 1);
constexpr uint32_t p1_joy_left   = (1u << 2);
constexpr uint32_t p1_joy_right  = (1u << 3);
constexpr uint32_t p1_button1    = (1u << 4);   // Quick/Fierce (HP/HK)
constexpr uint32_t p1_button2    = (1u << 5);   // Medium Punch
constexpr uint32_t p1_button3    = (1u << 6);   // Medium Kick
constexpr uint32_t p1_button4    = (1u << 7);   // Light Punch
constexpr uint32_t p1_button5    = (1u << 8);   // Light Kick
constexpr uint32_t p1_button6    = (1u << 9);   // Run/Block
constexpr uint32_t p1_start      = (1u << 10);
constexpr uint32_t p1_coin       = (1u << 11);

// ---- Wolf Unit session ----------------------------------------------------

class wolfunit_session final : public emulator_session {
public:
    wolfunit_session(std::shared_ptr<arcade_video_worker> video,
                     std::shared_ptr<arcade_cabinet_state> cabinet)
        : m_video(std::move(video)),
          m_cabinet_state(std::move(cabinet)),
          m_ram(ram0_size),
          m_ram1(ram1_size),
          m_boot_rom(boot_rom_size),
          m_framebuffer(video_width * video_height * 4)
    {}

    ~wolfunit_session() override {
        if (m_cpu) mips_destroy(m_cpu);
        flush_nvram();
    }

    arcade_board_type board_type() const noexcept override {
        return arcade_board_type::midway;
    }

    bool initialize(const std::string& rom_path,
                    const std::string& bios_path,
                    const emulator_settings& settings) override {
        (void)bios_path;
        m_rom_path = rom_path;

        fprintf(stderr, "[KI] initialize: rom_path=%s\n", rom_path.c_str());
        if (s_ki_log) { FILE* f = fopen("/tmp/ki_boot.log", "a"); if (f) { fprintf(f, "[KI] initialize called: rom_path=%s\n", rom_path.c_str()); fclose(f); } }

        // Load ROMs.
        const std::string chd_dir = settings.chd_directory.empty() ?
            "" : settings.chd_directory;
        fprintf(stderr, "[KI]   chd_dir=%s\n", chd_dir.c_str());
        midway_rom_load_result loaded =
            midway_rom_loader::load(rom_path, chd_dir);
        if (!loaded) {
            m_error = loaded.error;
            fprintf(stderr, "[KI]   ROM LOAD FAILED: %s\n", loaded.error.c_str());
            if (s_ki_log) { FILE* f = fopen("/tmp/ki_boot.log", "a"); if (f) { fprintf(f, "[KI] ROM LOAD FAILED: %s\n", loaded.error.c_str()); fclose(f); } }
            return false;
        }

        m_game_set = loaded.set;
        fprintf(stderr, "[KI]   ROM set=%s boot_rom=%zu bytes disc=%s\n",
                midway_rom_loader::set_short_name(loaded.set),
                loaded.boot_rom.size(),
                loaded.disc_path.c_str());
        std::memcpy(m_boot_rom.data(), loaded.boot_rom.data(),
                    std::min(loaded.boot_rom.size(), m_boot_rom.size()));

        // u10-u36 are DCS sound data, not CPU-addressable game code: the game
        // itself is on the IDE drive. Keep the bytes for the DCS side; they
        // are deliberately not mapped into the CPU bus.
        if (!loaded.game_roms.empty()) {
            m_dcs_rom = std::move(loaded.game_roms);
            fprintf(stderr, "[KI]   DCS sound data: %zu bytes\n",
                    m_dcs_rom.size());
        }

        // Dump first 4 instructions at reset vector (physical 0x1FC00000).
        {
            uint32_t reset_offset = 0x1FC00000 - boot_rom_base;
            fprintf(stderr, "[KI]   Boot ROM at reset vector (offset 0x%X): ",
                    reset_offset);
            for (int i = 0; i < 16 && reset_offset + i < m_boot_rom.size(); ++i)
                fprintf(stderr, "%02X ", m_boot_rom[reset_offset + i]);
            fprintf(stderr, "\n");
            if (reset_offset + 3 < m_boot_rom.size()) {
                uint32_t op = read32_le(&m_boot_rom[reset_offset]);
                fprintf(stderr, "[KI]   First instruction: 0x%08X\n", op);
            }
        }

        // Open the disc image (raw file extracted from CHD, or CHD directly).
        m_disc_path = loaded.disc_path;
        if (!m_disc_path.empty()) {
            m_chd_info = chd_open(m_disc_path);
            if (!m_chd_info) {
                // CHD failed — try as a raw disk image at the same path,
                // then try replacing .chd with .raw.
                fprintf(stderr, "[KI]   CHD open failed (%s)\n",
                        m_chd_info.error.c_str());
                auto try_raw = [&](const std::string& raw_path) -> bool {
                    std::error_code ec;
                    auto raw_size = fs::file_size(raw_path, ec);
                    if (!ec && raw_size >= 512) {
                        m_disc_path = raw_path;
                        m_chd_info.logical_bytes = (raw_size / 512) * 512;
                        m_chd_info.hunk_bytes = 4096;
                        m_chd_info.unit_bytes = 512;
                        m_chd_info.hunk_count = 0;
                        m_chd_info.error.clear();
                        m_raw_disk = true;
                        fprintf(stderr, "[KI]   Raw disk: %s (%llu bytes, %llu sectors)\n",
                                raw_path.c_str(),
                                (unsigned long long)m_chd_info.logical_bytes,
                                (unsigned long long)(m_chd_info.logical_bytes / 512));
                        return true;
                    }
                    return false;
                };
                // The extracted sibling FIRST, and only then the path itself.
                // A .chd that chd_open refused is a compressed container, and
                // every byte of it is wrong as a disk image - but it is a file
                // over 512 bytes, so treating the path as raw always "works"
                // and silently feeds the boot ROM garbage. That failure is
                // invisible: the drive answers every read, the CPU runs, and
                // the game simply never blits. Prefer the file chdman was told
                // to produce; fall back to the path itself only when it is not
                // a .chd at all (someone pointing straight at a .raw or .img).
                bool opened = false;
                std::string raw_path = m_disc_path;
                if (raw_path.size() > 4 &&
                    raw_path.compare(raw_path.size() - 4, 4, ".chd") == 0) {
                    raw_path.replace(raw_path.size() - 4, 4, ".raw");
                    opened = try_raw(raw_path);
                    if (!opened)
                        fprintf(stderr,
                                "[KI]   No extracted image at %s - run: "
                                "chdman extracthd -i %s -o %s\n",
                                raw_path.c_str(), m_disc_path.c_str(),
                                raw_path.c_str());
                }
                if (!opened) try_raw(m_disc_path);
            }
            if (!m_chd_info) {
                fprintf(stderr, "[KI]   No usable disc image\n");
                m_error = "No usable disc image";
                return false;
            }
        } else {
            fprintf(stderr, "[KI]   CHD: no disc path (fatal)\n");
            m_error = "No CHD disc image found";
            return false;
        }

        // Load NVRAM if present.
        load_nvram();

        // Create MIPS CPU.
        m_cpu = mips_create(this,
                            &wolfunit_session::s_read32,
                            &wolfunit_session::s_write32,
                            &wolfunit_session::s_read16,
                            &wolfunit_session::s_write16,
                            &wolfunit_session::s_read8,
                            &wolfunit_session::s_write8);
        if (!m_cpu) {
            m_error = "Failed to create MIPS CPU.";
            fprintf(stderr, "[KI]   CPU creation failed\n");
            return false;
        }
        fprintf(stderr, "[KI]   CPU created, PC=0x%08X\n", mips_last_pc(m_cpu));

        // Initialize video output.
        if (!m_video || !m_video->initialize(settings)) {
            m_error = "Failed to initialize video output.";
            fprintf(stderr, "[KI]   Video init failed\n");
            return false;
        }

        // Verify first instruction fetch.
        uint32_t first_pc = mips_last_pc(m_cpu);
        uint32_t phys = first_pc & 0x1FFFFFFF;
        fprintf(stderr, "[KI]   Reset PC=0x%08X phys=0x%08X\n", first_pc, phys);

        m_initialized = true;            fprintf(stderr, "[KI] initialize OK\n");
        if (s_ki_log) { FILE* f = fopen("/tmp/ki_boot.log", "a"); if (f) { fprintf(f, "[KI] initialize OK\n"); fclose(f); } }
        return true;
    }

    // ---- Emulator session interface ---------------------------------------

    void run_frame() override {
        if (!m_initialized) return;
        if (s_ki_log) { FILE* f = fopen("/tmp/ki_boot.log", "a"); if (f) { fprintf(f, "[KI] run_frame entry (frame=%d)\n", m_frame_count); fclose(f); } }

        // Poll input once per frame.
        poll_inputs();

        // Start of the active-display period: VBLANK deasserted. It is
        // asserted partway through the frame below, so a poll loop running
        // inside this frame actually observes the low->high edge rather than
        // the bit only ever being set between frames (invisible to polling).
        if (m_vblank_asserted) {
            m_vblank_asserted = false;
            mips_clear_interrupt(m_cpu, 0x08);
        }

        // KI's R4600 runs at ~100 MHz; at 60 Hz that's ~1,666,667 cycles/frame.
        constexpr uint32_t cycles_per_frame = 2000000;
        // Enter VBLANK at ~85% through the frame.
        const uint32_t vblank_at = cycles_per_frame - cycles_per_frame / 6;
        uint32_t ran = 0;
        m_blit_count = 0;
        m_pc_trace_count = 0;
        m_pc_trace_pos = 0;

        // Diagnostic: log first few PCs and first I/O access.
        static int s_frame_count = 0;
        bool first_frame = (s_frame_count == 0);
        int pc_log_count = 0;

        if (s_ki_log) { FILE* f = fopen("/tmp/ki_boot.log", "a"); if (f) { fprintf(f, "[KI] run_frame: starting CPU loop, cycles_per_frame=%u\n", cycles_per_frame); fclose(f); } }
        while (ran < cycles_per_frame) {
            constexpr uint32_t trace_interval = 8000;
            if ((ran % trace_interval) == 0 && m_pc_trace_count < pc_trace_size) {
                m_pc_trace[m_pc_trace_pos] = mips_last_pc(m_cpu);
                m_pc_trace_pos = (m_pc_trace_pos + 1) % pc_trace_size;
                ++m_pc_trace_count;
            }
            uint32_t pc_before = mips_last_pc(m_cpu);
            uint32_t cycles = mips_step(m_cpu);
            if (cycles == 0) {
                // CPU halted or error
                if (s_ki_log) { FILE* f = fopen("/tmp/ki_boot.log", "a"); if (f) { fprintf(f, "[KI] run_frame: mips_step returned 0 (halt/error) at PC=0x%08X\n", pc_before); fclose(f); } }
                break;
            }
            ran += cycles;
            if (ran > 5000000) break;

            // Cross into the VBLANK period mid-frame so an in-frame poll of
            // Cause sees the edge and proceeds.
            if (!m_vblank_asserted && ran >= vblank_at) {
                m_vblank_asserted = true;
                mips_set_interrupt(m_cpu, 0x08);  // IP3
            }

            // Log first 10 PCs of the first frame.
            if (first_frame && pc_log_count < 10) {
                uint32_t phys = pc_before & 0x1FFFFFFF;
                fprintf(stderr, "[KI]   PC[%d]=0x%08X (phys 0x%08X)\n",
                        pc_log_count, pc_before, phys);
                ++pc_log_count;
            }
        }

        if (first_frame) {
            fprintf(stderr, "[KI] Frame 0: %u cycles, ide_reg_touches=%d, "
                    "sector_reads=%d, PC=0x%08X\n",
                    ran, m_ide_access_count, m_ide_read_count,
                    mips_last_pc(m_cpu));
        }

        if (s_ki_log) { FILE* f = fopen("/tmp/ki_boot.log", "a"); if (f) { fprintf(f, "[KI] run_frame: CPU loop done, ran=%u cycles, blits=%u, PC=0x%08X\n", ran, m_blit_count, mips_last_pc(m_cpu)); fclose(f); } }

        // (VBLANK is asserted inside the CPU loop above, at vblank_at.)

        // Copy framebuffer to output.
        if (s_ki_log) { FILE* f = fopen("/tmp/ki_boot.log", "a"); if (f) { fprintf(f, "[KI] run_frame: calling render_frame\n"); fclose(f); } }
        render_frame();
        if (s_ki_log) { FILE* f = fopen("/tmp/ki_boot.log", "a"); if (f) { fprintf(f, "[KI] run_frame: render_frame done\n"); fclose(f); } }

        // Flush NVRAM periodically (every 300 frames = ~5 seconds).
        ++m_nvram_dirty_frames;
        if (m_nvram_dirty_frames >= 300) {
            flush_nvram();
            m_nvram_dirty_frames = 0;
        }
        ++s_frame_count;
    }

    arcade_host_action process_events() override {
        // Handle escape to return to menu.
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_KEY_DOWN) {
                if (event.key.key == SDLK_ESCAPE && !event.key.repeat)
                    return arcade_host_action::return_to_menu;
                if (event.key.key == SDLK_F2)
                    m_service_pressed = true;
                if (event.key.key == SDLK_F3)
                    m_test_mode = !m_test_mode;
            }
            if (event.type == SDL_EVENT_KEY_UP) {
                if (event.key.key == SDLK_F2)
                    m_service_pressed = false;
            }
        }
        return arcade_host_action::continue_running;
    }

    void set_rom_choices(const std::vector<rom_choice>& choices) override {
        (void)choices;
    }
    bool take_rom_selection(std::string& path) override {
        (void)path;
        return false;
    }
    bool take_operator_settings_request() override { return false; }
    bool take_controls_request() override { return false; }
    void open_operator_settings() override {}
    void reload_input_mappings() override {}
    bool take_settings_change(emulator_settings& settings) override {
        (void)settings;
        return false;
    }
    bool paused() const override { return m_paused; }
    void set_paused(bool paused) override { m_paused = paused; }
    void refresh_output() override {
        // Rendering is handled in run_frame() via render_frame().
        // Do not call m_video->refresh_output() here to avoid deadlock
        // with the presenter thread.
    }
    double frame_seconds() const override { return 1.0 / 60.0; }

private:
    // ---- Poll inputs from SDL ---------------------------------------------
    void poll_inputs() {
        // Read keyboard state for player 1.
        const bool* keys = SDL_GetKeyboardState(nullptr);

        m_p1_inputs = 0xFFFFFFFFu;  // All bits high = not pressed (active-low)

        // Joystick: arrow keys
        if (keys[SDL_SCANCODE_UP])    m_p1_inputs &= ~p1_joy_up;
        if (keys[SDL_SCANCODE_DOWN])  m_p1_inputs &= ~p1_joy_down;
        if (keys[SDL_SCANCODE_LEFT])  m_p1_inputs &= ~p1_joy_left;
        if (keys[SDL_SCANCODE_RIGHT]) m_p1_inputs &= ~p1_joy_right;

        // Buttons: A S D Z X C for the 6-button layout
        if (keys[SDL_SCANCODE_A]) m_p1_inputs &= ~p1_button1;  // HP/HK
        if (keys[SDL_SCANCODE_S]) m_p1_inputs &= ~p1_button2;  // MP
        if (keys[SDL_SCANCODE_D]) m_p1_inputs &= ~p1_button3;  // MK
        if (keys[SDL_SCANCODE_Z]) m_p1_inputs &= ~p1_button4;  // LP
        if (keys[SDL_SCANCODE_X]) m_p1_inputs &= ~p1_button5;  // LK
        if (keys[SDL_SCANCODE_C]) m_p1_inputs &= ~p1_button6;  // Run

        // Start & coin
        if (keys[SDL_SCANCODE_RETURN]) m_p1_inputs &= ~p1_start;
        if (keys[SDL_SCANCODE_5])      m_p1_inputs &= ~p1_coin;

        // P2: numpad
        m_p2_inputs = 0xFFFFFFFFu;
        if (keys[SDL_SCANCODE_KP_8]) m_p2_inputs &= ~p1_joy_up;
        if (keys[SDL_SCANCODE_KP_5] || keys[SDL_SCANCODE_KP_2]) m_p2_inputs &= ~p1_joy_down;
        if (keys[SDL_SCANCODE_KP_4]) m_p2_inputs &= ~p1_joy_left;
        if (keys[SDL_SCANCODE_KP_6]) m_p2_inputs &= ~p1_joy_right;
        if (keys[SDL_SCANCODE_KP_1]) m_p2_inputs &= ~p1_button1;
        if (keys[SDL_SCANCODE_KP_7]) m_p2_inputs &= ~p1_button2;
        if (keys[SDL_SCANCODE_KP_9]) m_p2_inputs &= ~p1_button3;
        if (keys[SDL_SCANCODE_KP_0]) m_p2_inputs &= ~p1_button4;
        if (keys[SDL_SCANCODE_KP_PERIOD]) m_p2_inputs &= ~p1_button5;
        if (keys[SDL_SCANCODE_KP_ENTER])  m_p2_inputs &= ~p1_button6;
        if (keys[SDL_SCANCODE_KP_PLUS])   m_p2_inputs &= ~p1_start;
        if (keys[SDL_SCANCODE_6])         m_p2_inputs &= ~p1_coin;
    }

    // ---- Bus callbacks ----------------------------------------------------

    static uint32_t s_read32(void* user, uint32_t addr) {
        return static_cast<wolfunit_session*>(user)->bus_read32(addr);
    }
    static void s_write32(void* user, uint32_t addr, uint32_t val) {
        static_cast<wolfunit_session*>(user)->bus_write32(addr, val);
    }
    static uint16_t s_read16(void* user, uint32_t addr) {
        return static_cast<wolfunit_session*>(user)->bus_read16(addr);
    }
    static void s_write16(void* user, uint32_t addr, uint16_t val) {
        static_cast<wolfunit_session*>(user)->bus_write16(addr, val);
    }
    static uint8_t s_read8(void* user, uint32_t addr) {
        return static_cast<wolfunit_session*>(user)->bus_read8(addr);
    }
    static void s_write8(void* user, uint32_t addr, uint8_t val) {
        static_cast<wolfunit_session*>(user)->bus_write8(addr, val);
    }

    // A memory helper for the three access widths. Everything decodes on
    // the 29-bit physical address so KSEG0, KSEG1 and KUSEG all reach the
    // same devices; open bus reads back 0xFFFFFFFF, matching MAME's
    // unmap_value_high.
    template <typename T>
    T mem_read(uint32_t addr) {
        const uint32_t phys = addr & 0x1FFFFFFF;
        const uint32_t a = phys & ~(sizeof(T) - 1);
        if (phys < ram0_size)
            return load_le<T>(&m_ram[a]);
        if (phys >= ram1_base && phys < ram1_base + ram1_size)
            return load_le<T>(&m_ram1[a - ram1_base]);
        if (phys >= boot_rom_base && phys < boot_rom_base + boot_rom_size)
            return load_le<T>(&m_boot_rom[(a - boot_rom_base) &
                                          (boot_rom_size - 1)]);
        if (phys >= io_ports_base && phys < ide_cs1_base + 4)
            return static_cast<T>(io_read(phys, sizeof(T)));
        return static_cast<T>(~static_cast<T>(0));
    }

    template <typename T>
    void mem_write(uint32_t addr, T val) {
        const uint32_t phys = addr & 0x1FFFFFFF;
        const uint32_t a = phys & ~(sizeof(T) - 1);
        if (phys < ram0_size) { store_le<T>(&m_ram[a], val); return; }
        if (phys >= ram1_base && phys < ram1_base + ram1_size) {
            store_le<T>(&m_ram1[a - ram1_base], val); return;
        }
        if (phys >= io_ports_base && phys < ide_cs1_base + 4)
            io_write(phys, val, sizeof(T));
        // ROM and open bus swallow writes.
    }

    uint32_t bus_read32(uint32_t addr) { return mem_read<uint32_t>(addr); }
    uint16_t bus_read16(uint32_t addr) { return mem_read<uint16_t>(addr); }
    uint8_t  bus_read8(uint32_t addr)  { return mem_read<uint8_t>(addr); }
    void bus_write32(uint32_t addr, uint32_t v) { mem_write<uint32_t>(addr, v); }
    void bus_write16(uint32_t addr, uint16_t v) { mem_write<uint16_t>(addr, v); }
    void bus_write8(uint32_t addr, uint8_t v)   { mem_write<uint8_t>(addr, v); }

    template <typename T>
    static T load_le(const void* p) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        T v = 0;
        for (unsigned i = 0; i < sizeof(T); ++i)
            v |= static_cast<T>(b[i]) << (8 * i);
        return v;
    }
    template <typename T>
    static void store_le(void* p, T v) {
        uint8_t* b = static_cast<uint8_t*>(p);
        for (unsigned i = 0; i < sizeof(T); ++i)
            b[i] = static_cast<uint8_t>(v >> (8 * i));
    }

    // ---- I/O read/write ---------------------------------------------------

    // Control ports and the IDE controller share the 0x10000000 decode.
    // `size` is 1/2/4; the CPU accesses these as 32-bit loads/stores but the
    // narrower widths are handled so nothing traps.
    uint32_t io_read(uint32_t phys, unsigned size) {
        if (phys >= ide_cs0_base && phys < ide_cs0_end) {
            const unsigned reg = (phys - ide_cs0_base) / 8;  // 8-byte spacing
            return ide_cs0_read(reg);
        }
        if (phys >= ide_cs1_base && phys < ide_cs1_base + 4)
            return ide_status();                              // alt status
        if (phys >= io_ports_base && phys < io_ports_end)
            return port_read((phys - io_ports_base) & ~3u);
        (void)size;
        return 0xFFFFFFFFu;
    }

    void io_write(uint32_t phys, uint32_t val, unsigned size) {
        if (phys >= ide_cs0_base && phys < ide_cs0_end) {
            ide_cs0_write((phys - ide_cs0_base) / 8, val);
            return;
        }
        if (phys >= ide_cs1_base && phys < ide_cs1_base + 4) {
            // Device control: SRST is bit 2.
            if (val & 0x04) { m_ide_error = 0; m_ide_busy = false;
                              m_ide_drq = false; }
            return;
        }
        if (phys >= io_ports_base && phys < io_ports_end)
            port_write((phys - io_ports_base) & ~3u, val);
        (void)size;
    }

    // The control-port block. kinst and kinst2 assign the same eight dword
    // slots to different jobs, so the port index is resolved through the set.
    uint32_t port_read(uint32_t slot) {
        const bool k2 = m_game_set == midway_rom_set::killer_instinct_2;
        switch (slot) {
        case 0x00: return k2 ? m_ki_volume : m_p1_inputs;      // P1 / VOLUME
        case 0x08: return k2 ? read_dsw()  : m_p2_inputs;      // P2 / DSW
        case 0x10: return k2 ? m_p2_inputs : m_ki_volume;      // VOLUME / P2
        case 0x18: return k2 ? m_p1_inputs : 0xFFFFFFFFu;      // UNUSED / P1
        case 0x20: return k2 ? 0xFFFFFFFFu : read_dsw();       // DSW / UNUSED
        default:   return 0xFFFFFFFFu;
        }
    }

    void port_write(uint32_t slot, uint32_t val) {
        const bool k2 = m_game_set == midway_rom_set::killer_instinct_2;
        // vram_control, sound_reset, sound_control, sound_data and
        // coin_control, at whichever slot this set puts them.
        auto vram    = [&]{ m_vram_control = val; };
        auto s_reset = [&]{ m_sound_reset = val; };
        auto s_ctrl  = [&]{ m_sound_control = val; };
        auto s_data  = [&]{ m_sound_data = val; };
        auto coin    = [&]{ m_coin_counter = val; };
        if (!k2) {
            switch (slot) {
            case 0x00: vram(); break; case 0x08: s_reset(); break;
            case 0x10: s_ctrl(); break; case 0x18: s_data(); break;
            case 0x30: coin(); break; default: break;
            }
        } else {
            switch (slot) {
            case 0x00: s_ctrl(); break; case 0x10: s_reset(); break;
            case 0x18: vram(); break;  case 0x20: s_data(); break;
            case 0x38: coin(); break; default: break;
            }
        }
    }

    uint32_t read_dsw() const {
        uint32_t dips = m_ki_dip_switches;
        if (m_service_pressed) dips &= ~(1u << 0);
        if (m_test_mode)       dips &= ~(1u << 1);
        return dips;
    }

    // ---- IDE register file (ATA over cs0, 8 registers) -------------------
    uint32_t ide_cs0_read(unsigned reg) {
        ++m_ide_access_count;
        switch (reg) {
        case 0: {  // data: two bytes, low first
            uint32_t lo = ide_pop_data_byte(0);
            uint32_t hi = ide_pop_data_byte(1);
            return lo | (hi << 8);
        }
        case 1: return m_ide_error;
        case 2: return m_ide_sector_count_val;
        case 3: return m_ide_sector_num_val;
        case 4: return m_ide_cylinder_low_val;
        case 5: return m_ide_cylinder_high_val;
        case 6: return m_ide_drive_head_val;
        case 7: return ide_status();
        default: return 0xFFFFFFFFu;
        }
    }

    void ide_cs0_write(unsigned reg, uint32_t val) {
        ++m_ide_access_count;
        const uint8_t b = static_cast<uint8_t>(val);
        switch (reg) {
        case 0:  // data
            if (m_ide_writing) {
                ide_capture_write_byte(static_cast<uint8_t>(val), 0);
                ide_capture_write_byte(static_cast<uint8_t>(val >> 8), 1);
            }
            break;
        case 1: m_ide_features = b; break;
        case 2: m_ide_sector_count_val = b; break;
        case 3: m_ide_sector_num_val = b; break;
        case 4: m_ide_cylinder_low_val = b; break;
        case 5: m_ide_cylinder_high_val = b; break;
        case 6: m_ide_drive_head_val = b; break;
        case 7: ide_command(b); break;
        default: break;
        }
    }

    // ---- DCS Audio stubs -------------------------------------------------
    //
    // The Wolf Unit DCS system uses an ADSP-2105 DSP. We provide minimal
    // register responses so the game's audio driver doesn't hang.
    // Real implementation would need full ADSP-2105 emulation.

    // ---- IDE Controller register state ----------------------------------
    uint8_t ide_status() const {
        uint8_t status = ide_status_rdy;
        if (m_ide_busy) status |= ide_status_bsy;
        if (m_ide_drq)  status |= ide_status_drq;
        if (m_ide_error != 0) status |= ide_status_err;
        return status;
    }

    // Compute LBA from current register values.
    // If the LBA bit (0x40) is set in the drive/head register, interpret
    // the registers as a 28-bit LBA; otherwise use standard CHS.
    uint64_t ide_chs_to_lba() const {
        if (m_ide_drive_head_val & 0x40) {
            // 28-bit LBA: head[3:0] = bits 27:24, cyl_high = 23:16,
            // cyl_low = 15:8, sector = 7:0.
            return (static_cast<uint64_t>(m_ide_drive_head_val & 0x0F) << 24) |
                   (static_cast<uint64_t>(m_ide_cylinder_high_val) << 16) |
                   (static_cast<uint64_t>(m_ide_cylinder_low_val) << 8) |
                   static_cast<uint64_t>(m_ide_sector_num_val);
        }
        const uint8_t  head   = m_ide_drive_head_val & 0x0F;
        const uint16_t cyl    = (static_cast<uint16_t>(m_ide_cylinder_high_val) << 8) |
                                 m_ide_cylinder_low_val;
        const uint8_t  sector = m_ide_sector_num_val;
        return (static_cast<uint64_t>(cyl) * 16 + head) * 63 +
               (sector > 0 ? sector - 1 : 0);
    }

    uint8_t ide_sector_count() const {
        uint8_t count = m_ide_sector_count_val;
        return count == 0 ? 256 : count;
    }

    void ide_command(uint8_t cmd) {
        switch (cmd) {
        case 0x20: // READ SECTORS (with retry)
        case 0x21: // READ SECTORS (no retry)
            ide_read_sectors();
            break;
        case 0x30: // WRITE SECTORS (with retry)
        case 0x31: // WRITE SECTORS (no retry)
            ide_write_sectors();
            break;
        case 0xC4: // READ MULTIPLE
            ide_read_sectors();
            break;
        case 0xC5: // WRITE MULTIPLE
            ide_write_sectors();
            break;
        case 0x91: // INITIALIZE DEVICE PARAMETERS
            m_ide_error = 0;
            m_ide_busy  = false;
            m_ide_drq   = false;
            break;
        case 0xE0: // STANDBY IMMEDIATE
        case 0xE1: // IDLE IMMEDIATE
        case 0xE7: // FLUSH CACHE
            m_ide_error = 0;
            m_ide_busy  = false;
            m_ide_drq   = false;
            break;
        case 0xE5: // CHECK POWER MODE
            m_ide_sector_count_val = 0xFF;  // Standby
            m_ide_error = 0;
            m_ide_busy  = false;
            break;
        case 0xEC: // IDENTIFY DEVICE
            ide_identify();
            break;
        case 0xEF: // SET FEATURES
            m_ide_error = 0;
            m_ide_busy  = false;
            break;
        default:
            m_ide_error = 0x04;  // ABRT
            m_ide_busy  = false;
            m_ide_drq   = false;
            break;
        }
    }

    void ide_read_sectors() {
        if (m_disc_path.empty() || m_chd_info.logical_bytes == 0) {
            m_ide_error = 0x04;
            m_ide_busy  = false;
            return;
        }

        const uint64_t lba = ide_chs_to_lba();
        const uint8_t count = ide_sector_count();

        std::vector<uint8_t> sector_buf(512 * count);
        bool read_ok = false;

        if (m_raw_disk) {
            std::ifstream raw(m_disc_path, std::ios::binary);
            if (raw) {
                raw.seekg(static_cast<std::streamoff>(lba * 512),
                          std::ios::beg);
                raw.read(reinterpret_cast<char*>(sector_buf.data()),
                         static_cast<std::streamsize>(512 * count));
                read_ok = (raw.gcount() ==
                           static_cast<std::streamsize>(512 * count));
            }
            if (!read_ok) {
                fprintf(stderr, "[KI IDE] raw disk read failed LBA=%llu\n",
                        static_cast<unsigned long long>(lba));
            }
        } else {
            std::size_t read = chd_read_sectors(m_disc_path, m_chd_info,
                                                lba, sector_buf.data(), count);
            read_ok = (read == count);
        }

        if (!read_ok) {
            m_ide_error = 0x04;
            m_ide_busy  = false;
            m_ide_drq   = false;
            return;
        }

        // Overlay NVRAM-persisted sectors onto the CHD data.
        for (uint8_t i = 0; i < count; ++i) {
            const uint64_t sector_lba = lba + i;
            const auto nvram_entry = m_nvram_map.find(sector_lba);
            if (nvram_entry != m_nvram_map.end()) {
                std::memcpy(&sector_buf[i * 512],
                            nvram_entry->second.data(), 512);
            }
        }

        m_ide_sector_buf = std::move(sector_buf);
        m_ide_sector_pos = 0;
        m_ide_drq  = true;
        m_ide_busy = false;
        m_ide_error = 0;

        if (m_ide_read_count < 4) {
            ++m_ide_read_count;
            fprintf(stderr, "[KI IDE] READ SECTORS LBA=%llu count=%u\n",
                    static_cast<unsigned long long>(lba),
                    (unsigned)count);
        }
    }

    void ide_write_sectors() {
        // Prepare buffer for write data. The game writes via the data register
        // (0x500000/0x500001), captured by ide_write() -> ide_capture_write_byte().
        const uint8_t count = ide_sector_count();
        m_ide_sector_buf.assign(512 * count, 0);
        m_ide_sector_pos = 0;
        m_ide_drq  = true;  // Ready to accept data
        m_ide_busy = false;
        m_ide_error = 0;
        m_ide_writing = true;
        m_ide_write_lba = ide_chs_to_lba();
        m_ide_write_count = count;
        m_ide_write_byte_pos = 0;
    }

    // Capture one byte of write data from an IDE data-register store.
    // When the full transfer completes, commit to the NVRAM overlay.
    void ide_capture_write_byte(uint8_t val, int byte_offset) {
        if (!m_ide_writing) return;
        if (m_ide_write_byte_pos >= m_ide_sector_buf.size()) return;
        m_ide_sector_buf[m_ide_write_byte_pos + byte_offset] = val;
        // Only advance on high-byte write (word-aligned; same as read path).
        if (byte_offset == 1) {
            m_ide_write_byte_pos += 2;
        }
        if (m_ide_write_byte_pos >= m_ide_sector_buf.size()) {
            // All data received: commit to NVRAM overlay.
            for (uint8_t i = 0; i < m_ide_write_count; ++i) {
                uint64_t lba = m_ide_write_lba + i;
                std::vector<uint8_t> sector(512);
                std::memcpy(sector.data(),
                            &m_ide_sector_buf[i * 512], 512);
                m_nvram_map[lba] = std::move(sector);
            }
            m_nvram_dirty_frames = 0;
            m_ide_writing = false;
            m_ide_drq = false;
        }
    }

    uint8_t ide_pop_data_byte(int byte_offset) {
        // During write, read path returns 0; data flows ide_write -> ide_capture_write_byte.
        if (m_ide_writing) return 0;
        if (m_ide_sector_pos >= m_ide_sector_buf.size()) {
            m_ide_drq = false;
            return 0;
        }
        uint32_t pos = m_ide_sector_pos & ~1u;
        if (pos + 1 >= m_ide_sector_buf.size()) {
            m_ide_drq = false;
            return 0;
        }
        uint8_t val = m_ide_sector_buf[pos + byte_offset];
        if (byte_offset == 1) {
            m_ide_sector_pos += 2;
            if (m_ide_sector_pos >= m_ide_sector_buf.size())
                m_ide_drq = false;
        }
        return val;
    }

    void ide_identify() {
        std::vector<uint8_t> identify(512, 0);

        // Word 0: General configuration (0x0040 = ATA device)
        write16_le(&identify[0], 0x0040);

        const uint32_t total_sectors =
            static_cast<uint32_t>(m_chd_info.logical_bytes / 512);
        write16_le(&identify[2], 16);           // heads
        write16_le(&identify[12], 63);          // sectors per track
        write16_le(&identify[6], total_sectors / (16 * 63));

        // Words 60-61: LBA28 total sectors
        uint8_t* lba = &identify[120];
        lba[0] = static_cast<uint8_t>(total_sectors);
        lba[1] = static_cast<uint8_t>(total_sectors >> 8);
        lba[2] = static_cast<uint8_t>(total_sectors >> 16);
        lba[3] = static_cast<uint8_t>(total_sectors >> 24);

        // Word 49: Capabilities (LBA supported)
        write16_le(&identify[98], 0x0200);

        const char* model = "KI HARD DISK     ";
        for (int i = 0; i < 20; ++i)
            identify[54 + i * 2] = static_cast<uint8_t>(model[i]);

        m_ide_sector_buf = std::move(identify);
        m_ide_sector_pos = 0;
        m_ide_drq  = true;
        m_ide_busy = false;
        m_ide_error = 0;
    }

    // ---- NVRAM persistence -----------------------------------------------
    //
    // Written sectors are accumulated in an in-memory overlay and flushed
    // to disk so high scores, settings, and operator adjustments survive
    // across sessions.

    void load_nvram() {
        if (m_game_set == midway_rom_set::unknown) return;
        const std::string name = midway_rom_loader::set_short_name(m_game_set);
        const fs::path nvram_path = nvram_file(name);
        std::error_code ec;
        if (!fs::is_regular_file(nvram_path, ec)) return;

        std::ifstream file(nvram_path, std::ios::binary);
        if (!file) return;
        m_nvram_overlay.assign(std::istreambuf_iterator<char>(file),
                               std::istreambuf_iterator<char>());

        // Apply overlay to CHD reads by pre-loading the overlay map.
        // Overlay entries: 8-byte LBA + 512-byte sector data, repeated.
        const std::size_t entry_size = 8 + 512;
        for (std::size_t pos = 0;
             pos + entry_size <= m_nvram_overlay.size();
             pos += entry_size) {
            uint64_t lba = read64_be(&m_nvram_overlay[pos]);
            std::vector<uint8_t> sector(512);
            std::memcpy(sector.data(), &m_nvram_overlay[pos + 8], 512);
            m_nvram_map[lba] = std::move(sector);
        }
    }

    void flush_nvram() {
        if (m_nvram_map.empty()) return;
        if (m_game_set == midway_rom_set::unknown) return;

        const std::string name = midway_rom_loader::set_short_name(m_game_set);
        const fs::path nvram_path = nvram_file(name);

        // Ensure parent directory exists.
        fs::path parent = nvram_path.parent_path();
        std::error_code ec;
        fs::create_directories(parent, ec);

        // Serialize: each entry is 8-byte LBA (big-endian) + 512-byte sector.
        std::vector<uint8_t> data;
        for (const auto& [lba, sector] : m_nvram_map) {
            uint8_t lba_bytes[8];
            write64_be(lba_bytes, lba);
            data.insert(data.end(), lba_bytes, lba_bytes + 8);
            data.insert(data.end(), sector.begin(), sector.end());
        }

        std::ofstream file(nvram_path, std::ios::binary | std::ios::trunc);
        if (file)
            file.write(reinterpret_cast<const char*>(data.data()),
                       static_cast<std::streamsize>(data.size()));

        m_nvram_map.clear();
        m_nvram_dirty_frames = 0;
    }

    fs::path nvram_file(const std::string& short_name) const {
        const char* home = std::getenv("HOME");
        fs::path base = home ? fs::path(home) / ".local" / "share" / "MANX" / "nvram"
                             : fs::path("nvram");
        return base / (short_name + ".nvram");
    }

    // ---- Rendering -------------------------------------------------------

    // The framebuffer is plain RAM in the low bank: 320x240, two BGR-555
    // pixels per 32-bit word, 640 bytes per row, at page 0x30000 or 0x58000
    // depending on vram_control bit 2. There is no ASIC register and no
    // blitter - the game software-renders straight into this RAM.
    void render_frame() {
        if (!m_video) return;
        constexpr int W = video_width, H = video_height;
        m_framebuffer.resize(W * H * 4);
        const uint32_t base = (m_vram_control & 0x04) ? video_fb_page1
                                                      : video_fb_page0;
        bool any = false;
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; x += 2) {
                const uint32_t off = base + y * 640 + x * 2;
                uint32_t word = 0;
                if (off + 3 < m_ram.size()) word = load_le<uint32_t>(&m_ram[off]);
                if (word) any = true;
                put_bgr555(x, y, static_cast<uint16_t>(word & 0x7FFF));
                put_bgr555(x + 1, y, static_cast<uint16_t>((word >> 16) & 0x7FFF));
            }
        }
        if (!any) draw_debug_frame(W, H);
        m_video->present_rgba_frame(m_framebuffer.data(), W, H);
        ++m_frame_count;
    }

    // BGR-555 -> RGBA8888 (R at bits 0-4, G 5-9, B 10-14; MAME BGR_555).
    void put_bgr555(int x, int y, uint16_t v) {
        const uint32_t dst = static_cast<uint32_t>(y * video_width + x) * 4;
        if (dst + 3 >= m_framebuffer.size()) return;
        const uint8_t r = static_cast<uint8_t>((v & 0x1F) << 3 | (v & 0x1F) >> 2);
        const uint8_t g = static_cast<uint8_t>(((v >> 5) & 0x1F) << 3 |
                                               ((v >> 5) & 0x1F) >> 2);
        const uint8_t b = static_cast<uint8_t>(((v >> 10) & 0x1F) << 3 |
                                               ((v >> 10) & 0x1F) >> 2);
        m_framebuffer[dst] = r;
        m_framebuffer[dst + 1] = g;
        m_framebuffer[dst + 2] = b;
        m_framebuffer[dst + 3] = 0xFF;
    }

    void draw_debug_frame(int fb_width, int fb_height) {
        // Dark blue gradient background.
        for (int y = 0; y < fb_height; ++y) {
            for (int x = 0; x < fb_width; ++x) {
                uint32_t dst = (y * fb_width + x) * 4;
                m_framebuffer[dst]     = static_cast<uint8_t>(x * 255 / fb_width);
                m_framebuffer[dst + 1] = 0;
                m_framebuffer[dst + 2] = static_cast<uint8_t>(255 - (y * 255 / fb_height));
                m_framebuffer[dst + 3] = 0xFF;
            }
        }

        // MIPS PC indicator.
        uint32_t pc = mips_last_pc(m_cpu);
        int px = static_cast<int>((pc & 0xFFF) * fb_width / 0x1000) % fb_width;
        int py = static_cast<int>(((pc >> 12) & 0xFF) * fb_height / 0x100) % fb_height;
        for (int dy = -4; dy <= 4; ++dy)
            for (int dx = -4; dx <= 4; ++dx) {
                int sx = px + dx, sy = py + dy;
                if (sx >= 0 && sx < fb_width && sy >= 0 && sy < fb_height) {
                    uint32_t dst = (sy * fb_width + sx) * 4;
                    m_framebuffer[dst]     = 0xFF;
                    m_framebuffer[dst + 1] = 0xFF;
                    m_framebuffer[dst + 2] = 0x00;
                    m_framebuffer[dst + 3] = 0xFF;
                }
            }

        // Status text via colored bars.
        // Green bar = blit count.
        int bar_w = std::min(static_cast<int>(m_blit_count * 3), fb_width);
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < bar_w; ++x) {
                uint32_t dst = (y * fb_width + x) * 4;
                m_framebuffer[dst]     = 0x00;
                m_framebuffer[dst + 1] = 0xFF;
                m_framebuffer[dst + 2] = 0x00;
                m_framebuffer[dst + 3] = 0xFF;
            }

        // Red dot = last blit PC.
        if (m_last_blit_pc != 0) {
            int bx = static_cast<int>((m_last_blit_pc & 0xFFF) * fb_width / 0x1000) % fb_width;
            int by = static_cast<int>(((m_last_blit_pc >> 12) & 0xFF) * fb_height / 0x100) % fb_height;
            for (int dy = -2; dy <= 2; ++dy)
                for (int dx = -2; dx <= 2; ++dx) {
                    int sx = bx + dx, sy = by + dy;
                    if (sx >= 0 && sx < fb_width && sy >= 0 && sy < fb_height) {
                        uint32_t dst = (sy * fb_width + sx) * 4;
                        m_framebuffer[dst]     = 0xFF;
                        m_framebuffer[dst + 1] = 0x00;
                        m_framebuffer[dst + 2] = 0x00;
                        m_framebuffer[dst + 3] = 0xFF;
                    }
                }
        }

        // PC trace at bottom.
        int trace_y = fb_height - 3;
        for (int i = 0; i < std::min(m_pc_trace_count, pc_trace_size); ++i) {
            int tx = i * fb_width / pc_trace_size;
            uint32_t pc_val = m_pc_trace[(m_pc_trace_pos - m_pc_trace_count + i + pc_trace_size) % pc_trace_size];
            for (int dy = 0; dy < 3; ++dy) {
                uint32_t dst = ((trace_y + dy) * fb_width + std::min(tx, fb_width - 1)) * 4;
                m_framebuffer[dst]     = static_cast<uint8_t>((pc_val >> 8) & 0xFF);
                m_framebuffer[dst + 1] = static_cast<uint8_t>((pc_val >> 16) & 0xFF);
                m_framebuffer[dst + 2] = static_cast<uint8_t>(pc_val & 0xFF);
                m_framebuffer[dst + 3] = 0xFF;
            }
        }

        // Info text: game name + frame count in top-left.
        const char* game_name = m_game_set == midway_rom_set::killer_instinct ?
            "KILLER INSTINCT" :
            (m_game_set == midway_rom_set::killer_instinct_2 ?
             "KILLER INSTINCT 2" : "MIDWAY WOLF UNIT");
        char info[64];
        snprintf(info, sizeof(info), "%s  FRAME %u  BLITS %u",
                 game_name, m_frame_count, m_blit_count);
        // Simple pixel text: just color the top row based on frame count.
        for (int x = 0; x < fb_width && x / 6 < static_cast<int>(strlen(info)); ++x) {
            if (info[x / 6] == 0) break;
            for (int dy = 0; dy < 6; ++dy) {
                uint32_t dst = (dy * fb_width + x) * 4;
                m_framebuffer[dst]     = 0xFF;
                m_framebuffer[dst + 1] = 0xFF;
                m_framebuffer[dst + 2] = 0xFF;
                m_framebuffer[dst + 3] = 0xFF;
            }
        }
    }

    // ---- Endian helpers ---------------------------------------------------

    // Killer Instinct runs an R4600 in LITTLE-endian mode - MAME configures
    // the CPU as R4600LE and loads the boot ROM as ROM_REGION32_LE. Read the
    // other way round, the very first instruction at the reset vector
    // (bytes E2 00 F0 0B = 0x0BF000E2, "j 0xBFC00388") decodes as a nonsense
    // store, so the CPU falls through the exception vectors one word at a
    // time and never reaches the code that talks to the drive.
    //
    // The ATA IDENTIFY block below shares these helpers for its own reason:
    // its 16-bit fields are little-endian on the wire, and the guest pulls
    // them a byte at a time through the data port.
    static uint32_t read32_le(const void* p) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        return static_cast<uint32_t>(b[0]) |
               (static_cast<uint32_t>(b[1]) << 8) |
               (static_cast<uint32_t>(b[2]) << 16) |
               (static_cast<uint32_t>(b[3]) << 24);
    }

    static void write32_le(void* p, uint32_t v) {
        uint8_t* b = static_cast<uint8_t*>(p);
        b[0] = static_cast<uint8_t>(v);
        b[1] = static_cast<uint8_t>(v >> 8);
        b[2] = static_cast<uint8_t>(v >> 16);
        b[3] = static_cast<uint8_t>(v >> 24);
    }

    static uint16_t read16_le(const void* p) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        return static_cast<uint16_t>(b[0] | (b[1] << 8));
    }

    static void write16_le(void* p, uint16_t v) {
        uint8_t* b = static_cast<uint8_t*>(p);
        b[0] = static_cast<uint8_t>(v);
        b[1] = static_cast<uint8_t>(v >> 8);
    }

    static uint64_t read64_be(const void* p) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        return (static_cast<uint64_t>(b[0]) << 56) |
               (static_cast<uint64_t>(b[1]) << 48) |
               (static_cast<uint64_t>(b[2]) << 40) |
               (static_cast<uint64_t>(b[3]) << 32) |
               (static_cast<uint64_t>(b[4]) << 24) |
               (static_cast<uint64_t>(b[5]) << 16) |
               (static_cast<uint64_t>(b[6]) << 8) |
               static_cast<uint64_t>(b[7]);
    }

    static void write64_be(void* p, uint64_t v) {
        uint8_t* b = static_cast<uint8_t*>(p);
        b[0] = static_cast<uint8_t>(v >> 56);
        b[1] = static_cast<uint8_t>(v >> 48);
        b[2] = static_cast<uint8_t>(v >> 40);
        b[3] = static_cast<uint8_t>(v >> 32);
        b[4] = static_cast<uint8_t>(v >> 24);
        b[5] = static_cast<uint8_t>(v >> 16);
        b[6] = static_cast<uint8_t>(v >> 8);
        b[7] = static_cast<uint8_t>(v);
    }

    // ---- Members ----------------------------------------------------------

    std::shared_ptr<arcade_video_worker> m_video;
    std::shared_ptr<arcade_cabinet_state> m_cabinet_state;

    std::string m_rom_path;
    std::string m_error;
    bool m_initialized{};
    bool m_paused{};
    bool m_in_pci_domain{};    // CPU is currently executing in PCI/Voodoo space
    uint32_t m_pci_entry_phys{}; // physical address where we entered PCI space

    midway_rom_set m_game_set{midway_rom_set::unknown};

    // Memory.
    std::vector<uint8_t> m_ram;         // 512 KiB low bank (framebuffer here)
    std::vector<uint8_t> m_ram1;        // 8 MiB main work RAM at 0x08000000
    std::vector<uint8_t> m_boot_rom;
    std::vector<uint8_t> m_dcs_rom;     // u10-u36 DCS sound data (not CPU bus)
    uint32_t m_vram_control{};          // bit 2 selects the framebuffer page
    uint32_t m_sound_control{};
    uint32_t m_sound_data{};
    uint32_t m_sound_reset{};
    int m_ide_access_count{};           // any IDE register touch (first_ide)

    // CHD / IDE.
    std::string m_disc_path;
    chd_info m_chd_info;
    bool m_raw_disk{};  // true if reading from raw file, not CHD
    std::vector<uint8_t> m_ide_sector_buf;
    std::size_t m_ide_sector_pos{};
    uint8_t m_ide_error{};
    uint8_t m_ide_sector_count_val{};
    uint8_t m_ide_sector_num_val{};
    uint8_t m_ide_cylinder_low_val{};
    uint8_t m_ide_cylinder_high_val{};
    uint8_t m_ide_drive_head_val{};
    uint8_t m_ide_features{};
    bool m_ide_busy{};
    bool m_ide_drq{};
    bool m_ide_writing{};
    uint64_t m_ide_write_lba{};
    uint8_t m_ide_write_count{};
    std::size_t m_ide_write_byte_pos{};

    // MIPS CPU.
    mips_cpu* m_cpu{};

    // ASIC / video state.
    uint32_t m_asic_fb_base{};
    uint32_t m_asic_fb_stride{};
    uint32_t m_asic_display_list{};
    uint32_t m_asic_control{};
    uint32_t m_frame_count{};

    // DMA blitter state.
    uint32_t m_blit_src{};
    uint32_t m_blit_dst{};
    uint32_t m_blit_size{};
    uint32_t m_blit_strides{};
    uint32_t m_blit_control{};
    uint32_t m_blit_fill_color{};
    uint32_t m_blit_count{};
    uint32_t m_last_blit_pc{};

    // Input state.
    uint32_t m_p1_inputs{0xFFFFFFFFu};
    uint32_t m_p2_inputs{0xFFFFFFFFu};
    uint32_t m_ki_dip_switches{0xFFFFFFFFu};  // All off by default
    uint32_t m_ki_volume{0xFFFFFFFFu};        // volume/analog port, unused
    bool m_service_pressed{};
    bool m_test_mode{};
    uint32_t m_coin_counter{};

    // DCS audio state.
    uint32_t m_dcs_control{};
    uint32_t m_dcs_data{};
    uint32_t m_dcs_reset_state{};

    // VBLANK interrupt management.
    bool m_vblank_asserted{};

    // NVRAM overlay for written sectors.
    std::vector<uint8_t> m_nvram_overlay;
    std::map<uint64_t, std::vector<uint8_t>> m_nvram_map;
    int m_nvram_dirty_frames{};

    // PC trace ring buffer.
    static constexpr int pc_trace_size = 256;
    uint32_t m_pc_trace[pc_trace_size]{};
    int m_pc_trace_pos{};
    int m_pc_trace_count{};

    // IDE read diagnostic.
    int m_ide_read_count{};

    // Framebuffer.
    std::vector<uint8_t> m_framebuffer;

    // Prevent accidental copying (owns MIPS CPU pointer).
    wolfunit_session(const wolfunit_session&) = delete;
    wolfunit_session& operator=(const wolfunit_session&) = delete;
};

} // namespace

// ---- Factory --------------------------------------------------------------

std::unique_ptr<emulator_session> make_midway_session(
    std::shared_ptr<arcade_video_worker> video,
    std::shared_ptr<arcade_cabinet_state> cabinet) {
    if (s_ki_log) { FILE* f = fopen("/tmp/ki_boot.log", "a"); if (f) { fprintf(f, "[KI FACTORY] make_midway_session called\n"); fclose(f); } }
    return std::make_unique<wolfunit_session>(
        std::move(video), std::move(cabinet));
}
