// MANX System 246 board: shared host hooks between pcsx2_host.cpp and
// the System 246 PCSX2 session. The window hooks let a windowed front-end
// register a native surface; the frame-capture API lets the surfaceless
// session pull the latest GS-produced frame that Host::BeginPresentFrame()
// snapshots each present.
#pragma once

#include <cstdint>
#include <thread>
#include <vector>

struct WindowInfo;

namespace MANXHost
{
	// Render-window registration (unused by the surfaceless session; kept for
	// the windowed pcsx2_boot_rrv.cpp harness).
	void SetRenderWindow(const WindowInfo& wi);
	void ClearRenderWindow();

	// Identify the PCSX2 CPU thread so Host::RunOnCPUThread can run work inline
	// when already on it.
	void SetCPUThreadId(std::thread::id id);

	// Frame capture. Host::BeginPresentFrame() publishes each GS frame as a
	// tightly-packed RGBA u32 buffer; the session waits for the next one.
	//
	// Blocks up to timeout_ms for a captured frame whose sequence differs from
	// last_sequence. Returns true and fills the out-parameters when a newer
	// frame is available, false on timeout (e.g. while the VM is paused or has
	// not produced its first frame yet).
	bool WaitForFrame(std::uint64_t last_sequence, int timeout_ms,
		std::vector<std::uint32_t>& out_pixels, int& out_width, int& out_height,
		std::uint64_t& out_sequence);

	// Non-blocking variant used by the pure-C module ABI: if the latest captured
	// frame's sequence differs from last_sequence (and is non-zero), copy it into
	// out_pixels, fill the geometry/sequence out-params and return true. Returns
	// false immediately when no newer frame exists (never waits).
	bool GetLatestFrame(std::uint64_t last_sequence,
		std::vector<std::uint32_t>& out_pixels, int& out_width, int& out_height,
		std::uint64_t& out_sequence);

	// Boot progress, for the host's loading screen. These games spend the best
	// part of a minute reading their disc before drawing anything, so the
	// session shows what the core is doing rather than an empty screen. The
	// stages are ordered; the reported one only ever moves forward.
	enum class BootStage
	{
		Starting,        // module entered, PCSX2 not configured yet
		Configuring,     // settings applied, BIOS and folders resolved
		StartingMachine, // CPU thread spawned, VMManager coming up
		LoadingGame,     // machine running, reading the disc, nothing drawn
		Drawing,         // the game has produced a picture
	};

	void SetBootStage(BootStage stage);
	BootStage GetBootStage();

	// Frames the GS has handed us, and how many carried any picture. A game
	// that is still loading produces blank frames, so the pair distinguishes
	// "not drawing yet" from "drawing".
	void NoteCapturedFrame(bool has_content);
	void GetCaptureCounts(std::uint64_t& frames, std::uint64_t& with_content);
} // namespace MANXHost
