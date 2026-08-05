// Shinobi (System 16-B) host-side boot test.
//
// Boots the Shinobi machine against an actual ROM image and dumps the
// first frame's RGBA pixels to /tmp/system16b_frame.rgba so we can verify
// the renderer ran without needing a display capture.
//
// Usage: shinobi_boot_test <rom_path_or_zip> [n_frames]
//   rom_path_or_zip: path to a Shinobi MAME rom set (zip), an
//                    extracted directory containing shinobi_main.bin +
//                    mpr-11363.a14 + ... + mpr-11369.b6, or any directory
//                    containing a single flat `shinobi_main.bin`.
//   n_frames       : number of game frames to advance (default 60).
//
// Writes `/tmp/system16b_frame.rgba` (320*224*4 = 286 720 bytes) and prints
// a one-line summary.
//
// The 68000 (instance-based m68000_cpu / Moira core) starts running the
// System 16-B Shinobi program image after reset(); we feed it 60 frames
// @ 166 667 cycles each (10 MHz / 60 Hz), with no front-end interaction.
#include "sega/system16b/system16b_machine.h"
#include "sega/system16b/system16b_rom.h"
#include "namco/galaxian/galaxian_machine.h"
#include "m68000_cpu.h"

#include "z80.h"

#include <algorithm>
#include <cstdio>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <fstream>
#include <memory>
#include <vector>

namespace {

class bus_adapter final : public m68000_bus {
public:
    explicit bus_adapter(system16b::board* b) : m_board(b) {}
    uint8_t read8(uint32_t address) override {
        if (g_log_io && (address & 0xFF0000u) >= 0xC00000u && g_log_io_count < 500u) {
            std::fprintf(stderr, "R8  %06x\n", address);
            ++g_log_io_count;
        }
        return m_board->byte_read(address);
    }
    uint16_t read16(uint32_t address) override {
        if (g_log_io && (address & 0xFF0000u) >= 0xC00000u && g_log_io_count < 500u) {
            std::fprintf(stderr, "R16 %06x\n", address);
            ++g_log_io_count;
        }
        return static_cast<uint16_t>(m_board->cpu_read_16(address));
    }
    void write8(uint32_t address, uint8_t value) override {
        m_board->byte_write(address, value);
    }
    void write16(uint32_t address, uint16_t value) override {
        m_board->cpu_write_16(address, value);
    }
private:
    system16b::board* m_board;
    static bool g_log_io;
    static int  g_log_io_count;
};

bool bus_adapter::g_log_io = []() {
    const char* e = std::getenv("SHINOBI_TRACE_IO");
    return e && std::atoi(e);
}();
int bus_adapter::g_log_io_count = 0;

// Ring of recently sampled PCs (sampled per execute chunk) for crash
// diagnosis.
uint32_t g_pc_ring[64];
unsigned g_pc_ring_pos = 0;
bool g_hit_ram_exec = false;

void run_main(system16b::board& board, bus_adapter&, m68000_cpu& cpu,
              int cycles) {
    while (cycles > 0 && !g_hit_ram_exec) {
        const int chunk = cycles > 8 ? 8 : cycles;
        const uint32_t pc = cpu.program_counter();
        // Exception catcher: a `bra.s *` at the PC is where these games'
        // vectors point on illegal/zero-divide faults. Stop so the ring
        // still holds the faulting path. (PC value alone is not enough:
        // Shinobi's reset vector legitimately enters at 0x400.)
        if (pc >= 0x400u && pc <= 0x406u &&
            board.byte_read(pc) == 0x60 && board.byte_read(pc + 1) == 0xfe)
            break;
        g_pc_ring[g_pc_ring_pos & 63] = pc;
        ++g_pc_ring_pos;
        if (pc == 0xFFFFEF00u) g_hit_ram_exec = true;  // faulting RAM trampoline
        cycles -= cpu.execute(chunk);
    }
}

}  // namespace



int main(int argc, char** argv) {
    {
        system16b::board input_board;
        // MANX's cabinet profile is English with attract audio on;
        // both settings are active-low in Shinobi's SW2 bank.
        assert((input_board.dsw2_ & 0x80) == 0);
        assert((input_board.dsw2_ & 0x02) == 0);
        input_state input;
        input.coin1 = true;
        input.start = true;
        input.left_stick_x = 0;
        input.buttons[0] = true;
        uint8_t ports[5]{};
        input_board.apply_input(input, ports, std::size(ports));
        // apply_input fills a caller-owned ports[] buffer; the runtime's
        // set_input() copies it into the board mirror that byte_read()
        // decodes (System 16-B I/O at 0xC40000). Mirror that here so the
        // standalone board sees the same state a session would.
        input_board.p1_input_      = ports[0];
        input_board.p2_input_      = ports[1];
        input_board.service_input_ = ports[2];
        input_board.dsw1_          = ports[3];
        input_board.dsw2_          = ports[4];
        assert((input_board.byte_read(0xc41000) & 0x01) == 0);
        assert((input_board.byte_read(0xc41000) & 0x10) == 0);
        assert((input_board.byte_read(0xc41002) & 0x80) == 0);
        assert((input_board.byte_read(0xc41002) & 0x02) == 0);

        // Mapper register 3 is visible through every otherwise-unmapped
        // address. A write through the address used by System 16 bootleg
        // maps must reach the Z80 latch and hold INT until the latch read.
        input_board.byte_write(0x123407, 0x5a);
        assert(input_board.sound_irq_pending_);
        assert(input_board.co_cpu_port_read(0xc0) == 0x5a);
        assert(!input_board.sound_irq_pending_);

        // The entire 32 KiB sound program must be directly addressable;
        // the old 8 KiB region mirrored initialization code four times.
        input_board.have_sound_rom_ = true;
        input_board.sound_prog_rom_[0x2345] = 0xa5;
        assert(input_board.co_cpu_read(0x2345) == 0xa5);
    }
    if (argc == 2 && std::strcmp(argv[1], "--input-test") == 0)
        return 0;
    if (argc < 2) {
        std::fprintf(stderr,
                     "Usage: %s <rom_path_or_zip_or_dir> [n_frames]\n", argv[0]);
        return 1;
    }
    const std::string rom_path = argv[1];
    const int n_frames = (argc >= 3) ? std::atoi(argv[2]) : 60;

    const auto rl = system16b::system16b_rom_loader::load(rom_path);
    if (!rl) {
        std::fprintf(stderr, "shinobi: failed to load %s: %s\n",
                     rom_path.c_str(), rl.error.c_str());
        return 2;
    }
    std::printf("shinobi: loaded %s set=%d program=%zu bytes\n",
                system16b::system16b_rom_loader::set_display_name(rl.set),
                static_cast<int>(rl.set),
                rl.roms.program.size());

    auto board_ptr = std::make_unique<system16b::board>();
    system16b::board* board = board_ptr.get();
    std::memcpy(board->program_rom().data(),   rl.roms.program.data(),
                rl.roms.program.size());
    std::memcpy(board->tile_plane0().data(),   rl.roms.tile_plane0.data(),
                rl.roms.tile_plane0.size());
    std::memcpy(board->tile_plane1().data(),   rl.roms.tile_plane1.data(),
                rl.roms.tile_plane1.size());
    std::memcpy(board->tile_plane2().data(),   rl.roms.tile_plane2.data(),
                rl.roms.tile_plane2.size());
    std::memcpy(board->sprite_gfx().data(),    rl.roms.sprite_gfx.data(),
                rl.roms.sprite_gfx.size());

    // The sound Z80 is part of the boot path for some games: Aurail's
    // 68000 holds its attract until the Z80 answers each music command
    // through the 315-5195 return port, so a Z80-less run parks on the
    // title screen for ever. Stage the sound ROMs and tick the Z80 the
    // same way system16b_machine_t does.
    const bool sound_present = std::any_of(
        rl.roms.sound_prog.begin(), rl.roms.sound_prog.end(),
        [](uint8_t b) { return b != 0xff; });
    if (sound_present) {
        std::memcpy(board->sound_prog_rom_.data(), rl.roms.sound_prog.data(),
                    rl.roms.sound_prog.size());
        std::memcpy(board->sound_data_rom_.data(), rl.roms.sound_data.data(),
                    rl.roms.sound_data.size());
        board->have_sound_rom_ = true;
    }
    if (!rl.roms.mcu.empty())
        board->install_mcu(rl.roms.mcu.data(), rl.roms.mcu.size());
    z80_t z80{};
    uint64_t z80_pins = z80_init(&z80);
    // The Z80 sound drivers pace their sequencers on the YM2151's timer
    // flags, so the status port must come from a real synth or the driver
    // (and any 68000 logic waiting on its answers) idles for ever.
    system16b_sound_synth synth;
    synth.reset();
    static uint64_t status_calls = 0;
    static uint8_t last_status = 0;
    board->ym_status_cb_ = [&synth]() -> uint8_t {
        ++status_calls;
        last_status = synth.read_status();
        return last_status;
    };
    uint64_t ym_clock_fraction = 0;

    board->set_game(rl.set);
    board->reset_extras();
    // Boot with palette/text/tile RAM zero so the renderer emits clean
    // black until the 68000 has populated the video RAM during its
    // init-time main-loop.

    bus_adapter bus(board);
    m68000_cpu cpu(bus);
    cpu.reset();

    constexpr int kCyclesPerFrame =
        system16b::kMainCyclesPerFrame;   // 166 667 @ 10 MHz / 60 Hz
    // Boot-progress diagnostic: during boot the game increments its
    // vblank counter at work-RAM offset 0xF01C inside the level-4 IRQ
    // handler. Once boot completes the game clears that path and idles
    // in its main loop, where a per-frame PC sample always lands on the
    // same address -- so PC equality alone is a false "stalled" report.
    // Only treat the boot as stuck if the counter never advances AND the
    // PC stops moving.
    const std::size_t kVblankCounterOff = 0xF01C;
    uint8_t last_vblank = board->work_ram()[kVblankCounterOff];
    bool boot_counter_advanced = false;
    uint32_t last_pc = 0;
    unsigned unchanged_pc_frames = 0;
    // Runs a main-CPU slice honouring the mapper's 68000 controls: the
    // i8751 can hold the CPU in reset (register 2) and raise its
    // interrupts (register 4).
    const auto run_main_mcu = [&](int cycles) {
        if (board->cpu_reset_hold_) return;
        if (board->main_spin_cycles_ > 0) {
            const int spun = cycles < board->main_spin_cycles_
                                 ? cycles
                                 : board->main_spin_cycles_;
            board->main_spin_cycles_ -= spun;
            cycles -= spun;
            if (cycles <= 0) return;
        }
        if (board->cpu_reset_release_) {
            board->cpu_reset_release_ = false;
            cpu.reset();
        }
        if (board->mapper_irq_pending_ >= 0) {
            cpu.set_irq(board->mapper_irq_pending_);
            board->mapper_irq_pending_ = -1;
            run_main(*board, bus, cpu, 4000);
            cpu.set_irq(0);
        }
        run_main(*board, bus, cpu, cycles);
    };

    for (int f = 0; f < n_frames; ++f) {
        if (board->has_mcu()) {
            const auto t0 = std::chrono::steady_clock::now();
            double main_ms = 0.0, mcu_ms = 0.0;
            int rem = kCyclesPerFrame;
            while (rem > 0) {
                const int chunk = rem > 2000 ? 2000 : rem;
                const auto a = std::chrono::steady_clock::now();
                run_main_mcu(chunk);
                const auto b = std::chrono::steady_clock::now();
                rem -= chunk;
                board->mcu_execute(chunk / 15);
                const auto c = std::chrono::steady_clock::now();
                main_ms += std::chrono::duration<double, std::milli>(b - a).count();
                mcu_ms += std::chrono::duration<double, std::milli>(c - b).count();
            }
            if (std::getenv("SHINOBI_TRACE_TIME"))
                std::fprintf(stderr,
                             "frame %d: main=%.1fms mcu=%.1fms pc=%06x "
                             "hold=%d irq=%d\n", f, main_ms, mcu_ms,
                             cpu.program_counter(),
                             board->cpu_reset_hold_ ? 1 : 0,
                             board->mapper_irq_pending_);
            (void)t0;
        } else {
            run_main(*board, bus, cpu, kCyclesPerFrame);
        }
        {
            static bool crash_reported = false;
            const uint32_t pc_now = cpu.program_counter();
            if (!crash_reported && pc_now >= 0x400 && pc_now <= 0x406 &&
                board->byte_read(pc_now) == 0x60 &&
                board->byte_read(pc_now + 1) == 0xfe) {
                crash_reported = true;
                const uint32_t sp = cpu.address_register(7);
                std::printf("CRASH: main 68000 in exception catcher "
                            "pc=%06x at frame %d sp=%08x frame: "
                            "sr=%04x pc=%04x%04x next=%04x\n",
                            pc_now, f, sp,
                            board->cpu_read_16(sp),
                            board->cpu_read_16(sp + 2),
                            board->cpu_read_16(sp + 4),
                            board->cpu_read_16(sp + 6));
                std::printf("CRASH regs: d0=%08x d1=%08x d2=%08x "
                            "d3=%08x a0=%08x a1=%08x\n",
                            cpu.data_register(0), cpu.data_register(1),
                            cpu.data_register(2), cpu.data_register(3),
                            cpu.address_register(0),
                            cpu.address_register(1));
                std::printf("CRASH ring:");
                for (unsigned i = 0; i < 64; ++i)
                    std::printf(" %06x",
                                g_pc_ring[(g_pc_ring_pos + i) & 63]);
                std::printf("\n");
            }
        }
        if (std::getenv("SHINOBI_DUMP_FLAGS") && (f + 1) % 200 == 0) {
            std::printf("F%04d pc=%06x ", f + 1, cpu.program_counter());
            for (uint32_t a = 0xffe70c; a <= 0xffe74f; ++a)
                std::printf("%02x ", board->byte_read(a));
            std::printf("\n");
        }
        if (board->have_sound_rom_) {
            if (std::getenv("SHINOBI_TRACE_Z80") && (f % 30) == 0)
                std::fprintf(stderr, "z80 f=%d pc=%04x sp=%04x iff1=%d "
                             "im=%d irq_pending=%d status_calls=%llu "
                             "status=%02x\n",
                             f, z80.pc, z80.sp, z80.iff1, z80.im,
                             board->sound_irq_pending_ ? 1 : 0,
                             static_cast<unsigned long long>(status_calls),
                             last_status);
            for (int c = 0; c < system16b::kSoundCyclesPerFrame; ++c) {
                if (board->sound_irq_pending_) z80_pins |= Z80_INT;
                z80_pins = z80_tick(&z80, z80_pins);
                const uint16_t a = Z80_GET_ADDR(z80_pins);
                if (z80_pins & Z80_MREQ) {
                    if (z80_pins & Z80_RD) {
                        Z80_SET_DATA(z80_pins, board->co_cpu_read(a));
                    } else if (z80_pins & Z80_WR) {
                        board->co_cpu_write(a, Z80_GET_DATA(z80_pins));
                    }
                } else if (z80_pins & Z80_IORQ) {
                    const unsigned port = a & 0xFFu;
                    if (z80_pins & Z80_RD) {
                        Z80_SET_DATA(z80_pins,
                                     board->co_cpu_port_read(port));
                    } else if (z80_pins & Z80_WR) {
                        const uint8_t data = Z80_GET_DATA(z80_pins);
                        board->co_cpu_port_write(port, data);
                        if (port == 0x00u || port == 0x01u ||
                            port == 0x80u)
                            synth.write_control(port, data);
                        static uint8_t ym_addr = 0;
                        if ((port & 0xC0u) == 0x00u) {
                            if ((port & 1u) == 0) {
                                ym_addr = data;
                            } else if (std::getenv("SHINOBI_TRACE_YM") &&
                                       ym_addr >= 0x10 && ym_addr <= 0x14) {
                                std::fprintf(stderr,
                                             "ym reg %02x = %02x (f=%d)\n",
                                             ym_addr, data, f);
                            }
                        }
                    }
                }
                // 4 MHz YM clock against the 5 MHz Z80, as the machine
                // runtime paces it.
                ym_clock_fraction += 4'000'000u;
                if (ym_clock_fraction >= 5'000'000u * 512u) {
                    synth.advance_timer_clocks(static_cast<uint32_t>(
                        ym_clock_fraction / 5'000'000u));
                    ym_clock_fraction %= 5'000'000u;
                }
            }
            z80_pins &= ~static_cast<uint64_t>(Z80_INT);
        }
        // Render before vblank so the frame uses the register values the
        // game scanned out with, not the next-frame values the vblank
        // handler copies in (see shinobi_machine.cpp).
        if (f >= n_frames - 6)
            std::printf("  frame %d pre-vblank regs: pages=%04x/%04x xs=%04x/%04x ys=%04x/%04x\n",
                        f,
                        board->cpu_read_16(0x410e80), board->cpu_read_16(0x410e82),
                        board->cpu_read_16(0x410e98), board->cpu_read_16(0x410e9a),
                        board->cpu_read_16(0x410e90), board->cpu_read_16(0x410e92));
        if (f == n_frames - 1)
            board->render_frame(const_cast<uint32_t*>(board->frame_buffer().data()));
        // VBLANK: assert level-4 and give the handler enough cycles to
        // run to its RTE -- exception entry alone costs ~44 cycles, so a
        // token slice would leave the handler body (including the game's
        // vblank bookkeeping) permanently unexecuted.
        if (board->has_mcu()) {
            // The vblank line lands on the MCU's INT0; the 68000 only
            // hears about it if the MCU relays it via mapper register 4.
            board->mcu_vblank(true);
            board->mcu_execute(500);
            board->mcu_vblank(false);
            for (int slice = 0; slice < 4; ++slice) {
                run_main_mcu(1000);
                board->mcu_execute(70);
            }
        } else {
            cpu.set_irq(4);
            run_main(*board, bus, cpu, 4000);
            cpu.set_irq(0);
        }
        const uint8_t vblank = board->work_ram()[kVblankCounterOff];
        if (vblank != last_vblank) {
            boot_counter_advanced = true;
            last_vblank = vblank;
        }
        const uint32_t pc = cpu.program_counter();
        static bool dumped_crash = false;
        if (!dumped_crash && g_hit_ram_exec) {
            dumped_crash = true;
            std::printf("shinobi: exception handler entered, SR=%04x recent PCs:",
                        cpu.status_register());
            for (int i = 0; i < 64; ++i)
                std::printf(" %06x",
                            g_pc_ring[(g_pc_ring_pos + i) & 63]);
            std::printf("\nshinobi: work_ram[ef00..ef7f]:");
            for (int i = 0xef00; i < 0xef80; ++i)
                std::printf(" %02x", board->work_ram()[i]);
            std::printf("\n");
        }
        if (pc == last_pc) {
            ++unchanged_pc_frames;
        } else {
            unchanged_pc_frames = 0;
            last_pc = pc;
        }
        if (!boot_counter_advanced && unchanged_pc_frames == 60) {
            std::printf("shinobi: boot stalled at PC=0x%06x after %d frames\n",
                        pc, f);
            std::printf("shinobi: recent PCs:");
            for (int i = 0; i < 64; ++i)
                std::printf(" %06x",
                            g_pc_ring[(g_pc_ring_pos + i) & 63]);
            std::printf("\n");
            if (std::getenv("SHINOBI_NO_STALL_BREAK")) continue;
            break;
        }
    }
    if (boot_counter_advanced) {
        std::printf("shinobi: boot completed; final PC=0x%06x\n",
                    cpu.program_counter());
    } else if (unchanged_pc_frames < 60u) {
        std::printf("shinobi: M68K still progressing; final PC=0x%06x\n",
                    cpu.program_counter());
    }

    // (Frame was rendered before the final vblank inside the loop.)
    if (std::getenv("SHINOBI_NOSPRITES")) {
        board->sprite_ram().fill(0);
        board->render_frame(const_cast<uint32_t*>(board->frame_buffer().data()));
    }

    std::ofstream out("/tmp/system16b_frame.rgba", std::ios::binary);
    if (!out) {
        std::fprintf(stderr, "shinobi: cannot write /tmp/system16b_frame.rgba\n");
        return 3;
    }
    out.write(reinterpret_cast<const char*>(board->frame_buffer().data()),
              static_cast<std::streamsize>(board->frame_buffer().size() * 4));
    out.close();
    {
        std::printf("shinobi: mapper regs:");
        for (unsigned r = 0; r < 0x20; ++r)
            std::printf(" %02x", board->mapper_regs_[r]);
        std::printf("\n");
        std::printf("shinobi: text regs:");
        for (int i = 0xe80; i < 0xea0; i += 2)
            std::printf(" %04x", board->cpu_read_16(0x410000 + i));
        std::printf("\nshinobi: tile page fill:");
        for (int p = 0; p < 16; ++p) {
            int nz = 0;
            for (int i = 0; i < 0x1000; ++i)
                if (board->byte_read(0x400000 + p * 0x1000 + i)) ++nz;
            std::printf(" %x:%d", p, nz);
        }
        std::printf("\n");
    }
    {
        std::ofstream t("/tmp/ours_textram.bin", std::ios::binary);
        for (int a = 0; a < 0x1000; ++a)
            t.put(static_cast<char>(board->byte_read(0x410000 + a)));
    }
    {
        std::ofstream t("/tmp/ours_tileram.bin", std::ios::binary);
        for (int a = 0; a < 0x10000; ++a)
            t.put(static_cast<char>(board->byte_read(0x400000 + a)));
    }
    {
        std::ofstream spr("/tmp/system16b_sprram.bin", std::ios::binary);
        spr.write(reinterpret_cast<const char*>(board->sprite_ram().data()),
                  board->sprite_ram().size());
    }

    // Histogram one-liner for plane0-style validation: nonzero R/G/B count.
    int nz = 0, total = 0;
    for (auto px : board->frame_buffer()) {
        if ((px >> 24) & 0xff) { nz++; }
        total++;
    }
    std::printf("shinobi: frames=%d PC=%08x framebuffer=%dx%d nonzero=%d/%d\n",
                n_frames,
                cpu.program_counter(),
                system16b::kScreenW, system16b::kScreenH, nz, total);
    std::printf("wrote /tmp/system16b_frame.rgba\n");
    return 0;
}
