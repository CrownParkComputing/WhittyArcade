// MANX System 246/256 board bring-up: minimal Host:: interface for the
// embedded PCSX2 core (libpcsx2.a).
//
// This is a link-foundation stub. It provides the Host:: (and a couple of
// InputManager::) symbols that libpcsx2.a references but does not define,
// adapted from PCSX2's headless reference frontend
// (pcsx2x6/pcsx2-gsrunner/Main.cpp). All GS-runner-specific frame dumping,
// GS statistics and perf-metric accumulation has been stripped -- these bodies
// exist only to satisfy the linker and return sane, inert defaults for a
// headless CPU-thread-only bring-up. No window, GS device or input source is
// created here.

#include "pcsx2/PrecompiledHeader.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "common/Assertions.h"
#include "common/Console.h"
#include "common/ProgressCallback.h"
#include "common/SmallString.h"
#include "common/WindowInfo.h"

#include "pcsx2/Achievements.h"
#include "pcsx2/GS.h"
#include "pcsx2/GS/GS.h"
#include "pcsx2/Host.h"
#include "pcsx2/ImGui/FullscreenUI.h"
#include "pcsx2/ImGui/ImGuiFullscreen.h"
#include "pcsx2/ImGui/ImGuiManager.h"
#include "pcsx2/Input/InputManager.h"
#include "pcsx2/VMManager.h"

#include "pcsx2/DEV9/ACJV.h"

#include "pcsx2_host.h"
#include "pcsx2_input.h"

// ---------------------------------------------------------------------------
// MANX host hooks
//
// State shared with a windowed front-end (e.g. pcsx2_boot_rrv.cpp). When a real
// render window has been registered, Host::AcquireRenderWindow hands the PCSX2
// GS thread a live X11/Wayland WindowInfo instead of a Surfaceless one. When no
// window is registered (the headless CPU-thread link test), everything falls
// back to Surfaceless so that target still builds/links unchanged.
//
// Host::RunOnCPUThread / Host::PumpMessagesOnCPUThread implement a minimal
// cross-thread work queue: the front-end's boot thread is the CPU thread, it
// drains the queue every VSync via PumpMessagesOnCPUThread (called from
// VMManager::Internal::PollInputOnCPUThread), and RunOnCPUThread either runs the
// work inline (if already on the CPU thread) or enqueues it.
// ---------------------------------------------------------------------------

namespace MANXHost
{
	void SetRenderWindow(const WindowInfo& wi);
	void ClearRenderWindow();
	void SetCPUThreadId(std::thread::id id);
} // namespace MANXHost

namespace
{
	std::mutex s_wa_window_mutex;
	std::optional<WindowInfo> s_wa_render_window;

	std::atomic<bool> s_wa_cpu_thread_id_valid{false};
	std::thread::id s_wa_cpu_thread_id;

	std::mutex s_wa_cpu_queue_mutex;
	std::vector<std::function<void()>> s_wa_cpu_queue;

	// Latest GS-produced frame, published by Host::BeginPresentFrame() (GS
	// thread) and consumed by the session's run_frame() (board thread).
	std::mutex s_wa_frame_mutex;
	std::condition_variable s_wa_frame_cv;
	std::vector<u32> s_wa_frame_pixels;
	int s_wa_frame_width = 0;
	int s_wa_frame_height = 0;
	std::uint64_t s_wa_frame_sequence = 0;
} // namespace

bool MANXHost::WaitForFrame(std::uint64_t last_sequence, int timeout_ms,
	std::vector<std::uint32_t>& out_pixels, int& out_width, int& out_height,
	std::uint64_t& out_sequence)
{
	std::unique_lock<std::mutex> lock(s_wa_frame_mutex);
	const bool have = s_wa_frame_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
		[last_sequence]() {
			return s_wa_frame_sequence != 0 && s_wa_frame_sequence != last_sequence;
		});
	if (!have)
		return false;

	out_pixels = s_wa_frame_pixels;
	out_width = s_wa_frame_width;
	out_height = s_wa_frame_height;
	out_sequence = s_wa_frame_sequence;
	return true;
}

bool MANXHost::GetLatestFrame(std::uint64_t last_sequence,
	std::vector<std::uint32_t>& out_pixels, int& out_width, int& out_height,
	std::uint64_t& out_sequence)
{
	std::lock_guard<std::mutex> lock(s_wa_frame_mutex);
	if (s_wa_frame_sequence == 0 || s_wa_frame_sequence == last_sequence)
		return false;

	out_pixels = s_wa_frame_pixels;
	out_width = s_wa_frame_width;
	out_height = s_wa_frame_height;
	out_sequence = s_wa_frame_sequence;
	return true;
}

namespace
{
	std::atomic<int> s_boot_stage{
		static_cast<int>(MANXHost::BootStage::Starting)};
	std::atomic<std::uint64_t> s_captured_frames{0};
	std::atomic<std::uint64_t> s_captured_with_content{0};
} // namespace

void MANXHost::SetBootStage(BootStage stage)
{
	// Only ever forward: the capture path reports "drawing" from the GS thread
	// while the CPU thread is still walking its own start-up steps.
	const int wanted = static_cast<int>(stage);
	int current = s_boot_stage.load(std::memory_order_relaxed);
	while (wanted > current &&
		!s_boot_stage.compare_exchange_weak(current, wanted,
			std::memory_order_relaxed))
	{
	}
}

MANXHost::BootStage MANXHost::GetBootStage()
{
	return static_cast<BootStage>(s_boot_stage.load(std::memory_order_relaxed));
}

void MANXHost::NoteCapturedFrame(bool has_content)
{
	s_captured_frames.fetch_add(1, std::memory_order_relaxed);
	if (has_content)
	{
		s_captured_with_content.fetch_add(1, std::memory_order_relaxed);
		SetBootStage(BootStage::Drawing);
	}
}

void MANXHost::GetCaptureCounts(std::uint64_t& frames,
	std::uint64_t& with_content)
{
	frames = s_captured_frames.load(std::memory_order_relaxed);
	with_content = s_captured_with_content.load(std::memory_order_relaxed);
}

void MANXHost::ResetBootState()
{
	// Boot stage back to Starting. SetBootStage only ever moves forward, so a
	// second game would otherwise stay stuck at Drawing and its loading screen
	// would never show boot progress.
	s_boot_stage.store(static_cast<int>(BootStage::Starting),
		std::memory_order_relaxed);
	// Capture counters back to zero so the new game's boot status reports its
	// own progress, not the previous game's accumulated totals.
	s_captured_frames.store(0, std::memory_order_relaxed);
	s_captured_with_content.store(0, std::memory_order_relaxed);
	// Frame sequence back to zero so the first frame the new game produces is
	// recognised as new (GetLatestFrame treats sequence==0 as "no frame yet").
	{
		std::lock_guard<std::mutex> lock(s_wa_frame_mutex);
		s_wa_frame_sequence = 0;
	}
}

void MANXHost::SetRenderWindow(const WindowInfo& wi)
{
	std::lock_guard<std::mutex> lock(s_wa_window_mutex);
	s_wa_render_window = wi;
}

void MANXHost::ClearRenderWindow()
{
	std::lock_guard<std::mutex> lock(s_wa_window_mutex);
	s_wa_render_window.reset();
}

void MANXHost::SetCPUThreadId(std::thread::id id)
{
	s_wa_cpu_thread_id = id;
	s_wa_cpu_thread_id_valid.store(true, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Cabinet input bridge (see pcsx2_input.h). Held state is written by the main
// SDL thread and read on the CPU thread; coin presses are edge-triggered and
// queued. ApplyToJVS() runs on the CPU thread and is the only place that touches
// ACJV, so ACJV state stays single-threaded (the FCA-1 frame reader is also on
// the CPU thread).
// ---------------------------------------------------------------------------

namespace
{
	std::atomic<bool> s_in_steer_left{false};
	std::atomic<bool> s_in_steer_right{false};
	std::atomic<bool> s_in_gas{false};
	std::atomic<bool> s_in_brake{false};
	std::atomic<bool> s_in_start{false};
	std::atomic<bool> s_in_service{false};
	std::atomic<bool> s_in_gear_up{false};
	std::atomic<bool> s_in_gear_down{false};
	std::atomic<bool> s_in_view{false};
	std::atomic<int> s_in_pending_coins{0};

	// Fighting-cabinet panels, one per player.
	constexpr int kPlayers = 2;
	constexpr int kAttacks = 6;
	std::atomic<bool> s_in_lever_up[kPlayers]{};
	std::atomic<bool> s_in_lever_down[kPlayers]{};
	std::atomic<bool> s_in_lever_left[kPlayers]{};
	std::atomic<bool> s_in_lever_right[kPlayers]{};
	std::atomic<bool> s_in_attack[kPlayers][kAttacks]{};
	std::atomic<bool> s_in_player_start[kPlayers]{};
	std::atomic<bool> s_in_test{false};
	std::atomic<int> s_in_pending_coins_p2{0};

	// Light-gun panel, one per player.
	std::atomic<bool> s_in_gun_aimed[kPlayers]{};
	std::atomic<float> s_in_gun_x[kPlayers]{};
	std::atomic<float> s_in_gun_y[kPlayers]{};
	std::atomic<bool> s_in_gun_trigger[kPlayers]{};
	std::atomic<bool> s_in_gun_pedal[kPlayers]{};
	std::atomic<bool> s_in_gun_offscreen[kPlayers]{};
} // namespace

void MANXInput::SetSteerLeft(bool held) { s_in_steer_left.store(held, std::memory_order_relaxed); }
void MANXInput::SetSteerRight(bool held) { s_in_steer_right.store(held, std::memory_order_relaxed); }
void MANXInput::SetGas(bool held) { s_in_gas.store(held, std::memory_order_relaxed); }
void MANXInput::SetBrake(bool held) { s_in_brake.store(held, std::memory_order_relaxed); }
void MANXInput::SetStart(bool held) { s_in_start.store(held, std::memory_order_relaxed); }
void MANXInput::SetService(bool held) { s_in_service.store(held, std::memory_order_relaxed); }
void MANXInput::SetGearUp(bool held) { s_in_gear_up.store(held, std::memory_order_relaxed); }
void MANXInput::SetGearDown(bool held) { s_in_gear_down.store(held, std::memory_order_relaxed); }
void MANXInput::SetView(bool held) { s_in_view.store(held, std::memory_order_relaxed); }
void MANXInput::InsertCoin() { s_in_pending_coins.fetch_add(1, std::memory_order_relaxed); }
void MANXInput::SetTest(bool held) { s_in_test.store(held, std::memory_order_relaxed); }

void MANXInput::SetLever(int player, bool up, bool down, bool left, bool right)
{
	if (player < 0 || player >= kPlayers)
		return;
	s_in_lever_up[player].store(up, std::memory_order_relaxed);
	s_in_lever_down[player].store(down, std::memory_order_relaxed);
	s_in_lever_left[player].store(left, std::memory_order_relaxed);
	s_in_lever_right[player].store(right, std::memory_order_relaxed);
}

void MANXInput::SetAttack(int player, int index, bool held)
{
	if (player < 0 || player >= kPlayers || index < 0 || index >= kAttacks)
		return;
	s_in_attack[player][index].store(held, std::memory_order_relaxed);
}

void MANXInput::SetPlayerStart(int player, bool held)
{
	if (player < 0 || player >= kPlayers)
		return;
	s_in_player_start[player].store(held, std::memory_order_relaxed);
	if (player == 0)
		s_in_start.store(held, std::memory_order_relaxed);
}

void MANXInput::SetGunAim(int player, bool aimed, float x, float y)
{
	if (player < 0 || player >= kPlayers)
		return;
	s_in_gun_aimed[player].store(aimed, std::memory_order_relaxed);
	if (!aimed)
		return;
	s_in_gun_x[player].store(x, std::memory_order_relaxed);
	s_in_gun_y[player].store(y, std::memory_order_relaxed);
}

void MANXInput::SetGunTrigger(int player, bool held)
{
	if (player >= 0 && player < kPlayers)
		s_in_gun_trigger[player].store(held, std::memory_order_relaxed);
}

void MANXInput::SetGunPedal(int player, bool held)
{
	if (player >= 0 && player < kPlayers)
		s_in_gun_pedal[player].store(held, std::memory_order_relaxed);
}

void MANXInput::SetGunOffscreen(int player, bool held)
{
	if (player >= 0 && player < kPlayers)
		s_in_gun_offscreen[player].store(held, std::memory_order_relaxed);
}

void MANXInput::InsertCoinSlot(int slot)
{
	if (slot <= 0)
		s_in_pending_coins.fetch_add(1, std::memory_order_relaxed);
	else
		s_in_pending_coins_p2.fetch_add(1, std::memory_order_relaxed);
}

void MANXInput::ApplyToJVS()
{
	const auto held = [](const std::atomic<bool>& flag) {
		return flag.load(std::memory_order_relaxed);
	};

	// The cabinet's control panel depends on the game: the core resolves the
	// JVS device mode from the game id (ACJV::ResolveModeFromGameId), so a
	// driving game gets the drive board's axes and a fighting game gets a
	// lever and attack switches per player.
	if (ACJV::GetMode() == JVS_MODE::DRIVE)
	{
		// SetWheelAxis: 0=steer right, 1=steer left, 2=gas, 3=brake (ACJV.cpp).
		ACJV::SetWheelAxis(0, held(s_in_steer_right) ? 1.0f : 0.0f);
		ACJV::SetWheelAxis(1, held(s_in_steer_left) ? 1.0f : 0.0f);
		ACJV::SetWheelAxis(2, held(s_in_gas) ? 1.0f : 0.0f);
		ACJV::SetWheelAxis(3, held(s_in_brake) ? 1.0f : 0.0f);

		ACJV::SetButtonState(0, JVS_BTN_START, held(s_in_start));
		ACJV::SetButtonState(0, JVS_BTN_1, held(s_in_gear_up));
		ACJV::SetButtonState(0, JVS_BTN_2, held(s_in_gear_down));
		ACJV::SetButtonState(0, JVS_BTN_3, held(s_in_view));
	}
	else if (ACJV::GetMode() == JVS_MODE::LIGHTGUN)
	{
		// The board normally takes gun aim from PCSX2's own pointer, which a
		// windowless host has none of. Selecting the per-player "aim device"
		// source instead lets us push an absolute position straight in, and
		// ACJV still does the camera geometry, the off-screen decision and the
		// sensor bit from it. Switch masks come from the running game's own
		// GunMapping, so Time Crisis 3's pedal and trigger land on its
		// switches and Time Crisis 4's on its different ones.
		const GunMapping& gun = ACJV::GetGunMapping();
		bool force_offscreen = false;
		for (int player = 0; player < kPlayers; ++player)
		{
			const u32 slot = static_cast<u32>(player);
			ACJV::SetGunAimSource(slot, true);
			if (held(s_in_gun_aimed[player]))
			{
				const float aim_x = s_in_gun_x[player].load(std::memory_order_relaxed);
				const float aim_y = s_in_gun_y[player].load(std::memory_order_relaxed);
				ACJV::SetGunRelativeAim(slot, aim_x, aim_y);

				// MANX_GUN_TRACE=1: the aim as the board receives it, next
				// to the switches. Pair this with the host-side line to see
				// whether a bad shot position came from the measurement or
				// from the conversion.
				static const bool trace_gun = std::getenv("MANX_GUN_TRACE") != nullptr;
				static u32 trace_counter = 0;
				if (trace_gun && player == 0 && (trace_counter++ % 30) == 0)
				{
					Console.WriteLnFmt(
						"WA_GUN aim={:.3f},{:.3f} trigger={} pedal={} offscreen={}",
						aim_x, aim_y, held(s_in_gun_trigger[player]),
						held(s_in_gun_pedal[player]),
						held(s_in_gun_offscreen[player]));
				}
			}

			const u16 trigger = player == 0 ? gun.p1_trigger : gun.p2_trigger;
			if (trigger)
				ACJV::SetButtonState(slot, trigger, held(s_in_gun_trigger[player]));
			if (gun.pedal)
				ACJV::SetButtonState(slot, gun.pedal, held(s_in_gun_pedal[player]));

			// Map standard cabinet buttons so the Test Menu can be navigated
			ACJV::SetButtonState(slot, JVS_BTN_START, held(s_in_player_start[player]));
			ACJV::SetButtonState(slot, JVS_BTN_UP, held(s_in_lever_up[player]));
			ACJV::SetButtonState(slot, JVS_BTN_DOWN, held(s_in_lever_down[player]));
			ACJV::SetButtonState(slot, JVS_BTN_LEFT, held(s_in_lever_left[player]));
			ACJV::SetButtonState(slot, JVS_BTN_RIGHT, held(s_in_lever_right[player]));

			// A mapping that names no start switch uses the standard one.
			const u16 start = player == 0 ? gun.p1_start : gun.p2_start;
			ACJV::SetButtonState(slot, start ? start : JVS_BTN_START,
				held(s_in_player_start[player]));

			// A gun cabinet's panel carries only a trigger and a pedal, so
			// nothing else was being sent - but a test menu asks for whichever
			// switch that game happens to use as "select", and on this board
			// that is one of the ordinary numbered buttons. Offer the ones the
			// gun mapping has not already claimed, so the menu is operable
			// whichever it reads. Skipping the claimed bits matters: writing a
			// released state over the bit the trigger just set would cancel it.
			constexpr u16 numbered[] = {JVS_BTN_1, JVS_BTN_2, JVS_BTN_3,
				JVS_BTN_4, JVS_BTN_5, JVS_BTN_6};
			for (int index = 0; index < kAttacks; ++index)
			{
				const u16 bit = numbered[index];
				if (bit == trigger || bit == gun.pedal) continue;
				ACJV::SetButtonState(slot, bit, held(s_in_attack[player][index]));
			}

			if (held(s_in_gun_offscreen[player]))
				force_offscreen = true;
		}
		// The force-offscreen flag is global to the board, not per player.
		ACJV::SetGunForceOffscreen(force_offscreen);
	}
	else
	{
		// Take the attack switches from the running game's own layout table
		// rather than hard-coding them: Tekken wires its four to Sw1/2/4/5,
		// Soul Calibur to Sw1/2/3/4, and a six-button layout uses all six.
		// Position n of the host panel therefore always closes the switch the
		// core expects for button n of this game.
		const std::span<const InputBindingInfo> layout =
			ACJV::GetMode() == JVS_MODE::STANDARD ? ACJV::GetStandardButtons()
			                                      : ACJV::GetFightingButtons();
		for (int player = 0; player < kPlayers; ++player)
		{
			const u32 slot = static_cast<u32>(player);
			ACJV::SetButtonState(slot, JVS_BTN_UP, held(s_in_lever_up[player]));
			ACJV::SetButtonState(slot, JVS_BTN_DOWN, held(s_in_lever_down[player]));
			ACJV::SetButtonState(slot, JVS_BTN_LEFT, held(s_in_lever_left[player]));
			ACJV::SetButtonState(slot, JVS_BTN_RIGHT, held(s_in_lever_right[player]));
			ACJV::SetButtonState(slot, JVS_BTN_START, held(s_in_player_start[player]));

			for (std::size_t index = 0; index < layout.size() && index < kAttacks; ++index)
				ACJV::SetButtonState(slot, layout[index].bind_index,
					held(s_in_attack[player][index]));
		}
	}

	// Service is the operator's switch on player 1's panel in every mode; the
	// coin slots likewise belong to the cabinet, not the game. Test is not a
	// panel switch at all on this board - it is DIP switch 0 - so it is held
	// by driving that DIP rather than a JVS button.
	ACJV::SetButtonState(0, JVS_BTN_SERVICE, held(s_in_service));
	const bool test_wanted = held(s_in_test);
	if (ACJV::GetDIPSwitchState(0) != test_wanted)
		ACJV::SetDIPSwitchState(0, test_wanted);

	int coins = s_in_pending_coins.exchange(0, std::memory_order_relaxed);
	while (coins-- > 0)
		ACJV::InsertCoin(0);
	int coins_p2 = s_in_pending_coins_p2.exchange(0, std::memory_order_relaxed);
	while (coins_p2-- > 0)
		ACJV::InsertCoin(1);
}

// ---------------------------------------------------------------------------
// Settings / configuration
// ---------------------------------------------------------------------------

void Host::CommitBaseSettingChanges()
{
	// Everything lives in the in-memory settings layer; nothing to persist.
}

void Host::LoadSettings(SettingsInterface& si, std::unique_lock<std::mutex>& lock)
{
}

void Host::CheckForSettingsChanges(const Pcsx2Config& old_config)
{
}

bool Host::RequestResetSettings(bool folders, bool core, bool controllers, bool hotkeys, bool ui)
{
	// No UI is running, so no reset requests will ever come in.
	return false;
}

void Host::SetDefaultUISettings(SettingsInterface& si)
{
}

bool Host::LocaleCircleConfirm()
{
	return false;
}

std::unique_ptr<ProgressCallback> Host::CreateHostProgressCallback()
{
	return ProgressCallback::CreateNullProgressCallback();
}

// ---------------------------------------------------------------------------
// Notifications / messaging
// ---------------------------------------------------------------------------

void Host::ReportInfoAsync(const std::string_view title, const std::string_view message)
{
	if (!title.empty() && !message.empty())
		INFO_LOG("ReportInfoAsync: {}: {}", title, message);
	else if (!message.empty())
		INFO_LOG("ReportInfoAsync: {}", message);
}

void Host::ReportErrorAsync(const std::string_view title, const std::string_view message)
{
	if (!title.empty() && !message.empty())
		ERROR_LOG("ReportErrorAsync: {}: {}", title, message);
	else if (!message.empty())
		ERROR_LOG("ReportErrorAsync: {}", message);
}

void Host::OpenURL(const std::string_view url)
{
}

bool Host::CopyTextToClipboard(const std::string_view text)
{
	return false;
}

void Host::BeginTextInput()
{
}

void Host::EndTextInput()
{
}

// ---------------------------------------------------------------------------
// Input devices
// ---------------------------------------------------------------------------

void Host::OnInputDeviceConnected(const std::string_view identifier, const std::string_view device_name)
{
}

void Host::OnInputDeviceDisconnected(const InputBindingKey key, const std::string_view identifier)
{
}

void Host::SetMouseMode(bool relative_mode, bool hide_cursor)
{
}

void Host::SetMouseLock(bool state)
{
}

// ---------------------------------------------------------------------------
// Render window / display (headless: always surfaceless)
// ---------------------------------------------------------------------------

std::optional<WindowInfo> Host::GetTopLevelWindowInfo()
{
	std::lock_guard<std::mutex> lock(s_wa_window_mutex);
	if (s_wa_render_window.has_value())
		return s_wa_render_window;

	WindowInfo wi;
	wi.type = WindowInfo::Type::Surfaceless;
	return wi;
}

std::optional<WindowInfo> Host::AcquireRenderWindow(bool recreate_window)
{
	std::lock_guard<std::mutex> lock(s_wa_window_mutex);
	if (s_wa_render_window.has_value())
		return s_wa_render_window;

	// No window registered (headless bring-up): stay surfaceless.
	WindowInfo wi;
	wi.type = WindowInfo::Type::Surfaceless;
	return wi;
}

void Host::ReleaseRenderWindow()
{
}

void Host::BeginPresentFrame()
{
	// Surfaceless: snapshot the current GS frame into memory so the MANX
	// video worker can present it. Runs on the GS thread inside the present path;
	// GSSaveSnapshotToMemory does a synchronous read-back (same call the built-in
	// screenshot path uses here), returning tightly-packed RGBA u32 pixels.
	u32 width = 0;
	u32 height = 0;
	std::vector<u32> pixels;

	// MANX_PCSX2_CAPTURE_RAW=1 grabs the display texture at its own internal
	// resolution with no aspect correction and no border crop. The default
	// path asks for a 640x480 window-fitted, cropped image, which is what a
	// cabinet wants - but if a game's picture survives the raw grab and not
	// the fitted one, the fitting is what lost it.
	static const bool raw_capture = []() {
		const char* value = std::getenv("MANX_PCSX2_CAPTURE_RAW");
		return value && *value && *value != '0';
	}();

	const bool ok = (raw_capture
			? GSSaveSnapshotToMemory(0, 0, /*apply_aspect*/ false, /*crop_borders*/ false,
				  &width, &height, &pixels)
			: GSSaveSnapshotToMemory(640, 480, /*apply_aspect*/ true, /*crop_borders*/ true,
				  &width, &height, &pixels)) &&
		width > 0 && height > 0 && !pixels.empty();

	// Rate-limited capture diagnostics. When a game boots to a black screen but
	// has audio, this distinguishes "GS produced no displayable frame"
	// (snapshot keeps failing -> nothing published -> MANX shows black)
	// from "frames captured fine" (the problem is downstream of capture). Logs
	// the first few calls and then one line every ~180 frames (~3s).
	{
		static std::atomic<u64> s_diag_calls{0};
		static std::atomic<u64> s_diag_ok{0};
		const u64 n = s_diag_calls.fetch_add(1);
		if (ok)
			s_diag_ok.fetch_add(1);
		if (n < 5 || (n % 180) == 0)
		{
			// A successful snapshot of an all-black frame looks identical to a
			// good one here, which is the difference between "the game is
			// showing black" and "the picture is lost downstream of capture".
			// Count the pixels with any colour in them, only on the frames we
			// log, and sample the centre so a uniform fill is recognisable.
			u64 lit = 0;
			u32 centre = 0;
			if (ok)
			{
				for (u32 pixel : pixels)
					lit += ((pixel & 0x00FFFFFFu) != 0) ? 1 : 0;
				const std::size_t middle =
					(static_cast<std::size_t>(height) / 2) * width + width / 2;
				if (middle < pixels.size())
					centre = pixels[middle];
			}
			Console.WriteLnFmt(
				"WA_CAPTURE[{}]: {} ({}x{}), frames_ok={}, lit={}/{}, centre={:08X}",
				n, ok ? "OK" : "NO_FRAME", width, height, s_diag_ok.load(),
				lit, pixels.size(), centre);
		}
	}

	if (ok)
	{
		// A game that is still loading hands us blank frames, so tell the
		// boot-progress tracker whether this one carried any picture.
		bool has_content = false;
		for (u32 pixel : pixels)
		{
			if ((pixel & 0x00FFFFFFu) != 0)
			{
				has_content = true;
				break;
			}
		}
		MANXHost::NoteCapturedFrame(has_content);
		{
			std::lock_guard<std::mutex> lock(s_wa_frame_mutex);
			s_wa_frame_pixels = std::move(pixels);
			s_wa_frame_width = static_cast<int>(width);
			s_wa_frame_height = static_cast<int>(height);
			++s_wa_frame_sequence;
		}
		s_wa_frame_cv.notify_all();
	}
}

void Host::RequestResizeHostDisplay(s32 width, s32 height)
{
}

bool Host::IsFullscreen()
{
	return false;
}

void Host::SetFullscreen(bool enabled)
{
}

// ---------------------------------------------------------------------------
// VM lifecycle callbacks
// ---------------------------------------------------------------------------

void Host::OnVMStarting()
{
}

void Host::OnVMStarted()
{
}

void Host::OnVMDestroyed()
{
}

void Host::OnVMPaused()
{
}

void Host::OnVMResumed()
{
}

void Host::OnGameChanged(const std::string& title, const std::string& elf_override, const std::string& disc_path,
	const std::string& disc_serial, u32 disc_crc, u32 current_crc)
{
}

void Host::OnPerformanceMetricsUpdated()
{
}

void Host::OnSaveStateLoading(const std::string_view filename)
{
}

void Host::OnSaveStateLoaded(const std::string_view filename, bool was_successful)
{
}

void Host::OnSaveStateSaved(const std::string_view filename)
{
}

void Host::RunOnCPUThread(std::function<void()> function, bool block /* = false */)
{
	const bool on_cpu_thread = s_wa_cpu_thread_id_valid.load(std::memory_order_acquire) &&
							   std::this_thread::get_id() == s_wa_cpu_thread_id;

	// Already on the CPU thread: just run it, avoiding a deadlock on block.
	if (on_cpu_thread)
	{
		function();
		return;
	}

	if (block)
	{
		auto done = std::make_shared<std::promise<void>>();
		std::future<void> future = done->get_future();
		{
			std::lock_guard<std::mutex> lock(s_wa_cpu_queue_mutex);
			s_wa_cpu_queue.push_back([function, done]() {
				function();
				done->set_value();
			});
		}
		future.wait();
		return;
	}

	std::lock_guard<std::mutex> lock(s_wa_cpu_queue_mutex);
	s_wa_cpu_queue.push_back(std::move(function));
}

void Host::RefreshGameListAsync(bool invalidate_cache)
{
}

void Host::CancelGameListRefresh()
{
}

// ---------------------------------------------------------------------------
// Capture
// ---------------------------------------------------------------------------

void Host::OnCaptureStarted(const std::string& filename)
{
}

void Host::OnCaptureStopped()
{
}

// ---------------------------------------------------------------------------
// Application / VM shutdown requests
// ---------------------------------------------------------------------------

void Host::RequestExitApplication(bool allow_confirm)
{
}

void Host::RequestExitBigPicture()
{
}

void Host::RequestVMShutdown(bool allow_confirm, bool allow_save_state, bool default_save_state)
{
	VMManager::SetState(VMState::Stopping);
}

// ---------------------------------------------------------------------------
// Achievements
// ---------------------------------------------------------------------------

void Host::OnAchievementsLoginSuccess(const char* username, u32 points, u32 sc_points, u32 unread_messages)
{
}

void Host::OnAchievementsLoginRequested(Achievements::LoginRequestReason reason)
{
}

void Host::OnAchievementsHardcoreModeChanged(bool enabled)
{
}

void Host::OnAchievementsRefreshed()
{
}

// ---------------------------------------------------------------------------
// UI open requests
// ---------------------------------------------------------------------------

void Host::OnCoverDownloaderOpenRequested()
{
}

void Host::OnCreateMemoryCardOpenRequested()
{
}

// ---------------------------------------------------------------------------
// Mode / batch queries
// ---------------------------------------------------------------------------

bool Host::InBatchMode()
{
	return false;
}

bool Host::InNoGUIMode()
{
	return false;
}

bool Host::ShouldPreferHostFileSelector()
{
	return false;
}

void Host::OpenHostFileSelectorAsync(std::string_view title, bool select_directory, FileSelectorCallback callback,
	FileSelectorFilters filters, std::string_view initial_directory)
{
	callback(std::string());
}

// ---------------------------------------------------------------------------
// Localisation
// ---------------------------------------------------------------------------

int Host::LocaleSensitiveCompare(std::string_view lhs, std::string_view rhs)
{
	const int res = std::strncmp(lhs.data(), rhs.data(), std::min(lhs.size(), rhs.size()));
	if (res != 0)
		return res;
	return lhs.size() > rhs.size() ? 1 : (lhs.size() < rhs.size() ? -1 : 0);
}

s32 Host::Internal::GetTranslatedStringImpl(
	const std::string_view context, const std::string_view msg, char* tbuf, size_t tbuf_space)
{
	if (msg.size() > tbuf_space)
		return -1;
	else if (msg.empty())
		return 0;

	std::memcpy(tbuf, msg.data(), msg.size());
	return static_cast<s32>(msg.size());
}

std::string Host::TranslatePluralToString(const char* context, const char* msg, const char* disambiguation, int count)
{
	TinyString count_str = TinyString::from_format("{}", count);

	std::string ret(msg);
	for (;;)
	{
		std::string::size_type pos = ret.find("%n");
		if (pos == std::string::npos)
			break;

		ret.replace(pos, pos + 2, count_str.view());
	}

	return ret;
}

void Host::PumpMessagesOnCPUThread()
{
	// Drain any work queued for the CPU thread via Host::RunOnCPUThread. Called
	// every VSync from VMManager::Internal::PollInputOnCPUThread while a game is
	// booting/running.
	std::vector<std::function<void()>> pending;
	{
		std::lock_guard<std::mutex> lock(s_wa_cpu_queue_mutex);
		pending.swap(s_wa_cpu_queue);
	}

	for (auto& fn : pending)
		fn();

	// Push the latest cabinet-input state into ACJV each VSync (CPU thread).
	MANXInput::ApplyToJVS();
}

// ---------------------------------------------------------------------------
// InputManager host-side glue (no host keyboard mapping in headless bring-up)
// ---------------------------------------------------------------------------

std::optional<u32> InputManager::ConvertHostKeyboardStringToCode(const std::string_view str)
{
	return std::nullopt;
}

std::optional<std::string> InputManager::ConvertHostKeyboardCodeToString(u32 code)
{
	return std::nullopt;
}

const char* InputManager::ConvertHostKeyboardCodeToIcon(u32 code)
{
	return nullptr;
}

BEGIN_HOTKEY_LIST(g_host_hotkeys)
END_HOTKEY_LIST()
