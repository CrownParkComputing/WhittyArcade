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

// ---- Midway Wolf Unit constants -------------------------------------------

constexpr uint32_t ram_size       = 16 * 1024 * 1024;  // 16 MB
constexpr uint32_t boot_rom_base  = 0x1FC00000;
constexpr uint32_t boot_rom_size  = 0x80000;          // 512 KiB
constexpr uint32_t io_base        = 0x1F000000;
constexpr uint32_t io_size        = 0x00C00000;  // covers up to 0x1FBFFFFF
constexpr uint32_t game_rom_base  = 0x1F800000;  // game ROMs u10-u36
constexpr uint32_t game_rom_size  = 0x400000;    // 8 x 512KB = 4MB

// I/O register offsets from io_base
constexpr uint32_t dcs_offset          = 0x200000;
constexpr uint32_t video_control_offset = 0x400000;
constexpr uint32_t blitter_offset       = 0x400100;
constexpr uint32_t p1_input_offset      = 0x400400;
constexpr uint32_t p2_input_offset      = 0x400C00;
constexpr uint32_t coin_dip_offset      = 0x401000;

// IDE register offsets
constexpr uint32_t ide_data_offset      = 0x500000;
constexpr uint32_t ide_error_offset     = 0x500004;
constexpr uint32_t ide_sector_count     = 0x500008;
constexpr uint32_t ide_sector_num       = 0x50000C;
constexpr uint32_t ide_cylinder_low     = 0x500010;
constexpr uint32_t ide_cylinder_high    = 0x500014;
constexpr uint32_t ide_drive_head       = 0x500018;
constexpr uint32_t ide_command_status   = 0x50001C;
constexpr uint32_t ide_alt_status_ctrl  = 0x500020;

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
          m_ram(ram_size),
          m_boot_rom(boot_rom_size),
          m_game_roms(game_rom_size, 0xFF),
          m_framebuffer(320 * 240 * 4)
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
        { FILE* f = fopen("/tmp/ki_boot.log", "a"); if (f) { fprintf(f, "[KI] initialize called: rom_path=%s\n", rom_path.c_str()); fclose(f); } }

        // Load ROMs.
        const std::string chd_dir = settings.chd_directory.empty() ?
            "" : settings.chd_directory;
        fprintf(stderr, "[KI]   chd_dir=%s\n", chd_dir.c_str());
        midway_rom_load_result loaded =
            midway_rom_loader::load(rom_path, chd_dir);
        if (!loaded) {
            m_error = loaded.error;
            fprintf(stderr, "[KI]   ROM LOAD FAILED: %s\n", loaded.error.c_str());
            { FILE* f = fopen("/tmp/ki_boot.log", "a"); if (f) { fprintf(f, "[KI] ROM LOAD FAILED: %s\n", loaded.error.c_str()); fclose(f); } }
            return false;
        }

        m_game_set = loaded.set;
        fprintf(stderr, "[KI]   ROM set=%s boot_rom=%zu bytes disc=%s\n",
                midway_rom_loader::set_short_name(loaded.set),
                loaded.boot_rom.size(),
                loaded.disc_path.c_str());
        std::memcpy(m_boot_rom.data(), loaded.boot_rom.data(),
                    std::min(loaded.boot_rom.size(), m_boot_rom.size()));

        // Copy game ROMs (u10-u36).
        if (!loaded.game_roms.empty()) {
            std::memcpy(m_game_roms.data(), loaded.game_roms.data(),
                       std::min(loaded.game_roms.size(), m_game_roms.size()));
            fprintf(stderr, "[KI]   Game ROMs loaded: %zu bytes\n",
                    loaded.game_roms.size());
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
                uint32_t op = read32_be(&m_boot_rom[reset_offset]);
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
                if (!try_raw(m_disc_path)) {
                    // Replace .chd -> .raw
                    std::string raw_path = m_disc_path;
                    if (raw_path.size() > 4 &&
                        raw_path.compare(raw_path.size() - 4, 4, ".chd") == 0) {
                        raw_path.replace(raw_path.size() - 4, 4, ".raw");
                        try_raw(raw_path);
                    }
                }
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
        { FILE* f = fopen("/tmp/ki_boot.log", "a"); if (f) { fprintf(f, "[KI] initialize OK\n"); fclose(f); } }
        return true;
    }

    // ---- Emulator session interface ---------------------------------------

    void run_frame() override {
        if (!m_initialized) return;
        { FILE* f = fopen("/tmp/ki_boot.log", "a"); if (f) { fprintf(f, "[KI] run_frame entry (frame=%d)\n", m_frame_count); fclose(f); } }

        // Poll input once per frame.
        poll_inputs();

        // KI's R4600 runs at ~100 MHz; at 60 Hz that's ~1,666,667 cycles/frame.
        constexpr uint32_t cycles_per_frame = 2000000;
        uint32_t ran = 0;
        m_blit_count = 0;
        m_pc_trace_count = 0;
        m_pc_trace_pos = 0;

        // Diagnostic: log first few PCs and first I/O access.
        static int s_frame_count = 0;
        bool first_frame = (s_frame_count == 0);
        int pc_log_count = 0;

        { FILE* f = fopen("/tmp/ki_boot.log", "a"); if (f) { fprintf(f, "[KI] run_frame: starting CPU loop, cycles_per_frame=%u\n", cycles_per_frame); fclose(f); } }
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
                { FILE* f = fopen("/tmp/ki_boot.log", "a"); if (f) { fprintf(f, "[KI] run_frame: mips_step returned 0 (halt/error) at PC=0x%08X\n", pc_before); fclose(f); } }
                break;
            }
            ran += cycles;
            if (ran > 5000000) break;

            // Log first 10 PCs of the first frame.
            if (first_frame && pc_log_count < 10) {
                uint32_t phys = pc_before & 0x1FFFFFFF;
                fprintf(stderr, "[KI]   PC[%d]=0x%08X (phys 0x%08X)\n",
                        pc_log_count, pc_before, phys);
                ++pc_log_count;
            }
        }

        if (first_frame) {
            fprintf(stderr, "[KI] Frame 0: %u cycles, %u blits, first_ide=%d\n",
                    ran, m_blit_count, m_ide_read_count);
            fprintf(stderr, "[KI]   VBLANK assert (IP1 set, held until ASIC write)\n");
        }

        { FILE* f = fopen("/tmp/ki_boot.log", "a"); if (f) { fprintf(f, "[KI] run_frame: CPU loop done, ran=%u cycles, blits=%u, PC=0x%08X\n", ran, m_blit_count, mips_last_pc(m_cpu)); fclose(f); } }

        // VBLANK interrupt.
        // Assert IP1 and leave it set until the game's interrupt handler
        // acknowledges it by writing to the video ASIC control register.
        m_vblank_asserted = true;
        mips_set_interrupt(m_cpu, 0x02);  // IP1

        // Copy framebuffer to output.
        { FILE* f = fopen("/tmp/ki_boot.log", "a"); if (f) { fprintf(f, "[KI] run_frame: calling render_frame\n"); fclose(f); } }
        render_frame();
        { FILE* f = fopen("/tmp/ki_boot.log", "a"); if (f) { fprintf(f, "[KI] run_frame: render_frame done\n"); fclose(f); } }

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

    uint32_t bus_read32(uint32_t addr) {
        uint32_t phys = addr & 0x1FFFFFFF;

        // Reset PCI-domain flag when the CPU reads outside Voodoo space.
        if (phys < 0x10000000 || phys >= 0x1F000000)
            m_in_pci_domain = false;

        // PCI / Voodoo Graphics space (0x10000000 - 0x1EFFFFFF).
        // The boot ROM JALs to 0xB4036BFC (phys 0x14036BFC) for GPU
        // init. Return JR $ra there so the function returns cleanly.
        // For all other PCI addresses (GPU status registers), return 0
        // so status polls see "ready" and continue.
        if (phys >= 0x10000000 && phys < 0x1F000000) {
            static int pci_log = 0;
            if (pci_log < 10) {
                { FILE* f = fopen("/tmp/ki_boot.log", "a"); if (f) { fprintf(f, "[PCI-R32] #%d PC=0x%08X phys=0x%08X\n", pci_log, mips_last_pc(m_cpu), phys); fclose(f); } }
                ++pci_log;
            }
            // Detect entry into PCI space from non-PCI domain.
            // This is a JAL/J target — return JR $ra pair so it returns
            // cleanly. Also write "GPU ready" flags to RAM so the boot
            // ROM's status check finds the GPU initialised.
            if (!m_in_pci_domain) {
                m_in_pci_domain = true;
                m_pci_entry_phys = phys;
                // Inject GPU-done flags into RAM.
                if (0x5AD4 < ram_size) write32_be(&m_ram[0x5AD4], 0x00000001u);
                if (0x6268 < ram_size) write32_be(&m_ram[0x6268], 0x00000001u);
                return 0x03E00008;  // JR $ra on entry
            }
            // Still in PCI: return NOP for delay slot and any subsequent
            // linear execution. After JR $ra returns, the CPU will be
            // back in boot ROM, which resets m_in_pci_domain below.
            return 0x00000000;  // NOP
        }

        // Log ALL unmapped reads (not RAM/ROM/I/O) to find status checks.
        if (phys >= 0x10000000 || phys >= ram_size) {
            static int unmapped_log = 0;
            if (unmapped_log < 30 && phys < 0x1F000000) {
                { FILE* f = fopen("/tmp/ki_boot.log", "a"); if (f) { fprintf(f, "[UNMAPPED-R32] #%d PC=0x%08X phys=0x%08X\n", unmapped_log, mips_last_pc(m_cpu), phys); fclose(f); } }
                ++unmapped_log;
            }
        }

        if (addr < ram_size)
            return read32_be(&m_ram[addr]);
        if (addr >= boot_rom_base && addr < boot_rom_base + boot_rom_size)
            return read32_be(&m_boot_rom[addr - boot_rom_base]);
        if (addr >= game_rom_base && addr < game_rom_base + game_rom_size)
            return read32_be(&m_game_roms[addr - game_rom_base]);
        if (addr >= io_base && addr < io_base + io_size)
            return io_read32(addr);
        return 0;
    }

    void bus_write32(uint32_t addr, uint32_t val) {
        if (addr < ram_size) {
            write32_be(&m_ram[addr], val);
            return;
        }
        if (addr >= io_base && addr < io_base + io_size) {
            io_write32(addr, val);
            return;
        }
    }

    uint16_t bus_read16(uint32_t addr) {
        uint32_t phys = addr & 0x1FFFFFFF;
        if (phys >= 0x10000000 && phys < 0x1F000000)
            return 1;
        if (addr < ram_size)
            return read16_be(&m_ram[addr & ~1u]);
        if (addr >= boot_rom_base && addr < boot_rom_base + boot_rom_size)
            return read16_be(&m_boot_rom[(addr & ~1u) - boot_rom_base]);
        if (addr >= game_rom_base && addr < game_rom_base + game_rom_size)
            return read16_be(&m_game_roms[(addr & ~1u) - game_rom_base]);
        if (addr >= io_base && addr < io_base + io_size)
            return io_read16(addr);
        return 0;
    }

    void bus_write16(uint32_t addr, uint16_t val) {
        if (addr < ram_size) {
            write16_be(&m_ram[addr & ~1u], val);
            return;
        }
        if (addr >= io_base && addr < io_base + io_size) {
            io_write16(addr, val);
            return;
        }
    }

    uint8_t bus_read8(uint32_t addr) {
        uint32_t phys = addr & 0x1FFFFFFF;
        if (phys >= 0x10000000 && phys < 0x1F000000)
            return 1;
        if (addr < ram_size)
            return m_ram[addr];
        if (addr >= boot_rom_base && addr < boot_rom_base + boot_rom_size)
            return m_boot_rom[addr - boot_rom_base];
        if (addr >= game_rom_base && addr < game_rom_base + game_rom_size)
            return m_game_roms[addr - game_rom_base];
        if (addr >= io_base && addr < io_base + io_size)
            return io_read8(addr);
        return 0xFF;
    }

    void bus_write8(uint32_t addr, uint8_t val) {
        if (addr < ram_size) {
            m_ram[addr] = val;
            return;
        }
        if (addr >= io_base && addr < io_base + io_size) {
            io_write8(addr, val);
            return;
        }
    }

    // ---- I/O read/write ---------------------------------------------------

    uint32_t io_read32(uint32_t addr) {
        uint32_t offset = addr - io_base;

        // Diagnostic: log ALL I/O reads for first 80 accesses.
        static int io_log_count = 0;
        if (io_log_count < 80) {
            { FILE* f = fopen("/tmp/ki_boot.log", "a"); if (f) { fprintf(f, "[IO-R32] #%d PC=0x%08X offset=0x%06X\n", io_log_count, mips_last_pc(m_cpu), offset); fclose(f); } }
            ++io_log_count;
        }

        // DCS audio (0x200000 - 0x200100)
        if (offset >= dcs_offset && offset < dcs_offset + 0x100)
            return dcs_read32(offset);

        // IDE controller (0x500000 - 0x500020)
        // The IDE data register is 16-bit at offsets 0x00/0x01.
        // Assemble the full 32-bit word so that 16-bit and 8-bit
        // sub-accesses get their correct bytes.
        // Use the full offset from io_base; ide_read subtracts
        // ide_data_offset internally.
        if (offset >= ide_data_offset && offset < ide_data_offset + 0x20) {
            uint32_t aligned_full = offset & ~3u;
            uint32_t word = static_cast<uint32_t>(ide_read(aligned_full)) << 24;
            word |= static_cast<uint32_t>(ide_read(aligned_full + 1)) << 16;
            word |= static_cast<uint32_t>(ide_read(aligned_full + 2)) << 8;
            word |= static_cast<uint32_t>(ide_read(aligned_full + 3));
            return word;
        }

        // Player 1 inputs (0x400400 - 0x400500)
        if (offset >= p1_input_offset && offset < p1_input_offset + 0x100) {
            uint32_t reg = (offset - p1_input_offset) & ~3u;
            if (reg == 0x00) return m_p1_inputs;
            return 0xFFFFFFFFu;  // Unused inputs = not pressed
        }

        // Player 2 inputs (0x400C00 - 0x400D00)
        if (offset >= p2_input_offset && offset < p2_input_offset + 0x100) {
            uint32_t reg = (offset - p2_input_offset) & ~3u;
            if (reg == 0x00) return m_p2_inputs;
            return 0xFFFFFFFFu;
        }

        // Coin / Service / DIP switches (0x401000 - 0x401100)
        if (offset >= coin_dip_offset && offset < coin_dip_offset + 0x100) {
            uint32_t reg = (offset - coin_dip_offset) & ~3u;
            switch (reg) {
            case 0x00: {
                // DIP switches + service/test
                uint32_t dips = m_ki_dip_switches;
                if (m_service_pressed) dips &= ~(1u << 0);  // Service switch
                if (m_test_mode)       dips &= ~(1u << 1);  // Test switch
                return dips;
            }
            case 0x04: return m_coin_counter;
            default:   return 0xFFFFFFFFu;
            }
        }

        // ASIC video registers (0x400000 - 0x400100)
        if (offset >= video_control_offset &&
            offset < video_control_offset + 0x100) {
            uint32_t reg = (offset - video_control_offset) & ~3u;
            switch (reg) {
            case 0x00: return m_asic_fb_base;
            case 0x04: return m_asic_display_list;
            case 0x08: return m_asic_control;
            case 0x0C: return (m_frame_count & 1) ? 0 : 1; // vsync
            case 0x10: return m_asic_fb_stride;
            default:   return 0;
            }
        }

        // DMA Blitter (0x400100 - 0x400200)
        if (offset >= blitter_offset && offset < blitter_offset + 0x100) {
            uint32_t reg = (offset - blitter_offset) & ~3u;
            switch (reg) {
            case 0x00: return m_blit_src;
            case 0x04: return m_blit_dst;
            case 0x08: return m_blit_size;
            case 0x0C: return m_blit_strides;
            case 0x10: return m_blit_control;
            case 0x14: return 0;  // command: always idle on read
            case 0x18: return m_blit_fill_color;
            default:   return 0;
            }
        }

        // Diagnostic: log reads to expanded I/O range (0x800000+)
        if (offset >= 0x800000 && offset < 0xC00000) {
            static int diag_logged = 0;
            if (diag_logged < 8) {
                fprintf(stderr, "[KI IO] read32 offset=0x%X (addr=0x%X) returning 0\n",
                        offset, addr);
                ++diag_logged;
            }
        }

        return 0;
    }

    void io_write32(uint32_t addr, uint32_t val) {
        uint32_t offset = addr - io_base;

        // DCS audio writes
        if (offset >= dcs_offset && offset < dcs_offset + 0x100) {
            dcs_write32(offset, val);
            return;
        }

        // IDE command register
        if (offset >= ide_command_status &&
            offset < ide_command_status + 4) {
            ide_command(val >> 24);
            return;
        }

        // IDE data + registers (0x500000 - 0x500020)
        // Write all four bytes individually so that sub-word stores
        // (io_write16, io_write8) work through read-modify-write.
        // Use the full offset from io_base; ide_write subtracts
        // ide_data_offset internally.
        if (offset >= ide_data_offset && offset < ide_data_offset + 0x20) {
            uint32_t aligned_full = offset & ~3u;
            ide_write(aligned_full,     static_cast<uint8_t>(val >> 24));
            ide_write(aligned_full + 1, static_cast<uint8_t>(val >> 16));
            ide_write(aligned_full + 2, static_cast<uint8_t>(val >> 8));
            ide_write(aligned_full + 3, static_cast<uint8_t>(val));
            return;
        }

        // IDE alternate status / device control
        if (offset >= ide_alt_status_ctrl &&
            offset < ide_alt_status_ctrl + 4) {
            if ((val >> 26) & 1) {
                // SRST: software reset
                m_ide_error = 0;
                m_ide_busy = false;
                m_ide_drq = false;
            }
            return;
        }

        // ASIC video registers
        if (offset >= video_control_offset &&
            offset < video_control_offset + 0x100) {
            uint32_t reg = (offset - video_control_offset) & ~3u;
            switch (reg) {
            case 0x00: m_asic_fb_base   = val; break;
            case 0x04: m_asic_display_list = val; break;
            case 0x08: {
                m_asic_control = val;
                // Writing to control register acknowledges VBLANK interrupt.
                if (m_vblank_asserted) {
                    m_vblank_asserted = false;
                    mips_clear_interrupt(m_cpu, 0x02);
                }
                break;
            }
            case 0x10: m_asic_fb_stride = val; break;
            default: break;
            }
            return;
        }

        // DMA Blitter registers
        if (offset >= blitter_offset && offset < blitter_offset + 0x100) {
            uint32_t reg = (offset - blitter_offset) & ~3u;
            switch (reg) {
            case 0x00: m_blit_src    = val; break;
            case 0x04: m_blit_dst    = val; break;
            case 0x08: m_blit_size   = val; break;
            case 0x0C: m_blit_strides = val; break;
            case 0x10: m_blit_control = val; break;
            case 0x14: blit_execute();     break;
            case 0x18: m_blit_fill_color = val; break;
            default: break;
            }
            return;
        }

        // Coin counter / DIP writes (counters)
        if (offset >= coin_dip_offset && offset < coin_dip_offset + 0x100) {
            uint32_t reg = (offset - coin_dip_offset) & ~3u;
            if (reg == 0x04) {
                m_coin_counter = val;
            }
            return;
        }
    }

    uint16_t io_read16(uint32_t addr) {
        uint32_t word = io_read32(addr & ~3u);
        // Big-endian: byte 0 at bits 31:24, byte 1 at 23:16, etc.
        return static_cast<uint16_t>((addr & 2) ? (word & 0xFFFF)
                                                : (word >> 16));
    }

    void io_write16(uint32_t addr, uint16_t val) {
        uint32_t aligned = addr & ~3u;
        if (addr & 2) {
            // Write to lower 16 bits, preserve upper 16.
            uint32_t current = io_read32(aligned);
            io_write32(aligned, (current & 0xFFFF0000u) | val);
        } else {
            io_write32(aligned, static_cast<uint32_t>(val) << 16);
        }
    }

    uint8_t io_read8(uint32_t addr) {
        uint32_t word = io_read32(addr & ~3u);
        // Big-endian: shift by byte position within the 32-bit word.
        uint8_t shift = static_cast<uint8_t>((3 - (addr & 3)) * 8);
        return static_cast<uint8_t>(word >> shift);
    }

    void io_write8(uint32_t addr, uint8_t val) {
        uint32_t aligned = addr & ~3u;
        uint8_t shift = static_cast<uint8_t>((3 - (addr & 3)) * 8);
        uint32_t mask = ~(0xFFu << shift);
        uint32_t current = io_read32(aligned);
        io_write32(aligned, (current & mask) | (static_cast<uint32_t>(val) << shift));
    }

    // ---- DCS Audio stubs -------------------------------------------------
    //
    // The Wolf Unit DCS system uses an ADSP-2105 DSP. We provide minimal
    // register responses so the game's audio driver doesn't hang.
    // Real implementation would need full ADSP-2105 emulation.

    uint32_t dcs_read32(uint32_t offset) {
        uint32_t reg = (offset - dcs_offset) & ~3u;
        switch (reg) {
        case 0x00: return m_dcs_control;     // Control/status
        case 0x04: return m_dcs_data;        // Data register
        case 0x08: return 0x00000001;        // Always "ready" for commands
        case 0x0C: return m_dcs_reset_state; // Reset state
        default:   return 0;
        }
    }

    void dcs_write32(uint32_t offset, uint32_t val) {
        uint32_t reg = (offset - dcs_offset) & ~3u;
        switch (reg) {
        case 0x00: m_dcs_control = val; break;
        case 0x04: m_dcs_data    = val; break;
        case 0x08:
            // Command register: acknowledge immediately
            m_dcs_control |= 0x00000001;  // Set "transfer done"
            break;
        case 0x0C:
            if (val == 0x00000001) {
                // Reset: set state to running after reset
                m_dcs_reset_state = 0x00000003;
                m_dcs_control = 0x00000001;
            }
            break;
        default: break;
        }
    }

    // ---- DMA Blitter -----------------------------------------------------

    void blit_execute() {
        if (m_blit_src >= ram_size || m_blit_dst >= ram_size) return;
        const uint32_t width  = m_blit_size & 0xFFFF;
        const uint32_t height = (m_blit_size >> 16) & 0xFFFF;
        if (width == 0 || height == 0 || width > 2048 || height > 2048) return;

        const uint32_t src_stride = m_blit_strides & 0xFFFF;
        const uint32_t dst_stride = (m_blit_strides >> 16) & 0xFFFF;
        const uint32_t row_bytes  = width * 2;
        const bool     transparent = (m_blit_control & 1) != 0;
        const uint16_t color_key   = static_cast<uint16_t>(
            (m_blit_control >> 8) & 0xFFFF);

        m_last_blit_pc = mips_last_pc(m_cpu);
        ++m_blit_count;

        for (uint32_t y = 0; y < height; ++y) {
            uint32_t src_row = m_blit_src + y * (src_stride != 0 ? src_stride : row_bytes);
            uint32_t dst_row = m_blit_dst + y * (dst_stride != 0 ? dst_stride : row_bytes);
            if (src_row + row_bytes > ram_size || dst_row + row_bytes > ram_size)
                continue;

            for (uint32_t x = 0; x < width; ++x) {
                uint32_t src = src_row + x * 2;
                uint32_t dst = dst_row + x * 2;
                uint16_t pixel = (static_cast<uint16_t>(m_ram[src]) << 8) |
                                  m_ram[src + 1];
                if (transparent && pixel == color_key) continue;
                m_ram[dst]     = static_cast<uint8_t>(pixel >> 8);
                m_ram[dst + 1] = static_cast<uint8_t>(pixel);
            }
        }

        // Record the framebuffer location from large blits.
        if (height > 100) {
            m_asic_fb_base = m_blit_dst;
            m_asic_fb_stride = (dst_stride != 0) ? dst_stride : static_cast<uint32_t>(width * 2);
        }
    }

    // ---- IDE Controller --------------------------------------------------

    uint16_t m_ide_data_reg{};

    uint8_t ide_read(uint32_t offset) {
        uint32_t reg = offset - ide_data_offset;
        switch (reg) {
        case 0x00: return ide_pop_data_byte(0);
        case 0x01: return ide_pop_data_byte(1);
        case 0x04: return m_ide_error;
        case 0x08: return m_ide_sector_count_val;
        case 0x0C: return m_ide_sector_num_val;
        case 0x10: return m_ide_cylinder_low_val;
        case 0x14: return m_ide_cylinder_high_val;
        case 0x18: return m_ide_drive_head_val;
        case 0x1C: return ide_status();
        default:   return 0;
        }
    }

    void ide_write(uint32_t offset, uint8_t val) {
        uint32_t reg = offset - ide_data_offset;
        switch (reg) {
        case 0x00:
            m_ide_data_reg = (m_ide_data_reg & 0xFF00) | val;
            if (m_ide_writing) ide_capture_write_byte(val, 0);
            break;
        case 0x01:
            m_ide_data_reg = (m_ide_data_reg & 0x00FF) |
                             (static_cast<uint16_t>(val) << 8);
            if (m_ide_writing) ide_capture_write_byte(val, 1);
            break;
        case 0x08: m_ide_sector_count_val = val; break;
        case 0x0C: m_ide_sector_num_val   = val; break;
        case 0x10: m_ide_cylinder_low_val  = val; break;
        case 0x14: m_ide_cylinder_high_val = val; break;
        case 0x18: m_ide_drive_head_val    = val; break;
        default: break;
        }
    }

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
        write16_be(&identify[0], 0x0040);

        const uint32_t total_sectors =
            static_cast<uint32_t>(m_chd_info.logical_bytes / 512);
        write16_be(&identify[2], 16);           // heads
        write16_be(&identify[12], 63);          // sectors per track
        write16_be(&identify[6], total_sectors / (16 * 63));

        // Words 60-61: LBA28 total sectors
        uint8_t* lba = &identify[120];
        lba[0] = static_cast<uint8_t>(total_sectors);
        lba[1] = static_cast<uint8_t>(total_sectors >> 8);
        lba[2] = static_cast<uint8_t>(total_sectors >> 16);
        lba[3] = static_cast<uint8_t>(total_sectors >> 24);

        // Word 49: Capabilities (LBA supported)
        write16_be(&identify[98], 0x0200);

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

    void render_frame() {
        if (!m_video) return;

        // KI framebuffer: typically 400x254 or 512x256. Use 512x256 RGBA.
        constexpr int fb_width  = 512;
        constexpr int fb_height = 256;
        m_framebuffer.resize(fb_width * fb_height * 4);

        bool got_frame = false;

        // Log frame info for debugging
        static int frame_log_count = 0;
        if ((m_frame_count % 30) == 0 && frame_log_count < 5) {
            fprintf(stderr, "[KI] Frame %d: asic_fb_base=0x%08X blit_count=%d\n",
                    m_frame_count, m_asic_fb_base, m_blit_count);
            ++frame_log_count;
        }

        // Priority 1: ASIC-configured framebuffer address.
        if (m_asic_fb_base != 0 &&
            m_asic_fb_base < ram_size - fb_width * fb_height * 2) {
            got_frame = scan_asic_framebuffer(fb_width, fb_height);
        }

        // Priority 2: Last large blit destination.
        if (!got_frame && m_blit_count > 0 && m_blit_dst != 0) {
            uint32_t blit_h = (m_blit_size >> 16) & 0xFFFF;
            uint32_t blit_w = m_blit_size & 0xFFFF;
            if (m_blit_dst < ram_size && blit_w > 0 && blit_h > 0) {
                int w = static_cast<int>(std::min(blit_w,
                    static_cast<uint32_t>(fb_width)));
                int h = static_cast<int>(std::min(blit_h,
                    static_cast<uint32_t>(fb_height)));
                got_frame = scan_rgb565_at(m_blit_dst, w, h);
            }
        }

        // Priority 3: Scan known framebuffer locations.
        if (!got_frame) {
            got_frame = scan_ram_for_fb(fb_width, fb_height);
        }

        // Priority 4: Diagnostic display.
        if (!got_frame) {
            draw_debug_frame(fb_width, fb_height);
        }

        m_video->present_rgba_frame(m_framebuffer.data(), fb_width, fb_height);
        ++m_frame_count;
    }

    bool scan_asic_framebuffer(int fb_width, int fb_height) {
        if (m_asic_fb_base == 0 || m_asic_fb_base >= ram_size) return false;
        bool any_pixel = false;
        for (int y = 0; y < fb_height; ++y) {
            for (int x = 0; x < fb_width; ++x) {
                uint32_t src_offset = m_asic_fb_base +
                    y * (m_asic_fb_stride != 0 ? m_asic_fb_stride : fb_width * 2) +
                    x * 2;
                if (src_offset + 1 >= ram_size) continue;
                uint16_t rgb565 = (static_cast<uint16_t>(m_ram[src_offset]) << 8) |
                                   m_ram[src_offset + 1];
                uint32_t dst = (y * fb_width + x) * 4;
                decode_rgb565_to_fb(rgb565, dst);
                if (rgb565 != 0) any_pixel = true;
            }
        }
        return any_pixel;
    }

    bool scan_ram_for_fb(int fb_width, int fb_height) {
        // KI stores its framebuffer at various addresses. Try common locations.
        static constexpr uint32_t fb_candidates[] = {
            0x00000000,   // base of RAM
            0x00100000,   // 1 MB
            0x00200000,   // 2 MB
            0x00300000,   // 3 MB
            0x00400000,   // 4 MB
            0x00500000,   // 5 MB
        };

        for (uint32_t fb_base : fb_candidates) {
            if (scan_rgb565_at(fb_base, fb_width, fb_height))
                return true;
        }
        return false;
    }

    bool scan_rgb565_at(uint32_t fb_base, int fb_width, int fb_height) {
        if (fb_base + fb_width * fb_height * 2 > ram_size) return false;
        int nonzero = 0;
        for (int y = 0; y < fb_height; ++y) {
            for (int x = 0; x < fb_width; ++x) {
                uint32_t src = fb_base + (y * fb_width + x) * 2;
                uint16_t rgb565 = (static_cast<uint16_t>(m_ram[src]) << 8) |
                                   m_ram[src + 1];
                uint32_t dst = (y * fb_width + x) * 4;
                decode_rgb565_to_fb(rgb565, dst);
                if (rgb565 != 0) ++nonzero;
            }
        }
        return nonzero > (fb_width * fb_height / 10);
    }

    void decode_rgb565_to_fb(uint16_t rgb565, uint32_t dst) {
        m_framebuffer[dst]     = static_cast<uint8_t>((rgb565 >> 11) << 3);
        m_framebuffer[dst + 1] = static_cast<uint8_t>(((rgb565 >> 5) & 0x3F) << 2);
        m_framebuffer[dst + 2] = static_cast<uint8_t>((rgb565 & 0x1F) << 3);
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

    static uint32_t read32_be(const void* p) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        return (static_cast<uint32_t>(b[0]) << 24) |
               (static_cast<uint32_t>(b[1]) << 16) |
               (static_cast<uint32_t>(b[2]) << 8) |
               static_cast<uint32_t>(b[3]);
    }

    static void write32_be(void* p, uint32_t v) {
        uint8_t* b = static_cast<uint8_t*>(p);
        b[0] = static_cast<uint8_t>(v >> 24);
        b[1] = static_cast<uint8_t>(v >> 16);
        b[2] = static_cast<uint8_t>(v >> 8);
        b[3] = static_cast<uint8_t>(v);
    }

    static uint16_t read16_be(const void* p) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        return static_cast<uint16_t>((b[0] << 8) | b[1]);
    }

    static void write16_be(void* p, uint16_t v) {
        uint8_t* b = static_cast<uint8_t*>(p);
        b[0] = static_cast<uint8_t>(v >> 8);
        b[1] = static_cast<uint8_t>(v);
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
    std::vector<uint8_t> m_ram;
    std::vector<uint8_t> m_boot_rom;
    std::vector<uint8_t> m_game_roms;   // u10-u36 game ROMs (4MB)

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
    { FILE* f = fopen("/tmp/ki_boot.log", "a"); if (f) { fprintf(f, "[KI FACTORY] make_midway_session called\n"); fclose(f); } }
    return std::make_unique<wolfunit_session>(
        std::move(video), std::move(cabinet));
}
