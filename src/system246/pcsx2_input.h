// WhittyArcade System 246 board: thread-safe cabinet-input bridge.
//
// The main SDL event loop (pcsx2_boot_rrv.cpp) captures keyboard state and
// calls the Set*/InsertCoin setters below. The PCSX2 CPU thread calls
// ApplyToJVS() once per VSync (from Host::PumpMessagesOnCPUThread) to push the
// latest state into PCSX2's ACJV/JVS layer, which RRV's FCA-1 drive board
// consumes via ACJV::UpdateFcaFrame(). Setters run on the main thread, ApplyToJVS
// on the CPU thread; the two are decoupled through atomics, so neither blocks the
// other and PCSX2's own SDL input source can stay disabled (no cross-thread SDL
// event pumping). Implemented in pcsx2_host.cpp (which has the ACJV headers).
#pragma once

namespace WhittyArcadeInput
{
	void SetSteerLeft(bool held);
	void SetSteerRight(bool held);
	void SetGas(bool held);
	void SetBrake(bool held);
	void SetStart(bool held);
	void SetService(bool held);
	void SetGearUp(bool held);
	void SetGearDown(bool held);
	void SetView(bool held);

	// Fighting-cabinet panel (Tekken and the other JVS fighting layouts):
	// `player` is 0 or 1, `index` is the position of the attack switch within
	// the running game's layout, not a fixed JVS bit.
	void SetLever(int player, bool up, bool down, bool left, bool right);
	void SetAttack(int player, int index, bool held);
	void SetPlayerStart(int player, bool held);
	void SetTest(bool held);

	// Light-gun cabinet. Aim is a fraction of the picture (0,0 = top-left).
	// Passing aimed = false leaves the board on the last position it had.
	void SetGunAim(int player, bool aimed, float x, float y);
	void SetGunTrigger(int player, bool held);
	void SetGunPedal(int player, bool held);
	void SetGunOffscreen(int player, bool held);

	// Edge-triggered: queued on the main thread, drained on the CPU thread.
	void InsertCoin();
	void InsertCoinSlot(int slot);

	// CPU-thread only: pushes the current held state + queued coins into ACJV.
	void ApplyToJVS();
} // namespace WhittyArcadeInput
