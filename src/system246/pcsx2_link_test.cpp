// WhittyArcade System 246/256 board bring-up: PCSX2 core embed link test.
//
// FOUNDATION MILESTONE ONLY. This standalone executable proves that WhittyArcade
// can link against the prebuilt PCSX2 static libraries (libpcsx2.a et al) and
// stand up + tear down the PCSX2 CPU thread headlessly. It does NOT boot a game,
// create a GS device, a window, audio or any UI.
//
// The minimal init/shutdown sequence is adapted from PCSX2's headless reference
// frontend (pcsx2x6/pcsx2-gsrunner/Main.cpp): configure an in-memory settings
// layer, resolve EmuFolders, run early hardware checks, then
// VMManager::Internal::CPUThreadInitialize() / CPUThreadShutdown().

#include "pcsx2/PrecompiledHeader.h"

#include <cstdio>
#include <cstdlib>

#include "common/Console.h"
#include "common/MemorySettingsInterface.h"

#include "pcsx2/Config.h" // EmuFolders
#include "pcsx2/Host.h"
#include "pcsx2/VMManager.h"

// BIOS directory shipped with the PCSX2 arcade build. CPUThreadInitialize does
// not itself load the BIOS (that happens at VMManager::Initialize time when a
// game is booted), but we point discovery at it now so the next milestone can
// find it.
static constexpr const char* PCSX2_BIOS_DIR = "/home/jon/pcsx2x6/build/bin/bios";

static MemorySettingsInterface s_settings_interface;

int main()
{
	// Route the emulator's console output to stdout so we can see any failures.
	Log::SetConsoleOutputLevel(LOGLEVEL_DEBUG);

	// Resolve the standard folder layout. Resources/data may not be present in a
	// headless bring-up; treat those as best-effort since the CPU thread does not
	// need them.
	EmuFolders::SetAppRoot();
	if (!EmuFolders::SetResourcesDirectory())
		Console.Warning("SetResourcesDirectory() failed (ignored for headless CPU bring-up).");
	if (!EmuFolders::SetDataDirectory(nullptr))
		Console.Warning("SetDataDirectory() failed (ignored for headless CPU bring-up).");

	const char* hw_error = nullptr;
	if (!VMManager::PerformEarlyHardwareChecks(&hw_error))
	{
		std::fprintf(stderr, "PerformEarlyHardwareChecks failed: %s\n", hw_error ? hw_error : "(unknown)");
		return EXIT_FAILURE;
	}

	// In-memory settings only -- no ini path, nothing written to disk.
	MemorySettingsInterface& si = s_settings_interface;
	Host::Internal::SetBaseSettingsLayer(&si);
	VMManager::SetDefaultSettings(si, true, true, true, true, true);

	// Nothing we can drive headlessly: no audio output, no input sources.
	si.SetStringValue("SPU2/Output", "OutputModule", "nullout");
	si.SetBoolValue("InputSources", "SDL", false);
	si.SetBoolValue("InputSources", "XInput", false);

	VMManager::Internal::LoadStartupSettings();

	// Point BIOS discovery at the arcade build's bios directory.
	EmuFolders::Bios = PCSX2_BIOS_DIR;

	if (!VMManager::Internal::CPUThreadInitialize())
	{
		std::fprintf(stderr, "VMManager::Internal::CPUThreadInitialize() failed\n");
		return EXIT_FAILURE;
	}

	std::printf("PCSX2 CPU thread init OK\n");
	std::fflush(stdout);

	VMManager::Internal::CPUThreadShutdown();

	std::printf("PCSX2 shutdown OK\n");
	std::fflush(stdout);

	return EXIT_SUCCESS;
}
