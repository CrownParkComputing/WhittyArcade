// WhittyArcade System 246/256 board bring-up: boot Ridge Racer V Arcade Battle
// in-process via the embedded PCSX2 arcade-fork core, rendering into a real
// SDL3 window with Vulkan and real (cubeb) audio.
//
// This is the next milestone after pcsx2_link_test.cpp. Where the link test
// only stands the PCSX2 CPU thread up and down headlessly, this harness:
//   * creates a real, resizable Vulkan-capable window with SDL3 and hands its
//     native (X11/Wayland) handle to the PCSX2 GS thread via the WhittyArcade
//     host hooks in pcsx2_host.cpp,
//   * configures the in-memory settings layer for the Vulkan HW renderer and
//     cubeb audio output, folders pointed at the PCSX2 arcade build tree,
//   * boots the RRV arcade manifest (rrvac.acgame) which VMManager auto-detects
//     as a System 246 arcade game, and
//   * runs the CPU-thread lifecycle on a dedicated std::thread while the main
//     thread pumps the SDL event loop, mirroring pcsx2-gsrunner's structure.
//
// The boot/init/shutdown sequence is adapted from PCSX2's headless reference
// frontend (pcsx2x6/pcsx2-gsrunner/Main.cpp, CPUThreadMain) and the windowed
// frontend (pcsx2x6/pcsx2-qt, EmuThread::startVM / Host::AcquireRenderWindow).

#include "pcsx2/PrecompiledHeader.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

// Keep SDL from hijacking main() -- we provide our own.
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>

#include "common/Console.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/MemorySettingsInterface.h"
#include "common/WindowInfo.h"

#include "pcsx2/Config.h" // EmuFolders, GSRendererType
#include "pcsx2/Host.h"
#include "pcsx2/ImGui/ImGuiManager.h"
#include "pcsx2/VMManager.h"

#include "pcsx2_input.h"

// ---------------------------------------------------------------------------
// Host hooks implemented in pcsx2_host.cpp (compiled into the same target).
// ---------------------------------------------------------------------------
namespace WhittyArcadeHost
{
	void SetRenderWindow(const WindowInfo& wi);
	void ClearRenderWindow();
	void SetCPUThreadId(std::thread::id id);
} // namespace WhittyArcadeHost

// ---------------------------------------------------------------------------
// Fixed paths into the PCSX2 arcade build tree.
// ---------------------------------------------------------------------------
static constexpr const char* PCSX2_BIN_DIR = "/home/jon/pcsx2x6/build/bin";
static constexpr const char* PCSX2_BIOS_DIR = "/home/jon/pcsx2x6/build/bin/bios";
static constexpr const char* RRV_ACGAME = "/home/jon/pcsx2x6/build/bin/roms/rrvac/rrvac.acgame";

static constexpr int WINDOW_WIDTH = 1280;
static constexpr int WINDOW_HEIGHT = 720;

static MemorySettingsInterface s_settings_interface;
static SDL_Window* s_window = nullptr;
static std::atomic<bool> s_cpu_thread_done{false};

// ---------------------------------------------------------------------------
// Config / settings
// ---------------------------------------------------------------------------

static bool InitializeConfig()
{
	// Resolve the standard folder layout, but force the app root at the PCSX2
	// arcade build tree so resources/, bios/, roms/ and the portable data root
	// (portable.ini lives in build/bin) all resolve there -- matching the
	// known-good standalone pcsx2-qt run.
	EmuFolders::SetAppRoot();
	EmuFolders::AppRoot = PCSX2_BIN_DIR;

	if (!EmuFolders::SetResourcesDirectory())
	{
		Console.Error("SetResourcesDirectory() failed.");
		return false;
	}
	if (!EmuFolders::SetDataDirectory(nullptr))
	{
		Console.Error("SetDataDirectory() failed.");
		return false;
	}

	const char* hw_error = nullptr;
	if (!VMManager::PerformEarlyHardwareChecks(&hw_error))
	{
		Console.ErrorFmt("PerformEarlyHardwareChecks failed: {}", hw_error ? hw_error : "(unknown)");
		return false;
	}

	// Load the ImGui font, needed by the OSD/GS once a real GS device exists.
	{
		const std::string roboto_path =
			EmuFolders::GetOverridableResourcePath("fonts" FS_OSPATH_SEPARATOR_STR "Roboto-Regular.ttf");
		const auto roboto_data = FileSystem::MapBinaryFileForRead(roboto_path.c_str());
		if (roboto_data.empty())
		{
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
	}

	// Everything lives in memory -- no ini path, nothing written to disk.
	MemorySettingsInterface& si = s_settings_interface;
	Host::Internal::SetBaseSettingsLayer(&si);
	VMManager::SetDefaultSettings(si, true, true, true, true, true);
	VMManager::Internal::LoadStartupSettings();
	return true;
}

static void ApplyBootSettings()
{
	MemorySettingsInterface& si = s_settings_interface;

	// Vulkan hardware renderer, drawing into our SDL window.
	si.SetIntValue("EmuCore/GS", "Renderer", static_cast<int>(GSRendererType::VK));

	// Real audio out via cubeb (NOT the null backend). "Backend" = "Cubeb" is
	// this fork's replacement for the legacy SPU2/Output OutputModule key.
	si.SetStringValue("SPU2/Output", "Backend", "Cubeb");
	si.SetBoolValue("SPU2/Output", "OutputMuted", false);

	// Run at real hardware speed: frame limiter on, host vsync off (the limiter
	// paces us, and vsync-off avoids being gated by the compositor).
	si.SetBoolValue("EmuCore/GS", "FrameLimitEnable", true);
	si.SetIntValue("EmuCore/GS", "VsyncEnable", 0);

	// No input sources: the main thread already owns the SDL event pump, so we
	// must not let PCSX2's SDL input source pump SDL events on the CPU thread.
	si.SetBoolValue("InputSources", "SDL", false);
	si.SetBoolValue("InputSources", "XInput", false);

	// A little on-screen feedback.
	si.SetBoolValue("EmuCore/GS", "OsdShowFPS", true);
	si.SetBoolValue("EmuCore/GS", "OsdShowResolution", true);

	// BIOS discovery for the arcade build.
	EmuFolders::Bios = PCSX2_BIOS_DIR;
}

// ---------------------------------------------------------------------------
// SDL window + native WindowInfo
// ---------------------------------------------------------------------------

static bool BuildWindowInfo(SDL_Window* window, WindowInfo* out_wi)
{
	WindowInfo wi;

	int w = WINDOW_WIDTH;
	int h = WINDOW_HEIGHT;
	SDL_GetWindowSizeInPixels(window, &w, &h);
	wi.surface_width = static_cast<u32>(w);
	wi.surface_height = static_cast<u32>(h);
	wi.surface_scale = 1.0f;

	const SDL_PropertiesID props = SDL_GetWindowProperties(window);
	const char* driver = SDL_GetCurrentVideoDriver();

	if (driver && SDL_strcmp(driver, "x11") == 0)
	{
		void* display = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
		const Sint64 xid = SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
		if (!display || xid == 0)
		{
			Console.Error("Failed to obtain X11 display/window handle from SDL.");
			return false;
		}
		wi.type = WindowInfo::Type::X11;
		wi.display_connection = display;
		wi.window_handle = reinterpret_cast<void*>(static_cast<uintptr_t>(xid));
	}
	else if (driver && SDL_strcmp(driver, "wayland") == 0)
	{
		void* display = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
		void* surface = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
		if (!display || !surface)
		{
			Console.Error("Failed to obtain Wayland display/surface handle from SDL.");
			return false;
		}
		wi.type = WindowInfo::Type::Wayland;
		wi.display_connection = display;
		wi.window_handle = surface; // PCSX2's Vulkan swapchain reads wl_surface from window_handle
	}
	else
	{
		Console.ErrorFmt("Unsupported SDL video driver '{}' (need x11 or wayland).", driver ? driver : "(null)");
		return false;
	}

	*out_wi = wi;
	return true;
}

static bool CreateWindowAndRegister()
{
	if (!SDL_Init(SDL_INIT_VIDEO))
	{
		Console.ErrorFmt("SDL_Init(SDL_INIT_VIDEO) failed: {}", SDL_GetError());
		return false;
	}

	// Fullscreen like every other board. WINDOW_WIDTH/HEIGHT only name the
	// size the window would fall back to if fullscreen were toggled off.
	s_window = SDL_CreateWindow("WhittyArcade - Ridge Racer V Arcade Battle", WINDOW_WIDTH, WINDOW_HEIGHT,
		SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_FULLSCREEN);
	if (!s_window)
	{
		Console.ErrorFmt("SDL_CreateWindow failed: {}", SDL_GetError());
		return false;
	}

	WindowInfo wi;
	if (!BuildWindowInfo(s_window, &wi))
		return false;

	WhittyArcadeHost::SetRenderWindow(wi);
	return true;
}

static void RefreshWindowInfoOnResize()
{
	if (!s_window)
		return;

	WindowInfo wi;
	if (BuildWindowInfo(s_window, &wi))
		WhittyArcadeHost::SetRenderWindow(wi);
}

// ---------------------------------------------------------------------------
// Keyboard -> cabinet controls (RRV: racing / FCA-1 drive board)
//
//   Arrows or WASD : steer (L/R), accelerate (up/W), brake (down/S)
//   E / Q          : gear up / gear down
//   V              : view change
//   Enter or 1     : START            9 : SERVICE (test-menu navigate)
//   5              : insert coin       Esc : quit
//
// Held state is idempotent, so re-asserting it on key-repeat is harmless; coin
// is edge-triggered (once per physical press).
// ---------------------------------------------------------------------------

static void HandleKey(SDL_Scancode sc, bool down)
{
	using namespace WhittyArcadeInput;
	switch (sc)
	{
		case SDL_SCANCODE_LEFT:
		case SDL_SCANCODE_A: SetSteerLeft(down); break;
		case SDL_SCANCODE_RIGHT:
		case SDL_SCANCODE_D: SetSteerRight(down); break;
		case SDL_SCANCODE_UP:
		case SDL_SCANCODE_W: SetGas(down); break;
		case SDL_SCANCODE_DOWN:
		case SDL_SCANCODE_S: SetBrake(down); break;
		case SDL_SCANCODE_E: SetGearUp(down); break;
		case SDL_SCANCODE_Q: SetGearDown(down); break;
		case SDL_SCANCODE_V: SetView(down); break;
		case SDL_SCANCODE_RETURN:
		case SDL_SCANCODE_1: SetStart(down); break;
		case SDL_SCANCODE_9: SetService(down); break;
		default: break;
	}
}

// ---------------------------------------------------------------------------
// CPU thread (Execute() blocks here until the VM stops)
// ---------------------------------------------------------------------------

static void CPUThreadEntry(VMBootParameters params, std::atomic<int>* ret)
{
	ret->store(EXIT_FAILURE);
	WhittyArcadeHost::SetCPUThreadId(std::this_thread::get_id());

	if (VMManager::Internal::CPUThreadInitialize())
	{
		// Pick up the renderer/audio/settings changes applied above.
		VMManager::ApplySettings();

		Error error;
		if (VMManager::Initialize(params, &error) == VMBootResult::StartupSuccess)
		{
			VMManager::SetState(VMState::Running);
			while (VMManager::GetState() == VMState::Running)
				VMManager::Execute();
			VMManager::Shutdown(false);
			ret->store(EXIT_SUCCESS);
		}
		else
		{
			Console.ErrorFmt("VMManager::Initialize failed: {}", error.GetDescription());
		}
	}
	else
	{
		Console.Error("VMManager::Internal::CPUThreadInitialize() failed.");
	}

	VMManager::Internal::CPUThreadShutdown();
	s_cpu_thread_done.store(true);

	// Nudge the main-thread SDL event loop so it wakes and joins us.
	SDL_Event quit_event{};
	quit_event.type = SDL_EVENT_QUIT;
	SDL_PushEvent(&quit_event);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main(int /*argc*/, char* /*argv*/[])
{
	Log::SetConsoleOutputLevel(LOGLEVEL_DEBUG);

	if (!InitializeConfig())
	{
		Console.Error("Failed to initialize PCSX2 config.");
		return EXIT_FAILURE;
	}

	ApplyBootSettings();

	if (!CreateWindowAndRegister())
	{
		Console.Error("Failed to create render window.");
		return EXIT_FAILURE;
	}

	VMBootParameters params;
	params.filename = RRV_ACGAME; // VMManager auto-detects the .acgame arcade manifest
	params.fullscreen = false;

	std::atomic<int> thread_ret{EXIT_FAILURE};
	std::thread cpu_thread(CPUThreadEntry, params, &thread_ret);

	// Main-thread SDL event loop.
	bool running = true;
	while (running && !s_cpu_thread_done.load())
	{
		SDL_Event ev;
		if (!SDL_WaitEventTimeout(&ev, 100))
			continue;

		do
		{
			switch (ev.type)
			{
				case SDL_EVENT_QUIT:
				case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
					running = false;
					break;

				case SDL_EVENT_WINDOW_RESIZED:
				case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
					RefreshWindowInfoOnResize();
					break;

				case SDL_EVENT_KEY_DOWN:
					if (ev.key.scancode == SDL_SCANCODE_ESCAPE)
						running = false;
					else if (ev.key.scancode == SDL_SCANCODE_5)
					{
						if (!ev.key.repeat)
							WhittyArcadeInput::InsertCoin();
					}
					else
						HandleKey(ev.key.scancode, true);
					break;

				case SDL_EVENT_KEY_UP:
					HandleKey(ev.key.scancode, false);
					break;

				default:
					break;
			}
		} while (SDL_PollEvent(&ev));
	}

	// Ask the VM to stop (if it hasn't already) and wait for the CPU thread.
	if (!s_cpu_thread_done.load())
		VMManager::SetState(VMState::Stopping);
	cpu_thread.join();

	WhittyArcadeHost::ClearRenderWindow();
	if (s_window)
	{
		SDL_DestroyWindow(s_window);
		s_window = nullptr;
	}
	SDL_Quit();

	return thread_ret.load();
}
