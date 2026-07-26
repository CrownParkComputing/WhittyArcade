// Shinobi (Sega System 16-B) board machine. The main 68000 is an
// instance-based Moira core and the sound CPU is a private Z80. This module
// has no dependency on the unrelated Galaxian Z80-board interface.
#pragma once

class m68000_bus;
class m68000_cpu;

#include "arcade_types.h"
#include "high_scores.h"
#include "sega/system16b/system16b_audio.h"
#include "sega/system16b/system16b_rom.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace system16b {

constexpr std::size_t kRomBytes          = kProgramRomBytes;
constexpr std::size_t kTileRamBytes      = 0x10000;
constexpr std::size_t kTextRamBytes      = 0x01000;
constexpr std::size_t kSpriteRamBytes    = 0x00800;
constexpr std::size_t kPaletteBytes      = 0x01000;
constexpr std::size_t kWorkRamBytes      = 0x10000;

// Sound CPU program + data ROM sizes (32 KiB program, 128 KiB data bank)
// are declared in shinobi_rom.h so the rom loader and the machine agree
// on the buffer size. Reference them as system16b::kSoundProgRomBytes /
// ::kSoundDataRomBytes at the use site.

constexpr std::size_t kTileGfxBytes      = kGfxFileBytes; // per plane, 3 planes

constexpr int kScreenW         = 320;
constexpr int kScreenH         = 224;
constexpr std::size_t kScreenPixels =
    static_cast<std::size_t>(kScreenW) * kScreenH;
constexpr int kRefreshHz       = 60;
constexpr int kMainCyclesPerFrame  = 10'000'000 / 60;
constexpr int kSoundCyclesPerFrame = 5'000'000  / 60;

// Per-game specialisation. All Shinobi-specific memory map, I/O, video
// decode, and input translation live here.
class board final {
public:
    board() = default;
    ~board() = default;

    unsigned cpu_read_16(unsigned address);
    void cpu_write_16(unsigned address, unsigned data);
    void reset_extras();
    void service_high_scores();
    void flush_high_scores();
    void render_frame(uint32_t* rgba);
    void     apply_input(const input_state& s, uint8_t* ports,
                         std::size_t port_count);

    // Sound Z80 memory and I/O buses.
    uint8_t co_cpu_read(unsigned address);
    void co_cpu_write(unsigned address, uint8_t data);

    // Buffers / output
    std::array<uint8_t, kWorkRamBytes>& work_ram()    noexcept { return work_ram_; }
    const std::array<uint8_t, kWorkRamBytes>& work_ram() const noexcept { return work_ram_; }
    std::array<uint8_t, kWorkRamBytes>& sound_work_ram() noexcept { return sound_work_ram_; }
    const std::array<uint8_t, kWorkRamBytes>& sound_work_ram() const noexcept { return sound_work_ram_; }
    std::array<uint8_t, kSpriteRamBytes>& sprite_ram() noexcept { return sprite_ram_; }
    std::array<uint8_t, kPaletteBytes>& palette_ram() noexcept { return palette_ram_; }
    const std::array<uint8_t, kPaletteBytes>& palette_ram() const noexcept { return palette_ram_; }
    const std::array<uint32_t, kScreenPixels>& frame_buffer() const noexcept {
        return frame_buffer_;
    }

    // Boot vectors -- reset PC/SP come from the program image at
    // address 0 (long) and 4 (long).
    static constexpr uint32_t kResetSP = 0x00000000;
    static constexpr uint32_t kResetPC = 0x00000004;
    std::array<uint8_t, kRomBytes>&       program_rom()   noexcept { return program_; }
    const std::array<uint8_t, kRomBytes>& program_rom()   const noexcept { return program_; }
    std::array<uint8_t, kTileGfxBytes>&   tile_plane0()   noexcept { return tile0_;  }
    const std::array<uint8_t, kTileGfxBytes>& tile_plane0() const noexcept { return tile0_; }
    std::array<uint8_t, kTileGfxBytes>&   tile_plane1()   noexcept { return tile1_;  }
    const std::array<uint8_t, kTileGfxBytes>& tile_plane1() const noexcept { return tile1_; }
    std::array<uint8_t, kTileGfxBytes>&   tile_plane2()   noexcept { return tile2_;  }
    const std::array<uint8_t, kTileGfxBytes>& tile_plane2() const noexcept { return tile2_; }
    std::array<uint8_t, kSpriteGfxBytes>& sprite_gfx()    noexcept { return sprite_; }
    const std::array<uint8_t, kSpriteGfxBytes>& sprite_gfx() const noexcept { return sprite_; }

    // Full 24-bit main-68000 bus access.
    uint8_t byte_read (uint32_t address) noexcept;
    void    byte_write(uint32_t address, uint8_t data) noexcept;

private:

    uint8_t mapper_read(uint32_t address) noexcept;
    void mapper_write(uint32_t address, uint8_t data) noexcept;

    // Hex input -> 32-bit big-endian long (used for boot vectors).
    static uint32_t be_long(const uint8_t* p) noexcept {
        return (static_cast<uint32_t>(p[0]) << 24) |
               (static_cast<uint32_t>(p[1]) << 16) |
               (static_cast<uint32_t>(p[2]) << 8)  |
                static_cast<uint32_t>(p[3]);
    }

    // ROM
    std::array<uint8_t, kRomBytes>        program_{};
    std::array<uint8_t, kTileGfxBytes>    tile0_{};
    std::array<uint8_t, kTileGfxBytes>    tile1_{};
    std::array<uint8_t, kTileGfxBytes>    tile2_{};
    std::array<uint8_t, kSpriteGfxBytes>  sprite_{};

    // RAM
    std::array<uint8_t, kTileRamBytes>    tile_ram_{};
    std::array<uint8_t, kTextRamBytes>    text_ram_{};
    std::array<uint8_t, kSpriteRamBytes>  sprite_ram_{};
    std::array<uint8_t, kPaletteBytes>    palette_ram_{};
    std::array<uint8_t, kWorkRamBytes>    work_ram_{};
    std::array<uint8_t, kWorkRamBytes>    sound_work_ram_{};

    // I/O mirror (active-low) -- the runtime writes these via
    // apply_input's translated ports[] bytes; the per-board byte_read()
    // reads them when the 68000 reads from the I/O range at 0xC40000.
public:
    uint8_t dsw1_{0xff};
    // English cabinet defaults: upright, demo sounds ON (bit 1 clear),
    // lives=3, difficulty=normal, enemy bullets slow and language=English
    // (bit 7 clear). The DIP manager can still select the Japanese setting.
    uint8_t dsw2_{0x7c};
    uint8_t p1_input_{0xff};
    uint8_t p2_input_{0xff};
    uint8_t service_input_{0xff};
    int16_t screen_x_origin_{189};   // 315-5196 sprite list origin

    // Sound CPU program ROM + data ROM staging (Z80).
    std::array<uint8_t, ::system16b::kSoundProgRomBytes> sound_prog_rom_{};
    std::array<uint8_t, ::system16b::kSoundDataRomBytes> sound_data_rom_{};
    bool have_sound_rom_{false};

    // Z80 I/O ports (standard System 16B sound_portmap).
    uint8_t  co_cpu_port_read (unsigned port);
    void     co_cpu_port_write(unsigned port, uint8_t data);

    // Main -> Z80 sound command latch. On real hardware this is register
    // 0x03 of the 315-5195 memory mapper, whose 32-byte register window
    // wraps anywhere no region is mapped.
    // The Z80 reads it at 0xE800 / port 0xC0. sound_irq_pending_ asserts
    // the Z80 INT for the frame in which the latch was written.
    // sound_bankoffs_ is the uPD7759-control-selected bank offset into
    // sound_data_rom_.
    uint8_t  sound_latch_{0};
    std::array<uint8_t, 0x20> mapper_regs_{};
    // YM2151 status provider, installed by the machine runtime so the Z80
    // can poll the real chip's busy/timer flags.
    std::function<uint8_t()> ym_status_cb_;
    bool     sound_irq_pending_{false};
    unsigned sound_bankoffs_{0};

    // Single-byte mailbox shared between main 68000 and the Z80 sound
    // CPU. The main CPU reads at byte address 0xFE0000 + N; the sound CPU
    // writes from its host via co_cpu_write(). Defaults to 0xFF which
    // means "no data".
    uint8_t sound_mailbox_{0xff};

    std::array<uint32_t, kScreenPixels> frame_buffer_{};
    high_score_runtime high_scores_{"shinobi4"};

public:
    // Selects the per-game bits the shared board carries: the high-score
    // table key and the cabinet's factory DIP defaults.
    void set_game(::system16b::system16b_rom_set set) {
        high_scores_ = high_score_runtime(
            ::system16b::system16b_rom_loader::set_short_name(set));
        if (set == ::system16b::system16b_rom_set::alien_syndrome) {
            // Demo sounds on, 3 lives, 150-second timer, normal difficulty.
            dsw2_ = 0xfd;
            // ROM board 171-5358 wires the sprite bank lines differently:
            // MAME segas16b.cpp alternate_banklist. 255 = no ROM there.
            static constexpr std::array<uint8_t, 16> alternate_banklist{
                0, 255, 255, 255, 255, 255, 255, 3,
                255, 255, 255, 2, 255, 1, 0, 255};
            sprite_banklist_ = alternate_banklist.data();
        } else if (set == ::system16b::system16b_rom_set::aurail) {
            // Upright, demo sounds on, 3 lives, normal difficulty.
            dsw2_ = 0xfd;
            // ROM board 171-5704: 512 KiB program, 8 identity-mapped
            // sprite banks of 128 KiB, and bankable tiles - the register
            // at 0x3f0000 maps each of the tilemap's two 4096-tile slots
            // onto one of eight pages of tile ROM. The mapper places the
            // second program ROM pair at 0x080000, not straight after the
            // first: its own pointer tables say 0x0Axxxx, and jumping
            // through them with the pair at 0x040000 lands in data.
            rom_bank1_base_ = 0x80000;
            sprite_bank_mod_ = 16;
            tile_banking_ = true;
        }
    }
    // Sprite bank LUT for the current ROM board (identity for Shinobi's).
    const uint8_t* sprite_banklist_{nullptr};
    // CPU address of the second 256 KiB program ROM pair (0 = none). The
    // 315-5195 mapper decides where each pair appears; Aurail's shows up
    // at 0x080000 while the buffer keeps it at offset 0x40000.
    uint32_t rom_bank1_base_{0};
    // Physical 64K-word sprite banks on the board (bank field wraps here).
    unsigned sprite_bank_mod_{4};
    // 171-5704 tile banking: two 4096-tile slots, each mapped to a page.
    bool tile_banking_{false};
    std::array<uint8_t, 2> tile_banks_{0, 1};
};

// =====================================================================
// system16b_machine_t — runtime that drives Shinobi on an m68000_cpu.
//
// Mirrors what galaxian_machine does for the Z80 boards: owns the
// per-board interface, pulses the M68K CPU each frame, and exposes a
// frame buffer for the video worker. Implemented in src/shinobi_machine.cpp.
// =====================================================================
class system16b_machine_t {
public:
    system16b_machine_t();
    ~system16b_machine_t();

    system16b_machine_t(const system16b_machine_t&) = delete;
    system16b_machine_t& operator=(const system16b_machine_t&) = delete;

    int  screen_width()      const noexcept { return kScreenW; }
    int  screen_height()     const noexcept { return kScreenH; }
    double refresh_rate()    const noexcept { return kRefreshHz; }
    int  clocks_per_frame()  const noexcept { return kMainCyclesPerFrame; }

    bool load_roms(const std::string& path);
    void reset();
    void run_frame();
    void set_input(const input_state& state);

    // Called for every YM2151/uPD7759 register write the Z80 sound CPU
    // performs (port 0x00 = YM address, 0x01 = YM data, 0x80 = uPD7759
    // sample port). Install before reset().
    void set_sound_write_callback(std::function<void(unsigned, uint8_t)> cb);
    // Advances the external YM2151 timer host in chip-clock units while the
    // Z80 runs. This keeps game sequencing deterministic when audio is
    // paused, muted, headless, or recovering from an output underrun.
    void set_sound_timer_callback(std::function<void(uint32_t)> cb);
    // Provider for YM2151 status-register reads by the sound CPU (busy +
    // timer flags); typically bound to the audio synth.
    void set_sound_status_callback(std::function<uint8_t()> cb) {
        m_board->ym_status_cb_ = std::move(cb);
    }

    const uint32_t* frame_buffer() const { return m_board->frame_buffer().data(); }
    board*         board_ptr()   { return m_board.get(); }
    // Debug/diagnostic: current main-CPU program counter (0 when off).
    uint32_t main_pc() const;

private:
    struct sound_cpu;
    std::unique_ptr<board> m_board;
    std::unique_ptr<m68000_bus> m_main_bus;
    std::unique_ptr<m68000_cpu> m_main_cpu; // instance-based 68000 (Moira)
    std::unique_ptr<sound_cpu> m_sound;   // chips Z80 @ 5 MHz (nullptr when
                                          //  no sound program ROM staged)
    bool                   m_ready{false};
};

}  // namespace system16b
