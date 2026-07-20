// galaxian_machine_phoenix.cpp - Amstar Phoenix (1980) raster board.
//
// Phoenix uses an 8085A driven from a 5.5 MHz input clock (2.75 MHz
// instruction clock). Its program is 8080-compatible, so the shared Z80
// core is used in 8080-compatible operation until a dedicated 8085 core
// is added. Video and memory behavior follow the original board.

#include "galaxian_machine.h"
#include "high_scores.h"

#include "z80.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <minizip/unzip.h>

namespace {

constexpr std::size_t kProgramSize = 0x4000;
constexpr std::size_t kVideoPageSize = 0x1000;
constexpr std::size_t kGraphicsSize = 0x1000;
constexpr std::size_t kPalettePromSize = 0x0200;
constexpr int kNativeWidth = 256;
constexpr int kNativeHeight = 208;
constexpr int kScreenWidth = kNativeHeight;   // ROT90
constexpr int kScreenHeight = kNativeWidth;
constexpr int kClocksPerFrame = 45'056;
constexpr double kRefreshHz =
    5'500'000.0 / (352.0 * 256.0);

constexpr std::array<std::pair<const char*, std::size_t>, 8> kProgramFiles = {
    {{"ic45", 0x0000}, {"ic46", 0x0800},
     {"ic47", 0x1000}, {"ic48", 0x1800},
     {"h5-ic49.5a", 0x2000}, {"h6-ic50.6a", 0x2800},
     {"h7-ic51.7a", 0x3000}, {"h8-ic52.8a", 0x3800}}};
constexpr std::array<std::pair<const char*, std::size_t>, 2> kBackgroundFiles = {
    {{"ic23.3d", 0x0000}, {"ic24.4d", 0x0800}}};
constexpr std::array<std::pair<const char*, std::size_t>, 2> kForegroundFiles = {
    {{"b1-ic39.3b", 0x0000}, {"b2-ic40.4b", 0x0800}}};
constexpr std::array<std::pair<const char*, std::size_t>, 2> kPaletteFiles = {
    {{"mmi6301.ic40", 0x0000}, {"mmi6301.ic41", 0x0100}}};

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

// Loads (name, offset) entries into a std::array destination. Loader
// is a callable `(const char* name, std::vector<uint8_t>& bytes) -> bool`
// that fills `bytes` for `name` and returns false if the file is
// missing. Used for both zip and directory sources.
template <std::size_t DestinationSize, std::size_t N, typename Loader>
bool load_region(Loader loader,
                 const std::array<std::pair<const char*, std::size_t>, N>& files,
                 std::array<uint8_t, DestinationSize>& destination,
                 std::string* err) {
    for (const auto& [name, offset] : files) {
        std::vector<uint8_t> bytes;
        if (!loader(name, bytes)) {
            *err = std::string("Phoenix ROM missing: ") + name;
            return false;
        }
        if (offset + bytes.size() > destination.size()) {
            *err = std::string("Phoenix ROM ") + name + " overruns its region";
            return false;
        }
        std::copy(bytes.begin(), bytes.end(), destination.begin() + offset);
    }
    return true;
}

uint32_t pack_rgba(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint32_t>(r) |
           (static_cast<uint32_t>(g) << 8) |
           (static_cast<uint32_t>(b) << 16) | 0xff000000u;
}

// Output of the original two 256x4 PROMs through Phoenix's 100/270 ohm
// pull-up, pull-down and RGB resistor network. The second half repeats.
constexpr std::array<uint32_t, 128> kStandardPaletteArgb{{
    0xff000000, 0xff0a07bd, 0xff9f1114, 0xffc8c8c8,
    0xff000000, 0xff2db8e0, 0xffd11eaa, 0xffedc63a,
    0xff000000, 0xffcdf040, 0xffa71bce, 0xffedc63a,
    0xff000000, 0xffa71bce, 0xff2ab5b6, 0xffd11eaa,
    0xff000000, 0xfff6f644, 0xff0a07bd, 0xffd5fbf9,
    0xff000000, 0xff2db8e0, 0xffd11eaa, 0xffcdf040,
    0xff000000, 0xff2db8e0, 0xffd11eaa, 0xffcdf040,
    0xff000000, 0xff2db8e0, 0xffd11eaa, 0xffcdf040,
    0xff000000, 0xff35e7bf, 0xff2db8e0, 0xff36e8e9,
    0xff000000, 0xffca1618, 0xfff6f644, 0xffffffff,
    0xff000000, 0xffca1618, 0xfff6f644, 0xffffffff,
    0xff000000, 0xffd31fd2, 0xfff4cdcf, 0xfff6f644,
    0xff000000, 0xffd31fd2, 0xfff4cdcf, 0xfff6f644,
    0xff000000, 0xffd31fd2, 0xfff4cdcf, 0xfff6f644,
    0xff000000, 0xffca1618, 0xffffffff, 0xffd31fd2,
    0xff000000, 0xff23ad24, 0xffa518a5, 0xffffffff,
    0xff000000, 0xff0a07bd, 0xff9f1114, 0xffc0c136,
    0xff000000, 0xff0a07bd, 0xffd11eaa, 0xffedc63a,
    0xff000000, 0xffcdf040, 0xffa71bce, 0xffedc63a,
    0xff000000, 0xffa71bce, 0xff2ab5b6, 0xffd11eaa,
    0xff000000, 0xffd11eaa, 0xff2db8e0, 0xffcdf040,
    0xff000000, 0xffd11eaa, 0xff2db8e0, 0xffcdf040,
    0xff000000, 0xffd11eaa, 0xff2db8e0, 0xffcdf040,
    0xff000000, 0xffd11eaa, 0xff2db8e0, 0xffcdf040,
    0xff000000, 0xff35e7bf, 0xff2db8e0, 0xfff6f644,
    0xff000000, 0xffca1618, 0xfff6f644, 0xffffffff,
    0xff000000, 0xffca1618, 0xfff6f644, 0xffffffff,
    0xff000000, 0xff2edf2d, 0xfff6f644, 0xfff5d0f5,
    0xff000000, 0xff2edf2d, 0xfff6f644, 0xfff5d0f5,
    0xff000000, 0xff2edf2d, 0xfff6f644, 0xfff5d0f5,
    0xff000000, 0xffd31fd2, 0xffffffff, 0xfff6f644,
    0xff000000, 0xff2edf2d, 0xffa518a5, 0xffffffff,
}};

}  // namespace

class phoenix_board_interface final : public galaxian_board_interface {
public:
    void configure(galaxian_machine* self) override {
        m_machine = self;
        self->set_dimensions(kScreenWidth, kScreenHeight, kRefreshHz,
                             kClocksPerFrame);
        program.fill(0xff);
        output_fill_black();
    }

    uint8_t cpu_read(uint16_t address, int frame_clock) override {
        if (address < 0x4000) return program[address];
        if (address >= 0x4000 && address < 0x5000)
            return video_pages[selected_page][address - 0x4000];
        if (address >= 0x7000 && address < 0x7400) return input_port;
        if (address >= 0x7800 && address < 0x7c00) {
            const bool vertical_blank =
                frame_clock >= kClocksPerFrame * kNativeHeight / 256;
            return static_cast<uint8_t>(dip_switches |
                                        (vertical_blank ? 0x00 : 0x80));
        }
        return 0xff;
    }

    void cpu_write(uint16_t address, uint8_t data) override {
        if (address >= 0x4000 && address < 0x5000) {
            video_pages[selected_page][address - 0x4000] = data;
        } else if (address >= 0x5000 && address < 0x5400) {
            selected_page = data & 1;
            palette_bank = (data >> 1) & 1;
        } else if (address >= 0x5800 && address < 0x5c00) {
            scroll = data;
        } else if (address >= 0x6000 && address < 0x6400) {
            sound_a = data;
            if (auto& h = m_machine->sound_write_handler()) h(0, data);
        } else if (address >= 0x6800 && address < 0x6c00) {
            sound_b = data;
            if (auto& h = m_machine->sound_write_handler()) h(1, data);
        }
    }

    void reset_extras() override {
        for (auto& page : video_pages) page.fill(0);
        selected_page = 0;
        palette_bank = 0;
        scroll = 0;
        sound_a = 0;
        sound_b = 0;
        input_port = 0xff;
        high_scores.reset();
    }

    void service_high_scores() override {
        high_scores.update(
            [this](std::uint32_t address) {
                return cpu_read(static_cast<std::uint16_t>(address), 0);
            },
            [this](std::uint32_t address, std::uint8_t value) {
                cpu_write(static_cast<std::uint16_t>(address), value);
            });
    }

    void flush_high_scores() override {
        high_scores.flush([this](std::uint32_t address) {
            return cpu_read(static_cast<std::uint16_t>(address), 0);
        });
    }

    void render_frame(uint32_t* rgba) override {
        const auto& page = video_pages[selected_page];
        for (int source_y = 0; source_y < kNativeHeight; ++source_y) {
            for (int source_x = 0; source_x < kNativeWidth; ++source_x) {
                const int background_x = (source_x + scroll) & 0xff;
                const int bg_tile = (source_y >> 3) * 32 + (background_x >> 3);
                const uint8_t bg_code = page[0x800 + bg_tile];
                const int bg_color = (bg_code >> 5) | (palette_bank << 4);
                const uint8_t bg_pixel = tile_pixel(
                    background_graphics, bg_code, background_x & 7,
                    source_y & 7);
                uint32_t pixel = palette[bg_color * 4 + bg_pixel];

                const int fg_tile = (source_y >> 3) * 32 + (source_x >> 3);
                const uint8_t fg_code = page[fg_tile];
                const uint8_t fg_pixel = tile_pixel(
                    foreground_graphics, fg_code, source_x & 7, source_y & 7);
                if (fg_pixel != 0) {
                    const int fg_color = (fg_code >> 5) | 0x08 |
                                         (palette_bank << 4);
                    pixel = palette[fg_color * 4 + fg_pixel];
                }

                const int output_x = kNativeHeight - 1 - source_y;
                const int output_y = source_x;
                rgba[output_y * kScreenWidth + output_x] = pixel;
            }
        }
    }

    bool load_roms_into(unzFile archive, std::string* err) override {
        program.fill(0xff);
        background_graphics.fill(0);
        foreground_graphics.fill(0);
        palette_prom.fill(0);
        auto zip_loader = [archive](const char* name,
                                   std::vector<uint8_t>& bytes) {
            return load_zip_entry(archive, name, bytes);
        };
        const bool ok =
            load_region<kProgramSize>(zip_loader, kProgramFiles, program, err) &&
            load_region<kGraphicsSize>(zip_loader, kBackgroundFiles,
                                       background_graphics, err) &&
            load_region<kGraphicsSize>(zip_loader, kForegroundFiles,
                                       foreground_graphics, err) &&
            load_region<kPalettePromSize>(zip_loader, kPaletteFiles,
                                          palette_prom, err);
        if (!ok) return false;
        build_palette();
        std::printf("Phoenix: loaded 8 program, 4 graphics and 2 palette ROMs\n");
        return true;
    }

    bool load_roms_from_dir(const std::string& dir,
                            std::string* err) override {
        program.fill(0xff);
        background_graphics.fill(0);
        foreground_graphics.fill(0);
        palette_prom.fill(0);
        auto dir_loader = [&dir](const char* name,
                                 std::vector<uint8_t>& bytes) {
            return load_dir_entry(dir, name, bytes);
        };
        const bool ok =
            load_region<kProgramSize>(dir_loader, kProgramFiles, program, err) &&
            load_region<kGraphicsSize>(dir_loader, kBackgroundFiles,
                                       background_graphics, err) &&
            load_region<kGraphicsSize>(dir_loader, kForegroundFiles,
                                       foreground_graphics, err) &&
            load_region<kPalettePromSize>(dir_loader, kPaletteFiles,
                                          palette_prom, err);
        if (!ok) return false;
        build_palette();
        std::printf("Phoenix: loaded ROMs from directory %s\n", dir.c_str());
        return true;
    }

    void apply_input(const input_state& s, uint8_t* ports,
                     std::size_t port_count) override {
        if (port_count < 1) return;
        // The CPU sees IN0 at 0x7000.  Its low nibble is the cabinet
        // coin/start wiring, while the upright CTRL nibble is multiplexed
        // into bits 4-7 by the original board (MAME's player_input_r).
        // Every line is active-low.
        uint8_t port = 0xff;
        if (s.coin1) port &= static_cast<uint8_t>(~0x01);
        if (s.start) port &= static_cast<uint8_t>(~0x02);
        if (s.p2_start) port &= static_cast<uint8_t>(~0x04);
        if (s.buttons[0]) port &= static_cast<uint8_t>(~0x10);
        if (s.left_stick_x > 0x90)
            port &= static_cast<uint8_t>(~0x20);  // right
        if (s.left_stick_x < 0x60)
            port &= static_cast<uint8_t>(~0x40);  // left
        if (s.buttons[1]) port &= static_cast<uint8_t>(~0x80);
        input_port = port;
        ports[0] = port;
    }

private:
    void output_fill_black() {
        for (std::size_t i = 0; i < palette.size(); ++i) palette[i] = 0;
    }

    void build_palette() {
        for (std::size_t pen = 0; pen < palette.size(); ++pen) {
            const uint32_t argb = kStandardPaletteArgb[pen & 0x7f];
            palette[pen] = pack_rgba(static_cast<uint8_t>(argb >> 16),
                                     static_cast<uint8_t>(argb >> 8),
                                     static_cast<uint8_t>(argb));
        }
    }

    static uint8_t tile_pixel(const std::array<uint8_t, kGraphicsSize>& graphics,
                              uint8_t code, int x, int y) {
        const std::size_t row = static_cast<std::size_t>(code) * 8 + y;
        const int bit = x;
        const uint8_t high_plane = (graphics[0x800 + row] >> bit) & 1;
        const uint8_t low_plane = (graphics[row] >> bit) & 1;
        return static_cast<uint8_t>((high_plane << 1) | low_plane);
    }

    galaxian_machine* m_machine{nullptr};
    std::array<uint8_t, kProgramSize> program{};
    std::array<uint8_t, kGraphicsSize> background_graphics{};
    std::array<uint8_t, kGraphicsSize> foreground_graphics{};
    std::array<uint8_t, kPalettePromSize> palette_prom{};
    std::array<std::array<uint8_t, kVideoPageSize>, 2> video_pages{};
    std::array<uint32_t, 256> palette{};
    uint8_t selected_page{0};
    uint8_t palette_bank{0};
    uint8_t scroll{0};
    uint8_t sound_a{0};
    uint8_t sound_b{0};
    uint8_t input_port{0xff};
    uint8_t dip_switches{0x60};
    high_score_runtime high_scores{"phoenix"};
};

std::unique_ptr<galaxian_board_interface> make_phoenix_board_interface() {
    return std::make_unique<phoenix_board_interface>();
}
