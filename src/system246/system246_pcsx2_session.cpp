// MANX System 246 board session (host side, plain GCC/C++17).
//
// This file is part of the main MANX binary and deliberately pulls in
// NO PCSX2 headers or libraries -- only <dlfcn.h>, MANX headers and the
// flat C ABI in pcsx2_module.h. All PCSX2 code lives in libsystem246_pcsx2.so,
// which we dlopen() at initialize() time. Keeping PCSX2 out of the main binary
// avoids the collision between PCSX2's bundled glad GL loader / static
// initialisers and MANX's GLEW-based OpenGL renderer (which previously
// hung polygon_renderer_gpu::initialize at startup for every game).
//
// Structure mirrors src/xbox360_session.cpp: a video_emulator_session whose
// run_frame updates input, pulls the emulator's latest produced frame and calls
// m_gpu_renderer->present_rgba_frame. The .so's frame getter is non-blocking, so
// this board paces normally through main() (producer_paced() == false).

#include <dlfcn.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <unistd.h>

#include "arcade_input.h"
#include "arcade_feedback.h"
#include "arcade_session_internal.h"
#include "system246_rom.h" // System 246/256 game identification (host side)
#include "system246/system246_loading_screen.h"

#include "pcsx2_module.h" // flat C ABI exported by libsystem246_pcsx2.so

namespace {

// Fixed paths into the PCSX2 arcade build tree (same as the previous in-process
// session and the boot harness).
constexpr const char* kPcsx2BinDir = "/home/jon/pcsx2x6/build/bin";
constexpr const char* kPcsx2BiosDir = "/home/jon/pcsx2x6/build/bin/bios";

// Each System 246/256 title boots its own PCSX2 ".acgame" manifest. The board
// is multi-game: the session maps the selected game's short name to its
// manifest and falls back to Ridge Racer V if the selection cannot be
// resolved (so a bad path degrades to a known-good boot rather than a hang).
constexpr const char* kRrvAcgame =
    "/home/jon/pcsx2x6/build/bin/roms/rrvac/rrvac.acgame";
constexpr const char* kMotogpAcgame =
    "/home/jon/pcsx2x6/build/bin/roms/motogp/motogp.acgame";

// Resolve which game launched from the rom_path the host passed into
// initialize(). This mirrors the app-wide dispatch: identify_arcade_game()
// routes the same rom_path to this board, and here the board's own loader maps
// it to a concrete title. Unknown selections default to RRV.
struct system246_game {
    std::string short_name;
    std::string acgame;        // path to boot (empty until unpacked)
    std::string display_name;
    std::string squashfs_path; // non-empty when the game ships as a .squashfs
};

system246_game resolve_game(const std::string& rom_path) {
    // Scalable path: when the selection is itself a `.acgame` manifest (as
    // surfaced by the pcsx2x6 roms discovery pass), boot it directly. This lets
    // ANY collection game work with no per-title code -- just drop its assets
    // under roms/<game>/ and it appears + launches.
    std::error_code ec;
    if (rom_path.size() >= 7 &&
        rom_path.compare(rom_path.size() - 7, 7, ".acgame") == 0 &&
        std::filesystem::is_regular_file(rom_path, ec)) {
        const std::string base =
            std::filesystem::path(rom_path).stem().string();
        return {base, rom_path, base, {}};
    }
    // A System 246/256 squashfs pack: keep the image squashed on disk and
    // unpack it to a per-game cache directory only once the player launches
    // the game, showing progress on the loading screen. Here we only resolve
    // identity (short name + display name from the manifest inside) and note
    // that the game must be unpacked before it can boot; the actual extract
    // runs on a worker thread so the loading screen can show it advancing.
    if (system246_rom_loader::is_squashfs(rom_path) &&
        system246_rom_loader::squashfs_is_system246(rom_path)) {
        const std::string stem =
            std::filesystem::path(rom_path).stem().string();
        // If a cached unpack already exists, resolve straight to it.
        const std::string cached =
            system246_rom_loader::squashfs_cached_acgame(rom_path);
        return {stem, cached, system246_rom_loader::squashfs_game_name(rom_path),
                rom_path};
    }
    // Fallback for library-scanned sets that arrive as a MAME zip/dir (RRV).
    system246_rom_set set = system246_rom_loader::identify_set(rom_path);
    if (set == system246_rom_set::unknown)
        set = system246_rom_set::ridge_racer_v_arcade_battle;
    const std::string acgame =
        (set == system246_rom_set::motogp) ? kMotogpAcgame : kRrvAcgame;
    return {system246_rom_loader::set_short_name(set), acgame,
            system246_rom_loader::set_display_name(set), {}};
}

// Shared-object name; dlopen tries an absolute path next to the executable
// first, then the bare name (letting the loader search LD_LIBRARY_PATH/rpath).
constexpr const char* kModuleSoName = "libsystem246_pcsx2.so";

// present_rgba_frame's intended monitor aspect (PS2 480i output).
constexpr int kDisplayWidth = 640;
constexpr int kDisplayHeight = 480;

// Function-pointer types matching the C ABI in pcsx2_module.h.
using start_fn = int (*)(const char*, const char*, const char*);
using get_frame_fn = int (*)(const unsigned int**, int*, int*, unsigned long long*);
using set_input_fn = void (*)(const wa_pcsx2_input*);
using set_paused_fn = void (*)(int);
using boot_status_fn = const char* (*)(unsigned long long*, unsigned long long*);
using take_impact_fn = float (*)(void);
using stop_fn = void (*)(void);

// Directory containing the running executable, for locating the .so beside it.
std::string executable_dir() {
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return {};
    buf[n] = '\0';
    std::string path(buf);
    const std::string::size_type slash = path.find_last_of('/');
    if (slash == std::string::npos) return {};
    return path.substr(0, slash);
}

class system246_pcsx2_emulator final : public video_emulator_session {
public:
    system246_pcsx2_emulator(std::shared_ptr<arcade_video_worker> video,
                             std::shared_ptr<arcade_cabinet_state> cabinet)
        : video_emulator_session(arcade_board_type::system246,
                                 std::move(video), std::move(cabinet)) {}

    ~system246_pcsx2_emulator() override {
        auto diag = [](const char* m) {
            if (std::FILE* f = std::fopen("/tmp/rrv_diag.log", "a")) {
                std::fprintf(f, "session: %s\n", m);
                std::fclose(f);
            }
        };
        diag("dtor: begin (stopping VM)");
        if (m_stop) m_stop();
        diag("dtor: VM stopped; NOT dlclosing (keep module resident)");
        // NB: we deliberately do NOT dlclose the PCSX2 module. Running its
        // static destructors on unload can hang/crash teardown; keeping it
        // resident lets ESC return to the loader cleanly and lets the next
        // game reuse it. (wa_pcsx2_stop already tore the VM down.)
        join_unpack();
        diag("dtor: unpack joined");
        if (m_gpu_renderer) m_gpu_renderer->reset_session();
        diag("dtor: renderer reset");
        if (m_input) m_input->shutdown();
        diag("dtor: done");
    }

    bool initialize(const std::string& rom_path, const std::string& bios_path,
                    const emulator_settings& settings) override {
        (void)bios_path;

        // Pick the selected game's PCSX2 manifest (rom_path routed here by the
        // app-wide board dispatch); unknown selections fall back to RRV.
        const system246_game game = resolve_game(rom_path);

        if (!m_gpu_renderer->initialize(settings)) {
            std::fprintf(stderr, "Failed to initialize MANX video\n");
            return false;
        }
        m_loading_title = game.display_name;
        m_game_short_name = game.short_name;
        m_started = std::chrono::steady_clock::now();

        // Gun cabinets swap the desktop pointer for the on-screen sight, the
        // same as the Model 2 and System 22 gun games.
        m_gpu_renderer->set_lightgun_cursor(
            arcade_input::is_lightgun_game(game.short_name));

        const auto trim = [](const char* name, float fallback) {
            const char* text = std::getenv(name);
            return text && *text ? std::strtof(text, nullptr) : fallback;
        };

        m_gun_scale_x = trim("MANX_GUN_SCALE_X", 1.0f);
        m_gun_scale_y = trim("MANX_GUN_SCALE_Y", 1.0f);
        m_gun_offset_x = trim("MANX_GUN_OFFSET_X", 0.0f);
        m_gun_offset_y = trim("MANX_GUN_OFFSET_Y", 0.0f);

        m_input = std::make_unique<arcade_input>();
        if (!m_input->initialize(game.short_name))
            std::fprintf(stderr,
                         "System 246 input failed; controls remain neutral\n");

        if (!load_module()) {
            std::fprintf(stderr,
                         "System 246: could not load %s; %s unavailable\n",
                         kModuleSoName, game.display_name.c_str());
            return false;
        }

        // A squashed game unpacks on a worker thread (with progress shown on
        // the loading screen); once it finishes we hand the extracted acgame
        // to the module. A cached copy resolves immediately so no unpack runs.
        m_pending_game = game;
        if (!game.acgame.empty()) {
            if (m_start(game.acgame.c_str(), kPcsx2BiosDir, kPcsx2BinDir) != 1) {
                std::fprintf(stderr,
                             "System 246: PCSX2 module failed to start\n");
                return false;
            }
            std::printf("System 246: %s booting via isolated PCSX2 module "
                        "(surfaceless): %s\n",
                        game.display_name.c_str(), game.acgame.c_str());
        } else {
            // No cached unpack: show an unpacking loading screen first.
            m_pending_game = game;
            m_loading_title = game.display_name;
            m_game_short_name = game.short_name;
            m_started = std::chrono::steady_clock::now();
            begin_unpack();
        }
        return true;
    }

    void run_frame() override {
        update_input();

        // A squashed game is unpacking on a worker thread: keep drawing the
        // loading screen with the real unpack percentage until it finishes,
        // then hand the extracted manifest to the module and boot it.
        if (!m_boot_started && m_unpack_thread.joinable()) {
            const float progress =
                m_unpack_progress.load(std::memory_order_relaxed);
            const double waited = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - m_started).count();
            system246_loading::render(m_loading_pixels, kDisplayWidth,
                                      kDisplayHeight, m_loading_title, waited,
                                      "UNPACKING GAME", 0, progress);
            m_gpu_renderer->present_rgba_frame(
                reinterpret_cast<const std::uint8_t*>(m_loading_pixels.data()),
                kDisplayWidth, kDisplayHeight, kDisplayWidth, kDisplayHeight);

            // Once extraction is done, boot the machine and let the normal
            // loading screen take over below.
            if (m_unpack_done.load(std::memory_order_acquire))
                start_module();
            ++m_frames_polled;
            return;
        }

        const unsigned int* pixels = nullptr;
        int width = 0;
        int height = 0;
        unsigned long long sequence = 0;
        if (m_get_frame && m_get_frame(&pixels, &width, &height, &sequence) &&
            pixels && width > 0 && height > 0) {
            m_gpu_renderer->present_rgba_frame(
                reinterpret_cast<const std::uint8_t*>(pixels), width, height,
                kDisplayWidth, kDisplayHeight);
            ++m_frames_presented;
            m_game_has_drawn = true;
        } else if (!m_game_has_drawn) {
            // Nothing from the game yet. These boards take the best part of a
            // minute to read their disc and start drawing, so show that the
            // machine is alive rather than leaving a black screen that reads
            // as a hang.
            const double waited = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - m_started).count();
            const char* status = "Starting the emulated board";
            unsigned long long produced = 0;
            unsigned long long drawn = 0;
            if (m_boot_status)
                status = m_boot_status(&produced, &drawn);
            system246_loading::render(m_loading_pixels, kDisplayWidth,
                                      kDisplayHeight, m_loading_title, waited,
                                      status, produced);
            m_gpu_renderer->present_rgba_frame(
                reinterpret_cast<const std::uint8_t*>(m_loading_pixels.data()),
                kDisplayWidth, kDisplayHeight, kDisplayWidth, kDisplayHeight);
        }
        // The module logs what it captured; this logs what actually reached
        // the renderer, so a lost picture can be placed on one side or the
        // other of the module boundary. One line every ~3 seconds.
        if ((++m_frames_polled % 180) == 0) {
            std::printf("SYSTEM246 frames: polled=%llu presented=%llu "
                        "last=%dx%d seq=%llu\n",
                        static_cast<unsigned long long>(m_frames_polled),
                        static_cast<unsigned long long>(m_frames_presented),
                        width, height,
                        static_cast<unsigned long long>(sequence));
            std::fflush(stdout);
        }
        if (m_take_impact && m_game_has_drawn)
            arcade_feedback::publish_impact(m_take_impact());
    }

    void reload_input_mappings() override {
        if (m_input) m_input->reload_mappings();
    }
    double frame_seconds() const override { return 1.0 / 60.0; }
    // The module's frame getter is non-blocking, so run_frame() never stalls;
    // main() paces this board normally like the other emulated displays.
    bool producer_paced() const override { return false; }

protected:
    // Pause/resume the emulated machine itself (audio + emulation), not just
    // presentation -- routed into the isolated module's VMManager pause.
    void set_audio_paused(bool paused) override {
        // The frame loop calls this every frame with the current state, not
        // just on a change. Every other board takes that cheaply (it only
        // gates an audio queue), but here it reaches VMManager::SetPaused and
        // logged a VM state transition sixty times a second. Forward edges
        // only.
        if (!m_set_paused || paused == m_paused) return;
        m_paused = paused;
        m_set_paused(paused ? 1 : 0);
    }

private:
    bool load_module() {
        const std::string local = executable_dir().empty()
                                      ? std::string()
                                      : executable_dir() + "/" + kModuleSoName;
        if (!local.empty())
            m_module = dlopen(local.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!m_module)
            m_module = dlopen(kModuleSoName, RTLD_NOW | RTLD_LOCAL);
        if (!m_module) {
            std::fprintf(stderr, "System 246: dlopen failed: %s\n", dlerror());
            return false;
        }

        m_start = reinterpret_cast<start_fn>(dlsym(m_module, "wa_pcsx2_start"));
        m_get_frame =
            reinterpret_cast<get_frame_fn>(dlsym(m_module, "wa_pcsx2_get_frame"));
        m_set_input =
            reinterpret_cast<set_input_fn>(dlsym(m_module, "wa_pcsx2_set_input"));
        m_stop = reinterpret_cast<stop_fn>(dlsym(m_module, "wa_pcsx2_stop"));
        m_set_paused =
            reinterpret_cast<set_paused_fn>(dlsym(m_module, "wa_pcsx2_set_paused"));
        m_boot_status = reinterpret_cast<boot_status_fn>(
            dlsym(m_module, "wa_pcsx2_boot_status"));
        m_take_impact = reinterpret_cast<take_impact_fn>(
            dlsym(m_module, "wa_pcsx2_take_impact"));

        if (!m_start || !m_get_frame || !m_set_input || !m_stop ||
            !m_take_impact) {
            std::fprintf(stderr,
                         "System 246: missing wa_pcsx2_* symbol in module\n");
            dlclose(m_module);
            m_module = nullptr;
            return false;
        }
        return true;
    }

    void update_input() {
        if (!m_input || !m_set_input) return;
        m_input->set_suppressed(m_gpu_renderer->input_suppressed());
        // Tell the gun where the picture actually is before reading the mouse.
        int px = 0, py = 0, pw = 0, ph = 0, dw = 0, dh = 0;
        if (m_gpu_renderer->picture_rect(px, py, pw, ph, dw, dh))
            m_input->set_picture_rect(px, py, pw, ph, dw, dh);
        m_input->update();
        const input_state& s = m_input->state();

        // input_state driving axes are 12-bit (steering centred at 0x800). The
        // module exposes only on/off cabinet switches, so translate the analog
        // axes into digital steer/gas/brake here.
        constexpr std::uint16_t center = 0x800;
        constexpr std::uint16_t steer_deadzone = 0x100;
        constexpr std::uint16_t pedal_threshold = 0x180;

        wa_pcsx2_input in{};
        in.steer_left = (s.steering + steer_deadzone < center) ? 1 : 0;
        in.steer_right = (s.steering > center + steer_deadzone) ? 1 : 0;
        in.gas = (s.gas > pedal_threshold) ? 1 : 0;
        in.brake = (s.brake > pedal_threshold) ? 1 : 0;
        in.start = s.start ? 1 : 0;
        in.service = s.service ? 1 : 0;
        in.gear_up = s.shift_up ? 1 : 0;
        in.gear_down = s.shift_down ? 1 : 0;
        in.view = s.view ? 1 : 0;

        // Fighting cabinets (Tekken and friends) read a lever and attack
        // switches per player instead of the drive board. The neutral input
        // model carries those as the action-board stick and buttons, using the
        // same <0x60 / >0x90 thresholds the other action boards use.
        constexpr std::uint8_t low = 0x60;
        constexpr std::uint8_t high = 0x90;
        // A gun cabinet carries the aim on the same axes a fighting cabinet
        // uses for its lever, so reading the lever from them would jam a
        // direction on whenever the player aims away from the middle of the
        // screen - which is most of the time, and is why the service menu
        // could not be navigated. Gun games therefore take their direction
        // switches from the second stick (the arrow keys by default), leaving
        // the first free for the gun.
        const bool gun_cabinet =
            arcade_input::is_lightgun_game(m_game_short_name);
        const std::uint8_t stick_x[2]{
            gun_cabinet ? s.right_stick_x : s.left_stick_x, s.p2_stick_x};
        const std::uint8_t stick_y[2]{
            gun_cabinet ? s.right_stick_y : s.left_stick_y, s.p2_stick_y};
        const std::uint8_t* const panel_buttons[2]{s.buttons, s.p2_buttons};
        for (int player = 0; player < 2; ++player) {
            in.lever_up[player] = stick_y[player] < low ? 1 : 0;
            in.lever_down[player] = stick_y[player] > high ? 1 : 0;
            in.lever_left[player] = stick_x[player] < low ? 1 : 0;
            in.lever_right[player] = stick_x[player] > high ? 1 : 0;
            // A gun cabinet's panel is a trigger and a pedal and nothing else,
            // and those are sent as the gun's own switches below. Sending the
            // fighting-panel attack buttons from the same physical inputs made
            // one trigger pull close two JVS switches at once - the trigger and
            // button 1 - and Time Crisis reads button 1 as "exit", so pulling
            // the trigger left the gun-calibration screen instead of firing.
            for (int index = 0; index < 6; ++index)
                in.attack[player][index] = !gun_cabinet &&
                    panel_buttons[player][index] ? 1 : 0;
            // A gun cabinet still needs one ordinary button for the test menu,
            // which reads it as "select". It comes from its own key rather
            // than the trigger's: driving both from one input closed two
            // switches per shot, and the gun calibration reads the second as
            // "exit", so pulling the trigger cancelled the calibration.
            if (gun_cabinet)
                in.attack[player][0] = panel_buttons[player][3] ? 1 : 0;
        }
        in.p2_start = s.p2_start ? 1 : 0;
        in.test = s.test ? 1 : 0;

        // Light-gun cabinets (Time Crisis 3/4). arcade_input already turns the
        // mouse into a full-range gun position for these games, carried on the
        // same stick axes; convert that to the fraction-of-picture the board
        // wants. Trigger is button 0, pedal button 1 (Time Crisis' cover
        // pedal), and button 2 forces the screen-lost report to reload.
        const std::uint8_t gun_x[2]{s.left_stick_x, s.p2_stick_x};
        const std::uint8_t gun_y[2]{s.left_stick_y, s.p2_stick_y};
        // Aim trim. A cabinet's I/O board does not always report the picture
        // over the full coordinate range - Time Crisis 3's board describes a
        // 640x448 field, for instance - so the fraction the host measures
        // against the window can land somewhere else on screen. These scale
        // about the centre of the picture and then shift, so a shot that is
        // consistently high and left is pulled back with a scale above one.
        for (int player = 0; player < 2; ++player) {
            in.gun_aimed[player] = 1;
            in.gun_x[player] = 0.5f + (static_cast<float>(gun_x[player]) /
                                       255.0f - 0.5f) * m_gun_scale_x + m_gun_offset_x;
            in.gun_y[player] = 0.5f + (static_cast<float>(gun_y[player]) /
                                       255.0f - 0.5f) * m_gun_scale_y + m_gun_offset_y;
            in.gun_trigger[player] = panel_buttons[player][0] ? 1 : 0;
            in.gun_offscreen[player] = panel_buttons[player][1] ? 1 : 0;
            in.gun_pedal[player] = panel_buttons[player][2] ? 1 : 0;
        }

        // Coins arrive as short pulses; insert exactly one per rising edge so
        // a held key does not stuff the coin counter.
        in.coin_pulse = (s.coin1 && !m_prev_coin) ? 1 : 0;
        m_prev_coin = s.coin1;
        in.p2_coin_pulse = (s.coin2 && !m_prev_coin2) ? 1 : 0;
        m_prev_coin2 = s.coin2;

        m_set_input(&in);
    }

    std::unique_ptr<arcade_input> m_input;
    void* m_module{nullptr};
    start_fn m_start{nullptr};
    get_frame_fn m_get_frame{nullptr};
    set_input_fn m_set_input{nullptr};
    stop_fn m_stop{nullptr};
    set_paused_fn m_set_paused{nullptr};
    boot_status_fn m_boot_status{nullptr};
    take_impact_fn m_take_impact{nullptr};
    bool m_prev_coin{false};
    bool m_prev_coin2{false};
    bool m_paused{false};
    unsigned long long m_frames_polled{0};
    unsigned long long m_frames_presented{0};
    bool m_game_has_drawn{false};
    std::chrono::steady_clock::time_point m_started{
        std::chrono::steady_clock::now()};
    std::string m_loading_title{"SYSTEM 246"};
    std::string m_game_short_name;
    std::vector<std::uint32_t> m_loading_pixels;
    float m_gun_scale_x{1.0f};
    float m_gun_scale_y{1.0f};
    float m_gun_offset_x{0.0f};
    float m_gun_offset_y{0.0f};

    // The game being launched. For a squashed pack this starts empty (unpack
    // pending) and is populated on the worker thread.
    system246_game m_pending_game;
    // The unpack thread's ready acgame path and error, published on completion.
    std::atomic<float> m_unpack_progress{-1.0f};
    std::atomic<bool> m_unpack_done{false};
    std::thread m_unpack_thread;
    std::mutex mutable m_unpack_result_mutex;
    std::string m_unpack_acgame;
    std::string m_unpack_error;
    bool m_boot_started{false};

    // Static trampoline for the loader's C progress callback.
    static void unpack_progress_cb(float fraction, void* user) {
        auto* self = static_cast<system246_pcsx2_emulator*>(user);
        self->m_unpack_progress.store(fraction, std::memory_order_relaxed);
    }

    // Begin unpacking a squashed game on a worker thread. No-op for a cached
    // (already-unpacked) game or if an unpack is already running.
    void begin_unpack() {
        if (m_pending_game.squashfs_path.empty()) return;
        if (m_unpack_thread.joinable()) return;
        const std::string image = m_pending_game.squashfs_path;
        m_unpack_progress.store(0.0f, std::memory_order_relaxed);
        m_unpack_done.store(false, std::memory_order_relaxed);
        m_unpack_thread = std::thread([this, image]() {
            std::string error;
            const std::string acgame =
                system246_rom_loader::squashfs_unpack(
                    image, &error, &system246_pcsx2_emulator::unpack_progress_cb,
                    this);
            {
                std::lock_guard<std::mutex> lock(m_unpack_result_mutex);
                m_unpack_acgame = acgame;
                m_unpack_error = error;
            }
            m_unpack_progress.store(1.0f, std::memory_order_relaxed);
            m_unpack_done.store(true, std::memory_order_release);
        });
    }

    // Boot the emulated machine with the unpacked (or directly-resolved)
    // acgame manifest. Returns false only when the module fails to start.
    bool start_module() {
        if (m_boot_started) return true;
        std::string error;
        const std::string acgame = unpack_result(&error);
        if (acgame.empty()) {
            std::fprintf(stderr, "System 246: unpack produced no acgame: %s\n",
                         error.c_str());
            return false;
        }
        m_pending_game.acgame = acgame;
        if (m_start(acgame.c_str(), kPcsx2BiosDir, kPcsx2BinDir) != 1) {
            std::fprintf(stderr, "System 246: PCSX2 module failed to start\n");
            return false;
        }
        m_boot_started = true;
        m_game_short_name = m_pending_game.short_name;
        m_loading_title = m_pending_game.display_name;
        m_started = std::chrono::steady_clock::now();
        std::printf("System 246: %s booting via isolated PCSX2 module "
                    "(surfaceless): %s\n",
                    m_pending_game.display_name.c_str(), acgame.c_str());
        return true;
    }

    // The unpacked acgame path once the worker has finished. Empty (with the
    // error set) when unpack is still running or failed.
    std::string unpack_result(std::string* error_out) const {
        if (error_out) error_out->clear();
        if (!m_unpack_done.load(std::memory_order_acquire)) return {};
        std::lock_guard<std::mutex> lock(m_unpack_result_mutex);
        if (error_out) *error_out = m_unpack_error;
        return m_unpack_acgame;
    }

    // Join and reap the unpack worker (e.g. at teardown).
    void join_unpack() {
        if (m_unpack_thread.joinable()) m_unpack_thread.join();
    }
};

} // namespace

std::unique_ptr<emulator_session> make_system246_session(
    std::shared_ptr<arcade_video_worker> video,
    std::shared_ptr<arcade_cabinet_state> cabinet) {
    return std::make_unique<system246_pcsx2_emulator>(std::move(video),
                                                      std::move(cabinet));
}
