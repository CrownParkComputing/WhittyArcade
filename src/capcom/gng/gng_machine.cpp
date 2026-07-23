#include "capcom/gng/gng_machine.h"
#include "high_scores.h"

extern "C" {
#include "m6809f.h"
}

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

namespace gng {
namespace {

uint8_t decode_pixel(const uint8_t* rom, std::size_t rom_bytes,
                     const uint32_t* planes, int plane_count,
                     const uint32_t* x_offsets, uint32_t y_step,
                     uint32_t char_increment, int code, int x, int y) {
    uint8_t pen = 0;
    for (int plane = 0; plane < plane_count; ++plane) {
        const uint32_t bit = planes[plane] +
            static_cast<uint32_t>(code) * char_increment +
            static_cast<uint32_t>(y) * y_step + x_offsets[x];
        if ((bit >> 3) >= rom_bytes) continue;
        const uint8_t value = (rom[bit >> 3] >> (7 - (bit & 7))) & 1;
        pen |= value << (plane_count - 1 - plane);
    }
    return pen;
}

uint32_t rgba(uint8_t red, uint8_t green, uint8_t blue) {
    return 0xff000000u | red | (static_cast<uint32_t>(green) << 8) |
           (static_cast<uint32_t>(blue) << 16);
}

} // namespace

struct machine::impl {
    roms rom{};
    std::array<uint8_t, 0x10000> bus{};
    std::array<uint8_t, 0x200> sprite_buffer{};
    std::array<uint8_t, 1024 * 64> chars{};
    std::array<uint8_t, 1024 * 256> tiles{};
    std::array<uint8_t, 1024 * 256> sprites{};
    std::array<uint32_t, 256> palette{};
    std::array<uint8_t, width * height> priority{};
    std::vector<uint32_t> frame =
        std::vector<uint32_t>(width * height, 0xff000000);
    M6809F cpu{};
    uint8_t system{0xff}, player1{0xff}, player2{0xff};
    uint8_t dsw1{0xdf}, dsw2{0xfb};
    uint16_t scroll_x{0}, scroll_y{0};
    bool flip{false};
    bool ready{false};
    std::function<void(uint8_t)> sound_command;
    std::function<void(bool)> sound_reset;
    high_score_runtime high_scores{"gng"};

    static void io_write(M6809F* cpu, u16 address, u8 value) {
        static_cast<impl*>(cpu->userdata)->write(address, value);
    }

    void set_bank(uint8_t value) {
        const std::size_t source = value == 4
            ? 0x4000 : 0x10000 + (value & 3) * 0x2000;
        std::copy_n(rom.main.data() + source, 0x2000, bus.data() + 0x4000);
    }

    void write(uint16_t address, uint8_t value) {
        switch (address) {
        case 0x3a00:
            if (sound_command) sound_command(value);
            break;
        case 0x3b08: scroll_x = (scroll_x & 0xff00) | value; break;
        case 0x3b09: scroll_x = (scroll_x & 0x00ff) | (value << 8); break;
        case 0x3b0a: scroll_y = (scroll_y & 0xff00) | value; break;
        case 0x3b0b: scroll_y = (scroll_y & 0x00ff) | (value << 8); break;
        case 0x3c00:
            std::copy_n(bus.data() + 0x1e00, sprite_buffer.size(),
                        sprite_buffer.data());
            break;
        case 0x3d00: flip = !(value & 1); break;
        case 0x3d01:
            if (sound_reset) sound_reset(!(value & 1));
            break;
        case 0x3e00: set_bank(value); break;
        default: break;
        }
    }

    void decode_graphics() {
        constexpr uint32_t char_planes[2]{4, 0};
        constexpr uint32_t char_x[8]{0, 1, 2, 3, 8, 9, 10, 11};
        constexpr uint32_t tile_planes[3]{0x80000, 0x40000, 0};
        constexpr uint32_t tile_x[16]{
            0,1,2,3,4,5,6,7,128,129,130,131,132,133,134,135};
        constexpr uint32_t sprite_planes[4]{0x80000 + 4, 0x80000, 4, 0};
        constexpr uint32_t sprite_x[16]{
            0,1,2,3,8,9,10,11,256,257,258,259,264,265,266,267};
        for (int code = 0; code < 1024; ++code) {
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 8; ++x)
                    chars[code * 64 + y * 8 + x] = decode_pixel(
                        rom.chars.data(), rom.chars.size(), char_planes, 2,
                        char_x, 16, 128, code, x, y);
            for (int y = 0; y < 16; ++y) {
                for (int x = 0; x < 16; ++x) {
                    tiles[code * 256 + y * 16 + x] = decode_pixel(
                        rom.tiles.data(), rom.tiles.size(), tile_planes, 3,
                        tile_x, 8, 256, code, x, y);
                    sprites[code * 256 + y * 16 + x] = decode_pixel(
                        rom.sprites.data(), rom.sprites.size(), sprite_planes, 4,
                        sprite_x, 16, 512, code, x, y);
                }
            }
        }
    }

    void build_palette() {
        for (int index = 0; index < 256; ++index) {
            // GnG uses MAME's split RGBx_444 layout. 0x3800 is the
            // extended/high byte (RRRRGGGG); 0x3900 is the base/low byte
            // (BBBBxxxx). The original Amiga prototype named these halves
            // in the opposite order, which swapped red/blue on desktop.
            const uint8_t high = bus[0x3800 + index];
            const uint8_t low = bus[0x3900 + index];
            const uint8_t red = static_cast<uint8_t>((high >> 4) * 17);
            const uint8_t green = static_cast<uint8_t>((high & 0x0f) * 17);
            const uint8_t blue = static_cast<uint8_t>((low >> 4) * 17);
            palette[index] = rgba(red, green, blue);
        }
    }

    void source_position(int output_x, int output_y,
                         int& screen_x, int& screen_y) const {
        screen_x = flip ? 255 - output_x : output_x;
        const int physical_y = output_y + 16;
        screen_y = flip ? 255 - physical_y : physical_y;
    }

    void draw_background() {
        const uint8_t* ram = bus.data() + 0x2800;
        for (int output_y = 0; output_y < height; ++output_y) {
            for (int output_x = 0; output_x < width; ++output_x) {
                int sx, sy;
                source_position(output_x, output_y, sx, sy);
                const int tile_x = (sx + scroll_x) & 0x1ff;
                const int tile_y = (sy + scroll_y) & 0x1ff;
                const int column = tile_x >> 4;
                const int row = tile_y >> 4;
                const int index = column * 32 + row;
                const uint8_t attr = ram[index + 0x400];
                const int code = ram[index] + ((attr & 0xc0) << 2);
                int px = tile_x & 15;
                int py = tile_y & 15;
                if (attr & 0x10) px = 15 - px;
                if (attr & 0x20) py = 15 - py;
                const uint8_t pen = tiles[code * 256 + py * 16 + px];
                const int output = output_y * width + output_x;
                frame[output] = palette[(attr & 7) * 8 + pen];
                priority[output] = ((attr & 8) && pen != 0 && pen != 6);
            }
        }
    }

    void draw_sprites() {
        for (int offset = 0x1fc; offset >= 0; offset -= 4) {
            const uint8_t attr = sprite_buffer[offset + 1];
            int sx = sprite_buffer[offset + 3] - 0x100 * (attr & 1);
            int sy = sprite_buffer[offset + 2];
            bool flip_x = attr & 4;
            bool flip_y = attr & 8;
            if (flip) {
                sx = 240 - sx;
                sy = 240 - sy;
                flip_x = !flip_x;
                flip_y = !flip_y;
            }
            const int code = sprite_buffer[offset] + ((attr << 2) & 0x300);
            const int color = (attr >> 4) & 3;
            for (int row = 0; row < 16; ++row) {
                const int output_y = sy + row - 16;
                if (output_y < 0 || output_y >= height) continue;
                const int source_y = flip_y ? 15 - row : row;
                for (int column = 0; column < 16; ++column) {
                    const int output_x = sx + column;
                    if (output_x < 0 || output_x >= width) continue;
                    const int source_x = flip_x ? 15 - column : column;
                    const uint8_t pen = sprites[
                        code * 256 + source_y * 16 + source_x];
                    if (pen == 15) continue;
                    frame[output_y * width + output_x] =
                        palette[0x40 + color * 16 + pen];
                }
            }
        }
    }

    void draw_background_front() {
        const uint8_t* ram = bus.data() + 0x2800;
        for (int output_y = 0; output_y < height; ++output_y) {
            for (int output_x = 0; output_x < width; ++output_x) {
                const int output = output_y * width + output_x;
                if (!priority[output]) continue;
                int sx, sy;
                source_position(output_x, output_y, sx, sy);
                const int tile_x = (sx + scroll_x) & 0x1ff;
                const int tile_y = (sy + scroll_y) & 0x1ff;
                const int index = (tile_x >> 4) * 32 + (tile_y >> 4);
                const uint8_t attr = ram[index + 0x400];
                int px = tile_x & 15;
                int py = tile_y & 15;
                if (attr & 0x10) px = 15 - px;
                if (attr & 0x20) py = 15 - py;
                const int code = ram[index] + ((attr & 0xc0) << 2);
                const uint8_t pen = tiles[code * 256 + py * 16 + px];
                frame[output] = palette[(attr & 7) * 8 + pen];
            }
        }
    }

    void draw_foreground() {
        const uint8_t* ram = bus.data() + 0x2000;
        for (int output_y = 0; output_y < height; ++output_y) {
            for (int output_x = 0; output_x < width; ++output_x) {
                int sx, sy;
                source_position(output_x, output_y, sx, sy);
                const int column = sx >> 3;
                const int row = sy >> 3;
                const int index = row * 32 + column;
                const uint8_t attr = ram[index + 0x400];
                int px = sx & 7;
                int py = sy & 7;
                if (attr & 0x10) px = 7 - px;
                if (attr & 0x20) py = 7 - py;
                const int code = ram[index] + ((attr & 0xc0) << 2);
                const uint8_t pen = chars[code * 64 + py * 8 + px];
                if (pen == 3) continue;
                frame[output_y * width + output_x] =
                    palette[0x80 + (attr & 0x0f) * 4 + pen];
            }
        }
    }

    void render() {
        build_palette();
        draw_background();
        draw_sprites();
        draw_background_front();
        draw_foreground();
    }
};

machine::machine() : m(std::make_unique<impl>()) {}
machine::~machine() {
    if (m && m->ready) {
        m->high_scores.flush([this](std::uint32_t address) {
            return m->bus[static_cast<std::uint16_t>(address)];
        });
    }
}

bool machine::initialize(const roms& rom_data) {
    if (!rom_data.complete()) return false;
    m->rom = rom_data;
    m->decode_graphics();
    m->ready = true;
    reset();
    return true;
}

void machine::reset() {
    if (!m->ready) return;
    std::fill(m->bus.begin(), m->bus.end(), 0);
    std::fill(m->sprite_buffer.begin(), m->sprite_buffer.end(), 0);
    for (int index = 0; index < 256; ++index) {
        const uint8_t gray = static_cast<uint8_t>((index & 3) * 0x55);
        m->bus[0x3800 + index] = gray;
        m->bus[0x3900 + index] = gray;
    }
    std::copy_n(m->rom.main.data() + 0x6000, 0x2000,
                m->bus.data() + 0x6000);
    std::copy_n(m->rom.main.data() + 0x8000, 0x8000,
                m->bus.data() + 0x8000);
    m->set_bank(4);
    m->scroll_x = m->scroll_y = 0;
    m->flip = false;
    m->cpu = {};
    m->cpu.mem = m->bus.data();
    m->cpu.userdata = m.get();
    m->cpu.writable_limit = 0x4000;
    m->cpu.iowrite = &impl::io_write;
    m6809f_reset(&m->cpu);
    m->high_scores.reset();
}

void machine::set_inputs(uint8_t system, uint8_t player1, uint8_t player2,
                         uint8_t dsw1, uint8_t dsw2) {
    m->system = system;
    m->player1 = player1;
    m->player2 = player2;
    m->dsw1 = dsw1;
    m->dsw2 = dsw2;
}

void machine::set_sound_callbacks(std::function<void(uint8_t)> command,
                                  std::function<void(bool)> reset_line) {
    m->sound_command = std::move(command);
    m->sound_reset = std::move(reset_line);
}

void machine::run_frame() {
    if (!m->ready || m->cpu.stop) return;
    m->bus[0x3000] = m->system;
    m->bus[0x3001] = m->player1;
    m->bus[0x3002] = m->player2;
    m->bus[0x3003] = m->dsw1;
    m->bus[0x3004] = m->dsw2;
    const uint32_t start = m->cpu.cycles;
    m->cpu.irq = 1;
    while (static_cast<uint32_t>(m->cpu.cycles - start) < cycles_per_frame &&
           !m->cpu.stop) {
        m6809f_irq(&m->cpu);
        m6809f_step(&m->cpu);
    }
    m->high_scores.update(
        [this](std::uint32_t address) {
            return m->bus[static_cast<std::uint16_t>(address)];
        },
        [this](std::uint32_t address, std::uint8_t value) {
            m->bus[static_cast<std::uint16_t>(address)] = value;
        });
    m->render();
}

const std::vector<uint32_t>& machine::framebuffer() const noexcept {
    return m->frame;
}
uint16_t machine::program_counter() const noexcept { return m->cpu.pc; }
uint8_t machine::stopped() const noexcept { return m->cpu.stop; }

} // namespace gng
