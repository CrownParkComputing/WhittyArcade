// Taito Z System machine: two MC68000s sharing work RAM, driving a
// TC0100SCN tilemap generator, a TC0150ROD road generator, a TC0110PCR
// palette chip and zoomed sprites, with a TC0040IOC handling inputs.
//
// CPU A owns the video chips; CPU B owns the I/O chip and the TC0140SYT
// mailbox to the sound board. Both free-run from reset, share
// 0x084000-0x087fff and take IRQ6 at vblank.
//
// Both CPUs are independent m68000_cpu instances rather than a single
// context-switched core, so nothing here is process-global and repeated
// ROM switches are safe.
#pragma once

#include "arcade_types.h"
#include "taito/taitoz/taitoz_rom.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class m68000_bus;
class m68000_cpu;

namespace taitoz {

constexpr int kScreenW = 320;
constexpr int kScreenH = 224;
constexpr double kRefreshHz = 60.0;
// Both 68000s run at 12 MHz.
constexpr int kCyclesPerFrame = 12'000'000 / 60;
// The CPUs are interleaved this many times per frame so their handshakes
// through shared RAM resolve within a frame.
constexpr int kSlicesPerFrame = 8;
// YM2610 clock (16 MHz / 2), used to convert Z80 cycles to chip clocks.
constexpr unsigned taitoz_sound_chip_hz = 8'000'000u;

constexpr std::size_t kScnRamBytes    = 0x14000;
constexpr std::size_t kScnCtrlBytes   = 0x10;
constexpr std::size_t kRoadRamBytes   = 0x2000;
constexpr std::size_t kSpriteRamBytes = 0x700;
constexpr std::size_t kPaletteEntries = 0x1000;
constexpr std::size_t kCpuRamBytes    = 0x4000;

class board final {
public:
    board();
    ~board();
    board(const board&) = delete;
    board& operator=(const board&) = delete;

    void install_roms(const taitoz_roms& roms);
    void reset();

    // Per-CPU byte bus. `sub` selects CPU B's map, which is almost
    // entirely different from CPU A's.
    uint8_t read8(bool sub, uint32_t address) noexcept;
    void write8(bool sub, uint32_t address, uint8_t value) noexcept;

    void apply_input(const input_state& state);
    // Composites the current video state into the board's own framebuffer.
    void render_frame();

    // ---- TC0140SYT, the mailbox between CPU B and the sound Z80 -------
    // Two 4-byte channels each way plus a status byte. Writing the second
    // byte of a channel latches it and NMIs the Z80; the Z80 clears the
    // flag by reading it back. Nibble-wide: only the low 4 bits travel.
    void     syt_master_port_w(uint8_t value) noexcept;
    void     syt_master_comm_w(uint8_t value) noexcept;
    uint8_t  syt_master_comm_r() noexcept;
    void     syt_slave_port_w(uint8_t value) noexcept;
    void     syt_slave_comm_w(uint8_t value) noexcept;
    uint8_t  syt_slave_comm_r() noexcept;
    bool     syt_nmi_pending() const noexcept { return syt_nmi_pending_; }
    // Diagnostics: sound commands latched by the 68000 and NMIs taken.
    unsigned syt_commands() const noexcept { return syt_commands_; }
    unsigned syt_nmis() const noexcept { return syt_nmis_; }
    const std::array<unsigned, 16>& syt_submode_hits() const noexcept {
        return syt_submode_hits_;
    }
    void     syt_clear_nmi() noexcept { syt_nmi_pending_ = false; ++syt_nmis_; }
    bool     sound_reset_held() const noexcept { return sound_reset_held_; }
    // Diagnostics: frame number stamped by the harness for trace output.
    int dbg_frame_{0};
    bool     take_sound_reset_release() noexcept {
        const bool released = sound_reset_released_;
        sound_reset_released_ = false;
        return released;
    }

    // Sound Z80 bus: 16 KiB fixed ROM, a 16 KiB bank, RAM and the chip
    // windows. The YM2610 ports are routed out to the audio system.
    uint8_t sound_read(uint16_t address) noexcept;
    void sound_write(uint16_t address, uint8_t value) noexcept;
    const std::vector<uint8_t>& sound_rom() const { return rom_audio_; }
    const std::array<uint8_t, 0x2000>& sound_work_ram() const {
        return sound_ram_;
    }
    void set_ym_ports(std::function<void(unsigned, uint8_t)> write,
                      std::function<uint8_t(unsigned)> read) {
        ym_write_ = std::move(write);
        ym_read_ = std::move(read);
    }

    const std::array<uint32_t, kScreenW * kScreenH>& frame_buffer() const {
        return frame_;
    }
    // Diagnostics for the boot test.
    const std::array<uint8_t, kScnRamBytes>& scn_ram() const { return scn_; }
    const std::array<uint16_t, kPaletteEntries>& palette() const {
        return pal_;
    }
    const std::array<uint8_t, kCpuRamBytes>& shared_ram() const {
        return shared_;
    }
    const std::array<uint8_t, kSpriteRamBytes>& sprite_ram() const {
        return spr_;
    }
    const std::array<uint8_t, kScnCtrlBytes>& scn_ctrl() const {
        return scnctl_;
    }
    const std::array<uint8_t, kRoadRamBytes>& road_ram() const {
        return road_;
    }
    int road_palbank() const { return road_palbank_; }

private:
    uint8_t ioc_read_selected() const noexcept;
    void draw_road();
    void draw_sprites(uint16_t* pens, int stride);
    void transcode_road_gfx();

    // ROM regions, sized by the loader.
    std::vector<uint8_t> rom_main_, rom_sub_;
    std::vector<uint8_t> gfx_scn_, gfx_spr_, gfx_road_, gfx_smap_;

    // RAM
    std::array<uint8_t, kCpuRamBytes> ram_main_{}, ram_sub_{}, shared_{};
    std::array<uint8_t, kScnRamBytes> scn_{};
    std::array<uint8_t, kScnCtrlBytes> scnctl_{};
    std::array<uint8_t, kRoadRamBytes> road_{};
    std::array<uint8_t, kSpriteRamBytes> spr_{};
    std::array<uint16_t, kPaletteEntries> pal_{};
    unsigned pal_addr_{0};
    int road_palbank_{3};

    // TC0040IOC: a multiplexed port register file. Port 0/1 are the DIP
    // banks, 2/3 the digital inputs and 8/9 the 16-bit steering value.
    uint8_t io_dswa_{0xff}, io_dswb_{0xcf};
    uint8_t io_in0_{0x13}, io_in1_{0x1f}, io_in2_{0xff};
    int io_steer_{0};
    uint8_t ioc_mport_{0};
    std::array<uint8_t, 8> ioc_regs_{};

    // TC0140SYT state. `mainmode`/`submode` are each side's current
    // channel index; the status byte carries the four "data waiting"
    // flags. The chip comes out of reset with the Z80's NMI disabled.
    void syt_update_nmi() noexcept;
    uint8_t syt_mainmode_{0}, syt_submode_{0}, syt_status_{0};
    std::array<uint8_t, 8> syt_to_slave_{}, syt_to_master_{};
    bool syt_nmi_enabled_{false};
    bool syt_nmi_pending_{false};
    unsigned syt_commands_{0};
    unsigned syt_nmis_{0};
    unsigned cmd_reads_{0};
    unsigned ymrd_logged_{0};
    std::array<unsigned, 16> syt_submode_hits_{};
    bool sound_reset_held_{true};
    bool sound_reset_released_{false};

    // Sound Z80
    std::vector<uint8_t> rom_audio_;
    std::array<uint8_t, 0x2000> sound_ram_{};
    unsigned sound_bank_{0};
    std::function<void(unsigned, uint8_t)> ym_write_;
    std::function<uint8_t(unsigned)> ym_read_;

    // Video working buffers (members, not file statics, so two boards can
    // exist at once).
    std::array<uint32_t, kScreenW * kScreenH> frame_{};
    std::vector<uint16_t> pen_buffer_;
    std::vector<uint16_t> road_layer_;
    std::vector<uint8_t> pri_layer_;
    // The road gfx ROM unpacked to one byte per pixel, built once.
    std::vector<uint8_t> road_pix_;
    int road_priority_switch_line_{0};
};

class taitoz_machine {
public:
    taitoz_machine();
    ~taitoz_machine();
    taitoz_machine(const taitoz_machine&) = delete;
    taitoz_machine& operator=(const taitoz_machine&) = delete;

    int screen_width() const noexcept { return kScreenW; }
    int screen_height() const noexcept { return kScreenH; }
    double refresh_rate() const noexcept { return kRefreshHz; }

    bool load_roms(const std::string& path);
    void reset();
    void run_frame();
    void set_input(const input_state& state);
    // Diagnostics: the sound Z80's program counter.
    uint16_t sound_pc() const;
    // Diagnostics: interrupt mode, IFF1 and INT acknowledge count.
    void sound_int_state(unsigned& im, bool& iff1, unsigned& acks) const;

    // Routes the sound Z80's YM2610 accesses, its timer clocks and its IRQ
    // line to whatever owns the chip. Install before reset().
    void set_ym_handlers(std::function<void(unsigned, uint8_t)> write,
                         std::function<uint8_t(unsigned)> read,
                         std::function<void(uint32_t)> advance_timer,
                         std::function<bool()> irq);

    const uint32_t* frame_buffer() const;
    board* board_ptr() { return m_board.get(); }
    // Diagnostics: the two CPUs' program counters.
    uint32_t main_pc() const;
    uint32_t sub_pc() const;

private:
    void run_sound(int cycles);
    void advance_chip(int z80_cycles);
    struct bus_a;
    struct bus_b;
    struct sound_cpu;
    std::function<void(uint32_t)> m_ym_advance_timer;
    std::function<bool()> m_ym_irq;
    std::unique_ptr<sound_cpu> m_sound;
    std::unique_ptr<board> m_board;
    std::unique_ptr<bus_a> m_bus_a;
    std::unique_ptr<bus_b> m_bus_b;
    std::unique_ptr<m68000_cpu> m_cpu_a;
    std::unique_ptr<m68000_cpu> m_cpu_b;
    bool m_ready{false};
};

}  // namespace taitoz
