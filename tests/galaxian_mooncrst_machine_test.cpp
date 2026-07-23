// galaxian_mooncrst_machine_test - native board integration test for
// Moon Cresta. Drives the shared galaxian_machine constructed with the
// mooncrst board_interface through a full frame loop, asserts the
// screen dimensions and refresh rate, and (with a real archive) that
// the produced frames are unique and contain a meaningful number of
// distinct colors and visible pixels.

#include "namco/galaxian/galaxian_machine.h"
#include "namco/galaxian/galaxian_audio.h"

#include <algorithm>
#include <cassert>
#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <set>
#include <unordered_set>
#include <vector>

std::unique_ptr<galaxian_board_interface>
make_mooncrst_board_interface();

namespace {
// Minimal board/ROM proving that the shared Z80 runner releases NMI between
// frames. The handler at 0x0066 increments one byte and returns; a stuck NMI
// pin only increments it once, while a correct pulse increments every frame.
class nmi_regression_board final : public galaxian_board_interface {
public:
    nmi_regression_board() {
        memory.fill(0x00);                      // NOP-filled ROM/RAM
        memory[0x0000] = 0x31;                  // LD SP,$8fff
        memory[0x0001] = 0xff;
        memory[0x0002] = 0x8f;
        memory[0x0003] = 0xc3;                  // JP $0003
        memory[0x0004] = 0x03;
        memory[0x0005] = 0x00;
        memory[0x0066] = 0x3a;                  // LD A,($9000)
        memory[0x0067] = 0x00;
        memory[0x0068] = 0x90;
        memory[0x0069] = 0x3c;                  // INC A
        memory[0x006a] = 0x32;                  // LD ($9000),A
        memory[0x006b] = 0x00;
        memory[0x006c] = 0x90;
        memory[0x006d] = 0xed;                  // RETN
        memory[0x006e] = 0x45;
    }

    void configure(galaxian_machine* machine) override {
        machine->set_dimensions(1, 1, 60.0, 256);
    }
    uint8_t cpu_read(uint16_t address, int) override {
        return memory[address];
    }
    void cpu_write(uint16_t address, uint8_t data) override {
        memory[address] = data;
    }
    void on_frame_start() override { pulse = true; }
    bool wants_nmi() override {
        const bool result = pulse;
        pulse = false;
        return result;
    }
    void render_frame(uint32_t* rgba) override { rgba[0] = 0xff000000u; }
    bool load_roms_into(unzFile, std::string*) override { return true; }
    bool load_roms_from_dir(const std::string&, std::string*) override {
        return true;
    }
    void apply_input(const input_state&, uint8_t*, std::size_t) override {}

    std::array<uint8_t, 0x10000> memory{};
    bool pulse{};
};
}  // namespace

int main(int argc, char** argv) {
    {
        auto board = std::make_unique<nmi_regression_board>();
        nmi_regression_board* observed = board.get();
        galaxian_machine machine(std::move(board));
        assert(machine.load_roms("."));
        machine.reset();
        for (int frame = 0; frame < 8; ++frame) machine.run_frame();
        assert(observed->memory[0x9000] >= 6);
    }
    {
        auto board = make_mooncrst_board_interface();
        input_state input;
        input.coin1 = true;
        input.coin2 = true;
        input.start = true;
        input.p2_start = true;
        input.left_stick_x = 0x00;
        input.buttons[0] = true;
        input.p2_stick_x = 0xff;
        input.p2_buttons[0] = true;
        std::array<uint8_t, 8> ports{};
        board->apply_input(input, ports.data(), ports.size());
        const uint8_t in0 = board->cpu_read(0xa000, 0);
        const uint8_t in1 = board->cpu_read(0xa800, 0);
        assert((in0 & 0x01) != 0);
        assert((in0 & 0x02) != 0);
        assert((in0 & 0x04) != 0);
        assert((in0 & 0x08) == 0);
        assert((in0 & 0x10) != 0);
        assert((in0 & 0x40) == 0);
        assert((in1 & 0x01) != 0);
        assert((in1 & 0x02) != 0);
        assert((in1 & 0x04) == 0);
        assert((in1 & 0x08) != 0);
        assert((in1 & 0x10) != 0);
        assert(in0 == ports[0]);
        assert(in1 == ports[1]);

        input = input_state{};
        input.left_stick_x = 0xff;
        input.p2_stick_x = 0x00;
        input.test = true;
        board->apply_input(input, ports.data(), ports.size());
        assert((board->cpu_read(0xa000, 0) & 0x08) != 0);
        assert((board->cpu_read(0xa000, 0) & 0x40) == 0);
        assert((board->cpu_read(0xa800, 0) & 0x04) != 0);
    }
    auto machine = std::make_unique<galaxian_machine>(
        make_mooncrst_board_interface());

    assert(machine->screen_width() == 224);
    assert(machine->screen_height() == 256);
    assert(machine->refresh_rate() > 60.0);
    assert(machine->refresh_rate() < 61.0);

    if (argc < 2) {
        std::puts("Moon Cresta board constants passed (no ROM integration path)");
        return 0;
    }

    assert(machine->load_roms(argv[1]));
    auto sound = make_mooncrst_sound_synth();
    sound->reset();
    std::size_t sound_line_writes = 0;
    std::size_t pitch_writes = 0;
    std::size_t lfo_writes = 0;
    machine->set_sound_write_handler(
        [&](unsigned port, uint8_t data) {
            sound->write_control(port, data);
            if ((port & ~0x07u) == mooncrst_audio_port::sound_base)
                ++sound_line_writes;
            else if (port == mooncrst_audio_port::pitch)
                ++pitch_writes;
            else if ((port & ~0x03u) == mooncrst_audio_port::lfo_base)
                ++lfo_writes;
        });
    machine->reset();
    std::set<std::size_t> unique_frames;
    std::array<int16_t, 792> audio_samples{};
    int rom_audio_peak = 0;
    std::size_t audible_audio_buffers = 0;
    for (int frame = 0; frame < 1200; ++frame) {
        // Exercise the real program's coin/start path so sound verification
        // observes gameplay latch writes rather than only power-on defaults.
        input_state cabinet_input;
        cabinet_input.coin1 = frame >= 300 && frame < 304;
        cabinet_input.start = frame >= 360 && frame < 364;
        machine->set_input(cabinet_input);
        machine->run_frame();
        sound->generate(audio_samples.data(),
                        static_cast<int>(audio_samples.size()), 100, 100);
        int buffer_peak = 0;
        for (const int16_t sample : audio_samples) {
            const int magnitude = sample < 0 ? -static_cast<int>(sample) :
                                               static_cast<int>(sample);
            buffer_peak = std::max(buffer_peak, magnitude);
        }
        rom_audio_peak = std::max(rom_audio_peak, buffer_peak);
        if (buffer_peak > 100) ++audible_audio_buffers;
        const uint32_t* fb = machine->frame_buffer();
        std::size_t hash = 0;
        for (int i = 0; i < machine->screen_width() * machine->screen_height();
             ++i)
            hash += fb[i];
        unique_frames.insert(hash);
    }
    std::printf("Moon Cresta: %zu unique frames out of 1200\n",
                unique_frames.size());
    std::printf("Moon Cresta ROM sound writes: lines=%zu pitch=%zu lfo=%zu\n",
                sound_line_writes, pitch_writes, lfo_writes);
    std::printf("Moon Cresta ROM-driven audio: peak=%d audible=%zu buffers\n",
                rom_audio_peak, audible_audio_buffers);

    const uint32_t* pixels = machine->frame_buffer();
    std::unordered_set<uint32_t> colors;
    std::size_t visible_pixels = 0;
    for (int index = 0;
         index < machine->screen_width() * machine->screen_height();
         ++index) {
        colors.insert(pixels[index]);
        if (pixels[index] != 0xff000000u) ++visible_pixels;
    }
    std::printf("Moon Cresta frame: %zu colors, %zu non-black pixels\n",
                colors.size(), visible_pixels);
    assert(colors.size() >= 7);
    assert(visible_pixels > 1000);
    assert(unique_frames.size() > 100);
    assert(sound_line_writes > 0);
    assert(pitch_writes > 0);
    assert(lfo_writes > 0);
    assert(rom_audio_peak > 1000);
    assert(audible_audio_buffers > 10);
    return 0;
}
