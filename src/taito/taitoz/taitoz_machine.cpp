// Taito Z System machine implementation.
//
// Memory maps and chip behaviour derived from MAME's taito_z.cpp and the
// TC01xx device sources; written independently against those facts.

#include "taito/taitoz/taitoz_machine.h"

#include "m68000_cpu.h"

extern "C" {
#include "z80.h"
}

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace taitoz {

board::board() {
    pen_buffer_.assign(static_cast<std::size_t>(kScreenW) * kScreenH, 0);
    road_layer_.assign(static_cast<std::size_t>(kScreenW) * kScreenH, 0x8000);
    pri_layer_.assign(static_cast<std::size_t>(kScreenW) * kScreenH, 0);
}

board::~board() = default;

void board::install_roms(const taitoz_roms& roms) {
    rom_main_ = roms.main_cpu;
    rom_sub_  = roms.sub_cpu;
    rom_audio_ = roms.audio_cpu;
    gfx_scn_  = roms.scn_gfx;
    gfx_spr_  = roms.sprite_gfx;
    gfx_road_ = roms.road_gfx;
    gfx_smap_ = roms.sprite_map;
    road_pix_.clear();   // rebuilt lazily against the new ROM
}

void board::reset() {
    ram_main_.fill(0);
    ram_sub_.fill(0);
    shared_.fill(0);
    scn_.fill(0);
    scnctl_.fill(0);
    road_.fill(0);
    spr_.fill(0);
    pal_.fill(0);
    pal_addr_ = 0;
    road_palbank_ = 3;
    ioc_mport_ = 0;
    ioc_regs_.fill(0);
    syt_mainmode_ = 0;
    syt_submode_ = 0;
    syt_status_ = 0;
    syt_to_slave_.fill(0);
    syt_to_master_.fill(0);
    // The chip powers up with the Z80's NMI masked and the Z80 held in
    // reset until CPU A releases it.
    syt_nmi_enabled_ = false;
    syt_nmi_pending_ = false;
    syt_commands_ = 0;
    syt_nmis_ = 0;
    syt_submode_hits_.fill(0);
    sound_reset_held_ = true;
    sound_reset_released_ = false;
    sound_ram_.fill(0);
    sound_bank_ = 0;
    frame_.fill(0);
}

// ---------------------------------------------------------------------
// TC0040IOC
// ---------------------------------------------------------------------

uint8_t board::ioc_read_selected() const noexcept {
    const unsigned port = ioc_mport_;
    if (port == 0x08 || port == 0x09) {
        // 16-bit steering wheel, centred so an idle wheel reads straight.
        const unsigned steer = (0xff80u + (0x80u + io_steer_)) & 0xffffu;
        return static_cast<uint8_t>(port == 0x08 ? (steer & 0xff)
                                                 : (steer >> 8));
    }
    switch (port) {
    case 0: return io_dswa_;
    case 1: return io_dswb_;
    case 2: return io_in0_;
    case 3: return io_in1_;
    case 4: return ioc_regs_[4];
    case 7: return io_in2_;
    default: return 0xff;
    }
}

// ---------------------------------------------------------------------
// TC0140SYT
// ---------------------------------------------------------------------
//
// Status bits: 0x01/0x02 mark master->slave channels 0-1 and 2-3 as full,
// 0x04/0x08 the same for slave->master. The Z80 is NMI'd whenever a
// master->slave channel is full and NMIs are enabled.

namespace {
constexpr uint8_t kToSlave01  = 0x01;
constexpr uint8_t kToSlave23  = 0x02;
constexpr uint8_t kToMaster01 = 0x04;
constexpr uint8_t kToMaster23 = 0x08;
}  // namespace

void board::syt_update_nmi() noexcept {
    syt_nmi_pending_ =
        (syt_status_ & (kToSlave01 | kToSlave23)) && syt_nmi_enabled_;
}

void board::syt_master_port_w(uint8_t value) noexcept {
    syt_mainmode_ = value & 0x0f;
}

void board::syt_master_comm_w(uint8_t value) noexcept {
    const uint8_t d = value & 0x0f;
    switch (syt_mainmode_) {
    case 0: syt_to_slave_[syt_mainmode_++] = d; break;
    case 1:
        syt_to_slave_[syt_mainmode_++] = d;
        syt_status_ |= kToSlave01;
        ++syt_commands_;
        if (std::getenv("TAITOZ_TRACE_SYT"))
            std::fprintf(stderr, "CMD f=%d %x%x\n", dbg_frame_, syt_to_slave_[0], d);
        syt_update_nmi();
        break;
    case 2: syt_to_slave_[syt_mainmode_++] = d; break;
    case 3:
        syt_to_slave_[syt_mainmode_++] = d;
        syt_status_ |= kToSlave23;
        syt_update_nmi();
        break;
    case 4: {
        // Channel 4 is the Z80's reset line: non-zero holds it.
        const bool hold = d != 0;
        if (std::getenv("TAITOZ_TRACE_RST"))
            std::fprintf(stderr, "RST syt f=%d d=%x hold=%d (was %d)\n",
                         dbg_frame_, d, hold, sound_reset_held_);
        if (sound_reset_held_ && !hold) sound_reset_released_ = true;
        sound_reset_held_ = hold;
        break;
    }
    default: break;
    }
}

uint8_t board::syt_master_comm_r() noexcept {
    uint8_t r = 0;
    switch (syt_mainmode_) {
    case 0: r = syt_to_master_[syt_mainmode_++]; break;
    case 1:
        r = syt_to_master_[syt_mainmode_];
        syt_status_ &= static_cast<uint8_t>(~kToMaster01);
        syt_mainmode_++;
        if (std::getenv("TAITOZ_TRACE_RST"))
            std::fprintf(stderr, "68KRD f=%d %x%x\n", dbg_frame_,
                         syt_to_master_[0], r);
        break;
    case 2: r = syt_to_master_[syt_mainmode_++]; break;
    case 3:
        r = syt_to_master_[syt_mainmode_];
        syt_status_ &= static_cast<uint8_t>(~kToMaster23);
        syt_mainmode_++;
        break;
    case 4: r = syt_status_; break;
    default: break;
    }
    return r;
}

void board::syt_slave_port_w(uint8_t value) noexcept {
    syt_submode_ = value & 0x0f;
    ++syt_submode_hits_[value & 0x0f];
    if (std::getenv("TAITOZ_TRACE_SYT")) {
        static int n = 0;
        if (++n < 40)
            std::fprintf(stderr, "syt slave port <- %x\n", value & 0x0f);
    }
}

void board::syt_slave_comm_w(uint8_t value) noexcept {
    const uint8_t d = value & 0x0f;
    switch (syt_submode_) {
    case 0: syt_to_master_[syt_submode_++] = d; break;
    case 1:
        syt_to_master_[syt_submode_++] = d;
        syt_status_ |= kToMaster01;
        if (std::getenv("TAITOZ_TRACE_RST"))
            std::fprintf(stderr, "Z80POST f=%d %x%x\n", dbg_frame_,
                         syt_to_master_[0], d);
        break;
    case 2: syt_to_master_[syt_submode_++] = d; break;
    case 3:
        syt_to_master_[syt_submode_++] = d;
        syt_status_ |= kToMaster23;
        break;
    // The sound program masks its own NMI around critical sections.
    case 5: syt_nmi_enabled_ = false; syt_update_nmi(); break;
    case 6: syt_nmi_enabled_ = true;  syt_update_nmi(); break;
    default: break;
    }
}

uint8_t board::syt_slave_comm_r() noexcept {
    uint8_t r = 0;
    switch (syt_submode_) {
    case 0: r = syt_to_slave_[syt_submode_++]; break;
    case 1:
        r = syt_to_slave_[syt_submode_];
        syt_status_ &= static_cast<uint8_t>(~kToSlave01);
        syt_submode_++;
        ++cmd_reads_;
        if (std::getenv("TAITOZ_TRACE_SYT"))
            std::fprintf(stderr, "Z80RD f=%d %x%x\n", dbg_frame_, syt_to_slave_[0], r);
        syt_update_nmi();
        break;
    case 2: r = syt_to_slave_[syt_submode_++]; break;
    case 3:
        r = syt_to_slave_[syt_submode_];
        syt_status_ &= static_cast<uint8_t>(~kToSlave23);
        syt_submode_++;
        syt_update_nmi();
        break;
    case 4:
        r = syt_status_;
        if (std::getenv("TAITOZ_TRACE_SYT")) {
            static int n = 0;
            static uint8_t last = 0xff;
            if (r != last && ++n < 30) {
                std::fprintf(stderr, "z80 polls status -> %02x "
                             "(commands=%u)\n", r, syt_commands_);
                last = r;
            }
        }
        break;
    default: break;
    }
    return r;
}

// ---------------------------------------------------------------------
// Sound Z80 bus
// ---------------------------------------------------------------------

uint8_t board::sound_read(uint16_t address) noexcept {
    if (address < 0x4000u)
        return address < rom_audio_.size() ? rom_audio_[address] : 0xff;
    if (address < 0x8000u) {
        // Banked window: eight 16 KiB banks selected at 0xf200.
        const std::size_t offset =
            0x4000u * sound_bank_ + (address - 0x4000u);
        return offset < rom_audio_.size() ? rom_audio_[offset] : 0xff;
    }
    if (address >= 0xc000u && address < 0xe000u)
        return sound_ram_[address - 0xc000u];
    if (address >= 0xe000u && address <= 0xe003u) {
        const uint8_t v = ym_read_ ? ym_read_(address & 3u) : 0;
        if (cmd_reads_ >= 5 && ymrd_logged_ < 400 &&
            std::getenv("TAITOZ_TRACE_YMRD")) {
            ++ymrd_logged_;
            std::fprintf(stderr, "YMRD %04x=%02x\n", address, v);
        }
        return v;
    }
    if (address == 0xe201u) return syt_slave_comm_r();
    return 0xff;
}

void board::sound_write(uint16_t address, uint8_t value) noexcept {
    if (address >= 0xc000u && address < 0xe000u) {
        sound_ram_[address - 0xc000u] = value;
        return;
    }
    if (address >= 0xe000u && address <= 0xe003u) {
        if (ym_write_) ym_write_(address & 3u, value);
        return;
    }
    if (address == 0xe200u) { syt_slave_port_w(value); return; }
    if (address == 0xe201u) { syt_slave_comm_w(value); return; }
    // 0xe400-0xe403 is the per-channel pan latch, which this mono/stereo
    // mix does not use. 0xf200 selects the banked ROM window.
    if (address == 0xf200u) { sound_bank_ = value & 7u; return; }
}

// ---------------------------------------------------------------------
// Buses
// ---------------------------------------------------------------------

uint8_t board::read8(bool sub, uint32_t address) noexcept {
    const uint32_t a = address & 0xffffffu;
    if (!sub) {
        if (a < 0x040000u)
            return a < rom_main_.size() ? rom_main_[a] : 0xff;
        if (a >= 0x080000u && a < 0x084000u) return ram_main_[a - 0x080000u];
        if (a >= 0x084000u && a < 0x088000u) return shared_[a - 0x084000u];
        if (a >= 0x100000u && a < 0x100008u) {
            // TC0110PCR: word 0 latches the palette address, word 1 is the
            // data port.
            if (((a >> 1) & 3u) == 1u) {
                const uint16_t v = pal_[pal_addr_ & 0xfffu];
                return static_cast<uint8_t>((a & 1u) ? (v & 0xff) : (v >> 8));
            }
            return 0;
        }
        if (a >= 0x200000u && a < 0x210000u) return scn_[a - 0x200000u];
        if (a >= 0x220000u && a < 0x220010u) return scnctl_[a - 0x220000u];
        if (a >= 0x300000u && a < 0x302000u) return road_[a - 0x300000u];
        if (a >= 0x400000u && a < 0x400700u) return spr_[a - 0x400000u];
        // Unmapped reads return 0x00, as MAME's unmap default does. This
        // matters: CPU B word-reads 0x200002, whose upper byte is
        // unmapped, and folds it into the sound handshake echo. With 0xff
        // here the Z80 received command 0xef instead of 0xe0, never
        // acknowledged, and the 68000 reset it as dead mid-boot.
        return 0x00;
    }
    if (a < 0x040000u) return a < rom_sub_.size() ? rom_sub_[a] : 0xff;
    if (a >= 0x080000u && a < 0x084000u) return ram_sub_[a - 0x080000u];
    if (a >= 0x084000u && a < 0x088000u) return shared_[a - 0x084000u];
    if (a == 0x100001u) return ioc_read_selected();
    if (a == 0x100003u) return 0x00;                 // watchdog pet
    if (a == 0x200003u) return syt_master_comm_r();
    if (a >= 0x200000u && a < 0x200004u &&
        std::getenv("TAITOZ_TRACE_RST"))
        std::fprintf(stderr, "68K unmapped RD f=%d a=%06x mode=%d\n",
                     dbg_frame_, a, syt_mainmode_);
    return 0x00;   // unmapped: MAME's unmap default (see CPU A note)
}

void board::write8(bool sub, uint32_t address, uint8_t value) noexcept {
    const uint32_t a = address & 0xffffffu;
    if (!sub) {
        if (a >= 0x080000u && a < 0x084000u) {
            ram_main_[a - 0x080000u] = value;
            return;
        }
        if (a >= 0x084000u && a < 0x088000u) {
            shared_[a - 0x084000u] = value;
            return;
        }
        if (a == 0x090001u) {
            // contcirc_out_w. Bit 0 is the sound Z80's reset line and is
            // active low: set = running, clear = held in reset. Bits 4-5
            // drive the 3D shutter glasses (not modelled); bits 6-7 select
            // the road palette bank.
            const bool hold = (value & 0x01) == 0;
            if (std::getenv("TAITOZ_TRACE_RST"))
                std::fprintf(stderr, "RST out_w f=%d val=%02x hold=%d (was %d)\n",
                             dbg_frame_, value, hold, sound_reset_held_);
            if (sound_reset_held_ && !hold) sound_reset_released_ = true;
            sound_reset_held_ = hold;
            road_palbank_ = (value & 0xc0) >> 6;
            return;
        }
        if (a >= 0x100000u && a < 0x100008u) {
            const unsigned off = (a >> 1) & 3u;
            if (off == 0u) {
                pal_addr_ = (a & 1u)
                    ? ((pal_addr_ & 0xff00u) | value)
                    : ((pal_addr_ & 0x00ffu) |
                       (static_cast<unsigned>(value) << 8));
            } else if (off == 1u) {
                const unsigned i = pal_addr_ & 0xfffu;
                pal_[i] = (a & 1u)
                    ? static_cast<uint16_t>((pal_[i] & 0xff00u) | value)
                    : static_cast<uint16_t>((pal_[i] & 0x00ffu) |
                                            (static_cast<uint16_t>(value) << 8));
            }
            return;
        }
        if (a >= 0x200000u && a < 0x210000u) { scn_[a - 0x200000u] = value; return; }
        if (a >= 0x220000u && a < 0x220010u) { scnctl_[a - 0x220000u] = value; return; }
        if (a >= 0x300000u && a < 0x302000u) { road_[a - 0x300000u] = value; return; }
        if (a >= 0x400000u && a < 0x400700u) { spr_[a - 0x400000u] = value; return; }
        return;
    }
    if (a >= 0x080000u && a < 0x084000u) { ram_sub_[a - 0x080000u] = value; return; }
    if (a >= 0x084000u && a < 0x088000u) { shared_[a - 0x084000u] = value; return; }
    if (a == 0x100001u) { ioc_regs_[ioc_mport_ & 7u] = value; return; }
    if (a == 0x100003u) { ioc_mport_ = value; return; }
    if (a == 0x200001u) { syt_master_port_w(value); return; }
    if (a == 0x200003u) { syt_master_comm_w(value); return; }
}

void board::apply_input(const input_state& state) {
    // Continental Circus is a cockpit cabinet (DSWA bit 0 set), so the
    // pedals are analogue. The two coin inputs are ACTIVE HIGH while
    // everything else on these ports is active low -- getting that
    // backwards means coins are simply never seen.
    //
    // IN0: b0,b1 unused(H)  b2 COIN2(active H)  b3 COIN1(active H)
    //      b4 SERVICE1(active L)  b5-7 gas pedal (active H)
    // IN1: b0,b1 unused(H)  b2 TILT(active L)   b3 START1(active L)
    //      b4 shifter(active L, toggle)  b5-7 brake pedal (active H)
    uint8_t in0 = 0x03 | 0x10;   // idle: unused bits high, service released
    uint8_t in1 = 0x03 | 0x04 | 0x08 | 0x10;  // + tilt, start, shifter idle

    if (state.coin1) in0 |= 0x08;
    if (state.coin2) in0 |= 0x04;
    if (state.service) in0 &= static_cast<uint8_t>(~0x10);
    if (state.start) in1 &= static_cast<uint8_t>(~0x08);

    // The shifter is a toggle switch on the real cabinet: high gear while
    // held. shift_up selects it, matching the driving-board convention.
    if (state.shift_up) in1 &= static_cast<uint8_t>(~0x10);

    // Pedals report a 3-bit value through a non-linear code rather than a
    // plain magnitude (MAME taitoz_state::gas_pedal_r).
    static constexpr uint8_t kPedalCode[8] = {0, 1, 3, 2, 6, 7, 5, 4};
    const auto pedal_bits = [](uint16_t value) -> uint8_t {
        // The board's pedals are 3-bit; MANX supplies a 16-bit axis.
        const unsigned level = std::min<unsigned>(value >> 9, 7u);
        return static_cast<uint8_t>(kPedalCode[level] << 5);
    };
    // Fall back to the action buttons when no analogue pedal is bound, so
    // a plain pad can still drive.
    const uint16_t gas = state.gas ? state.gas
                                   : (state.buttons[0] ? 0xffff : 0);
    const uint16_t brake = state.brake ? state.brake
                                       : (state.buttons[1] ? 0xffff : 0);
    in0 = static_cast<uint8_t>((in0 & 0x1f) | pedal_bits(gas));
    in1 = static_cast<uint8_t>((in1 & 0x1f) | pedal_bits(brake));

    io_in0_ = in0;
    io_in1_ = in1;

    // Steering: MAME reads 0xff80 + wheel, where the wheel centres at
    // 0x80 and is reversed. Prefer the dedicated 12-bit steering axis and
    // fall back to the left stick.
    int wheel;
    if (state.steering != 0x800)
        wheel = 0x80 + ((static_cast<int>(state.steering) - 0x800) >> 4);
    else
        wheel = static_cast<int>(state.left_stick_x);
    wheel = std::clamp(wheel, 0x20, 0xe0);
    io_steer_ = (0x80 - wheel);   // PORT_REVERSE
}

// ---------------------------------------------------------------------
// Machine runtime
// ---------------------------------------------------------------------

struct taitoz_machine::bus_a final : public m68000_bus {
    explicit bus_a(board* b) : m_board(b) {}
    uint8_t read8(uint32_t a) override { return m_board->read8(false, a); }
    uint16_t read16(uint32_t a) override {
        return static_cast<uint16_t>((m_board->read8(false, a) << 8) |
                                     m_board->read8(false, a + 1));
    }
    void write8(uint32_t a, uint8_t v) override { m_board->write8(false, a, v); }
    void write16(uint32_t a, uint16_t v) override {
        m_board->write8(false, a, static_cast<uint8_t>(v >> 8));
        m_board->write8(false, a + 1, static_cast<uint8_t>(v));
    }
    board* m_board;
};

struct taitoz_machine::bus_b final : public m68000_bus {
    explicit bus_b(board* b) : m_board(b) {}
    uint8_t read8(uint32_t a) override { return m_board->read8(true, a); }
    uint16_t read16(uint32_t a) override {
        return static_cast<uint16_t>((m_board->read8(true, a) << 8) |
                                     m_board->read8(true, a + 1));
    }
    void write8(uint32_t a, uint8_t v) override { m_board->write8(true, a, v); }
    void write16(uint32_t a, uint16_t v) override {
        m_board->write8(true, a, static_cast<uint8_t>(v >> 8));
        m_board->write8(true, a + 1, static_cast<uint8_t>(v));
    }
    board* m_board;
};

// The sound Z80 runs at 4 MHz beside the two 12 MHz 68000s.
constexpr int kSoundCyclesPerFrame = 4'000'000 / 60;

struct taitoz_machine::sound_cpu {
    z80_t cpu{};
    uint64_t pins{0};
    unsigned int_acks{0};
    // Z80 cycles are converted to YM2610 chip clocks (4 MHz -> 8 MHz) so
    // the chip's timers advance in emulated time.
    uint64_t ym_clock_fraction{0};
};

void taitoz_machine::set_ym_handlers(
        std::function<void(unsigned, uint8_t)> write,
        std::function<uint8_t(unsigned)> read,
        std::function<void(uint32_t)> advance_timer,
        std::function<bool()> irq) {
    m_board->set_ym_ports(std::move(write), std::move(read));
    m_ym_advance_timer = std::move(advance_timer);
    m_ym_irq = std::move(irq);
}

taitoz_machine::taitoz_machine()
    : m_sound(std::make_unique<sound_cpu>()),
      m_board(std::make_unique<board>()),
      m_bus_a(std::make_unique<bus_a>(m_board.get())),
      m_bus_b(std::make_unique<bus_b>(m_board.get())),
      m_cpu_a(std::make_unique<m68000_cpu>(*m_bus_a)),
      m_cpu_b(std::make_unique<m68000_cpu>(*m_bus_b)) {}

taitoz_machine::~taitoz_machine() = default;

bool taitoz_machine::load_roms(const std::string& path) {
    const taitoz_rom_load_result result = taitoz_rom_loader::load(path);
    if (!result) {
        std::fprintf(stderr, "taitoz: failed to load %s: %s\n", path.c_str(),
                     result.error.c_str());
        return false;
    }
    m_board->install_roms(result.roms);
    m_ready = true;
    reset();
    return true;
}

void taitoz_machine::reset() {
    if (!m_ready) return;
    m_board->reset();
    m_cpu_a->reset();
    m_cpu_b->reset();
}

void taitoz_machine::run_frame() {
    if (!m_ready) return;
    // Both CPUs take IRQ6 at vblank. MAME uses irq6_line_hold, i.e. the
    // line drops once the CPU acknowledges -- but m68000_cpu's pin is
    // level-triggered, so holding it asserted all frame would re-enter the
    // handler forever and the main loop would never run. Assert it, give
    // each CPU enough cycles to take the exception and reach its RTE, then
    // release.
    // MAME uses irq6_line_hold: the line drops as soon as the CPU
    // acknowledges, and the handler then runs on as ordinary code. So
    // assert only long enough for the exception to be taken -- entry costs
    // ~44 cycles -- and release. Holding it for a long slice instead lets
    // the CPU re-enter the handler as soon as it RTEs, which starves the
    // work the handler is supposed to do (Continental Circus builds its
    // road data there, and it never got written).
    constexpr int kIrqAckCycles = 80;

    // The visible frame runs first: vblank comes AFTER it, as on the real
    // board. Firing the IRQ at the start of the frame instead shifts the
    // vblank counters the 68000s keep in shared RAM by one count relative
    // to MAME -- and Continental Circus paces its sound commands off that
    // counter, which put the "fe" handshake into the exact frame the
    // sound Z80 was being reset, where the Z80's boot drain silently
    // discarded it. The board then never acknowledged and the 68000 kept
    // resetting it as dead.
    const int remaining = kCyclesPerFrame - kIrqAckCycles;
    const int per_slice = remaining / kSlicesPerFrame;
    const int sound_per_slice = kSoundCyclesPerFrame / kSlicesPerFrame;
    for (int slice = 0; slice < kSlicesPerFrame; ++slice) {
        m_cpu_a->execute(per_slice);
        m_cpu_b->execute(per_slice);
        run_sound(sound_per_slice);
    }

    // Then vblank, in MAME HOLD_LINE fashion: the request stays pending
    // until each CPU actually services it -- the kernel in this game masks
    // interrupts across task switches, and a dropped pulse there loses a
    // vblank count and shifts its whole task schedule.
    m_cpu_a->set_irq_hold(6);
    m_cpu_a->execute(kIrqAckCycles);
    m_cpu_b->set_irq_hold(6);
    m_cpu_b->execute(kIrqAckCycles);
    m_board->render_frame();
}

// The sound Z80 rides along with the 68000 slices so the SYT handshake
// resolves inside a frame rather than a frame late.
void taitoz_machine::run_sound(int cycles) {
    if (m_board->sound_reset_held()) {
        // Held in reset by CPU A: keep the core parked and its pins idle.
        m_sound->pins = z80_prefetch(&m_sound->cpu, 0x0000);
        return;
    }
    if (m_board->take_sound_reset_release())
        m_sound->pins = z80_reset(&m_sound->cpu);

    uint64_t pins = m_sound->pins;
    // Feed the chip in small steps rather than once per slice: the busy
    // window after a register write is only ~200 chip clocks, so a whole
    // slice of granularity would make it invisible or absurdly long.
    constexpr int kChipStep = 64;
    int since_step = 0;
    for (int clock = 0; clock < cycles; ++clock) {
        // The YM2610's timer IRQ paces the sound driver; the SYT's NMI
        // delivers each new command from the 68000.
        if (m_ym_irq && m_ym_irq()) pins |= Z80_INT; else pins &= ~Z80_INT;
        if (m_board->syt_nmi_pending()) {
            pins |= Z80_NMI;
            m_board->syt_clear_nmi();
        } else {
            pins &= ~Z80_NMI;
        }
        pins = z80_tick(&m_sound->cpu, pins);
        const uint16_t address = Z80_GET_ADDR(pins);
        // Z80_SET_DATA expands to a braced block, so every use needs its
        // own braces or the trailing semicolon detaches the else.
        if (pins & Z80_MREQ) {
            if (pins & Z80_RD) {
                Z80_SET_DATA(pins, m_board->sound_read(address));
            } else if (pins & Z80_WR) {
                m_board->sound_write(address, Z80_GET_DATA(pins));
            }
        } else if (pins & Z80_IORQ) {
            if (pins & Z80_M1) {
                ++m_sound->int_acks;
                // Interrupt acknowledge cycle (IORQ|M1, no RD): the bus
                // must supply an opcode/vector. Nothing on this board
                // drives it, so it floats to 0xff -- RST 38h under IM 0,
                // ignored under IM 1, vector 0xff under IM 2. This is
                // exactly what MAME's Z80 default IRQ callback returns;
                // leaving the bus stale here fed the YM2610 timer
                // interrupt handler garbage and the music sequencer that
                // lives in it never ran.
                Z80_SET_DATA(pins, 0xff);
            } else if (pins & Z80_RD) {
                // The board wires no Z80 I/O ports; an IN reads open bus.
                Z80_SET_DATA(pins, 0xff);
            }
        }
        if (++since_step >= kChipStep) {
            advance_chip(since_step);
            since_step = 0;
        }
    }
    if (since_step) advance_chip(since_step);
    m_sound->pins = pins;

}

// Convert Z80 cycles into YM2610 chip clocks (4 -> 8 MHz) and hand them to
// the chip, which uses them for both its timers and its busy window.
void taitoz_machine::advance_chip(int z80_cycles) {
    if (!m_ym_advance_timer || z80_cycles <= 0) return;
    m_sound->ym_clock_fraction +=
        static_cast<uint64_t>(z80_cycles) * taitoz_sound_chip_hz;
    const uint32_t chip_clocks =
        static_cast<uint32_t>(m_sound->ym_clock_fraction / 4'000'000u);
    m_sound->ym_clock_fraction %= 4'000'000u;
    if (chip_clocks) m_ym_advance_timer(chip_clocks);
}

void taitoz_machine::set_input(const input_state& state) {
    m_board->apply_input(state);
}

const uint32_t* taitoz_machine::frame_buffer() const {
    return m_board->frame_buffer().data();
}

uint32_t taitoz_machine::main_pc() const { return m_cpu_a->program_counter(); }
uint32_t taitoz_machine::sub_pc() const { return m_cpu_b->program_counter(); }
uint16_t taitoz_machine::sound_pc() const { return m_sound->cpu.pc; }
void taitoz_machine::sound_int_state(unsigned& im, bool& iff1,
                                     unsigned& acks) const {
    im = m_sound->cpu.im;
    iff1 = m_sound->cpu.iff1;
    acks = m_sound->int_acks;
}

}  // namespace taitoz
