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
#include "system16b_machine.h"
#include "system16b_rom.h"
#include "galaxian_machine.h"
#include "m68000_cpu.h"

#include <cstdio>
#include <cassert>
#include <cstdlib>
#include <cstring>
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
    (void)board;
    while (cycles > 0 && !g_hit_ram_exec) {
        const int chunk = cycles > 8 ? 8 : cycles;
        const uint32_t pc = cpu.program_counter();
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
        // WhittyArcade's cabinet profile is English with attract audio on;
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
    for (int f = 0; f < n_frames; ++f) {
        run_main(*board, bus, cpu, kCyclesPerFrame);
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
        cpu.set_irq(4);
        run_main(*board, bus, cpu, 4000);
        cpu.set_irq(0);
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
