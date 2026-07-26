// Shinobi (Sega System 16-B) per-game specialisation.
//
// This file is the single source of truth for Shinobi-specific hardware:
//   - the System 16-B memory map (program, work RAM, tilemap/text/sprite
//     RAM, palette, I/O),
//   - the 315-5196 sprite list decode,
//   - the planar 8x8x3 tile ROM decode,
//   - the System 16 video blend: background -> foreground -> text -> sprites.
//
// The renderer below is a port of `tools/shinobi_shot.c` from the in-tree
// Amiga System 16 ports (MIT, Crown Park Computing), which itself mirrors
// MAME's BSD-3-Clause `segas16b_v.cpp / segaic16.cpp / sega16sp.cpp` drivers.

#include "sega/system16b/system16b_machine.h"
#include "namco/galaxian/galaxian_machine.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

namespace system16b {

namespace {

inline uint16_t ram_word_be(const uint8_t* ram, unsigned off) {
    return static_cast<uint16_t>(
        (static_cast<uint16_t>(ram[off]) << 8) |
         static_cast<uint16_t>(ram[off + 1]));
}

inline int tile_pixel(const board& b, int code, int x, int y) {
    if (code < 0 || code >= 0x8000) return 0;
    const unsigned byte_index = static_cast<unsigned>(code) * 8u + static_cast<unsigned>(y);
    const int sh = 7 - x;
    const auto& t0 = b.tile_plane0();
    const auto& t1 = b.tile_plane1();
    const auto& t2 = b.tile_plane2();
    if (byte_index >= t0.size()) return 0;
    return (((t2[byte_index] >> sh) & 1) << 2) |
           (((t1[byte_index] >> sh) & 1) << 1) |
            ((t0[byte_index] >> sh) & 1);
}

inline unsigned sprite_word_be(const board& b, unsigned wi) {
    const auto& sg = b.sprite_gfx();
    if (wi * 2u + 1u >= sg.size()) return 0;
    return (static_cast<unsigned>(sg[wi * 2    ]) << 8) |
            static_cast<unsigned>(sg[wi * 2 + 1]);
}

void palette_rgb(const board& b, int index, int& r, int& g, int& blue) {
    const auto& pal = b.palette_ram();
    const std::size_t need = static_cast<std::size_t>(index) * 2u + 1u;
    if (need >= pal.size()) { r = g = blue = 0; return; }
    const unsigned w =
        (static_cast<unsigned>(pal[index * 2    ]) << 8) |
         static_cast<unsigned>(pal[index * 2 + 1]);
    const int r5 = ((w >> 12) & 0x01) | ((w << 1) & 0x1e);
    const int g5 = ((w >> 13) & 0x01) | ((w >> 3) & 0x1e);
    const int b5 = ((w >> 14) & 0x01) | ((w >> 7) & 0x1e);
    r     = (r5 * 255 + 15) / 31;
    g     = (g5 * 255 + 15) / 31;
    blue  = (b5 * 255 + 15) / 31;
}

inline uint32_t pack_rgba(int r, int g, int b) {
    return static_cast<uint32_t>(r) |
           (static_cast<uint32_t>(g) << 8) |
           (static_cast<uint32_t>(b) << 16) |
           static_cast<uint32_t>(0xff000000u);
}

constexpr int kSpritePalBase = 0x400;

}  // namespace

uint8_t board::mapper_read(uint32_t address) noexcept {
    // The 315-5195 is byte-wide on the low lane of the 68000 bus. Its
    // 32-byte register file is the fallback mapping at every address not
    // claimed by a configured region.
    if ((address & 1u) == 0) return 0xff;
    const unsigned reg = (address >> 1) & 0x1fu;
    switch (reg) {
    case 0x00:
    case 0x01: return mapper_regs_[reg];
    case 0x02:
        // MAME 315_5195.cpp: reads back 0x00 ("68000 halted") once the
        // game has written 3 into the low bits, else 0x0f (executing).
        return (mapper_regs_[0x02] & 3) == 3 ? 0x00 : 0x0f;
    case 0x03: return sound_mailbox_;
    default:   return 0xff;
    }
}

void board::mapper_write(uint32_t address, uint8_t data) noexcept {
    if ((address & 1u) == 0) return;
    const unsigned reg = (address >> 1) & 0x1fu;
    if (std::getenv("SHINOBI_TRACE_MAPPER")) {
        static int reported = 0;
        if (reported < 60)
            std::fprintf(stderr, "mapper write #%d %06x reg %02x = %02x\n",
                         ++reported, address, reg, data);
    }
    mapper_regs_[reg] = data;
    if (reg == 0x03) {
        // Main -> sound command. PBF/INT remains asserted until the Z80
        // consumes the mapper port, rather than being dropped at frame end.
        sound_latch_ = data;
        sound_irq_pending_ = true;
        if (std::getenv("SHINOBI_TRACE_SOUND"))
            std::fprintf(stderr, "shinobi sound command %02x\n", data);
    } else if (reg == 0x05) {
        if (data == 0x02) {
            const uint32_t read_address =
                (uint32_t{mapper_regs_[0x07]} << 17) |
                (uint32_t{mapper_regs_[0x08]} << 9) |
                (uint32_t{mapper_regs_[0x09]} << 1);
            const uint16_t value = cpu_read_16(read_address);
            mapper_regs_[0x00] = static_cast<uint8_t>(value >> 8);
            mapper_regs_[0x01] = static_cast<uint8_t>(value);
        } else if (data == 0x01) {
            const uint32_t write_address =
                (uint32_t{mapper_regs_[0x0a]} << 17) |
                (uint32_t{mapper_regs_[0x0b]} << 9) |
                (uint32_t{mapper_regs_[0x0c]} << 1);
            cpu_write_16(write_address, static_cast<uint16_t>(
                (mapper_regs_[0x00] << 8) | mapper_regs_[0x01]));
        }
    }
}

// =====================================================================
// Memory map - System 16-B Shinobi (68000 bus, big-endian 16-bit words)
// =====================================================================

// Resolve an address against the eight 315-5195 mapper regions the game
// has programmed (registers 0x10-0x1f: control byte then base byte per
// region). Lower regions win, exactly as MAME maps them last.
bool board::mapper_region(uint32_t address, unsigned& region_out,
                          uint32_t& offset_out) const noexcept {
    static constexpr uint32_t size_masks[4] = {
        0x00ffffu, 0x01ffffu, 0x07ffffu, 0x1fffffu};
    for (unsigned region = 0; region < 8; ++region) {
        const uint32_t mask =
            size_masks[mapper_regs_[0x10 + 2 * region] & 3];
        const uint32_t base =
            (static_cast<uint32_t>(mapper_regs_[0x11 + 2 * region]) << 16) &
            ~mask & 0xFFFFFFu;
        if ((address & ~mask & 0xFFFFFFu) != base) continue;
        region_out = region;
        offset_out = address & mask;
        return true;
    }
    return false;
}

// The standard_io_r decode, shared by the fixed Shinobi layout and the
// mapper-decoded boards (the region base differs, the offsets do not).
uint8_t board::io_region_read(uint32_t offset) noexcept {
    const unsigned off = (offset & 0x3fffu) >> 1;
    switch (off & 0x1800u) {
    case 0x0800u:
        switch (off & 3u) {
        case 0u: return service_input_;
        case 1u: return p1_input_;
        case 2u: return 0xFF;
        case 3u: return p2_input_;
        }
        return 0xFF;
    case 0x1000u: return (off & 1u) ? dsw1_ : dsw2_;
    default:      return 0xFF;
    }
}

uint8_t board::byte_read(uint32_t address) noexcept {
    address &= 0xFFFFFFu;
    if (mapper_decode_) {
        unsigned region;
        uint32_t offset;
        if (mapper_region(address, region, offset)) {
            switch (region) {
            case 0: return program_[offset & 0x3ffffu];
            case 1: return program_[0x40000u + (offset & 0x3ffffu)];
            case 2: return 0xFF; // 5704 tile bank register, write-only
            case 3: return work_ram_[offset & (kWorkRamBytes - 1)];
            case 4: return sprite_ram_[offset & (kSpriteRamBytes - 1)];
            case 5: return (offset & 0x10000u)
                        ? text_ram_[offset & (kTextRamBytes - 1)]
                        : tile_ram_[offset & (kTileRamBytes - 1)];
            case 6: return palette_ram_[offset & (kPaletteBytes - 1)];
            case 7: return io_region_read(offset);
            }
        }
        return mapper_read(address);
    }
    if (address < 0x040000u) {
        return program_[address];
    }
    if (address >= 0x3F0000u && address < 0x400000u) return 0xFF;
    if (address >= 0x400000u && address < 0x410000u) {
        return tile_ram_[(address - 0x400000u) & (kTileRamBytes - 1)];
    }
    if (address >= 0x410000u && address < 0x411000u) {
        return text_ram_[(address - 0x410000u) & (kTextRamBytes - 1)];
    }
    if (address >= 0x440000u && address < 0x440800u) {
        return sprite_ram_[(address - 0x440000u) & (kSpriteRamBytes - 1)];
    }
    if (address >= 0x840000u && address < 0x841000u) {
        return palette_ram_[(address - 0x840000u) & (kPaletteBytes - 1)];
    }
    if (address >= 0xC40000u && address < 0xC44000u) {
        // I/O region decode (MAME standard_io_r): the offset here is a
        // WORD offset, so the region select bits are 0x1800, not 0x3000.
        //   0xC41000-0xC41007: SERVICE / P1 / unused / P2
        //   0xC42001: DSW2     0xC42003: DSW1
        const unsigned off = (address - 0xC40000u) >> 1;
        switch (off & 0x1800u) {
        case 0x0800u:
            switch (off & 3u) {
            case 0u: return service_input_;
            case 1u: return p1_input_;
            case 2u: return 0xFF;
            case 3u: return p2_input_;
            }
            return 0xFF;
        case 0x1000u: return (off & 1u) ? dsw1_ : dsw2_;
        default:      return 0xFF;
        }
    }
    if (address >= 0xC60000u && address < 0xC60002u) return 0xFF;
    if (address >= 0xFE0000u && address < 0xFE0040u) {
        return mapper_read(address);
    }
    if (address >= 0xF0000u && address < 0x100000u) {
        // System 16-B status registers (F01C, F01E and friends). Real
        // MAME reads return 0 from these when no event is pending; we
        // mimic that so the boot ROM's `tst.w / bne.w` clears and the
        // main CPU keeps polling without branching into the
        // infinite-spin wait.
        return 0x00;
    }
    if (address >= 0xFF0000u) {
        return work_ram_[(address - 0xFF0000u) & (kWorkRamBytes - 1)];
    }
    return mapper_read(address);
}

void board::byte_write(uint32_t address, uint8_t data) noexcept {
    address &= 0xFFFFFFu;
    if (mapper_decode_) {
        unsigned region;
        uint32_t offset;
        if (mapper_region(address, region, offset)) {
            switch (region) {
            case 0:
            case 1: return; // ROM
            case 2:
                // 5704 tile bank register: word offset bit 0 picks the
                // tilemap slot, the low three data bits pick which
                // 4096-tile page it shows; only the odd byte carries
                // data (MAME rom_5704_bank_w, ACCESSING_BITS_0_7).
                if (address & 1u) {
                    tile_banks_[(offset >> 1) & 1u] = data & 7u;
                    if (std::getenv("SHINOBI_TRACE_BANK")) {
                        static int reported = 0;
                        if (reported < 64)
                            std::fprintf(stderr,
                                         "tile bank #%d %06x = %02x\n",
                                         ++reported, address, data);
                    }
                }
                return;
            case 3:
                work_ram_[offset & (kWorkRamBytes - 1)] = data;
                return;
            case 4:
                sprite_ram_[offset & (kSpriteRamBytes - 1)] = data;
                return;
            case 5:
                if (offset & 0x10000u)
                    text_ram_[offset & (kTextRamBytes - 1)] = data;
                else
                    tile_ram_[offset & (kTileRamBytes - 1)] = data;
                return;
            case 6:
                palette_ram_[offset & (kPaletteBytes - 1)] = data;
                return;
            case 7: return; // I/O writes (coin counters, lamps)
            }
        }
        mapper_write(address, data);
        return;
    }
    if (address < 0x040000u) return;
    if (address >= 0x400000u && address < 0x410000u) {
        tile_ram_[(address - 0x400000u) & (kTileRamBytes - 1)] = data;
        return;
    }
    if (address >= 0x410000u && address < 0x411000u) {
        text_ram_[(address - 0x410000u) & (kTextRamBytes - 1)] = data;
        return;
    }
    if (address >= 0x440000u && address < 0x440800u) {
        sprite_ram_[(address - 0x440000u) & (kSpriteRamBytes - 1)] = data;
        return;
    }
    if (address >= 0x840000u && address < 0x841000u) {
        if (std::getenv("SHINOBI_TRACE_PALETTE")) {
            static int reported = 0;
            if (reported < 4)
                std::fprintf(stderr, "palette write #%d %06x=%02x\n",
                             ++reported, address, data);
        }
        palette_ram_[(address - 0x840000u) & (kPaletteBytes - 1)] = data;
        return;
    }
    if (address >= 0xFF0000u) {
        static const char* watch = std::getenv("SHINOBI_WATCH_RAM");
        if (watch) {
            static uint32_t base = std::strtoul(watch, nullptr, 16);
            if (address >= base && address < base + 0x20 &&
                work_ram_[(address - 0xFF0000u) & (kWorkRamBytes - 1)] !=
                    data) {
                static int reported = 0;
                if (reported < 64)
                    std::fprintf(stderr, "ram write #%d %06x=%02x\n",
                                 ++reported, address, data);
            }
        }
    }
    if (address >= 0x3F0000u && address < 0x400000u) return;
    if (address >= 0xC40000u && address < 0xC44000u) {
        static bool trace_io = std::getenv("SHINOBI_TRACE_IO_WRITES");
        if (trace_io)
            std::fprintf(stderr, "shinobi io write %06x = %02x\n",
                         address, data);
        return;
    }
    if (address >= 0xC60000u && address < 0xC60002u) return;
    if (address >= 0xFE0000u && address < 0xFE0040u) {
        mapper_write(address, data);
        return;
    }
    if (address >= 0xFF0000u) {
        work_ram_[(address - 0xFF0000u) & (kWorkRamBytes - 1)] = data;
        return;
    }
    mapper_write(address, data);
}

void board::reset_extras() {
    work_ram_.fill(0);
    sound_work_ram_.fill(0);
    palette_ram_.fill(0);
    mapper_regs_.fill(0);
    // System 16-B Shinobi boot sequence: the M68K resets with SR=2700
    // (interrupts masked) and the boot ROM eventually writes a "ready"
    // byte that the M68K polls at 0xFE0000 to release the wait. We seed
    // the mailbox with 0x00. The Z80 itself runs, but this secondary board
    // handshake is not yet represented by its current memory map.
    sound_mailbox_ = 0x00;
    sound_latch_ = 0;
    sound_irq_pending_ = false;
    sound_bankoffs_ = 0;
    tile_banks_ = {0, 1};
    high_scores_.reset();
}

void board::service_high_scores() {
    high_scores_.update(
        [this](std::uint32_t address) { return byte_read(address); },
        [this](std::uint32_t address, std::uint8_t value) {
            byte_write(address, value);
        });
}

void board::flush_high_scores() {
    high_scores_.flush(
        [this](std::uint32_t address) { return byte_read(address); });
}
void board::apply_input(const input_state& s, uint8_t* ports,
                        std::size_t port_count) {
    // The Shinobi (Sega System 16B) I/O mirror exposed at 0xC40000
    // maps to five 8-bit bytes:
    //   port[0] = P1 input (active low), 0xC41002/03
    //   port[1] = P2 input (active low), 0xC41006/07
    //   port[2] = service/coin/start (active low), 0xC41000/01
    //   port[3] = DSW1, 0xC42002/03
    //   port[4] = DSW2, 0xC42000/01
    //
    // Direction and action bits follow MAME's system16b_generic ports.

    if (port_count >= 1) {
        uint8_t p1 = 0xff;       // active-low; assume released
        // 4-direction joystick.
        if (s.left_stick_x < 0x60) p1 &= static_cast<uint8_t>(~0x80);   // left
        if (s.left_stick_x > 0x90) p1 &= static_cast<uint8_t>(~0x40);   // right
        if (s.left_stick_y < 0x60) p1 &= static_cast<uint8_t>(~0x20);   // up
        if (s.left_stick_y > 0x90) p1 &= static_cast<uint8_t>(~0x10);   // down
        if (s.buttons[0]) p1 &= static_cast<uint8_t>(~0x02); // button 1
        if (s.buttons[1]) p1 &= static_cast<uint8_t>(~0x04); // button 2
        if (s.buttons[2]) p1 &= static_cast<uint8_t>(~0x01); // button 3
        ports[0] = p1;
    }
    if (port_count >= 2) {
        uint8_t p2 = 0xff;
        // Two-player co-op cabinet: full second joystick.
        if (s.p2_stick_x < 0x60) p2 &= static_cast<uint8_t>(~0x80);
        if (s.p2_stick_x > 0x90) p2 &= static_cast<uint8_t>(~0x40);
        if (s.p2_stick_y < 0x60) p2 &= static_cast<uint8_t>(~0x20);
        if (s.p2_stick_y > 0x90) p2 &= static_cast<uint8_t>(~0x10);
        if (s.p2_buttons[0]) p2 &= static_cast<uint8_t>(~0x02);
        if (s.p2_buttons[1]) p2 &= static_cast<uint8_t>(~0x04);
        if (s.p2_buttons[2]) p2 &= static_cast<uint8_t>(~0x01);
        ports[1] = p2;
    }
    if (port_count >= 3) {
        uint8_t svc = 0xff;
        if (s.coin1)  svc &= static_cast<uint8_t>(~0x01);
        if (s.coin2)   svc &= static_cast<uint8_t>(~0x02);
        if (s.test) svc &= static_cast<uint8_t>(~0x04);
        if (s.service) svc &= static_cast<uint8_t>(~0x08);
        if (s.start) svc &= static_cast<uint8_t>(~0x10);
        if (s.p2_start) svc &= static_cast<uint8_t>(~0x20);
        ports[2] = svc;
    }
    if (port_count >= 4) ports[3] = dsw1_;
    if (port_count >= 5) ports[4] = dsw2_;
}

// Sound CPU (Z80) bus wiring -- standard System 16B sound map (MAME
// segas16b.cpp sound_map / sound_portmap, shinobi4 = ROM board 171-5521):
//
//   0x0000-0x7fff  program ROM (epr-11361.a10, 32 KiB)
//   0x8000-0xdfff  banked data ROM (mpr-11362.a11 region; bank offset
//                  selected by uPD7759 control port 0x40:
//                  bankoffs = ((data>>3)&1)*0x20000 + (data&7)*0x4000)
//   0xe800         sound command latch read (written by main at 0xFE0005)
//   0xf800-0xffff  work RAM
//
// I/O ports (handled by the machine's Z80 pin loop, which calls
// co_cpu_port_read/co_cpu_port_write):
//   0x00/0x01      YM2151 address/data
//   0x40           uPD7759 control (bank + reset; also sets bankoffs)
//   0x80           uPD7759 sample port write / busy status read
//   0xc0           sound command latch read

uint8_t board::co_cpu_read(unsigned address) {
    const unsigned a = address & 0xFFFFu;
    if (a == 0xE800u) {
        sound_irq_pending_ = false;
        return sound_latch_;
    }
    if (a < 0x8000u) {
        if (!have_sound_rom_) return 0xFF;
        return sound_prog_rom_[a];
    }
    if (a < 0xE000u) {
        if (!have_sound_rom_) return 0xFF;
        const unsigned off = (sound_bankoffs_ + (a - 0x8000u))
                             % sound_data_rom_.size();
        return sound_data_rom_[off];
    }
    if (a >= 0xF800u) return sound_work_ram_[a & 0x7FFu];
    return 0xFF;
}

void board::co_cpu_write(unsigned address, uint8_t data) {
    const unsigned a = address & 0xFFFFu;
    if (a >= 0xF800u) sound_work_ram_[a & 0x7FFu] = data;
}

uint8_t board::co_cpu_port_read(unsigned port) {
    // MAME shinobi sound_portmap mirrors each device across its 0x40
    // window: 0x00-0x3f YM2151, 0x40-0x7f uPD7759 control, 0x80-0xbf
    // uPD7759 data/status, 0xc0-0xff sound latch.
    switch (port & 0xC0u) {
    case 0x00u:                         // YM2151 status (busy + timer flags)
        return ym_status_cb_ ? ym_status_cb_() : 0x00;
    case 0x80u: return 0x00;            // uPD7759 busy=0 (stub: instant ACK)
    case 0xC0u:
        sound_irq_pending_ = false;
        return sound_latch_;            // sound command latch
    default:    return 0xFF;
    }
}

void board::co_cpu_port_write(unsigned port, uint8_t data) {
    if (std::getenv("SHINOBI_TRACE_SOUND"))
        std::fprintf(stderr, "shinobi z80 port write %02x = %02x\n",
                     port, data);
    switch (port & 0xC0u) {
    case 0x40u:
        // uPD7759 control: bits 3/0-2 select the Z80 bank window offset
        // (ROM board 171-5521). Sample playback itself is not emulated.
        sound_bankoffs_ = ((data & 0x08u) >> 3) * 0x20000u +
                          (data & 0x07u) * 0x4000u;
        break;
    case 0xC0u:
        // Sound -> main return byte (315-5195 pwrite/m_from_sound). The
        // 68000 reads it back as mapper register 3. Aurail sequences its
        // whole attract on these: it holds the title screen until the Z80
        // answers the music command, so without this wire the attract
        // never leaves the title.
        sound_mailbox_ = data;
        break;
    default:
        break;
    }
}

unsigned board::cpu_read_16(unsigned address) {
    const uint8_t hi = byte_read(static_cast<uint32_t>(address));
    const uint8_t lo = byte_read(static_cast<uint32_t>(address + 1));
    return (static_cast<unsigned>(hi) << 8) | static_cast<unsigned>(lo);
}
void     board::cpu_write_16(unsigned address, unsigned data) {
    byte_write(static_cast<uint32_t>(address),     static_cast<uint8_t>(data >> 8));
    byte_write(static_cast<uint32_t>(address + 1), static_cast<uint8_t>(data     ));
}

// =====================================================================
// Renderer -- port of shinobi_shot.c::render_frame
// =====================================================================

void board::render_frame(uint32_t* rgba) {
    std::memset(rgba, 0, sizeof(uint32_t) * kScreenW * kScreenH);

    const auto& textram = text_ram_;
    const auto& tilram   = tile_ram_;
    const auto& sprram   = sprite_ram_;

    auto draw_tilemap = [&](int which, bool opaque) {
        const uint16_t pages   = ram_word_be(textram.data(), 0xe80 + which * 2);
        const uint16_t xscroll = ram_word_be(textram.data(), 0xe98 + which * 2);
        const uint16_t yscroll = ram_word_be(textram.data(), 0xe90 + which * 2);
        // MAME segaic16.cpp tilemap_16b_draw_layer: the effective scroll
        // is (0xc0 - xscroll + xoffs) & 0x3ff (xoffs = 0 for Shinobi) and
        // bit 15 of xscroll selects per-8-row rowscroll from textram
        // 0xf80 + which*0x40.
        const bool rowscroll = (xscroll & 0x8000) != 0;
        for (int sy = 0; sy < kScreenH; ++sy) {
            uint16_t effxscroll = xscroll;
            if (rowscroll) {
                effxscroll = ram_word_be(textram.data(),
                    0xf80 + which * 0x40 + (sy >> 3) * 2);
            }
            const int effx = (0xc0 - effxscroll) & 0x3ff;
            const int effy = yscroll & 0x1ff;
            for (int sx = 0; sx < kScreenW; ++sx) {
                const int vx = (sx + effx) & 0x3ff;
                const int vy = (sy + effy) & 0x1ff;
                const int shift = ((vx >= 512) ? 4 : 0) + ((vy >= 256) ? 8 : 0);
                const int page  = (pages >> shift) & 0xf;
                const int pcol  = (vx & 511) >> 3;
                const int prow  = (vy & 255) >> 3;
                const int tindex = prow * 64 + pcol;
                const uint16_t w = ram_word_be(tilram.data(),
                    page * 0x1000u + static_cast<unsigned>(tindex) * 2u);
                const int color = (w >> 6) & 0x7f;
                // Tile bits 0-11 index within a 4096-tile page; bit 12
                // picks the bank slot. The default banks {0, 1} reproduce
                // the unbanked 13-bit index for boards without a 5704.
                const int raw   = w & 0x1fff;
                const int code  = tile_banks_[(raw >> 12) & 1] * 0x1000 +
                                  (raw & 0xfff);
                const int pen   = tile_pixel(*this, code, vx & 7, vy & 7);
                if (!opaque && pen == 0) continue;
                int r, g, blue;
                palette_rgb(*this, color * 8 + pen, r, g, blue);
                rgba[sy * kScreenW + sx] = pack_rgba(r, g, blue);
            }
        }
    };

    auto draw_text = [&]() {
        for (int sy = 0; sy < kScreenH; ++sy) {
            const int row = sy >> 3;
            if (row >= 28) continue;
            for (int sx = 0; sx < kScreenW; ++sx) {
                const int tx = sx + 192;
                const int col = (tx >> 3) & 63;
                const int tindex = row * 64 + col;
                const uint16_t w = ram_word_be(textram.data(),
                    static_cast<unsigned>(tindex) * 2u);
                const int color = (w >> 9) & 7;
                const int code  = w & 0x1ff;
                const int pen   = tile_pixel(*this, code, tx & 7, sy & 7);
                if (pen == 0) continue;
                int r, g, blue;
                palette_rgb(*this, color * 8 + pen, r, g, blue);
                rgba[sy * kScreenW + sx] = pack_rgba(r, g, blue);
            }
        }
    };

    auto plot_spr_pix = [&](int x, int y, int colpri) {
        const int pix = colpri & 0xf;
        if (pix == 0 || pix == 15) return;
        if (x < 0 || x >= kScreenW || y < 0 || y >= kScreenH) return;
        const int palidx = kSpritePalBase | (colpri & 0x3ff);
        int r, g, blue;
        palette_rgb(*this, palidx, r, g, blue);
        rgba[y * kScreenW + x] = pack_rgba(r, g, blue);
    };

    auto draw_sprites = [&]() {
        constexpr int kOriginX = 189;
        const int num_entries = kSpriteRamBytes / 16;
        for (int e = 0; e < num_entries; ++e) {
            const uint8_t* d = sprram.data() + e * 16;
            uint16_t sdata[8];
            for (int i = 0; i < 8; ++i) {
                sdata[i] = static_cast<uint16_t>(
                    (static_cast<uint16_t>(d[i * 2]) << 8) |
                     static_cast<uint16_t>(d[i * 2 + 1]));
            }
            if (sdata[2] & 0x8000) break;
            const int bottom = sdata[0] >> 8;
            const int top    = sdata[0] & 0xff;
            const int xpos   = sdata[1] & 0x1ff;
            const bool hide  = (sdata[2] & 0x4000) != 0;
            const bool flip  = (sdata[2] & 0x100)  != 0;
            const int pitch  = static_cast<int8_t>(sdata[2] & 0xff);
            unsigned    addr  = sdata[3];
            const int   bank  = (sdata[4] >> 8) & 0xf;
            const int   colpri= ((sdata[4] & 0xff) << 4) |
                                (((sdata[1] >> 9) & 0xf) << 12);
            const int   vzoom = (sdata[5] >> 5) & 0x1f;
            const int   hzoom = sdata[5] & 0x1f;
            if (hide || top >= bottom) continue;
            // ROM boards wire the sprite bank lines through a per-board
            // LUT (MAME segas16b.cpp banklists); an unmapped bank has no
            // ROM behind it and draws nothing.
            unsigned mapped_bank = static_cast<unsigned>(bank);
            if (sprite_banklist_) {
                mapped_bank = sprite_banklist_[bank & 0xf];
                if (mapped_bank == 255u) continue;
            }
            const unsigned bank_word_base =
                0x10000u * (mapped_bank % sprite_bank_mod_);
            const int sx0 = xpos - kOriginX;
            unsigned yacc = 0;
            for (int y = top; y < bottom; ++y) {
                addr += pitch;
                yacc += static_cast<unsigned>(vzoom) << 10;
                if (yacc & 0x8000u) { addr += pitch; yacc &= ~0x8000u; }
                const int scry = y;
                if (scry < 0 || scry >= kScreenH) continue;
                int xacc = 4 * hzoom;
                unsigned a = addr;
                if (!flip) {
                    int x = sx0;
                    for (;;) {
                        const unsigned pixels =
                            sprite_word_be(*this,
                                bank_word_base + (a & 0xffffu));
                        a++;
                        int pix;
                        pix = (pixels >> 12) & 0xf;
                        xacc = (xacc & 0x3f) + hzoom;
                        if (xacc < 0x40) { plot_spr_pix(x, scry, colpri | pix); x++; }
                        pix = (pixels >> 8) & 0xf;
                        xacc = (xacc & 0x3f) + hzoom;
                        if (xacc < 0x40) { plot_spr_pix(x, scry, colpri | pix); x++; }
                        pix = (pixels >> 4) & 0xf;
                        xacc = (xacc & 0x3f) + hzoom;
                        if (xacc < 0x40) { plot_spr_pix(x, scry, colpri | pix); x++; }
                        pix = (pixels >> 0) & 0xf;
                        xacc = (xacc & 0x3f) + hzoom;
                        if (xacc < 0x40) { plot_spr_pix(x, scry, colpri | pix); x++; }
                        if (pix == 15) break;
                        if (x > sx0 + 0x200) break;
                    }
                } else {
                    int x = sx0;
                    for (;;) {
                        const unsigned pixels =
                            sprite_word_be(*this,
                                bank_word_base + (a & 0xffffu));
                        a--;
                        int pix;
                        pix = (pixels >> 0) & 0xf;
                        xacc = (xacc & 0x3f) + hzoom;
                        if (xacc < 0x40) { plot_spr_pix(x, scry, colpri | pix); x++; }
                        pix = (pixels >> 4) & 0xf;
                        xacc = (xacc & 0x3f) + hzoom;
                        if (xacc < 0x40) { plot_spr_pix(x, scry, colpri | pix); x++; }
                        pix = (pixels >> 8) & 0xf;
                        xacc = (xacc & 0x3f) + hzoom;
                        if (xacc < 0x40) { plot_spr_pix(x, scry, colpri | pix); x++; }
                        pix = (pixels >> 12) & 0xf;
                        xacc = (xacc & 0x3f) + hzoom;
                        if (xacc < 0x40) { plot_spr_pix(x, scry, colpri | pix); x++; }
                        if (pix == 15) break;
                        if (x > sx0 + 0x200) break;
                    }
                }
            }
        }
    };

    draw_tilemap(1, /*opaque=*/true);
    draw_tilemap(0, /*opaque=*/false);
    draw_text();
    draw_sprites();
}

}  // namespace system16b
