// WhittyArcade System 246 board: pure-C ABI for the isolated PCSX2 module.
//
// The PCSX2 arcade core (libpcsx2.a) bundles its own GL loader (glad) and a pile
// of static initialisers that collide with WhittyArcade's GLEW-based OpenGL
// renderer if linked into the main binary. To keep them apart, everything PCSX2
// lives behind this flat C interface inside libsystem246_pcsx2.so, which the
// main (GCC-built) WhittyArcade binary dlopen()s at runtime. Nothing in this
// header pulls in a C++ or PCSX2 type, so the GCC side can include it directly.
#ifndef WHITTY_SYSTEM246_PCSX2_MODULE_H
#define WHITTY_SYSTEM246_PCSX2_MODULE_H

// The module is built with -fvisibility=hidden (inherited from the PCSX2 flags),
// so the exported entry points must be forced to default visibility or the .so
// exposes nothing to dlsym. Harmless on the GCC/host side, which only takes the
// symbols' addresses via dlsym and never links against them.
#if defined(__GNUC__)
#  define WA_PCSX2_EXPORT __attribute__((visibility("default")))
#else
#  define WA_PCSX2_EXPORT
#endif

#ifdef __cplusplus
extern "C" {
#endif

// One frame's worth of cabinet input, pushed from the host each run_frame().
// All fields are simple held flags (0 = released, 1 = held) except coin_pulse,
// which the host sets to 1 for exactly one call per physical coin insert (the
// rising edge is detected host-side).
typedef struct {
    // Driving cabinets (Ridge Racer V, MotoGP): the drive board's axes and
    // switches. start/service are player 1's panel switches in every mode.
    int steer_left;
    int steer_right;
    int gas;
    int brake;
    int start;
    int service;
    int gear_up;
    int gear_down;
    int view;
    int coin_pulse; // set to 1 to insert one coin on this call

    // Fighting cabinets (Tekken 4, Tekken 5 and 5 Dark Resurrection): a JVS
    // control panel per player - an eight-way lever plus the panel's attack
    // switches. Index 0 is player 1. `attack` is positional, in the order the
    // running game's layout lists its buttons, so Tekken's four are left
    // punch, right punch, left kick, right kick; another layout's are its own.
    // Which JVS switch each one closes is the core's business, not ours.
    int lever_up[2];
    int lever_down[2];
    int lever_left[2];
    int lever_right[2];
    int attack[2][6];
    int p2_start;
    int p2_coin_pulse; // second coin slot, same one-per-call rule
    int test;

    // Light-gun cabinets (Time Crisis 3 and 4, Vampire Night, Cobra): where
    // each player's gun is pointing, as a fraction of the picture - 0,0 is the
    // top-left corner and 1,1 the bottom-right. The board turns that into the
    // camera's own coordinates and decides whether the gun can still see the
    // screen, so the host only has to say where it points and which switches
    // are closed. gun_aimed = 0 means "no aim from the host this frame".
    float gun_x[2];
    float gun_y[2];
    int gun_aimed[2];
    int gun_trigger[2];
    int gun_pedal[2];
    int gun_offscreen[2]; // held = report the screen as lost (reload)
} wa_pcsx2_input;

// Boot Ridge Racer V (the .acgame manifest) in-process, surfaceless, on a
// dedicated PCSX2 CPU thread. bios_dir/bin_dir locate the arcade build tree.
// Returns 1 on success (config accepted + CPU thread spawned), 0 on failure.
WA_PCSX2_EXPORT int wa_pcsx2_start(const char* acgame, const char* bios_dir,
                                   const char* bin_dir);

// Non-blocking: if a GS frame newer than the one last returned is available,
// fill *pixels (tightly-packed RGBA, valid until the next call), *w, *h and
// *seq, and return 1. Returns 0 when no new frame is ready (never blocks).
WA_PCSX2_EXPORT int wa_pcsx2_get_frame(const unsigned int** pixels, int* w,
                                       int* h, unsigned long long* seq);

// Push the latest cabinet input. Held flags update immediately; coin_pulse != 0
// queues exactly one coin. Safe to call from the host thread every frame.
WA_PCSX2_EXPORT void wa_pcsx2_set_input(const wa_pcsx2_input* in);

// Pause/resume the emulated machine (audio + emulation), not just presentation.
// paused != 0 pauses; 0 resumes. No-op if the VM isn't running.
WA_PCSX2_EXPORT void wa_pcsx2_set_paused(int paused);

// What the core is doing while a game boots, for the host's loading screen.
// Returns a short sentence ("Reading game data from the disc") that stays
// valid until the next call, plus the frames the GS has produced and how many
// of those carried a picture. Once *drawing is non-zero the game is on screen
// and the host stops showing its own loading picture.
WA_PCSX2_EXPORT const char* wa_pcsx2_boot_status(unsigned long long* frames,
                                                 unsigned long long* drawing);

// Stop the VM and join the CPU thread. Safe to call if never started.
WA_PCSX2_EXPORT void wa_pcsx2_stop(void);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // WHITTY_SYSTEM246_PCSX2_MODULE_H
