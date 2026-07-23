// galaxian_machine_mooncrst.cpp - discrete-sound Galaxian board profiles.
//
// Ported from the Kotlin reference in AndroidStudioProjects/zarcade
// machine-galaxian (GalaxianBaseBoard, MoonCrestaBoard, PiscesBoard,
// GalaxianVideo and GalaxianRoms). Video is the shared 32x32 tilemap + 8
// sprites + 17-bit LFSR starfield rotated ROT90. Board profiles contain Moon
// Cresta's alternate map/decryption and UniWar S's Pisces graphics bank at
// 0x6002. The CPU remains the shared native chips/z80.h implementation.

#include "namco/galaxian/galaxian_machine.h"
#include "namco/galaxian/galaxian_audio.h"
#include "high_scores.h"

#include "z80.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <minizip/unzip.h>

namespace {

constexpr std::array<const char*, 5> kGalaxianProgramRoms = {
    "galmidw.u", "galmidw.v", "galmidw.w", "galmidw.y", "7l"};
constexpr std::array<const char*, 2> kGalaxianCharRoms = {
    "1h.bin", "1k.bin"};
constexpr char kGalaxianPaletteRom[] = "6l.bpr";

constexpr std::array<const char*, 8> kMooncrstProgramRoms = {
    "mc1", "mc2", "mc3", "mc4", "mc5.7r", "mc6.8d", "mc7.8e", "mc8"};
constexpr std::array<const char*, 4> kMooncrstCharRoms = {
    "mcs_b", "mcs_d", "mcs_a", "mcs_c"};
constexpr char kMooncrstPaletteRom[] = "mmi6331.6l";

constexpr std::array<const char*, 8> kUniwarsProgramRoms = {
    "f07_1a.bin", "h07_2a.bin", "k07_3a.bin", "m07_4a.bin",
    "d08p_5a.bin", "gg6", "m08p_7a.bin", "n08p_8a.bin"};
constexpr std::array<const char*, 4> kUniwarsCharRoms = {
    "egg10", "h01_2.bin", "egg9", "k01_2.bin"};
constexpr char kUniwarsPaletteRom[] = "uniwars.clr";

enum class galaxian_discrete_profile : uint8_t {
    galaxian,
    mooncrst,
    uniwars,
};

const char* profile_name(galaxian_discrete_profile profile) {
    switch (profile) {
    case galaxian_discrete_profile::galaxian: return "Galaxian";
    case galaxian_discrete_profile::mooncrst: return "Moon Cresta";
    case galaxian_discrete_profile::uniwars: return "UniWar S";
    }
    return "Galaxian-family game";
}

const char* profile_short_name(galaxian_discrete_profile profile) {
    switch (profile) {
    case galaxian_discrete_profile::galaxian: return "galaxian";
    case galaxian_discrete_profile::mooncrst: return "mooncrst";
    case galaxian_discrete_profile::uniwars: return "uniwars";
    }
    return "";
}

bool load_zip_entry(unzFile archive, const char* name,
                    std::vector<uint8_t>& bytes) {
    if (unzLocateFile(archive, name, 0) != UNZ_OK) return false;
    unz_file_info info{};
    if (unzGetCurrentFileInfo(archive, &info, nullptr, 0, nullptr, 0,
                              nullptr, 0) != UNZ_OK ||
        unzOpenCurrentFile(archive) != UNZ_OK)
        return false;
    bytes.resize(info.uncompressed_size);
    const int read = unzReadCurrentFile(
        archive, bytes.data(), static_cast<unsigned>(bytes.size()));
    unzCloseCurrentFile(archive);
    return read == static_cast<int>(bytes.size());
}

bool load_dir_entry(const std::string& dir, const char* name,
                    std::vector<uint8_t>& bytes) {
    namespace fs = std::filesystem;
    fs::path p = fs::path(dir) / name;
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) return false;
    FILE* f = std::fopen(p.string().c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size < 0) {
        std::fclose(f);
        return false;
    }
    bytes.resize(static_cast<std::size_t>(size));
    const std::size_t got = std::fread(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    return got == bytes.size();
}

uint32_t pack_rgba(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint32_t>(r) |
           (static_cast<uint32_t>(g) << 8) |
           (static_cast<uint32_t>(b) << 16) | 0xff000000u;
}

uint8_t bitswap(uint8_t v, int b7, int b2, int b5, int b4, int b3, int b6,
                int b1, int b0) {
    auto bit = [&](int b) -> uint8_t {
        return static_cast<uint8_t>((v >> b) & 1);
    };
    return static_cast<uint8_t>((bit(b7) << 7) | (bit(b2) << 6) |
                                (bit(b5) << 5) | (bit(b4) << 4) |
                                (bit(b3) << 3) | (bit(b6) << 2) |
                                (bit(b1) << 1) | bit(b0));
}

// Moon Cresta program ROMs are protected with a bit-shuffle + two XOR
// patches. Ported from the Kotlin reference.
template <typename ByteRange>
void decode_mooncrst(ByteRange& program) {
    for (std::size_t offs = 0; offs < program.size(); ++offs) {
        uint8_t data = program[offs];
        uint8_t res = data;
        if (((data >> 1) & 1) != 0) res ^= 0x40;
        if (((data >> 5) & 1) != 0) res ^= 0x04;
        if ((offs & 1) == 0) res = bitswap(res, 7, 2, 5, 4, 3, 6, 1, 0);
        program[offs] = res;
    }
}

constexpr int kNative = 256;
constexpr int kVisibleY0 = 16;
constexpr int kVisibleY1 = 240;
constexpr uint32_t kBlack = 0xff000000u;
constexpr int kBulletsBase = 0x60;
constexpr int kPlayerMissileSlot = 7;
constexpr int kStarRngPeriod = (1 << 17) - 1;
constexpr int kStarsScrollPerFrame = 1;
constexpr int kScreenWidth = 224;
constexpr int kScreenHeight = 256;
constexpr double kRefreshHz = 60.606;
constexpr int kCpuHz = 3'072'000;
constexpr int kClocksPerFrame = kCpuHz / static_cast<int>(kRefreshHz);

}  // namespace

class galaxian_discrete_board_interface final :
        public galaxian_board_interface {
public:
    explicit galaxian_discrete_board_interface(
            galaxian_discrete_profile profile)
        : m_profile(profile),
          high_scores(profile_short_name(profile)) {
        if (m_profile == galaxian_discrete_profile::uniwars) {
            dip1 = 0x00;
            dip2 = 0x01;
        } else if (m_profile == galaxian_discrete_profile::galaxian) {
            dip1 = 0x00;
            dip2 = 0x04;
        }
    }

    void configure(galaxian_machine* self) override {
        m_machine = self;
        self->set_dimensions(kScreenWidth, kScreenHeight, kRefreshHz,
                             kClocksPerFrame);
        program.assign(0x4000, 0xff);
        build_star_data();
        build_star_colors();
    }

    uint8_t cpu_read(uint16_t address, int /*frame_clock*/) override {
        const uint16_t a = address & 0xffff;
        if (a < 0x4000) return program[a];
        if (m_profile != galaxian_discrete_profile::mooncrst) {
            if (a >= 0x4000 && a < 0x4800)
                return ram[(a - 0x4000) & 0x03ff];
            if (a >= 0x5000 && a < 0x5800)
                return vram[(a - 0x5000) & 0x03ff];
            if (a >= 0x5800 && a < 0x6000)
                return objram[(a - 0x5800) & 0x00ff];
            if (a >= 0x6000 && a < 0x6800) return in0;
            if (a >= 0x6800 && a < 0x7000) return in1;
            if (a >= 0x7000 && a < 0x7800) return dsw;
        } else {
            if (a >= 0x8000 && a < 0x8800)
                return ram[(a - 0x8000) & 0x03ff];
            if (a >= 0x9000 && a < 0x9800)
                return vram[(a - 0x9000) & 0x03ff];
            if (a >= 0x9800 && a < 0xa000)
                return objram[(a - 0x9800) & 0x00ff];
            if (a >= 0xa000 && a < 0xa800) return in0;
            if (a >= 0xa800 && a < 0xb000) return in1;
            if (a >= 0xb000 && a < 0xb800) return dsw;
        }
        return 0xff;
    }

    void cpu_write(uint16_t address, uint8_t data) override {
        const uint16_t a = address & 0xffff;
        const uint8_t v = data & 0xff;
        if (m_profile != galaxian_discrete_profile::mooncrst) {
            cpu_write_galaxian_map(a, v);
            return;
        }
        cpu_write_mooncrst(a, v);
    }

private:
    void cpu_write_mooncrst(uint16_t a, uint8_t v) {
        if (a >= 0x8000 && a < 0x8800) {
            ram[(a - 0x8000) & 0x03ff] = v;
        } else if (a >= 0x9000 && a < 0x9800) {
            vram[a - 0x9000] = v;
        } else if (a >= 0x9800 && a < 0xa000) {
            objram[a - 0x9800] = v;
        } else if (a >= 0xa000 && a < 0xa800) {
            const int off = a & 0x07;
            if (off <= 2) {
                if (off >= 0 && off < static_cast<int>(gfx_bank.size()))
                    gfx_bank[off] = v & 1;
            } else if (off >= 4 && off <= 7) {
                if (auto& h = m_machine->sound_write_handler())
                    h(mooncrst_audio_port::lfo_base |
                          static_cast<unsigned>(off - 4),
                      v);
            }
        } else if (a >= 0xa800 && a < 0xb000) {
            if (auto& h = m_machine->sound_write_handler())
                h(mooncrst_audio_port::sound_base |
                      static_cast<unsigned>(a & 0x07),
                  v);
        } else if (a >= 0xb000 && a < 0xb800) {
            switch (a & 0x07) {
                case 0: nmi_enabled = (v & 1) != 0; break;
                case 4: stars_enabled = (v & 1) != 0; break;
                case 6: flip_x = (v & 1) != 0; break;
                case 7: flip_y = (v & 1) != 0; break;
                default: break;
            }
        } else if (a >= 0xb800 && a < 0xc000) {
            if (auto& h = m_machine->sound_write_handler())
                h(mooncrst_audio_port::pitch, v);
        }
    }

    void cpu_write_galaxian_map(uint16_t a, uint8_t v) {
        if (a >= 0x4000 && a < 0x4800) {
            ram[(a - 0x4000) & 0x03ff] = v;
        } else if (a >= 0x5000 && a < 0x5800) {
            vram[(a - 0x5000) & 0x03ff] = v;
        } else if (a >= 0x5800 && a < 0x6000) {
            objram[(a - 0x5800) & 0x00ff] = v;
        } else if (a >= 0x6000 && a < 0x6800) {
            const int off = a & 0x07;
            if (off == 2 &&
                m_profile == galaxian_discrete_profile::uniwars) {
                // Pisces replaces the Galaxian coin-lockout output with a
                // graphics-bank latch (tiles +0x100, sprites +0x40).
                gfx_bank[0] = v & 1;
            } else if (off >= 4) {
                if (auto& h = m_machine->sound_write_handler())
                    h(mooncrst_audio_port::lfo_base |
                          static_cast<unsigned>(off - 4),
                      v);
            }
        } else if (a >= 0x6800 && a < 0x7000) {
            if (auto& h = m_machine->sound_write_handler())
                h(mooncrst_audio_port::sound_base |
                      static_cast<unsigned>(a & 0x07),
                  v);
        } else if (a >= 0x7000 && a < 0x7800) {
            switch (a & 0x07) {
                case 1: nmi_enabled = (v & 1) != 0; break;
                case 4: stars_enabled = (v & 1) != 0; break;
                case 6: flip_x = (v & 1) != 0; break;
                case 7: flip_y = (v & 1) != 0; break;
                default: break;
            }
        } else if (a >= 0x7800 && a < 0x8000) {
            if (auto& h = m_machine->sound_write_handler())
                h(mooncrst_audio_port::pitch, v);
        }
    }

public:

    void on_frame_start() override {
        nmi_armed = nmi_enabled;
    }

    bool wants_nmi() override {
        if (!nmi_armed) return false;
        nmi_armed = false;
        return true;
    }

    void reset_extras() override {
        ram.fill(0);
        vram.fill(0);
        objram.fill(0);
        gfx_bank.fill(0);
        stars_enabled = false;
        flip_x = false;
        flip_y = false;
        star_scroll = 0;
        frame = 0;
        nmi_enabled = false;
        nmi_armed = false;
        // Inputs are active-high; leave only configured DIP bits asserted
        // until the frontend supplies its first sample.
        in0 = dip0 & 0x20;
        in1 = dip1 & 0xc0;
        dsw = dip2;
        high_scores.reset();
    }

    void service_high_scores() override {
        if (m_profile != galaxian_discrete_profile::mooncrst) return;
        high_scores.update(
            [this](std::uint32_t address) {
                return cpu_read(static_cast<std::uint16_t>(address), 0);
            },
            [this](std::uint32_t address, std::uint8_t value) {
                cpu_write(static_cast<std::uint16_t>(address), value);
            });
    }

    void flush_high_scores() override {
        if (m_profile != galaxian_discrete_profile::mooncrst) return;
        high_scores.flush([this](std::uint32_t address) {
            return cpu_read(static_cast<std::uint16_t>(address), 0);
        });
    }

    void render_frame(uint32_t* rgba) override {
        ++frame;
        if (char_rom.empty()) {
            render_test_pattern(rgba);
            return;
        }
        std::fill(native.begin(), native.end(), kBlack);
        if (stars_enabled) render_stars();
        render_tilemap();
        render_sprites();
        render_bullets();
        rotate(rgba);
    }

    bool load_roms_into(unzFile archive, std::string* err) override {
        auto loader = [archive](const char* name,
                                std::vector<uint8_t>& bytes) {
            return load_zip_entry(archive, name, bytes);
        };
        return load_with(loader, err, "ROM", "zip");
    }

    bool load_roms_from_dir(const std::string& dir,
                            std::string* err) override {
        auto loader = [&dir](const char* name,
                             std::vector<uint8_t>& bytes) {
            return load_dir_entry(dir, name, bytes);
        };
        return load_with(loader, err, "ROM",
                         ("directory " + dir).c_str());
    }

private:
    template <typename Loader>
    bool load_with(Loader loader, std::string* err, const char* rom_label,
                   const char* source_label) {
        program.assign(0x4000, 0xff);
        char_rom.clear();
        std::vector<uint8_t> bytes;

        const char* const* program_roms = nullptr;
        std::size_t program_rom_count = 0;
        const char* const* char_roms = nullptr;
        std::size_t char_rom_count = 0;
        const char* palette_rom = nullptr;
        switch (m_profile) {
        case galaxian_discrete_profile::galaxian:
            program_roms = kGalaxianProgramRoms.data();
            program_rom_count = kGalaxianProgramRoms.size();
            char_roms = kGalaxianCharRoms.data();
            char_rom_count = kGalaxianCharRoms.size();
            palette_rom = kGalaxianPaletteRom;
            break;
        case galaxian_discrete_profile::mooncrst:
            program_roms = kMooncrstProgramRoms.data();
            program_rom_count = kMooncrstProgramRoms.size();
            char_roms = kMooncrstCharRoms.data();
            char_rom_count = kMooncrstCharRoms.size();
            palette_rom = kMooncrstPaletteRom;
            break;
        case galaxian_discrete_profile::uniwars:
            program_roms = kUniwarsProgramRoms.data();
            program_rom_count = kUniwarsProgramRoms.size();
            char_roms = kUniwarsCharRoms.data();
            char_rom_count = kUniwarsCharRoms.size();
            palette_rom = kUniwarsPaletteRom;
            break;
        }
        const char* title = profile_name(m_profile);

        for (std::size_t i = 0; i < program_rom_count; ++i) {
            if (!loader(program_roms[i], bytes) || bytes.size() != 0x800) {
                *err = std::string(title) + " " + rom_label +
                       " missing: " + program_roms[i];
                return false;
            }
            std::copy(bytes.begin(), bytes.end(),
                      program.begin() + i * 0x800);
        }
        if (m_profile == galaxian_discrete_profile::mooncrst)
            decode_mooncrst(program);

        for (std::size_t index = 0; index < char_rom_count; ++index) {
            const char* name = char_roms[index];
            if (!loader(name, bytes) || bytes.size() != 0x800) {
                *err = std::string(title) + " graphics ROM missing: " + name;
                return false;
            }
            char_rom.insert(char_rom.end(), bytes.begin(), bytes.end());
        }

        std::vector<uint8_t> prom;
        if (!loader(palette_rom, prom) || prom.size() < 32) {
            *err = std::string(title) + " palette PROM missing: ";
            *err += palette_rom;
            return false;
        }
        plane_split = char_rom.size() / 2;
        build_palette(prom);
        std::printf(
            "%s: loaded %zu program, %zu graphics and 1 palette ROM from %s\n",
            title, program_rom_count, char_rom_count, source_label);
        return true;
    }

    void apply_input(const input_state& s, uint8_t* ports,
                     std::size_t port_count) override {
        // All profiles use the Galaxian active-high joystick layout.
        uint8_t port0 = dip0 & 0x20;
        if (s.coin1) port0 |= 0x01;
        if (m_profile != galaxian_discrete_profile::uniwars && s.coin2)
            port0 |= 0x02;
        if (s.left_stick_x < 0x60) port0 |= 0x04;
        if (s.left_stick_x > 0x90) port0 |= 0x08;
        if (s.buttons[0]) port0 |= 0x10;
        // Moon Cresta's bit 6 is an undocumented RESET input. Galaxian and
        // UniWar S use the standard test/service inputs on bits 6/7.
        if (m_profile != galaxian_discrete_profile::mooncrst) {
            if (s.test) port0 |= 0x40;
            if (s.service) port0 |= 0x80;
        }
        in0 = port0;
        if (port_count >= 1) ports[0] = port0;

        // IN1: start 1/2 and the cocktail P2 controls.
        uint8_t port1 = 0;
        if (s.start) port1 |= 0x01;
        if (s.p2_start) port1 |= 0x02;
        if (s.p2_stick_x < 0x60) port1 |= 0x04;
        if (s.p2_stick_x > 0x90) port1 |= 0x08;
        if (s.p2_buttons[0]) port1 |= 0x10;
        port1 |= dip1 & 0xc0;
        in1 = port1;
        if (port_count >= 2) ports[1] = port1;

        dsw = dip2;
    }

private:
    void build_palette(const std::vector<uint8_t>& prom) {
        auto bit = [](uint8_t v, int n) -> int { return (v >> n) & 1; };
        for (int i = 0; i < 32; ++i) {
            const uint8_t v = prom[i];
            int r = bit(v, 0) * 0x21 + bit(v, 1) * 0x47 + bit(v, 2) * 0x97;
            int g = bit(v, 3) * 0x21 + bit(v, 4) * 0x47 + bit(v, 5) * 0x97;
            int b = bit(v, 6) * 0x51 + bit(v, 7) * 0xae;
            r = std::min(r, 255);
            g = std::min(g, 255);
            b = std::min(b, 255);
            palette[i] = pack_rgba(static_cast<uint8_t>(r),
                                   static_cast<uint8_t>(g),
                                   static_cast<uint8_t>(b));
        }
    }

    void build_star_data() {
        int sr = 0;
        for (int i = 0; i < kStarRngPeriod; ++i) {
            const int enabled = (((sr & 0x1fe01) == 0x1fe00) ? 1 : 0);
            const int color = (~sr & 0x1f8) >> 3;
            star_data[i] = static_cast<uint8_t>(((color & 0x3f) |
                                                 (enabled << 7)));
            sr = (sr >> 1) | ((((sr >> 12) ^ ~sr) & 1) << 16);
        }
    }

    void build_star_colors() {
        for (int i = 0; i < 64; ++i) {
            const int r = (i & 3) * 85;
            const int g = ((i >> 2) & 3) * 85;
            const int b = ((i >> 4) & 3) * 85;
            star_color[i] = pack_rgba(static_cast<uint8_t>(r),
                                      static_cast<uint8_t>(g),
                                      static_cast<uint8_t>(b));
        }
    }

    static int floor_mod(int a, int m) {
        const int r = a % m;
        return r < 0 ? r + m : r;
    }

    void put_native(int x, int y, uint32_t argb) {
        const int fxp = flip_x ? kNative - 1 - x : x;
        const int fyp = flip_y ? kNative - 1 - y : y;
        if (fxp >= 0 && fxp < kNative && fyp >= 0 && fyp < kNative)
            native[fyp * kNative + fxp] = argb;
    }

    void draw_tile8(int tile, int px, int py, int color_base) {
        const int p0 = tile * 8;
        const int p1 = tile * 8 + static_cast<int>(plane_split);
        for (int y = 0; y < 8; ++y) {
            const uint8_t b0 =
                (static_cast<std::size_t>(p0 + y) < char_rom.size())
                    ? char_rom[p0 + y] : 0;
            const uint8_t b1 =
                (static_cast<std::size_t>(p1 + y) < char_rom.size())
                    ? char_rom[p1 + y] : 0;
            for (int x = 0; x < 8; ++x) {
                const int bit = 7 - x;
                const int pen = ((b0 >> bit) & 1) | (((b1 >> bit) & 1) << 1);
                if (pen != 0)
                    put_native(px + x, py + y,
                               palette[(color_base + pen) & 31]);
            }
        }
    }

    void draw_sprite16(int code, int px, int py, int color_base, bool fx,
                       bool fy) {
        const int block0 = code * 32;
        const int block1 = code * 32 + static_cast<int>(plane_split);
        for (int y = 0; y < 16; ++y) {
            for (int x = 0; x < 16; ++x) {
                const int sxp = fx ? 15 - x : x;
                const int syp = fy ? 15 - y : y;
                const int byte_index = (syp < 8 ? 0 : 16) +
                                       (sxp < 8 ? 0 : 8) + (syp & 7);
                const int bit = 7 - (sxp & 7);
                const uint8_t b0 =
                    (static_cast<std::size_t>(block0 + byte_index) <
                     char_rom.size()) ? char_rom[block0 + byte_index] : 0;
                const uint8_t b1 =
                    (static_cast<std::size_t>(block1 + byte_index) <
                     char_rom.size()) ? char_rom[block1 + byte_index] : 0;
                const int pen = ((b0 >> bit) & 1) | (((b1 >> bit) & 1) << 1);
                if (pen != 0)
                    put_native(px + x, py + y,
                               palette[(color_base + pen) & 31]);
            }
        }
    }

    int extend_tile_code(int code) const {
        if (m_profile == galaxian_discrete_profile::uniwars)
            return code | (gfx_bank[0] << 8);
        if (gfx_bank[2] != 0 && (code & 0xc0) == 0x80)
            return (code & 0x3f) | (gfx_bank[0] << 6) |
                   (gfx_bank[1] << 7) | 0x0100;
        return code;
    }

    int extend_sprite_code(int code) const {
        if (m_profile == galaxian_discrete_profile::uniwars)
            return code | (gfx_bank[0] << 6);
        if (gfx_bank[2] != 0 && (code & 0x30) == 0x20)
            return (code & 0x0f) | (gfx_bank[0] << 4) |
                   (gfx_bank[1] << 5) | 0x40;
        return code;
    }

    void render_stars() {
        star_scroll = floor_mod(star_scroll - kStarsScrollPerFrame,
                                kStarRngPeriod);
        for (int y = 0; y < kNative; ++y) {
            int offs = floor_mod(star_scroll + y * 512, kStarRngPeriod);
            const int base = y * kNative;
            for (int x = 0; x < kNative; ++x) {
                const int s = star_data[offs] & 0xff;
                if (++offs >= kStarRngPeriod) offs = 0;
                if ((((y ^ (x >> 3)) & 1) != 0) && (s & 0x80) != 0)
                    native[base + x] = star_color[s & 0x3f];
            }
        }
    }

    void render_tilemap() {
        for (int row = 0; row < 32; ++row) {
            for (int col = 0; col < 32; ++col) {
                const int raw_tile = vram[(row * 32 + col) & 0x3ff] & 0xff;
                const int tile = extend_tile_code(raw_tile);
                const int color_attr = objram[(col * 2 + 1) & 0xff] & 0x07;
                const int scroll_y = objram[(col * 2) & 0xff] & 0xff;
                draw_tile8(tile, col * 8, (row * 8 - scroll_y) & 0xff,
                           color_attr * 4);
            }
        }
    }

    void render_sprites() {
        for (int n = 7; n >= 0; --n) {
            const int base = 0x40 + n * 4;
            const uint8_t b0 = objram[base] & 0xff;
            const uint8_t b1 = objram[base + 1] & 0xff;
            const uint8_t b2 = objram[base + 2] & 0xff;
            const uint8_t b3 = objram[base + 3] & 0xff;
            if (b0 == 0 && b3 == 0) continue;
            const int sy = (240 - (b0 - (n < 3 ? 1 : 0))) & 0xff;
            const int code = extend_sprite_code(b1 & 0x3f);
            const bool sp_flip_x = (b1 & 0x40) != 0;
            const bool sp_flip_y = (b1 & 0x80) != 0;
            const int color = b2 & 0x07;
            const int sx = b3;
            draw_sprite16(code, sx, sy, color * 4, sp_flip_x, sp_flip_y);
        }
    }

    void draw_bullet(int slot, int x, int y) {
        const uint32_t color = (slot == kPlayerMissileSlot)
                                   ? pack_rgba(0xef, 0xef, 0x97)
                                   : pack_rgba(0xff, 0xff, 0xff);
        int draw_x = x - 4;
        for (int i = 0; i < 4; ++i) put_native(draw_x++, y, color);
    }

    void render_bullets() {
        for (int y = kVisibleY0; y < kVisibleY1; ++y) {
            int shell = -1;
            int missile = -1;
            int y_eff = flip_y ? ((y - 1) ^ 0xff) : (y - 1);
            for (int which = 0; which < 3; ++which) {
                const int raw_y =
                    objram[(kBulletsBase + which * 4 + 1) & 0xff] & 0xff;
                if (((raw_y + y_eff) & 0xff) == 0xff) shell = which;
            }
            y_eff = flip_y ? (y ^ 0xff) : y;
            for (int which = 3; which < 8; ++which) {
                const int raw_y =
                    objram[(kBulletsBase + which * 4 + 1) & 0xff] & 0xff;
                if (((raw_y + y_eff) & 0xff) == 0xff) {
                    if (which != kPlayerMissileSlot) shell = which;
                    else missile = which;
                }
            }
            if (shell >= 0) {
                const int raw_x =
                    objram[(kBulletsBase + shell * 4 + 3) & 0xff] & 0xff;
                const int nx = flip_x ? raw_x : 255 - raw_x;
                draw_bullet(shell, nx, y);
            }
            if (missile >= 0) {
                const int raw_x =
                    objram[(kBulletsBase + missile * 4 + 3) & 0xff] & 0xff;
                const int nx = flip_x ? raw_x : 255 - raw_x;
                draw_bullet(missile, nx, y);
            }
        }
    }

    void render_test_pattern(uint32_t* rgba) {
        const int t = frame;
        for (int y = 0; y < kScreenHeight; ++y) {
            for (int x = 0; x < kScreenWidth; ++x) {
                const int r = x * 255 / kScreenWidth;
                const int g = y * 255 / kScreenHeight;
                const int b = (x + y + t * 2) & 0xff;
                rgba[y * kScreenWidth + x] =
                    kBlack | (r << 16) | (g << 8) | b;
            }
        }
    }

    void rotate(uint32_t* rgba) {
        for (int fy = 0; fy < kScreenHeight; ++fy) {
            for (int fx = 0; fx < kScreenWidth; ++fx) {
                const int nx = fy;
                const int ny = (kScreenWidth - 1 - fx) + kVisibleY0;
                rgba[fy * kScreenWidth + fx] =
                    native[(ny & (kNative - 1)) * kNative +
                           (nx & (kNative - 1))];
            }
        }
    }

    galaxian_discrete_profile m_profile;
    galaxian_machine* m_machine{nullptr};
    std::vector<uint8_t> program;
    std::array<uint8_t, 0x400> ram{};
    std::array<uint8_t, 0x400> vram{};
    std::array<uint8_t, 0x100> objram{};
    std::vector<uint8_t> char_rom;
    std::size_t plane_split{0};
    std::array<uint32_t, 32> palette{};
    std::array<uint32_t, kNative * kNative> native{};

    bool stars_enabled{false};
    bool flip_x{false};
    bool flip_y{false};
    std::array<int, 5> gfx_bank{};
    int frame{0};

    std::array<uint8_t, kStarRngPeriod> star_data{};
    std::array<uint32_t, 64> star_color{};
    int star_scroll{0};

    uint8_t in0{0xff};
    uint8_t in1{0x80};
    uint8_t dsw{0x00};
    uint8_t dip0{0x00};
    uint8_t dip1{0x80};
    uint8_t dip2{0x00};

    bool nmi_enabled{false};
    bool nmi_armed{false};
    high_score_runtime high_scores;
};

std::unique_ptr<galaxian_board_interface> make_mooncrst_board_interface() {
    return std::make_unique<galaxian_discrete_board_interface>(
        galaxian_discrete_profile::mooncrst);
}

std::unique_ptr<galaxian_board_interface> make_galaxian_board_interface() {
    return std::make_unique<galaxian_discrete_board_interface>(
        galaxian_discrete_profile::galaxian);
}

std::unique_ptr<galaxian_board_interface> make_uniwars_board_interface() {
    return std::make_unique<galaxian_discrete_board_interface>(
        galaxian_discrete_profile::uniwars);
}
