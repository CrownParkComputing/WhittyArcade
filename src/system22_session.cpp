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
#include "system22_sprites.h"
#include "system22_types.h"

#include <algorithm>
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
    uint64_t m_capture_frames_written{0};
    uint64_t m_polygon_dump_frames_written{0};
    uint16_t m_last_led_data{0xffff};
    std::size_t m_last_sprite_count{0};
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
    void reload_input_mappings() override {
        if (m_input) m_input->reload_mappings();
    }
    double frame_seconds() const override {
        return (814.0 * 525.0) / 25600000.0;
    }

protected:
    operator_menu_definition operator_menu() const override {
        return make_system22_operator_menu(
            m_cabinet->system22_dip_switches, m_game_set);
    }
    void apply_operator_action(const operator_menu_action& action) override {
        apply_system22_operator_action(
            m_cabinet->system22_dip_switches, action);
        if (m_bus)
            m_bus->set_dip_switches(m_cabinet->system22_dip_switches);
    }
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

    const ridge_racer_rom_set game_set = rom_loader::identify_set(rom_path);
    m_game_set = game_set;
    if (game_set != ridge_racer_rom_set::unknown)
        m_nvram_short_name = rom_loader::set_short_name(game_set);
    m_bus->set_super_system22(
        game_set == ridge_racer_rom_set::time_crisis ||
        game_set == ridge_racer_rom_set::dirt_dash ||
        game_set == ridge_racer_rom_set::aqua_jet);

    // Initialize GPU renderer
    if (!m_gpu_renderer->initialize(settings)) {
        printf("Failed to initialize GPU renderer\n");
        return false;
    }
    m_gpu_renderer->set_lightgun_cursor(
        game_set == ridge_racer_rom_set::time_crisis);

    m_input = std::make_unique<arcade_input>();
    if (!m_input->initialize(m_nvram_short_name))
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
    case ridge_racer_rom_set::time_crisis:
        m_bus->set_driving_profile(system22_driving_profile::time_crisis);
        break;
    case ridge_racer_rom_set::dirt_dash:
        m_bus->set_driving_profile(system22_driving_profile::dirt_dash);
        break;
    case ridge_racer_rom_set::aqua_jet:
        m_bus->set_driving_profile(system22_driving_profile::aqua_jet);
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
    case ridge_racer_rom_set::dirt_dash:
        m_bus->set_keycus(0x01a2, 0);
        break;
    default:
        m_bus->clear_keycus();
        break;
    }

    if (m_roms->maincpu_rom.empty() ||
        !m_main_cpu->load_rom(m_roms->maincpu_rom.data(), m_roms->maincpu_rom.size())) {
        std::fprintf(stderr,
                     "A complete %zu MiB System 22 program ROM is required\n",
                     m_bus->program_rom_size() / (1024 * 1024));
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
            addresses->second <= m_bus->program_rom_size()) {
            const auto [start, end] = *addresses;
            for (uint32_t address = start; address < end;) {
                std::size_t length = 0;
                const std::string instruction = m_main_cpu->disassemble(address, &length);
                std::printf("%08x: %s\n", address, instruction.c_str());
                address += static_cast<uint32_t>(length ? length : 2);
            }
        }
    }
    std::printf("Device firmware: C71=%s MCU=%s\n",
                m_roms->has_c71_firmware() ? "ready" : "missing",
                m_roms->has_mcu_firmware() ? "ready" : "missing");

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
    if (game_set == ridge_racer_rom_set::dirt_dash) {
        m_mcu->set_input_profile(system22_mcu_input_profile::dirt_dash);
    } else if (game_set == ridge_racer_rom_set::time_crisis) {
        m_mcu->set_input_profile(system22_mcu_input_profile::time_crisis);
    } else if (game_set == ridge_racer_rom_set::aqua_jet) {
        m_mcu->set_input_profile(system22_mcu_input_profile::aqua_jet);
    }
    const bool mcu_ready = m_roms->super_system22 ?
        m_mcu->initialize_super_system22(m_roms->mcu_rom.data(),
                                          m_roms->mcu_rom.size()) :
        m_mcu->initialize(m_roms->c74_firmware.data(),
                          m_roms->c74_firmware.size(),
                          m_roms->mcu_rom.data(), m_roms->mcu_rom.size());
    if (!mcu_ready) {
        std::fprintf(stderr,
                     "Valid System 22 MCU firmware and a 512 KiB data ROM are required\n");
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
    if (!m_roms->sprite_rom.empty())
        m_gpu_renderer->submit_sprites(m_roms->sprite_rom.data(),
                                       m_roms->sprite_rom.size());
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
    if (m_roms->super_system22) {
        m_bus->update_game_inputs(m_input->state());
        m_mcu->update_inputs(m_input->state());
    } else if (m_mcu->enabled()) {
        m_bus->update_game_inputs(m_input->state());
    }

    // Vblank is source 4 in the non-Super System 22 controller. The game
    // chooses its 68020 IRQ level and acknowledges it through syscon RAM.
    m_bus->signal_vblank();
    m_dsp->signal_vblank();

    // Deterministic short slices let the 68020 and C71 observe shared-memory
    // handshakes within the same video frame without racing host threads.
    int scheduler_slices = std::getenv("RRACER_LED_TRACE") ? 4096 :
        (m_roms->super_system22 ?
            (m_dsp->pdp_begin_count() == 0 ? 512 : 256) : 64);
    if (const char* configured_slices =
            std::getenv("RRACER_SCHEDULER_SLICES")) {
        scheduler_slices = std::clamp(
            static_cast<int>(std::strtol(configured_slices, nullptr, 0)),
            64, 8192);
    }
    int previous_cpu = 0;
    int previous_dsp = 0;
    int previous_mcu = 0;
    for (int slice = 1; slice <= scheduler_slices; ++slice) {
        if (m_roms->super_system22 && slice == scheduler_slices / 2)
            m_mcu->signal_irq2();
        if (m_roms->super_system22 && slice == scheduler_slices)
            m_mcu->signal_irq0();
        const int cpu_target = cpu_cycles * slice / scheduler_slices;
        const int dsp_target = dsp_clocks * slice / scheduler_slices;
        const int mcu_target = mcu_cycles * slice / scheduler_slices;
        m_main_cpu->execute(cpu_target - previous_cpu);
        if (std::getenv("RRACER_LED_TRACE") &&
            m_bus->cpu_led_data() != m_last_led_data) {
            m_last_led_data = m_bus->cpu_led_data();
            std::printf("CPU LEDs=%04x D3=%08x PC=%08x\n",
                        m_last_led_data, m_main_cpu->data_register(3),
                        m_main_cpu->program_counter());
        }
        m_dsp->execute(dsp_target - previous_dsp);
        m_mcu->execute(mcu_target - previous_mcu);
        previous_cpu = cpu_target;
        previous_dsp = dsp_target;
        previous_mcu = mcu_target;
    }

    std::vector<polygon_object> polygons = m_dsp->take_rendered_polygons();
    m_last_sprite_count = 0;
    if (m_roms->super_system22 &&
        (m_bus->read_mixer_byte(0x1f) & 0x01) == 0)
        polygons.clear();
    if (m_roms->super_system22 &&
        (m_bus->read_mixer_byte(0x1f) & 0x02) != 0) {
        std::vector<polygon_object> sprites = system22_sprite_decoder::decode(
            m_bus->sprite_ram_data(), m_bus->sprite_ram_size(),
            m_bus->vics_data(), m_bus->vics_data_size(),
            m_bus->vics_control_data(), m_bus->vics_control_size(),
            m_roms->sprite_rom.size(),
            m_game_set == ridge_racer_rom_set::dirt_dash ?
                system22_sprite_profile::dirt_dash :
                system22_sprite_profile::standard);
        m_last_sprite_count = sprites.size();
        if (std::getenv("RRACER_SPRITE_TRACE") &&
            m_frame_number % 60 == 0 && !sprites.empty()) {
            const polygon_object& sample = sprites.front();
            std::printf(
                "Sprite sample: count=%zu tile=%u color=%02x "
                "xy=(%.1f,%.1f)-(%.1f,%.1f) uv=(%d,%d)-(%d,%d) "
                "control=%08x/%08x/%08x/%08x\n",
                sprites.size(), sample.sprite_tile, sample.color,
                sample.vertices[0].x, sample.vertices[0].y,
                sample.vertices[2].x, sample.vertices[2].y,
                sample.vertices[0].u, sample.vertices[0].v,
                sample.vertices[2].u, sample.vertices[2].v,
                m_bus->read32(0x980000), m_bus->read32(0x980004),
                m_bus->read32(0x980008), m_bus->read32(0x98000c));
        }
        sprites.insert(sprites.end(), polygons.begin(), polygons.end());
        polygons = std::move(sprites);
    }
    if (const char* dump_path = std::getenv("RRACER_POLYGON_DUMP")) {
        const char* dump_frame_text =
            std::getenv("RRACER_POLYGON_DUMP_FRAME");
        const char* dump_count_text =
            std::getenv("RRACER_POLYGON_DUMP_COUNT");
        const uint64_t dump_frame = dump_frame_text ?
            std::strtoull(dump_frame_text, nullptr, 0) : 0;
        const uint64_t dump_count = std::max<uint64_t>(
            1, dump_count_text ?
                std::strtoull(dump_count_text, nullptr, 0) : 1);
        if (*dump_path && m_frame_number >= dump_frame &&
            m_polygon_dump_frames_written < dump_count) {
            const std::string frame_path = std::string(dump_path) + "." +
                std::to_string(m_frame_number) + ".csv";
            if (std::FILE* output = std::fopen(frame_path.c_str(), "wb")) {
                std::fprintf(output,
                    "index,zsort,color,cmode,bank,direct,objectflags,czadjust,");
                for (int vertex = 0; vertex < 4; ++vertex)
                    std::fprintf(output, "x%d,y%d,z%d,u%d,v%d,bri%d%s",
                                 vertex, vertex, vertex, vertex, vertex,
                                 vertex, vertex == 3 ? "\n" : ",");
                for (std::size_t index = 0; index < polygons.size(); ++index) {
                    const polygon_object& polygon = polygons[index];
                    std::fprintf(output, "%zu,%u,%u,%u,%u,%u,%u,%u,",
                                 index, polygon.zsort, polygon.color,
                                 polygon.cmode, polygon.texturebank,
                                 polygon.direct ? 1u : 0u,
                                 polygon.objectflags, polygon.cz_adjust);
                    for (int vertex = 0; vertex < 4; ++vertex) {
                        const poly_vertex& point = polygon.vertices[vertex];
                        std::fprintf(output, "%.9g,%.9g,%.9g,%d,%d,%d%s",
                                     point.x, point.y, point.z, point.u,
                                     point.v, point.bri,
                                     vertex == 3 ? "\n" : ",");
                    }
                }
                std::fclose(output);
                std::printf("Dumped %zu polygons for frame %llu to %s\n",
                            polygons.size(),
                            static_cast<unsigned long long>(m_frame_number),
                            frame_path.c_str());
            }
            ++m_polygon_dump_frames_written;
        }
    }
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
        const char* capture_count_text =
            std::getenv("RRACER_CAPTURE_COUNT");
        const uint64_t capture_count = std::max<uint64_t>(
            1, capture_count_text ?
                std::strtoull(capture_count_text, nullptr, 0) : 1);
        if (capture_path && *capture_path && m_frame_number >= capture_frame) {
            std::vector<uint32_t> pixels(
                static_cast<std::size_t>(SYSTEM22_SCREEN_WIDTH) *
                SYSTEM22_SCREEN_HEIGHT);
            m_gpu_renderer->read_framebuffer(pixels.data());
            std::string frame_path = capture_path;
            if (capture_count > 1)
                frame_path += "." + std::to_string(m_frame_number) + ".ppm";
            if (std::FILE* output = std::fopen(frame_path.c_str(), "wb")) {
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
                            frame_path.c_str());
            }
            ++m_capture_frames_written;
            m_capture_done = m_capture_frames_written >= capture_count;
        }
    }

    if (m_frame_number % 60 == 0 && session_trace_enabled()) {
        const uint32_t pc = m_main_cpu->program_counter();
        const uint32_t a6 = m_main_cpu->address_register(6);
        const std::size_t palette_nonzero = static_cast<std::size_t>(
            std::count_if(m_bus->palette_ram_data(),
                          m_bus->palette_ram_data() +
                              m_bus->palette_ram_size(),
                          [](uint8_t value) { return value != 0; }));
        printf("Frame: %llu PC=%08x %s C71=%04x/%04x C74=%06x "
               "C352=%llu/%llu audio=%d/%d dropped=%llu polys=%llu/%llu PDP=%llu/%llu "
               "sprites=%zu layer=%02x alpha=%02x/%02x/%02x "
               "bg=%02x%02x%02x pal=%zu "
               "A6=%08x wait=%04x\n",
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
               m_last_sprite_count, m_bus->read_mixer_byte(0x1f),
               m_bus->read_mixer_byte(0x0f),
               m_bus->read_mixer_byte(0x10),
               m_bus->read_mixer_byte(0x11),
               m_bus->read_mixer_byte(0x08),
               m_bus->read_mixer_byte(0x09),
               m_bus->read_mixer_byte(0x0a), palette_nonzero,
               a6, m_bus->read16(a6 - 0x7ffa));
    }
    if (m_frame_number % 60 == 0 && std::getenv("RRACER_INPUT_TRACE")) {
        if (m_game_set == ridge_racer_rom_set::dirt_dash) {
            const input_state& state = m_input->state();
            std::printf(
                "Dirt Dash input: steer=%04x gas=%04x brake=%04x "
                "coin=%d view=%d shift=%d/%d\n",
                state.steering, state.gas, state.brake, state.coin1,
                state.view, state.shift_down, state.shift_up);
        } else if (m_game_set == ridge_racer_rom_set::aqua_jet) {
            const input_state& state = m_input->state();
            std::printf(
                "Aqua Jet input: handle=%04x throttle=%04x lean=%02x "
                "coin=%d start=%d\n",
                state.steering, state.gas, state.left_stick_y, state.coin1,
                state.start);
        } else if (m_roms->super_system22) {
            std::printf("Time Crisis input: x=%u y=%u coin=%d trigger=%d pedal=%d\n",
                        m_bus->gun_x(), m_bus->gun_y(),
                        m_input->state().coin1,
                        m_input->state().buttons[0],
                        m_input->state().buttons[1]);
        } else {
            std::printf("Input shared: flags=%04x steer=%04x gas=%04x "
                        "brake=%04x credits=%04x\n",
                        m_bus->read16(0x60004030),
                        m_bus->read16(0x60004032),
                        m_bus->read16(0x60004034),
                        m_bus->read16(0x60004036),
                        m_bus->read16(0x6000403a));
        }
    }
}

void system22_emulator::render_frame() {
    if (m_roms->super_system22) {
        std::array<uint8_t, 0x300> gamma{};
        bool programmed = false;
        for (std::size_t component = 0; component < 3; ++component) {
            for (std::size_t value = 0; value < 0x100; ++value) {
                const uint8_t entry = m_bus->read_mixer_byte(
                    (component + 1) * 0x100 + value);
                gamma[component * 0x100 + value] = entry;
                programmed = programmed || entry != 0;
            }
        }
        if (programmed)
            m_gpu_renderer->submit_gamma(gamma.data(), gamma.size());
        m_gpu_renderer->set_system22_layer_mask(
            m_bus->read_mixer_byte(0x1f));
    } else {
        m_gpu_renderer->set_system22_layer_mask(0x07);
    }
    m_gpu_renderer->submit_palette(m_bus->palette_ram_data(),
                                   m_bus->palette_ram_size());
    m_gpu_renderer->submit_text_layer(
        m_bus->character_ram_data(), m_bus->character_ram_size(),
        m_bus->text_ram_data(), m_bus->text_ram_size(),
        m_bus->text_attr_data(), m_bus->mixer_data(), m_bus->mixer_size(),
        m_roms->super_system22);
    uint32_t background_rgba = 0xff;
    if (m_roms->super_system22) {
        uint8_t red = m_bus->read_mixer_byte(0x08);
        uint8_t green = m_bus->read_mixer_byte(0x09);
        uint8_t blue = m_bus->read_mixer_byte(0x0a);
        const uint8_t fade_factor = m_bus->read_mixer_byte(0x19);
        if ((m_bus->read_mixer_byte(0x1a) & 0x01) != 0 &&
            fade_factor != 0) {
            const auto fade = [fade_factor](uint8_t source,
                                             uint8_t target) {
                const unsigned inverse = 0xffu - fade_factor;
                return static_cast<uint8_t>(
                    (static_cast<unsigned>(source) * inverse +
                     static_cast<unsigned>(target) * fade_factor) / 0xffu);
            };
            red = fade(red, m_bus->read_mixer_byte(0x16));
            green = fade(green, m_bus->read_mixer_byte(0x17));
            blue = fade(blue, m_bus->read_mixer_byte(0x18));
        }
        background_rgba = (static_cast<uint32_t>(red) << 24) |
                          (static_cast<uint32_t>(green) << 16) |
                          (static_cast<uint32_t>(blue) << 8) | 0xff;
    } else {
        const uint32_t background_index =
            ((static_cast<uint32_t>(m_bus->read_mixer_byte(0x04)) << 8) &
             0x7f00) | 0xff;
        const uint8_t* palette = m_bus->palette_ram_data();
        background_rgba =
            (static_cast<uint32_t>(palette[background_index]) << 24) |
            (static_cast<uint32_t>(palette[0x8000 + background_index]) << 16) |
            (static_cast<uint32_t>(palette[0x10000 + background_index]) << 8) |
            0xff;
    }
    m_gpu_renderer->render_scene(m_viewmatrix, rgba_color(background_rgba));
}

} // namespace

std::unique_ptr<emulator_session> make_system22_session(
    std::shared_ptr<arcade_video_worker> video,
    std::shared_ptr<arcade_cabinet_state> cabinet) {
    return std::make_unique<system22_emulator>(std::move(video),
                                                std::move(cabinet));
}
