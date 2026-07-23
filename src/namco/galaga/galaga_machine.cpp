#include "namco/galaga/galaga_machine.h"

extern "C" {
#include "z80.h"
}

#include <algorithm>
#include <array>
#include <utility>
#include <vector>

namespace namco {
namespace {

constexpr uint32_t rgba(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint32_t>(r) |
        (static_cast<uint32_t>(g) << 8) |
        (static_cast<uint32_t>(b) << 16) | 0xff000000u;
}

struct cpu_state {
    z80_t cpu{};
    uint64_t pins{};
    bool irq{};
    bool nmi{};

    void initialize() { pins = z80_init(&cpu); }
    void reset() {
        pins = z80_reset(&cpu);
        irq = nmi = false;
    }
};

class namco51 {
public:
    void reset() {
        mode = arguments = credits = reads = last_coins = last_buttons = 0;
        players = 0;
        coins.fill(0);
        coins_per_credit.fill(1);
        credits_per_coin.fill(1);
        remap = false;
    }

    void set_ports(const std::array<uint8_t, 4>& value, bool test_value) {
        ports = value;
        test = test_value;
    }

    void write(uint8_t data) {
        if (arguments) {
            switch (arguments--) {
            case 4: coins_per_credit[0] = data & 0x0f; break;
            case 3: credits_per_coin[0] = data & 0x0f; break;
            case 2: coins_per_credit[1] = data & 0x0f; break;
            case 1: credits_per_coin[1] = data & 0x0f; break;
            default: break;
            }
            return;
        }
        switch (data & 7) {
        case 1: arguments = 4; credits = 0; break;
        case 2: mode = 1; reads = 0; players = 0; break;
        case 3: remap = false; break;
        case 4: remap = true; break;
        case 5: mode = 0; reads = 0; break;
        default: break;
        }
    }

    uint8_t read() {
        if (mode == 0) {
            const unsigned phase = reads++ % 3;
            if (phase == 0) return ports[0] | (ports[1] << 4);
            if (phase == 1) return ports[2] | (ports[3] << 4);
            return 0;
        }
        const unsigned phase = reads++ % 3;
        if (phase == 0) return credit_status();
        return joystick(phase - 1);
    }

    int credit_count() const { return credits; }
    int selected_players() const { return players; }

private:
    void insert_coin(unsigned slot) {
        if (coins_per_credit[slot] == 0) return;
        if (++coins[slot] >= coins_per_credit[slot]) {
            credits = std::min(99, credits + credits_per_coin[slot]);
            coins[slot] -= coins_per_credit[slot];
        }
    }

    uint8_t credit_status() {
        const uint8_t now =
            static_cast<uint8_t>(~(ports[0] | (ports[1] << 4)));
        const uint8_t edges = now ^ last_coins;
        last_coins = now;
        if (coins_per_credit[0] == 0) {
            credits = 100;
        } else if (credits < 99) {
            if (edges & now & 0x10) insert_coin(0);
            if (edges & now & 0x20) insert_coin(1);
            if (edges & now & 0x40) ++credits;
        }
        if (mode == 1) {
            if ((edges & now & 0x04) && credits >= 1) {
                --credits;
                players = 1;
                mode = 2;
            } else if ((edges & now & 0x08) && credits >= 2) {
                credits -= 2;
                players = 2;
                mode = 2;
            }
        }
        if (test) return 0xbb;
        return static_cast<uint8_t>((credits / 10) * 16 + credits % 10);
    }

    uint8_t joystick(unsigned player) {
        static constexpr std::array<uint8_t, 16> joy_map{
            0xf, 0xe, 0xd, 0x5, 0xc, 0x9, 0x7, 0x6,
            0xb, 0x3, 0xa, 0x4, 0x1, 0x2, 0x0, 0x8};
        uint8_t joy = ports[2 + player] & 0x0f;
        const uint8_t now = static_cast<uint8_t>(~ports[0]) & 0x0f;
        const uint8_t mask = static_cast<uint8_t>(1u << player);
        const uint8_t edge = (now ^ last_buttons) & mask;
        last_buttons = (last_buttons & ~mask) | (now & mask);
        if (remap) joy = joy_map[joy];
        if (edge && (now & mask)) joy |= 0x10;
        if (now & mask) joy |= 0x20;
        return joy;
    }

    std::array<uint8_t, 4> ports{0x0f, 0x0f, 0x0f, 0x0f};
    std::array<int, 2> coins{}, coins_per_credit{1, 1},
        credits_per_coin{1, 1};
    int mode{}, arguments{}, credits{}, reads{}, players{};
    uint8_t last_coins{}, last_buttons{};
    bool remap{}, test{};
};

} // namespace

struct galaga_machine::impl {
    galaga_roms roms;
    std::array<uint8_t, 0x2000> ram{};
    std::array<uint32_t, width * height> frame{};
    std::array<uint32_t, 288 * 224> native{};
    std::array<uint32_t, 32> palette{};
    std::array<uint8_t, 4> input_ports{0x0f, 0x0f, 0x0f, 0x0f};
    std::array<uint8_t, 6> star_control{};
    cpu_state main, sub, sound;
    namco51 io51;
    std::function<void(unsigned, uint8_t)> sound_write;
    uint8_t io_control{};
    uint8_t dip_a{0xff}, dip_b{0xff};
    bool irq_main{}, irq_sub{}, sound_nmi{}, secondary_run{}, flip{}, test{};
    bool network_two_player{};
    bool loaded{};
    unsigned frame_number{};
    uint16_t star_lfsr{0x7fff};

    uint8_t read(cpu_state& which, uint16_t address) {
        if (address < 0x4000) {
            if (&which == &main) return roms.main[address];
            if (&which == &sub)
                return address < roms.sub.size() ? roms.sub[address] : 0xff;
            return address < roms.sound.size() ? roms.sound[address] : 0xff;
        }
        if (address >= 0x6800 && address <= 0x6807) {
            const unsigned bit = address & 7;
            return static_cast<uint8_t>(((dip_a >> bit) & 1) |
                                        (((dip_b >> bit) & 1) << 1));
        }
        if (address >= 0x7000 && address <= 0x70ff) {
            if ((io_control & 0x10) && (io_control & 0x0f) == 1)
                return io51.read();
            return (io_control & 0x10) ? 0xff : 0;
        }
        if (address == 0x7100) return io_control;
        if (address >= 0x8000 && address <= 0x9fff)
            return ram[address - 0x8000];
        return 0xff;
    }

    void write(uint16_t address, uint8_t value) {
        if (address >= 0x6800 && address <= 0x681f) {
            if (sound_write) sound_write(address - 0x6800, value & 0x0f);
            return;
        }
        if (address >= 0x6820 && address <= 0x6827) {
            switch (address & 7) {
            case 0: irq_main = value & 1; break;
            case 1: irq_sub = value & 1; break;
            case 2: sound_nmi = (value & 1) == 0; break;
            case 3: {
                const bool run = value & 1;
                if (run && !secondary_run) {
                    sub.reset();
                    sound.reset();
                    io51.reset();
                }
                secondary_run = run;
                break;
            }
            default: break;
            }
            return;
        }
        if (address >= 0x7000 && address <= 0x70ff) {
            if ((io_control & 0x10) == 0 && (io_control & 0x0f) == 1)
                io51.write(value);
            return;
        }
        if (address == 0x7100) {
            io_control = value;
            return;
        }
        if (address >= 0x8000 && address <= 0x9fff) {
            ram[address - 0x8000] = value;
            return;
        }
        if (address >= 0xa000 && address <= 0xa005) {
            star_control[address - 0xa000] = value & 1;
            return;
        }
        if (address == 0xa007) flip = value & 1;
    }

    void tick(cpu_state& cpu) {
        if (cpu.irq) cpu.pins |= Z80_INT;
        else cpu.pins &= ~static_cast<uint64_t>(Z80_INT);
        if (cpu.nmi) {
            cpu.pins |= Z80_NMI;
            cpu.nmi = false;
        } else {
            cpu.pins &= ~static_cast<uint64_t>(Z80_NMI);
        }
        cpu.pins = z80_tick(&cpu.cpu, cpu.pins);
        if ((cpu.pins & (Z80_IORQ | Z80_M1)) == (Z80_IORQ | Z80_M1)) {
            cpu.irq = false;
            cpu.pins &= ~static_cast<uint64_t>(Z80_INT);
        }
        if ((cpu.pins & Z80_MREQ) == 0) return;
        const uint16_t address = Z80_GET_ADDR(cpu.pins);
        if (cpu.pins & Z80_RD) {
            Z80_SET_DATA(cpu.pins, read(cpu, address));
        } else if (cpu.pins & Z80_WR) {
            write(address, Z80_GET_DATA(cpu.pins));
        }
    }

    void build_palette() {
        for (unsigned i = 0; i < palette.size(); ++i) {
            const uint8_t value = roms.palette[i];
            const int r = ((value >> 0) & 1) * 0x21 +
                          ((value >> 1) & 1) * 0x47 +
                          ((value >> 2) & 1) * 0x97;
            const int g = ((value >> 3) & 1) * 0x21 +
                          ((value >> 4) & 1) * 0x47 +
                          ((value >> 5) & 1) * 0x97;
            const int b = ((value >> 6) & 1) * 0x47 +
                          ((value >> 7) & 1) * 0x97;
            palette[i] = rgba(static_cast<uint8_t>(std::min(r, 255)),
                              static_cast<uint8_t>(std::min(g, 255)),
                              static_cast<uint8_t>(std::min(b, 255)));
        }
    }

    static uint8_t pen(const uint8_t* gfx, std::size_t size,
                       std::size_t base, unsigned offset, unsigned x) {
        if (base + offset >= size) return 0;
        const uint8_t packed = gfx[base + offset];
        const unsigned shift = 3 - (x & 3);
        return static_cast<uint8_t>(((packed >> shift) & 1) |
            (((packed >> (shift + 4)) & 1) << 1));
    }

    void put(int x, int y, uint32_t color) {
        if (flip && !network_two_player) {
            x = 287 - x;
            y = 223 - y;
        }
        if (x >= 0 && x < 288 && y >= 0 && y < 224)
            native[static_cast<std::size_t>(y) * 288 + x] = color;
    }

    void draw_tile(unsigned code, int px, int py, unsigned color) {
        const std::size_t base = (code & 0xff) * 16;
        for (unsigned y = 0; y < 8; ++y) {
            for (unsigned x = 0; x < 8; ++x) {
                const uint8_t p = pen(roms.chars.data(), roms.chars.size(),
                    base, y + (x < 4 ? 8 : 0), x);
                const uint8_t entry =
                    roms.char_lookup[((color & 0x3f) << 2) | p] & 0x0f;
                if (entry != 0x0f) put(px + x, py + y,
                                       palette[0x10 | entry]);
            }
        }
    }

    void draw_sprite(unsigned code, unsigned color, int sx, int sy,
                     unsigned flip_x, unsigned flip_y) {
        const std::size_t base = (code & 0x7f) * 64;
        for (unsigned y = 0; y < 16; ++y) {
            for (unsigned x = 0; x < 16; ++x) {
                const unsigned ex = flip_x ? 15 - x : x;
                const unsigned ey = flip_y ? 15 - y : y;
                const unsigned offset =
                    (ex >> 2) * 8 + (ey & 7) + (ey >> 3) * 32;
                const uint8_t p = pen(roms.sprites.data(),
                                      roms.sprites.size(), base, offset, ex);
                const uint8_t entry =
                    roms.sprite_lookup[((color & 0x3f) << 2) | p] & 0x0f;
                if (entry != 0x0f)
                    put(sx + static_cast<int>(x), sy + static_cast<int>(y),
                        palette[entry]);
            }
        }
    }

    static uint16_t next_star_lfsr(uint16_t value) {
        const uint16_t bit = static_cast<uint16_t>(
            ((value >> 0) ^ (value >> 3) ^ (value >> 5) ^
             (value >> 10)) & 1);
        return static_cast<uint16_t>((value >> 1) | (bit << 15));
    }

    void advance_stars(unsigned count) {
        while (count--) star_lfsr = next_star_lfsr(star_lfsr);
    }

    void draw_stars() {
        // Namco 05XX beam-following star generator. Galaga ties Y-speed to
        // zero; its six latches select X speed, two star sets and STARCLR.
        if (!star_control[5]) {
            star_lfsr = 0x7fff;
            return;
        }
        static constexpr std::array<int, 8> x_offsets{
            0, 1, 2, 3, -4, -3, -2, -1};
        const unsigned speed = star_control[0] |
            (star_control[1] << 1) | (star_control[2] << 2);
        advance_stars(static_cast<unsigned>(22 * 256 + x_offsets[speed]));
        const uint8_t set_a = star_control[3];
        const uint8_t set_b = star_control[4] | 2;
        static constexpr std::array<uint8_t, 4> level{
            0x00, 0x47, 0x97, 0xde};
        for (int y = 0; y < 224; ++y) {
            for (int x = 16; x < 272; ++x) {
                if ((star_lfsr & 0xfa14) == 0x7800) {
                    const uint8_t set = static_cast<uint8_t>(
                        (((star_lfsr >> 10) & 1) << 1) |
                         ((star_lfsr >> 8) & 1));
                    if (set == set_a || set == set_b) {
                        uint8_t color =
                            static_cast<uint8_t>((star_lfsr >> 5) & 7);
                        color |= static_cast<uint8_t>((star_lfsr << 3) & 0x18);
                        color |= static_cast<uint8_t>((star_lfsr << 2) & 0x20);
                        color = static_cast<uint8_t>(~color) & 0x3f;
                        native[static_cast<std::size_t>(y) * 288 + x] =
                            rgba(level[color & 3],
                                 level[(color >> 2) & 3],
                                 level[(color >> 4) & 3]);
                    }
                }
                star_lfsr = next_star_lfsr(star_lfsr);
            }
        }
        advance_stars(10 * 256);
    }

    void render() {
        native.fill(rgba(0, 0, 0));
        draw_stars();
        for (unsigned offset = 0; offset < 0x80; offset += 2) {
            const unsigned code = ram[0xb80 + offset] & 0x7f;
            const unsigned color = ram[0xb81 + offset] & 0x3f;
            const uint8_t flags = ram[0x1b80 + offset];
            const unsigned flip_x = flags & 1;
            const unsigned flip_y = (flags >> 1) & 1;
            const unsigned size_x = (flags >> 2) & 1;
            const unsigned size_y = (flags >> 3) & 1;
            const int sx = ram[0x1381 + offset] - 40 +
                           0x100 * (ram[0x1b81 + offset] & 3);
            int sy = 256 - ram[0x1380 + offset] + 1 -
                     16 * static_cast<int>(size_y);
            sy = (sy & 0xff) - 32;
            for (unsigned y = 0; y <= size_y; ++y)
                for (unsigned x = 0; x <= size_x; ++x) {
                    const unsigned part =
                        ((y ^ (size_y * flip_y)) << 1) |
                        (x ^ (size_x * flip_x));
                    draw_sprite(code + part, color, sx + 16 * x, sy + 16 * y,
                                flip_x, flip_y);
                }
        }
        for (int row = 0; row < 28; ++row) {
            for (int col = 0; col < 36; ++col) {
                const int adjusted_row = row + 2;
                const int adjusted_col = col - 2;
                const unsigned index = static_cast<unsigned>(
                    ((adjusted_col & 0x20) ?
                         adjusted_row + ((adjusted_col & 0x1f) << 5) :
                         adjusted_col + (adjusted_row << 5)) & 0x3ff);
                draw_tile(ram[index], col * 8, row * 8,
                          ram[0x400 + index] & 0x3f);
            }
        }
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
                frame[static_cast<std::size_t>(y) * width + x] =
                    native[static_cast<std::size_t>(223 - x) * 288 + y];
    }
};

galaga_machine::galaga_machine() : m_impl(std::make_unique<impl>()) {
    m_impl->main.initialize();
    m_impl->sub.initialize();
    m_impl->sound.initialize();
}
galaga_machine::~galaga_machine() = default;

bool galaga_machine::initialize(const galaga_roms& roms) {
    if (!roms.valid) return false;
    m_impl->roms = roms;
    m_impl->build_palette();
    m_impl->loaded = true;
    reset();
    return true;
}

void galaga_machine::reset() {
    m_impl->ram.fill(0);
    m_impl->main.reset();
    m_impl->sub.reset();
    m_impl->sound.reset();
    m_impl->io51.reset();
    m_impl->io_control = 0;
    m_impl->irq_main = m_impl->irq_sub = m_impl->sound_nmi = false;
    m_impl->secondary_run = m_impl->flip = false;
    m_impl->frame_number = 0;
    m_impl->star_control.fill(0);
    m_impl->star_lfsr = 0x7fff;
}

void galaga_machine::configure_network_two_player(bool enabled) {
    m_impl->network_two_player = enabled;
    // Galaga selects the P2 joystick/fire port only in cocktail mode. Network
    // play uses that authentic turn gate, but suppresses the physical
    // cocktail monitor rotation because each player has an upright display.
    if (enabled)
        m_impl->dip_a &= static_cast<uint8_t>(~0x80U);
    else
        m_impl->dip_a |= 0x80U;
}

void galaga_machine::set_input(const input_state& input) {
    std::array<uint8_t, 4> ports{0x0f, 0x0f, 0x0f, 0x0f};
    const auto clear = [](uint8_t& port, unsigned bit, bool pressed) {
        if (pressed) port &= static_cast<uint8_t>(~(1u << bit));
    };
    clear(ports[0], 0, input.buttons[0]);
    clear(ports[0], 1, input.p2_buttons[0]);
    clear(ports[0], 2, input.start);
    clear(ports[0], 3, input.p2_start);
    clear(ports[1], 0, input.coin1);
    clear(ports[1], 1, input.coin2);
    clear(ports[1], 2, input.service);
    clear(ports[2], 0, input.left_stick_y < 0x60);
    clear(ports[2], 1, input.left_stick_x > 0x90);
    clear(ports[2], 2, input.left_stick_y > 0x90);
    clear(ports[2], 3, input.left_stick_x < 0x60);
    clear(ports[3], 0, input.p2_stick_y < 0x60);
    clear(ports[3], 1, input.p2_stick_x > 0x90);
    clear(ports[3], 2, input.p2_stick_y > 0x90);
    clear(ports[3], 3, input.p2_stick_x < 0x60);
    m_impl->test = input.test;
    m_impl->input_ports = ports;
    m_impl->io51.set_ports(ports, input.test);
}

void galaga_machine::set_sound_write_handler(
        std::function<void(unsigned, uint8_t)> handler) {
    m_impl->sound_write = std::move(handler);
}

void galaga_machine::run_frame() {
    if (!m_impl->loaded) return;
    constexpr int cycles = static_cast<int>(
        3'072'000LL * 1000 / 60'606);
    constexpr int slices = 8;
    m_impl->main.irq = m_impl->irq_main;
    if (m_impl->secondary_run) m_impl->sub.irq = m_impl->irq_sub;
    for (int slice = 0; slice < slices; ++slice) {
        for (int cycle = 0; cycle < cycles / slices; ++cycle) {
            m_impl->tick(m_impl->main);
            if (m_impl->secondary_run) {
                m_impl->tick(m_impl->sub);
                m_impl->tick(m_impl->sound);
            }
            if ((cycle % 614) == 613 &&
                (m_impl->io_control & 0x0f) != 0)
                m_impl->main.nmi = true;
        }
        if (m_impl->secondary_run && m_impl->sound_nmi &&
            (slice == 2 || slice == 6))
            m_impl->sound.nmi = true;
    }
    ++m_impl->frame_number;
    m_impl->render();
}

const uint32_t* galaga_machine::framebuffer() const {
    return m_impl->frame.data();
}

uint16_t galaga_machine::program_counter() const {
    return m_impl->main.cpu.pc;
}

int galaga_machine::active_player() const {
    // Original Galaga work RAM $9840 is the current-player selector.
    const uint8_t player = m_impl->ram[0x1840];
    return player == 0 ? 1 : player == 1 ? 2 : -1;
}

int galaga_machine::credit_count() const {
    return m_impl->io51.credit_count();
}

int galaga_machine::selected_players() const {
    return m_impl->io51.selected_players();
}

} // namespace namco
