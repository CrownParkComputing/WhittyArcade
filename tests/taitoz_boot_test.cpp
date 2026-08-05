// Taito Z System boot/frame test.
//
// Boots Continental Circus against a real archive, runs N frames, and
// dumps the rendered RGBA frame to /tmp/taitoz_frame.rgba plus the video
// RAM to /tmp/taitoz_scn.bin so both can be diffed against MAME.
//
// Usage: taitoz_boot_test <rom.zip> [frames]

#include "taito/taitoz/taitoz_audio.h"
#include "taito/taitoz/taitoz_machine.h"
#include "taito/taitoz/taitoz_rom.h"

#include <algorithm>
#include <map>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("taitoz_boot_test: no ROM supplied, nothing to do\n");
        return 0;
    }
    const std::string rom = argv[1];
    const int frames = argc > 2 ? std::atoi(argv[2]) : 60;

    // The sound board is driven here directly rather than through the
    // OpenAL worker, so the test can run headless and still exercise the
    // Z80 -> TC0140SYT -> YM2610 path.
    const taitoz::taitoz_rom_load_result loaded =
        taitoz::taitoz_rom_loader::load(rom);
    if (!loaded) {
        std::fprintf(stderr, "taitoz_boot_test: %s\n", loaded.error.c_str());
        return 1;
    }
    taitoz_sound_synth synth;
    synth.set_adpcm_a(loaded.roms.adpcm_a.data(), loaded.roms.adpcm_a.size());
    synth.set_adpcm_b(loaded.roms.adpcm_b.data(), loaded.roms.adpcm_b.size());

    taitoz::taitoz_machine machine;
    unsigned ym_writes = 0;
    uint8_t last_reg = 0;
    std::map<unsigned, int> reg_hits;
    machine.set_ym_handlers(
        [&](unsigned port, uint8_t data) {
            ++ym_writes;
            // Track which chip registers the driver actually programs:
            // ports 0/2 latch an address, 1/3 write its data.
            if (port == 0 || port == 2) last_reg = data;
            else reg_hits[(port == 1 ? 0x000 : 0x100) | last_reg]++;
            synth.write_port(port, data);
        },
        [&](unsigned port) { return synth.read_port(port); },
        [&](uint32_t clocks) { synth.advance_timer_clocks(clocks); },
        [&] { return synth.irq_asserted(); });
    if (!machine.load_roms(rom)) {
        std::fprintf(stderr, "taitoz_boot_test: could not load %s\n",
                     rom.c_str());
        return 1;
    }

    input_state idle{};
    idle.left_stick_x = 0x80;
    idle.left_stick_y = 0x80;
    machine.set_input(idle);

    constexpr int kAudioFramesPerVideoFrame =
        taitoz_sound_synth::sample_rate / 60;
    std::vector<int16_t> frame_audio(kAudioFramesPerVideoFrame * 2);
    int live_peak = 0;
    std::map<uint32_t, int> pc_hist_a, pc_hist_b, pc_hist_z;
    uint32_t last_a = 0, last_b = 0;
    int stuck_a = 0, stuck_b = 0;
    // Optional scripted credit: hold coin for a few frames around frame
    // 120, then tap start, so the coin/start path is actually exercised.
    const bool do_coin = std::getenv("TAITOZ_COIN") != nullptr;
    for (int f = 0; f < frames; ++f) {
        if (do_coin) {
            input_state in = idle;
            in.left_stick_x = 0x80;
            in.coin1 = (f >= 120 && f < 132);
            in.start = (f >= 200 && f < 212);
            machine.set_input(in);
        }
        machine.board_ptr()->dbg_frame_ = f;
        machine.run_frame();
        {
            // Watch the driver's command queue and ready flag.
            const auto& sr = machine.board_ptr()->sound_work_ram();
            static uint8_t last_wr = 0xaa, last_rd = 0xaa, last_ready = 0xaa;
            if (sr[0xf00] != last_wr || sr[0xf01] != last_rd ||
                sr[0x009] != last_ready) {
                std::printf("QSTATE f=%d wr=%02x rd=%02x ready=%02x "
                            "queue=%02x %02x %02x %02x\n",
                            f, sr[0xf00], sr[0xf01], sr[0x009],
                            sr[0xf02], sr[0xf03], sr[0xf04], sr[0xf05]);
                last_wr = sr[0xf00]; last_rd = sr[0xf01];
                last_ready = sr[0x009];
            }
        }
        // Render this frame's audio, exactly as the OpenAL worker would.
        // Without this the chip never advances, so envelopes and ADPCM
        // playback never move and every measurement reads silence.
        synth.generate(frame_audio.data(), kAudioFramesPerVideoFrame);
        for (int i = 0; i < kAudioFramesPerVideoFrame * 2; ++i)
            live_peak = std::max(live_peak,
                                 std::abs(static_cast<int>(frame_audio[i])));
        const uint32_t pa = machine.main_pc(), pb = machine.sub_pc();
        ++pc_hist_z[machine.sound_pc() & ~0xfu];
        ++pc_hist_a[pa & ~0xfu];
        ++pc_hist_b[pb & ~0xfu];
        stuck_a = (pa == last_a) ? stuck_a + 1 : 0;
        stuck_b = (pb == last_b) ? stuck_b + 1 : 0;
        last_a = pa;
        last_b = pb;
    }

    const uint32_t* fb = machine.frame_buffer();
    std::size_t nonzero = 0;
    for (int i = 0; i < taitoz::kScreenW * taitoz::kScreenH; ++i)
        if ((fb[i] & 0x00ffffffu) != 0) ++nonzero;

    {
        std::ofstream out("/tmp/taitoz_frame.rgba", std::ios::binary);
        out.write(reinterpret_cast<const char*>(fb),
                  static_cast<std::streamsize>(
                      taitoz::kScreenW * taitoz::kScreenH * 4));
    }
    {
        std::ofstream out("/tmp/taitoz_sndram.bin", std::ios::binary);
        const auto& sr = machine.board_ptr()->sound_work_ram();
        out.write(reinterpret_cast<const char*>(sr.data()), sr.size());
    }
    {
        std::ofstream out("/tmp/taitoz_share1.bin", std::ios::binary);
        const auto& sh = machine.board_ptr()->shared_ram();
        out.write(reinterpret_cast<const char*>(sh.data()), sh.size());
    }
    {
        std::ofstream out("/tmp/taitoz_spr.bin", std::ios::binary);
        const auto& sp = machine.board_ptr()->sprite_ram();
        out.write(reinterpret_cast<const char*>(sp.data()), sp.size());
    }
    {
        std::printf("taitoz: scnctl:");
        const auto& c = machine.board_ptr()->scn_ctrl();
        for (unsigned i = 0; i < c.size(); i += 2)
            std::printf(" %04x", (c[i] << 8) | c[i + 1]);
        std::printf("\n");
    }
    {
        // TC0100SCN RAM, for a byte-level comparison against MAME's
        // tc0100scn share.
        const auto& scn = machine.board_ptr()->scn_ram();
        std::ofstream out("/tmp/taitoz_scn.bin", std::ios::binary);
        out.write(reinterpret_cast<const char*>(scn.data()), 0x10000);
    }

    // Render a second of audio and report its peak: a silent board and a
    // playing one are indistinguishable from register writes alone.
    std::vector<int16_t> audio(static_cast<std::size_t>(
        taitoz_sound_synth::sample_rate) * 2);
    synth.generate(audio.data(), taitoz_sound_synth::sample_rate);
    int peak = 0;
    for (int16_t s : audio) peak = std::max(peak, std::abs(static_cast<int>(s)));

    {
        auto top = [](std::map<uint32_t,int>& h, const char* label) {
            std::vector<std::pair<int,uint32_t>> v;
            for (auto& kv : h) v.push_back({kv.second, kv.first});
            std::sort(v.rbegin(), v.rend());
            std::printf("taitoz: %s PC hot spots:", label);
            for (std::size_t i = 0; i < v.size() && i < 5; ++i)
                std::printf(" %06x(%d)", v[i].second, v[i].first);
            std::printf("  distinct=%zu\n", h.size());
        };
        top(pc_hist_a, "CPU-A");
        top(pc_hist_b, "CPU-B");
        top(pc_hist_z, "Z80  ");
    }
    {
        std::printf("taitoz: YM regs written:");
        std::vector<std::pair<int,unsigned>> byhits;
        for (auto& kv : reg_hits) byhits.push_back({kv.second, kv.first});
        std::sort(byhits.rbegin(), byhits.rend());
        for (std::size_t i = 0; i < byhits.size() && i < 12; ++i)
            std::printf(" %03x(%d)", byhits[i].second, byhits[i].first);
        std::printf("  distinct=%zu keyon(0x28)=%d\n", reg_hits.size(),
                    reg_hits.count(0x028) ? reg_hits[0x028] : 0);
    }
    {
        unsigned r=0, ar=0, oob=0;
        synth.adpcm_counters(r, ar, oob);
        std::printf("taitoz: ADPCM fetches total=%u adpcm_a=%u "
                    "out_of_range=%u (rom=%zu bytes)\n",
                    r, ar, oob, loaded.roms.adpcm_a.size());
    }
    {
        unsigned im=0, acks=0; bool iff1=false;
        machine.sound_int_state(im, iff1, acks);
        std::printf("taitoz: Z80 im=%u iff1=%d int_acks=%u\n",
                    im, iff1 ? 1 : 0, acks);
    }
    unsigned sched=0, expd=0, irqs=0;
    synth.debug_counters(sched, expd, irqs);
    {
        const auto& rd = machine.board_ptr()->road_ram();
        std::size_t nz = 0;
        for (uint8_t b : rd) if (b) ++nz;
        std::printf("taitoz: road RAM %zu/%zu non-zero, palbank=%d, "
                    "road_ctrl=%04x\n",
                    nz, rd.size(), machine.board_ptr()->road_palbank(),
                    (rd[0xfff * 2] << 8) | rd[0xfff * 2 + 1]);
        std::printf("taitoz: road line0:");
        for (int i = 0; i < 8; ++i)
            std::printf(" %04x", (rd[i * 2] << 8) | rd[i * 2 + 1]);
        std::printf("\n");
    }
    std::printf("taitoz: sound commands=%u z80 NMIs=%u reset_held=%d\n",
                machine.board_ptr()->syt_commands(),
                machine.board_ptr()->syt_nmis(),
                machine.board_ptr()->sound_reset_held() ? 1 : 0);
    {
        std::printf("taitoz: Z80 SYT submode writes:");
        const auto& h = machine.board_ptr()->syt_submode_hits();
        for (unsigned i = 0; i < h.size(); ++i)
            if (h[i]) std::printf(" %u:%u", i, h[i]);
        std::printf("\n");
    }
    std::printf("taitoz: timers scheduled=%u expired=%u irqs=%u\n",
                sched, expd, irqs);
    std::printf("taitoz: frames=%d CPU-A PC=0x%06x CPU-B PC=0x%06x "
                "Z80 PC=0x%04x nonzero=%zu/%d ym_writes=%u "
                "live_audio_peak=%d tail_peak=%d\n",
                frames, last_a, last_b, machine.sound_pc(), nonzero,
                taitoz::kScreenW * taitoz::kScreenH, ym_writes, live_peak,
                peak);
    if (stuck_a >= frames - 1 && frames > 1)
        std::printf("taitoz: WARNING CPU A never advanced\n");
    if (stuck_b >= frames - 1 && frames > 1)
        std::printf("taitoz: WARNING CPU B never advanced\n");
    return 0;
}
