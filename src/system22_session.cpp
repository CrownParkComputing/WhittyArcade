// Namco System 22 runtime session.

#include "arcade_session_internal.h"
#include "arcade_frontend.h"
#include "persistent_data.h"
#include "system22_audio.h"
#include "system22_config.h"
#include "system22_cpu.h"
#include "system22_dsp.h"
#include "system22_mcu.h"
#include "system22_rom.h"
#include "system22_types.h"

#include <array>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

std::optional<std::pair<uint32_t, uint32_t>> parse_hex_range(
    std::string_view text) {
    const std::size_t separator = text.find(':');
    if (separator == std::string_view::npos || separator == 0 ||
        separator + 1 == text.size())
        return std::nullopt;

    uint32_t start = 0;
    uint32_t end = 0;
    const char* const begin = text.data();
    const char* const separator_position = begin + separator;
    const char* const finish = begin + text.size();
    const auto start_result = std::from_chars(
        begin, separator_position, start, 16);
    const auto end_result = std::from_chars(
        separator_position + 1, finish, end, 16);
    if (start_result.ec != std::errc{} ||
        start_result.ptr != separator_position ||
        end_result.ec != std::errc{} ||
        end_result.ptr != finish)
        return std::nullopt;
    return std::pair<uint32_t, uint32_t>{start, end};
}

class system22_emulator : public video_emulator_session {
private:
    std::unique_ptr<system22_bus> m_bus;
    std::unique_ptr<arcade_input> m_input;

    // Audio system - dedicated audio thread
    std::unique_ptr<audio_system> m_audio;

    // System 22 processors, cooperatively scheduled in short deterministic
    // slices. Audio sample generation remains on its own host thread.
    std::unique_ptr<mc68020_core> m_main_cpu;
    std::unique_ptr<system22_dsp_system> m_dsp;
    std::unique_ptr<system22_c74_mcu> m_mcu;

    std::unique_ptr<ridge_racer_roms> m_roms;

    // Renderer state
    view_matrix m_viewmatrix;
    double m_cpu_cycle_remainder{0.0};
    double m_dsp_cycle_remainder{0.0};
    double m_mcu_cycle_remainder{0.0};
    uint64_t m_frame_number{0};
    bool m_capture_done{false};
    std::string m_nvram_short_name;
    ridge_racer_rom_set m_game_set{ridge_racer_rom_set::unknown};

public:
    system22_emulator(std::shared_ptr<arcade_video_worker> video,
                      std::shared_ptr<arcade_cabinet_state> cabinet);
    ~system22_emulator() override;

    bool initialize(const std::string& rom_path, const std::string& bios_path,
                    const emulator_settings& settings) override;
    void run_frame() override;
    void render_frame();
    void open_operator_settings() override;
    double frame_seconds() const override {
        return (814.0 * 525.0) / 25600000.0;
    }

protected:
    void apply_audio_settings(const emulator_settings& settings) override {
        if (m_audio)
            m_audio->set_mix_levels(settings.master_volume,
                                    settings.music_volume,
                                    settings.effects_volume);
    }
    void set_audio_paused(bool paused) override {
        if (m_audio) m_audio->set_paused(paused);
    }
};

system22_emulator::system22_emulator(
    std::shared_ptr<arcade_video_worker> video,
    std::shared_ptr<arcade_cabinet_state> cabinet)
    : video_emulator_session(arcade_board_type::system22, std::move(video),
                             std::move(cabinet)),
      m_bus(std::make_unique<system22_bus>()) {}

system22_emulator::~system22_emulator() {
    shutdown_session_devices(m_audio, m_gpu_renderer, m_input);
    if (!m_nvram_short_name.empty() && m_bus &&
        !save_system22_eeprom(m_nvram_short_name, m_bus->eeprom_data(),
                              m_bus->eeprom_size()))
        std::fprintf(stderr, "Could not save System 22 EEPROM for %s\n",
                     m_nvram_short_name.c_str());
}

bool system22_emulator::initialize(const std::string& rom_path,
                                      const std::string& bios_path,
                                      const emulator_settings& settings) {
    printf("Initializing WhittyArcade System 22 backend...\n");
    printf("ROM path: %s\n", rom_path.c_str());
    printf("BIOS path: %s\n", bios_path.c_str());

    // Initialize GPU renderer
    if (!m_gpu_renderer->initialize(settings)) {
        printf("Failed to initialize GPU renderer\n");
        return false;
    }

    m_input = std::make_unique<arcade_input>();
    if (!m_input->initialize())
        std::fprintf(stderr, "Input initialization failed; controls are neutral\n");

    // Initialize audio
    m_audio = std::make_unique<audio_system>();
    if (!m_audio->initialize()) {
        printf("Failed to initialize audio - continuing without audio\n");
    }
    m_audio->set_mix_levels(settings.master_volume, settings.music_volume,
                            settings.effects_volume);

    m_main_cpu = std::make_unique<mc68020_core>(*m_bus);
    // Load ROMs and set up CPU
    m_roms = std::make_unique<ridge_racer_roms>(
        rom_loader::load_ridge_racer(rom_path, bios_path));

    const ridge_racer_rom_set game_set = rom_loader::identify_set(rom_path);
    m_game_set = game_set;
    if (game_set != ridge_racer_rom_set::unknown)
        m_nvram_short_name = rom_loader::set_short_name(game_set);
    m_bus->set_dip_switches(m_cabinet->system22_dip_switches);
    switch (game_set) {
    case ridge_racer_rom_set::rave_racer:
        m_bus->set_driving_profile(system22_driving_profile::rave_racer);
        break;
    case ridge_racer_rom_set::ace_driver:
    case ridge_racer_rom_set::victory_lap:
        m_bus->set_driving_profile(system22_driving_profile::ace_driver);
        break;
    case ridge_racer_rom_set::cyber_commando:
        m_bus->set_driving_profile(system22_driving_profile::cyber_commando);
        break;
    default:
        m_bus->set_driving_profile(system22_driving_profile::ridge_racer);
        break;
    }

    // C370 protection responses are game-specific and appear at different
    // word registers. Games without a fixed response use the deterministic
    // random-value behavior of the original Ridge Racer board.
    switch (game_set) {
    case ridge_racer_rom_set::ridge_racer_2:
        m_bus->set_keycus(0x0172, 0);
        break;
    case ridge_racer_rom_set::ace_driver:
        m_bus->set_keycus(0x0173, 3);
        break;
    case ridge_racer_rom_set::cyber_commando:
        m_bus->set_keycus(0x0185, 1);
        break;
    case ridge_racer_rom_set::victory_lap:
        m_bus->set_keycus(0x0188, 2);
        break;
    default:
        m_bus->clear_keycus();
        break;
    }

    if (m_roms->maincpu_rom.empty() ||
        !m_main_cpu->load_rom(m_roms->maincpu_rom.data(), m_roms->maincpu_rom.size())) {
        std::fprintf(stderr, "A complete 2 MiB System 22 program ROM is required\n");
        return false;
    }
    if (!m_roms->eeprom.empty())
        m_bus->load_eeprom(m_roms->eeprom.data(), m_roms->eeprom.size());
    std::array<uint8_t, system22_bus::EEPROM_SIZE> saved_eeprom{};
    if (!m_nvram_short_name.empty() &&
        load_system22_eeprom(m_nvram_short_name, saved_eeprom.data(),
                             saved_eeprom.size())) {
        m_bus->load_eeprom(saved_eeprom.data(), saved_eeprom.size());
        std::printf("Loaded persistent System 22 EEPROM: %s\n",
                    m_nvram_short_name.c_str());
    }
    std::printf("MC68020 reset: PC=%08x SP=%08x\n",
                m_main_cpu->program_counter(), m_main_cpu->stack_pointer());

    // Development aid: RRACER_DISASSEMBLE=START:END dumps a bounded ROM
    // range without requiring a debugger or extracting the interleaved ROMs.
    if (const char* range = std::getenv("RRACER_DISASSEMBLE")) {
        const auto addresses = parse_hex_range(range);
        if (addresses && addresses->first < addresses->second &&
            addresses->second <= system22_bus::PROGRAM_ROM_SIZE) {
            const auto [start, end] = *addresses;
            for (uint32_t address = start; address < end;) {
                std::size_t length = 0;
                const std::string instruction = m_main_cpu->disassemble(address, &length);
                std::printf("%08x: %s\n", address, instruction.c_str());
                address += static_cast<uint32_t>(length ? length : 2);
            }
        }
    }
    std::printf("Device firmware: C71=%s C74=%s\n",
                m_roms->has_c71_firmware() ? "ready" : "missing",
                m_roms->has_c74_firmware() ? "ready" : "missing");

    m_dsp = std::make_unique<system22_dsp_system>(*m_bus);
    if (!m_dsp->initialize(m_roms->c71_firmware.data(),
                           m_roms->c71_firmware.size(),
                           m_roms->point_rom.data(), m_roms->point_rom.size())) {
        std::fprintf(stderr, "A valid C71 firmware and point ROM are required\n");
        return false;
    }
    m_bus->set_dsp_control_handler(
        [this](uint8_t value) { m_dsp->control(value); });

    m_mcu = std::make_unique<system22_c74_mcu>(*m_bus, *m_audio);
    if (!m_mcu->initialize(m_roms->c74_firmware.data(),
                           m_roms->c74_firmware.size(),
                           m_roms->mcu_rom.data(), m_roms->mcu_rom.size())) {
        std::fprintf(stderr, "A valid C74 firmware and 512 KiB MCU data ROM are required\n");
        return false;
    }
    m_bus->set_mcu_control_handler(
        [this](uint8_t value) { m_mcu->control(value); });

    if (!m_roms->texture_rom.empty())
        m_gpu_renderer->submit_textures(m_roms->texture_rom.data(),
                                        m_roms->texture_rom.size(),
                                        m_roms->tilemap_rom.data(),
                                        m_roms->tilemap_rom.size(),
                                        m_roms->texture_region_offset,
                                        m_roms->texture_bank_count,
                                        m_roms->texture_tile_high_bit_from_attr);
    if (!m_roms->gamma_proms.empty())
        m_gpu_renderer->submit_gamma(m_roms->gamma_proms.data(),
                                     m_roms->gamma_proms.size());
    if (!m_roms->c352_samples.empty())
        m_audio->set_sample_rom(m_roms->c352_samples.data(),
                                m_roms->c352_samples.size());
    m_audio->start();

    printf("%s initialized successfully\n",
           rom_loader::set_display_name(game_set));
    return true;
}

void system22_emulator::open_operator_settings() {
    if (m_game_set == ridge_racer_rom_set::unknown) return;
    show_modal([this] {
        show_dip_switches(m_cabinet->system22_dip_switches, m_game_set);
    });
    m_bus->set_dip_switches(m_cabinet->system22_dip_switches);
}

void system22_emulator::run_frame() {
    // 24.576 MHz MC68020 paced from the System 22 raster timing:
    // 25.6 MHz pixel clock / (814 * 525) lines per frame.
    constexpr double cpu_cycles_per_frame =
        24576000.0 * (814.0 * 525.0) / 25600000.0;
    constexpr double dsp_clocks_per_frame =
        40000000.0 * (814.0 * 525.0) / 25600000.0;
    // The C74 M37702 sound MCU is clocked directly at 49.152 MHz / 3.  It
    // services the game's music command protocol as well as the C352, so it
    // must remain in lockstep with the 68020 rather than being half-clocked.
    constexpr double mcu_cycles_per_frame =
        16384000.0 * (814.0 * 525.0) / 25600000.0;
    m_cpu_cycle_remainder += cpu_cycles_per_frame;
    m_dsp_cycle_remainder += dsp_clocks_per_frame;
    m_mcu_cycle_remainder += mcu_cycles_per_frame;
    const int cpu_cycles = static_cast<int>(m_cpu_cycle_remainder);
    const int dsp_clocks = static_cast<int>(m_dsp_cycle_remainder);
    const int mcu_cycles = static_cast<int>(m_mcu_cycle_remainder);
    m_cpu_cycle_remainder -= cpu_cycles;
    m_dsp_cycle_remainder -= dsp_clocks;
    m_mcu_cycle_remainder -= mcu_cycles;

    // The external control C74 publishes the standard cabinet switches and
    // ADC channels immediately before vblank, matching the real board/MAME
    // handoff into the sound-C74 shared RAM.
    m_input->set_suppressed(m_gpu_renderer->settings_visible());
    m_input->update();
    if (m_mcu->enabled()) m_bus->update_game_inputs(m_input->state());

    // Vblank is source 4 in the non-Super System 22 controller. The game
    // chooses its 68020 IRQ level and acknowledges it through syscon RAM.
    m_bus->signal_vblank();
    m_dsp->signal_vblank();

    // Deterministic short slices let the 68020 and C71 observe shared-memory
    // handshakes within the same video frame without racing host threads.
    constexpr int scheduler_slices = 64;
    int previous_cpu = 0;
    int previous_dsp = 0;
    int previous_mcu = 0;
    for (int slice = 1; slice <= scheduler_slices; ++slice) {
        const int cpu_target = cpu_cycles * slice / scheduler_slices;
        const int dsp_target = dsp_clocks * slice / scheduler_slices;
        const int mcu_target = mcu_cycles * slice / scheduler_slices;
        m_main_cpu->execute(cpu_target - previous_cpu);
        m_dsp->execute(dsp_target - previous_dsp);
        m_mcu->execute(mcu_target - previous_mcu);
        previous_cpu = cpu_target;
        previous_dsp = dsp_target;
        previous_mcu = mcu_target;
    }

    std::vector<polygon_object> polygons = m_dsp->take_rendered_polygons();
    if (!polygons.empty())
        m_gpu_renderer->submit_polygons(polygons.data(),
                                        static_cast<int>(polygons.size()));

    // Render the frame using GPU
    render_frame();

    ++m_frame_number;
    if (!m_capture_done) {
        const char* capture_path = std::getenv("RRACER_CAPTURE");
        const char* capture_frame_text = std::getenv("RRACER_CAPTURE_FRAME");
        const uint64_t capture_frame = capture_frame_text ?
            std::strtoull(capture_frame_text, nullptr, 0) : 180;
        if (capture_path && *capture_path && m_frame_number >= capture_frame) {
            std::vector<uint32_t> pixels(
                static_cast<std::size_t>(SYSTEM22_SCREEN_WIDTH) *
                SYSTEM22_SCREEN_HEIGHT);
            m_gpu_renderer->read_framebuffer(pixels.data());
            if (std::FILE* output = std::fopen(capture_path, "wb")) {
                std::fprintf(output, "P6\n%d %d\n255\n",
                             SYSTEM22_SCREEN_WIDTH, SYSTEM22_SCREEN_HEIGHT);
                for (int y = SYSTEM22_SCREEN_HEIGHT - 1; y >= 0; --y) {
                    for (int x = 0; x < SYSTEM22_SCREEN_WIDTH; ++x) {
                        const uint32_t pixel =
                            pixels[y * SYSTEM22_SCREEN_WIDTH + x];
                        const uint8_t rgb[3] = {
                            static_cast<uint8_t>(pixel >> 16),
                            static_cast<uint8_t>(pixel >> 8),
                            static_cast<uint8_t>(pixel),
                        };
                        std::fwrite(rgb, sizeof(rgb), 1, output);
                    }
                }
                std::fclose(output);
                std::printf("Captured frame %llu to %s\n",
                            static_cast<unsigned long long>(m_frame_number),
                            capture_path);
            }
            m_capture_done = true;
        }
    }

    if (m_frame_number % 60 == 0 && session_trace_enabled()) {
        const uint32_t pc = m_main_cpu->program_counter();
        printf("Frame: %llu PC=%08x %s C71=%04x/%04x C74=%06x "
               "C352=%llu/%llu audio=%d/%d dropped=%llu polys=%llu/%llu PDP=%llu/%llu "
               "game_vblank=%u\n",
               static_cast<unsigned long long>(m_frame_number), pc,
               m_main_cpu->disassemble(pc).c_str(),
               m_dsp->master_pc(), m_dsp->slave_pc(),
               m_mcu->program_counter(),
               static_cast<unsigned long long>(m_mcu->c352_read_count()),
               static_cast<unsigned long long>(m_mcu->c352_write_count()),
               m_audio->busy_voice_count(), m_audio->peak_sample(),
               static_cast<unsigned long long>(m_audio->dropped_commands()),
               static_cast<unsigned long long>(m_dsp->direct_polygon_count()),
               static_cast<unsigned long long>(m_dsp->display_polygon_count()),
               static_cast<unsigned long long>(m_dsp->pdp_begin_count()),
               static_cast<unsigned long long>(m_dsp->display_parse_errors()),
               m_bus->read16(0x10007ff0));
    }
    if (m_frame_number % 60 == 0 && std::getenv("RRACER_INPUT_TRACE")) {
        std::printf("Input shared: flags=%04x steer=%04x gas=%04x "
                    "brake=%04x credits=%04x\n",
                    m_bus->read16(0x60004030),
                    m_bus->read16(0x60004032),
                    m_bus->read16(0x60004034),
                    m_bus->read16(0x60004036),
                    m_bus->read16(0x6000403a));
    }
}

void system22_emulator::render_frame() {
    m_gpu_renderer->submit_palette(m_bus->palette_ram_data(),
                                   m_bus->palette_ram_size());
    m_gpu_renderer->submit_text_layer(
        m_bus->character_ram_data(), m_bus->character_ram_size(),
        m_bus->text_ram_data(), m_bus->text_ram_size(),
        m_bus->text_attr_data(), m_bus->mixer_data(), m_bus->mixer_size());
    const uint32_t background_index =
        ((static_cast<uint32_t>(m_bus->read_mixer_byte(0x04)) << 8) &
         0x7f00) | 0xff;
    const uint8_t* palette = m_bus->palette_ram_data();
    const uint32_t background_rgba =
        (static_cast<uint32_t>(palette[background_index]) << 24) |
        (static_cast<uint32_t>(palette[0x8000 + background_index]) << 16) |
        (static_cast<uint32_t>(palette[0x10000 + background_index]) << 8) | 0xff;
    m_gpu_renderer->render_scene(m_viewmatrix, rgba_color(background_rgba));
}

} // namespace

std::unique_ptr<emulator_session> make_system22_session(
    std::shared_ptr<arcade_video_worker> video,
    std::shared_ptr<arcade_cabinet_state> cabinet) {
    return std::make_unique<system22_emulator>(std::move(video),
                                                std::move(cabinet));
}
