// MANX System 246 board: PCSX2 arcade-core module.
//
// This translation unit is the ONLY place PCSX2 headers/libraries are pulled in.
// It is compiled with clang++ (matching the ABI libpcsx2.a was built with) and
// linked, together with pcsx2_host.cpp, into libsystem246_pcsx2.so. The main
// MANX binary (GCC) never links any PCSX2 symbol; it dlopen()s this .so
// and drives it purely through the flat C ABI declared in pcsx2_module.h. This
// keeps PCSX2's bundled glad GL loader and static initialisers out of the main
// binary, where they collided with WhittyArcade's GLEW-based OpenGL renderer.
//
// The boot/threading sequence here (initialize_pcsx2_config / apply_boot_settings
// / cpu_thread_main) is lifted verbatim from the previous in-process session so
// behaviour is unchanged; only the entry points are now the four wa_pcsx2_*
// exports instead of a C++ session class.

#include "pcsx2/PrecompiledHeader.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "common/Console.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/MemorySettingsInterface.h"
#include "common/Path.h"

#include "pcsx2/Config.h" // EmuFolders, GSRendererType
#include "pcsx2/Host.h"
#include "pcsx2/ImGui/ImGuiManager.h"
#include "pcsx2/VMManager.h"

#include "pcsx2_host.h"   // WhittyArcadeHost frame capture + CPU-thread id hook
#include "pcsx2_input.h"  // WhittyArcadeInput cabinet setters
#include "pcsx2_module.h" // the flat C ABI we export

namespace {

// Bring-up diagnostic: append a checkpoint line to a fixed log so a failing boot
// can be traced without a terminal. Harmless in production.
void rrv_diag(const char* msg) {
    if (std::FILE* f = std::fopen("/tmp/rrv_diag.log", "a")) {
        std::fprintf(f, "%s\n", msg);
        std::fclose(f);
    }
    std::fprintf(stderr, "[rrv] %s\n", msg);
    std::fflush(stderr);
}

// ---------------------------------------------------------------------------
// Module state (single VM per process).
// ---------------------------------------------------------------------------
MemorySettingsInterface g_settings;
std::thread g_cpu_thread;
std::atomic<bool> g_vm_running{false};

// Boot parameters captured from wa_pcsx2_start (must outlive the CPU thread).
std::string g_bin_dir;
std::string g_bios_dir;
std::string g_acgame;

// Host-owned copy of the most-recently fetched frame. wa_pcsx2_get_frame returns
// a pointer into this buffer; only the (single) host caller touches it, so the
// returned pointer stays valid until the next wa_pcsx2_get_frame call.
std::vector<std::uint32_t> g_host_frame;
std::uint64_t g_host_frame_seq = 0;

bool initialize_pcsx2_config() {
    // Force the app root at the PCSX2 arcade build tree so resources/, bios/,
    // roms/ and the portable data root all resolve there.
    EmuFolders::SetAppRoot();
    EmuFolders::AppRoot = g_bin_dir;

    if (!EmuFolders::SetResourcesDirectory()) {
        Console.Error("SetResourcesDirectory() failed.");
        return false;
    }
    if (!EmuFolders::SetDataDirectory(nullptr)) {
        Console.Error("SetDataDirectory() failed.");
        return false;
    }
    // Portable mode is detected from the *executable's* directory, which is now
    // WhittyArcade's (no portable.ini) -- so DataRoot defaults to the system
    // data dir and PCSX2 can't find the BIOS/memcards/dongle in the pcsx2x6
    // build tree. Pin DataRoot at that tree so every EmuFolders::LoadConfig
    // (including the one ApplySettings runs later) resolves bios/, memcards/,
    // cache/ under it.
    EmuFolders::DataRoot = g_bin_dir;
    rrv_diag(("initialize: DataRoot pinned to " + g_bin_dir).c_str());

    const char* hw_error = nullptr;
    if (!VMManager::PerformEarlyHardwareChecks(&hw_error)) {
        Console.ErrorFmt("PerformEarlyHardwareChecks failed: {}",
                         hw_error ? hw_error : "(unknown)");
        return false;
    }

    // The OSD/GS needs an ImGui font once a GS device exists.
    const std::string roboto_path = EmuFolders::GetOverridableResourcePath(
        "fonts" FS_OSPATH_SEPARATOR_STR "Roboto-Regular.ttf");
    const auto roboto_data = FileSystem::MapBinaryFileForRead(roboto_path.c_str());
    if (roboto_data.empty()) {
        Console.ErrorFmt("Failed to load font file '{}'.", roboto_path);
        return false;
    }
    std::vector<ImGuiManager::FontInfo> fonts;
    ImGuiManager::FontInfo fi{};
    fi.data = roboto_data;
    fi.exclude_ranges = {};
    fi.face_name = nullptr;
    fi.is_emoji_font = false;
    fonts.push_back(fi);
    ImGuiManager::SetFonts(std::move(fonts));

    // Everything lives in the in-memory settings layer; nothing on disk.
    Host::Internal::SetBaseSettingsLayer(&g_settings);
    VMManager::SetDefaultSettings(g_settings, true, true, true, true, true);
    VMManager::Internal::LoadStartupSettings();
    return true;
}

void apply_boot_settings() {
    MemorySettingsInterface& si = g_settings;

    // Vulkan hardware renderer, drawing offscreen (no registered window).
    // WHITTY_PCSX2_RENDERER=sw switches to the software rasteriser: if a game
    // draws under software but not hardware, the fault is in GS hardware
    // emulation rather than in the game, the media or our frame capture.
    const char* renderer_override = std::getenv("WHITTY_PCSX2_RENDERER");
    const bool software_renderer =
        renderer_override && (*renderer_override == 's' || *renderer_override == 'S');
    si.SetIntValue("EmuCore/GS", "Renderer",
                   static_cast<int>(software_renderer ? GSRendererType::SW
                                                      : GSRendererType::VK));

    // Real audio out via cubeb (this fork's SPU2 output backend key).
    si.SetStringValue("SPU2/Output", "Backend", "Cubeb");
    si.SetBoolValue("SPU2/Output", "OutputMuted", false);

    // Real hardware speed: frame limiter on, host vsync off (the limiter paces
    // us; there is no swapchain to vsync against anyway).
    si.SetBoolValue("EmuCore/GS", "FrameLimitEnable", true);
    si.SetIntValue("EmuCore/GS", "VsyncEnable", 0);

    // Rendering quality vs speed.
    //  - accurate_blending_unit = High (AccBlendLevel::High = 3): the RRV car
    //    paint/reflection shaders use blend/transparency effects that render as
    //    white checkerboard at the default (Basic) blending accuracy.
    //  - upscale_multiplier = 1.0: native internal resolution -> fastest render
    //    and smallest per-frame readback (we capture the frame to CPU each
    //    present, so internal res directly drives that cost). Bump later for
    //    sharper visuals once speed headroom is confirmed.
    //  - texture_preloading = Full (2): upload whole textures up front, avoids
    //    partial-texture artifacts and reduces per-draw uploads.
    si.SetIntValue("EmuCore/GS", "accurate_blending_unit", 4);
    si.SetFloatValue("EmuCore/GS", "upscale_multiplier", 1.0f);
    si.SetIntValue("EmuCore/GS", "texture_preloading", 2);

    // WhittyArcade owns the keyboard/pad; PCSX2's own SDL input source must not
    // pump SDL events on the CPU thread.
    si.SetBoolValue("InputSources", "SDL", false);
    si.SetBoolValue("InputSources", "XInput", false);

    // BIOS discovery for the arcade build. Set it as an ABSOLUTE path in the
    // settings layer too: LoadPathFromSettings uses an absolute value as-is, so
    // this survives the LoadConfig that ApplySettings runs on the CPU thread
    // (which would otherwise recompute Bios from DataRoot + "bios").
    si.SetStringValue("Folders", "Bios", g_bios_dir.c_str());
    EmuFolders::Bios = g_bios_dir;

    // Select the actual BIOS file. VMManager::Initialize only builds the BIOS
    // path when BaseFilenames.Bios ([Filenames]/BIOS) is non-empty; our fresh
    // in-memory settings never picked one, so PCSX2 reported "no BIOS" despite
    // the folder being correct. r27v1602f.7d is the System 246 arcade BIOS.
    si.SetStringValue("Filenames", "BIOS", "r27v1602f.7d");
}

void cpu_thread_main(VMBootParameters params) {
    WhittyArcadeHost::SetCPUThreadId(std::this_thread::get_id());
    rrv_diag("cpu_thread: start");

    if (VMManager::Internal::CPUThreadInitialize()) {
        rrv_diag("cpu_thread: CPUThreadInitialize ok");
        VMManager::ApplySettings();

        Error error;
        rrv_diag(("cpu_thread: EmuFolders::Bios=" + EmuFolders::Bios).c_str());
        rrv_diag("cpu_thread: calling VMManager::Initialize");
        if (VMManager::Initialize(params, &error) == VMBootResult::StartupSuccess) {
            rrv_diag("cpu_thread: VMManager::Initialize OK -> running");
            WhittyArcadeHost::SetBootStage(WhittyArcadeHost::BootStage::LoadingGame);
            g_vm_running.store(true, std::memory_order_release);
            VMManager::SetState(VMState::Running);
            while (VMManager::GetState() == VMState::Running)
                VMManager::Execute();
            VMManager::Shutdown(false);
            g_vm_running.store(false, std::memory_order_release);
        } else {
            rrv_diag("cpu_thread: VMManager::Initialize FAILED");
            rrv_diag(error.GetDescription().c_str());
            Console.ErrorFmt("VMManager::Initialize failed: {}", error.GetDescription());
        }
    } else {
        rrv_diag("cpu_thread: CPUThreadInitialize FAILED");
        Console.Error("VMManager::Internal::CPUThreadInitialize() failed.");
    }

    rrv_diag("cpu_thread: exiting (CPUThreadShutdown)");
    VMManager::Internal::CPUThreadShutdown();
}

} // namespace

// ===========================================================================
// Exported C ABI (see pcsx2_module.h)
// ===========================================================================

extern "C" int wa_pcsx2_start(const char* acgame, const char* bios_dir,
                              const char* bin_dir) {
    if (g_cpu_thread.joinable()) {
        rrv_diag("wa_pcsx2_start: already running");
        return 0;
    }

    std::remove("/tmp/rrv_diag.log");
    rrv_diag("wa_pcsx2_start: begin");

    g_acgame = acgame ? acgame : "";
    g_bios_dir = bios_dir ? bios_dir : "";
    g_bin_dir = bin_dir ? bin_dir : "";

    // Process-global set-up: resource and data folders, the hardware checks,
    // the ImGui font and the base settings layer. It must run exactly once.
    // Repeating it for a second game - the module stays resident after the
    // first one is stopped - hung inside PCSX2 before the VM was even asked
    // to boot, so selecting any System 246 game after the first opened no
    // screen at all. The per-game work below still runs on every launch.
    static bool configured = false;
    if (!configured) {
        if (!initialize_pcsx2_config()) {
            rrv_diag("wa_pcsx2_start: PCSX2 config FAILED");
            std::fprintf(stderr, "Failed to initialize PCSX2 config\n");
            return 0;
        }
        configured = true;
        rrv_diag("wa_pcsx2_start: PCSX2 config ok");
    } else {
        rrv_diag("wa_pcsx2_start: PCSX2 already configured; reusing");
    }
    WhittyArcadeHost::SetBootStage(WhittyArcadeHost::BootStage::Configuring);
    apply_boot_settings();
    rrv_diag("wa_pcsx2_start: boot settings applied, spawning CPU thread");
    WhittyArcadeHost::SetBootStage(WhittyArcadeHost::BootStage::StartingMachine);

    VMBootParameters params;
    params.filename = g_acgame; // VMManager auto-detects the .acgame
    params.fullscreen = false;

    g_cpu_thread = std::thread(cpu_thread_main, params);
    rrv_diag("wa_pcsx2_start: CPU thread spawned, returning 1");
    return 1;
}

extern "C" int wa_pcsx2_get_frame(const unsigned int** pixels, int* w, int* h,
                                  unsigned long long* seq) {
    int width = 0;
    int height = 0;
    std::uint64_t sequence = 0;
    if (!WhittyArcadeHost::GetLatestFrame(g_host_frame_seq, g_host_frame, width,
                                          height, sequence) ||
        width <= 0 || height <= 0) {
        return 0;
    }

    g_host_frame_seq = sequence;
    if (pixels)
        *pixels = reinterpret_cast<const unsigned int*>(g_host_frame.data());
    if (w) *w = width;
    if (h) *h = height;
    if (seq) *seq = sequence;
    return 1;
}

extern "C" void wa_pcsx2_set_input(const wa_pcsx2_input* in) {
    if (!in) return;
    using namespace WhittyArcadeInput;
    SetSteerLeft(in->steer_left != 0);
    SetSteerRight(in->steer_right != 0);
    SetGas(in->gas != 0);
    SetBrake(in->brake != 0);
    SetStart(in->start != 0);
    SetService(in->service != 0);
    SetGearUp(in->gear_up != 0);
    SetGearDown(in->gear_down != 0);
    SetView(in->view != 0);
    SetTest(in->test != 0);

    // Fighting panels. Player 1's start arrives twice - once as the driving
    // cabinet's start switch, once as player_start - so send the pair.
    SetPlayerStart(0, in->start != 0);
    SetPlayerStart(1, in->p2_start != 0);
    for (int player = 0; player < 2; ++player) {
        SetLever(player, in->lever_up[player] != 0, in->lever_down[player] != 0,
                 in->lever_left[player] != 0, in->lever_right[player] != 0);
        for (int index = 0; index < 6; ++index)
            SetAttack(player, index, in->attack[player][index] != 0);
    }

    for (int player = 0; player < 2; ++player) {
        SetGunAim(player, in->gun_aimed[player] != 0, in->gun_x[player],
                  in->gun_y[player]);
        SetGunTrigger(player, in->gun_trigger[player] != 0);
        SetGunPedal(player, in->gun_pedal[player] != 0);
        SetGunOffscreen(player, in->gun_offscreen[player] != 0);
    }

    if (in->coin_pulse != 0)
        InsertCoinSlot(0);
    if (in->p2_coin_pulse != 0)
        InsertCoinSlot(1);
}

extern "C" const char* wa_pcsx2_boot_status(unsigned long long* frames,
                                            unsigned long long* drawing) {
    std::uint64_t captured = 0;
    std::uint64_t with_content = 0;
    WhittyArcadeHost::GetCaptureCounts(captured, with_content);
    if (frames) *frames = captured;
    if (drawing) *drawing = with_content;

    using Stage = WhittyArcadeHost::BootStage;
    switch (WhittyArcadeHost::GetBootStage()) {
    case Stage::Starting:        return "Starting the emulated board";
    case Stage::Configuring:     return "Loading BIOS and dongle";
    case Stage::StartingMachine: return "Starting the virtual machine";
    case Stage::LoadingGame:     return "Reading game data from the disc";
    case Stage::Drawing:         return "Game running";
    }
    return "Starting the emulated board";
}

extern "C" void wa_pcsx2_set_paused(int paused) {
    if (!g_vm_running.load(std::memory_order_acquire))
        return;
    const bool want_paused = (paused != 0);
    // VM pause state must be toggled on the CPU thread.
    Host::RunOnCPUThread([want_paused]() { VMManager::SetPaused(want_paused); },
                         false);
}

extern "C" void wa_pcsx2_stop(void) {
    if (!g_cpu_thread.joinable())
        return;
    rrv_diag("wa_pcsx2_stop: stopping VM");
    VMManager::SetState(VMState::Stopping);
    g_cpu_thread.join();
    g_vm_running.store(false, std::memory_order_release);
    rrv_diag("wa_pcsx2_stop: CPU thread joined");
}
