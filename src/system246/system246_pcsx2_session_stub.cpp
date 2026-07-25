// Fallback make_system246_session for builds where the embedded PCSX2 core is
// unavailable: non-x86-64 targets (PCSX2 has no ARM backend) or a host without
// the prebuilt libpcsx2 static libraries. Keeps the board wired into the
// catalog/factory while reporting that it cannot run here.

#include "arcade_session_internal.h"

#include <cstdio>

std::unique_ptr<emulator_session> make_system246_session(
    std::shared_ptr<arcade_video_worker>,
    std::shared_ptr<arcade_cabinet_state>) {
    std::fprintf(stderr,
                 "System 246/256 (Ridge Racer V, MotoGP) is unsupported on "
                 "this architecture: the embedded PCSX2 core requires "
                 "x86-64.\n");
    return nullptr;
}
