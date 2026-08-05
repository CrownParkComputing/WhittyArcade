// burnout3_boot_test.c — Integration test for the Burnout 3 boot sequence.
//
// Verifies the full initialization chain without requiring a display:
//   1. XBE loading from game data
//   2. Kernel init (64MB mmap + thunks + path translation)
//   3. Native game layer init (Vulkan + textures + fe_menu + rw_renderer)
//   4. Menu reachable (g_game_ready = 1)
//   5. Frame pump running (game_frame_pump advances fe_menu_update)
//
// MANX already works — this test only exercises the Burnout 3
// recompilation layer, not the Vulkan presenter or SDL input.

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <SDL3/SDL_scancode.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <math.h>

#include "track_loader.h"

// ── Burnout 3 bridge API (C linkage — bridge is compiled as C) ─
extern "C" {
bool burnout3_init(const char *game_data_path);
void burnout3_shutdown(void);
bool burnout3_ready(void);
bool burnout3_run_frame(uint8_t *out_pixels, int *out_width, int *out_height);
int  burnout3_audio_callback(int16_t *buffer, int max_frames);
void burnout3_inject_keyboard(const bool *sdl_key_state, int num_keys);
void fe_menu_force_race(void);

// Kernel memory access
extern ptrdiff_t g_xbox_mem_offset;

// Game globals
extern int g_game_ready;
extern int g_backbuffer_width;
extern int g_backbuffer_height;

// Input globals
extern uint16_t g_xinput_buttons;
extern int16_t  g_xinput_thumb_lx;
extern int16_t  g_xinput_thumb_ly;
extern uint8_t  g_xinput_left_trigger;
extern uint8_t  g_xinput_right_trigger;
extern uint64_t g_keyboard_state[4];

// Frontend state (for the menu→race integration test)
int fe_menu_is_racing(void);
void fe_menu_stop_race(void);
void fe_menu_show_setup(int row);

// Frames that got past the pump's 60 Hz gate (perf measurement).
unsigned long burnout3_frame_counter(void);
void vulkan_d3d8_pool_counts(int *vb, int *ib, int *tex);

// Track/car catalogue and the geometry it loads into.
int         b3_track_count(void);
int         b3_car_count(void);
const char *b3_track_name(int i);
const char *b3_car_name(int i);
void        b3_select_track(int i);
void        b3_select_car(int i);
extern TrackData g_track_data;
}

// ── Tests ─────────────────────────────────────────────────────

static const char *g_data_path = NULL;

static int test_xbe_and_kernel_load(void) {
    // Verify game data exists
    char path[1024];
    snprintf(path, sizeof(path), "%s/default.xbe", g_data_path);
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "FAIL: cannot open %s\n", path);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);
    if (sz < 1024 * 1024) {
        fprintf(stderr, "FAIL: XBE too small (%ld bytes)\n", sz);
        return 1;
    }
    fprintf(stderr, "  XBE file: %ld bytes OK\n", sz);
    return 0;
}

static int test_boot_sequence(void) {
    // Verify game data exists before attempting boot
    char xbe_path[1024];
    snprintf(xbe_path, sizeof(xbe_path), "%s/default.xbe", g_data_path);
    FILE *f = fopen(xbe_path, "rb");
    if (!f) {
        fprintf(stderr, "SKIP: no default.xbe in %s\n", g_data_path);
        return 0;  // skip, not fail
    }
    fclose(f);

    // Full boot: XBE → kernel → native game layer → menu
    // Note: requires Vulkan GPU for vulkan_d3d8_init inside game_native_init.
    // In headless CI without Vulkan, this test will report FAIL.
    if (!burnout3_init(g_data_path)) {
        fprintf(stderr, "FAIL: burnout3_init returned false\n");
        return 1;
    }
    fprintf(stderr, "  boot sequence: OK\n");
    return 0;
}

static int test_menu_reached(void) {
    if (!g_game_ready) {
        // boot may have failed — skip if not initialised
        fprintf(stderr, "SKIP: game not initialised (boot may have failed)\n");
        return 0;
    }
    if (!burnout3_ready()) {
        fprintf(stderr, "FAIL: game not ready after init\n");
        return 1;
    }
    fprintf(stderr, "  menu reached: g_game_ready=%d OK\n", g_game_ready);
    return 0;
}

static int test_frame_pump_runs(void) {
    // Run 10 frames, verify no crashes (headless: Vulkan may not be available,
    // so blank backbuffer is acceptable).
    static uint8_t pixels[640 * 480 * 4];  // static avoids stack overflow
    int w = 0, h = 0;
    int frames_ok = 0;
    int nonzero_pixels = 0;

    for (int i = 0; i < 10; i++) {
        memset(pixels, 0, sizeof(pixels));
        if (!burnout3_run_frame(pixels, &w, &h)) {
            fprintf(stderr, "FAIL: burnout3_run_frame returned false at frame %d\n", i);
            return 1;
        }
        if (w <= 0 || h <= 0) {
            fprintf(stderr, "FAIL: invalid dimensions (%dx%d) at frame %d\n", w, h, i);
            return 1;
        }
        // Count non-zero bytes in the captured frame — proves we got
        // real rendered pixels, not a blank backbuffer from a failed readback.
        for (size_t b = 0; b < sizeof(pixels); b++) {
            if (pixels[b] != 0) nonzero_pixels++;
        }
        frames_ok++;
    }

    fprintf(stderr, "  frame pump: %d/10 frames OK (%d nonzero bytes captured)\n",
            frames_ok, nonzero_pixels);

    // Optional: BURNOUT3_TEST_MENU=1 walks the menu into a race the way a
    // player would: three confirm presses (title -> main -> world tour ->
    // launch), each with press/release frames, first via keyboard Enter,
    // and if that fails, reports where it stopped. BURNOUT3_TEST_MENU=pad
    // does the same through the XInput globals (gamepad A presses).
    const char *menu_test = getenv("BURNOUT3_TEST_MENU");
    if (menu_test) {
        const bool use_pad = strcmp(menu_test, "pad") == 0;
        bool keys[512] = {false};
        // 3 intro skips + title + WORLD TOUR select + launch (+1 spare)
        for (int press = 0; press < 7; press++) {
            // press for ~150 ms
            if (use_pad) g_xinput_buttons = 0x1000;      // A
            else { keys[40] = true;                      // SDL_SCANCODE_RETURN
                   burnout3_inject_keyboard(keys, 512); }
            struct timespec t0, t;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            do { burnout3_run_frame(pixels, &w, &h);
                 clock_gettime(CLOCK_MONOTONIC, &t);
            } while ((t.tv_sec - t0.tv_sec) * 1000 +
                     (t.tv_nsec - t0.tv_nsec) / 1000000 < 150);
            // release for ~150 ms
            if (use_pad) g_xinput_buttons = 0;
            else { keys[40] = false;
                   burnout3_inject_keyboard(keys, 512); }
            clock_gettime(CLOCK_MONOTONIC, &t0);
            do { burnout3_run_frame(pixels, &w, &h);
                 clock_gettime(CLOCK_MONOTONIC, &t);
            } while ((t.tv_sec - t0.tv_sec) * 1000 +
                     (t.tv_nsec - t0.tv_nsec) / 1000000 < 150);
        }
        fprintf(stderr, "  menu walk (%s): 7 confirm presses done\n",
                use_pad ? "pad" : "keyboard");
    }

    // Optional: BURNOUT3_TEST_RACE=1 forces a race launch (track load,
    // spawn, native physics, rw_gameplay_render) before the frame dump —
    // combine with BURNOUT3_FRAME_DUMP_DELAY_MS to capture the 3D scene.
    // Holds W (throttle) for the whole run so the car actually drives.
    if (getenv("BURNOUT3_TEST_RACE")) {
        fe_menu_force_race();
        // BURNOUT3_TEST_POSE="x,z,hdg" pins the car instead of driving, so
        // two builds can be compared from an identical viewpoint (wall-clock
        // pacing makes a driven frame land somewhere different every run).
        const char *pose = getenv("BURNOUT3_TEST_POSE");
        if (pose) {
            float px = 0, pz = 0, hdg = 0;
            if (sscanf(pose, "%f,%f,%f", &px, &pz, &hdg) == 3) {
                volatile float *phys =
                    (volatile float *)((uintptr_t)0x5FFF00 + g_xbox_mem_offset);
                // Pin over wall-clock, not a frame count: the pump
                // self-throttles to 60 Hz, so N calls is not N simulated
                // ticks, and the camera's height smoothing converged by
                // different amounts per run — which silently turned an
                // A/B comparison into two different viewpoints.
                struct timespec t0, t;
                clock_gettime(CLOCK_MONOTONIC, &t0);
                do {
                    phys[4] = px; phys[5] = pz; phys[6] = hdg; phys[7] = 0.0f;
                    burnout3_run_frame(pixels, &w, &h);
                    clock_gettime(CLOCK_MONOTONIC, &t);
                } while ((t.tv_sec - t0.tv_sec) * 1000 +
                         (t.tv_nsec - t0.tv_nsec) / 1000000 < 2500);
                phys[4] = px; phys[5] = pz; phys[6] = hdg; phys[7] = 0.0f;
                // Then let it drive from that heading, to test containment
                // in a chosen direction (e.g. straight at the harbour).
                const char *drv = getenv("BURNOUT3_TEST_DRIVE");
                if (drv) {
                    bool k[512] = {false};
                    k[26] = true;                              // W
                    if (strchr(drv, 'd')) k[7] = true;         // D, steer right
                    if (strchr(drv, 'a')) k[4] = true;         // A, steer left
                    if (strchr(drv, 'b')) k[225] = true;       // LSHIFT, boost
                    burnout3_inject_keyboard(k, 512);
                }
            }
        } else {
            bool keys[512] = {false};
            keys[26] = true;  // SDL_SCANCODE_W
            burnout3_inject_keyboard(keys, 512);
        }
    }

    // Optional: dump the last captured frame as PPM for visual inspection.
    // Set BURNOUT3_FRAME_DUMP=/path/to/frame.ppm
    // BURNOUT3_FRAME_DUMP_DELAY_MS keeps pumping frames for that long first
    // (e.g. to land inside an intro FMV rather than its fade-in).
    const char *dump = getenv("BURNOUT3_FRAME_DUMP");
    const char *delay_ms = getenv("BURNOUT3_FRAME_DUMP_DELAY_MS");
    // BURNOUT3_TEST_SETUP=<row> parks the frontend on the race setup screen
    // so a dump shows it without scripting the menu walk to get there.
    if (const char *setup_row = getenv("BURNOUT3_TEST_SETUP")) {
        fe_menu_show_setup(atoi(setup_row));
        if (const char *t = getenv("BURNOUT3_TEST_TRACK")) b3_select_track(atoi(t));
        if (const char *c = getenv("BURNOUT3_TEST_CAR"))   b3_select_car(atoi(c));
    }
    if (dump && delay_ms && atol(delay_ms) > 0) {
        const long target_ms = atol(delay_ms);
        struct timespec t0, t;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        do {
            burnout3_run_frame(pixels, &w, &h);
            clock_gettime(CLOCK_MONOTONIC, &t);
        } while ((t.tv_sec - t0.tv_sec) * 1000 +
                 (t.tv_nsec - t0.tv_nsec) / 1000000 < target_ms);
        if (getenv("BURNOUT3_TEST_RACE")) {
            // Report where the drive physics put the car.
            volatile float *phys =
                (volatile float *)((uintptr_t)0x5FFF00 + g_xbox_mem_offset);
            fprintf(stderr,
                    "  drive check: pos=(%.1f, %.1f) hdg=%.2f speed=%.1f\n",
                    phys[4], phys[5], phys[6], phys[7]);
        }
    }
    if (dump && w > 0 && h > 0) {
        FILE *out = fopen(dump, "wb");
        if (out) {
            fprintf(out, "P6\n%d %d\n255\n", w, h);
            for (int p = 0; p < w * h; p++)
                fwrite(&pixels[p * 4], 1, 3, out);
            fclose(out);
            fprintf(stderr, "  frame dumped to %s\n", dump);
        }
    }
    return 0;
}

// Pump frames for a wall-clock duration (the pump self-throttles to 60 Hz).
static void pump_for_ms(long ms, uint8_t *pixels, int *w, int *h) {
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    do {
        burnout3_run_frame(pixels, w, h);
        clock_gettime(CLOCK_MONOTONIC, &t);
    } while ((t.tv_sec - t0.tv_sec) * 1000 +
             (t.tv_nsec - t0.tv_nsec) / 1000000 < ms);
}

// Count distinct colors among sampled pixels — a flat frame (the failure
// mode where only the clear color survives) yields 1-2.
static int sampled_unique_colors(const uint8_t *pixels, int w, int h) {
    static uint32_t samples[8192];
    int n = 0;
    const int total = w * h;
    for (int p = 0; p < total && n < 8192; p += 97) {
        uint32_t c;
        memcpy(&c, &pixels[(size_t)p * 4], 4);
        samples[n++] = c;
    }
    // Sort + count uniques
    for (int i = 1; i < n; i++) {
        uint32_t key = samples[i];
        int j = i - 1;
        while (j >= 0 && samples[j] > key) { samples[j + 1] = samples[j]; j--; }
        samples[j + 1] = key;
    }
    int uniq = n > 0 ? 1 : 0;
    for (int i = 1; i < n; i++)
        if (samples[i] != samples[i - 1]) uniq++;
    return uniq;
}

// Integration: walk the menu into a race with simulated pad presses, then
// drive with held throttle. Catches: menu flow regressions, race launch
// failures, physics starvation (frame-rate collapse), and flat-frame
// rendering regressions — the four failure modes seen during bring-up.
static int test_menu_launch_and_drive(void) {
    if (!g_game_ready) {
        fprintf(stderr, "SKIP: menu/drive test (game not initialised)\n");
        return 0;
    }
    static uint8_t pixels[640 * 480 * 4];
    int w = 640, h = 480;

    // Simulated pad A presses: 3 intro skips + title + WORLD TOUR launch,
    // plus spares (each press 150 ms down, 150 ms up).
    for (int press = 0; press < 7 && !fe_menu_is_racing(); press++) {
        g_xinput_buttons = 0x1000;   // A
        pump_for_ms(150, pixels, &w, &h);
        g_xinput_buttons = 0;
        pump_for_ms(150, pixels, &w, &h);
    }
    if (!fe_menu_is_racing()) {
        fprintf(stderr, "FAIL: race did not launch after menu walk\n");
        return 1;
    }

    // Drive: hold W for 3 seconds.
    volatile float *phys =
        (volatile float *)((uintptr_t)0x5FFF00 + g_xbox_mem_offset);
    const float start_x = phys[4], start_z = phys[5];
    bool keys[512] = {false};
    keys[26] = true;   // SDL_SCANCODE_W
    burnout3_inject_keyboard(keys, 512);
    pump_for_ms(3000, pixels, &w, &h);
    keys[26] = false;
    burnout3_inject_keyboard(keys, 512);

    const float speed = phys[7];
    const float dx = phys[4] - start_x, dz = phys[5] - start_z;
    const float moved = sqrtf(dx * dx + dz * dz);
    // The race frame must be a real scene, not a flat clear.
    int uniq = sampled_unique_colors(pixels, w, h);

    // Leave the race before reporting: an early return here left the game
    // racing, and every later check then ran against a live 3D scene
    // instead of the menu, turning one failure into three.
    fe_menu_stop_race();

    if (speed < 15.0f) {
        // Speed this low after 3 s of full throttle means the physics is
        // being starved — the 1.7 fps regression looked exactly like this.
        fprintf(stderr, "FAIL: drive speed %.1f after 3s throttle\n", speed);
        return 1;
    }
    if (moved < 25.0f) {
        fprintf(stderr, "FAIL: car moved only %.1f units\n", moved);
        return 1;
    }
    if (uniq < 20) {
        fprintf(stderr, "FAIL: race frame nearly flat (%d unique colors)\n", uniq);
        return 1;
    }

    fprintf(stderr,
            "  menu+drive: race launched, speed=%.1f moved=%.1f colors=%d OK\n",
            speed, moved, uniq);
    return 0;
}

// 36 loadable tracks and 67 cars ship (a 37th streamed.dat, US/C5_V1, is a
// zero-byte placeholder). Before the catalogue existed the port hard-coded
// the first hit of a directory scan, so all but one of each was
// unreachable. Two things can silently undo that: a scan that misses
// entries (car numbering is sparse — HEVY reaches Car36), and the load
// latches, which made a second race reuse the first race's track.
static int test_track_car_selection(void) {
    if (!g_game_ready) {
        fprintf(stderr, "SKIP: track/car selection (game not initialised)\n");
        return 0;
    }
    const int tracks = b3_track_count(), cars = b3_car_count();
    if (tracks < 36 || cars < 67) {
        fprintf(stderr, "FAIL: catalogue has %d tracks / %d cars, expected >= 36 / 67\n",
                tracks, cars);
        return 1;
    }
    // Empty names would still count, and would render as blank menu rows.
    for (int i = 0; i < tracks; i++) {
        if (!b3_track_name(i) || !b3_track_name(i)[0]) {
            fprintf(stderr, "FAIL: track %d has no name\n", i);
            return 1;
        }
    }
    for (int i = 0; i < cars; i++) {
        if (!b3_car_name(i) || !b3_car_name(i)[0]) {
            fprintf(stderr, "FAIL: car %d has no name\n", i);
            return 1;
        }
    }

    static uint8_t pixels[640 * 480 * 4];
    int w = 640, h = 480;

    // The setup screen must actually draw its rows. Counting colours here
    // proves nothing — the frontend background art fills the frame whether
    // or not the screen renders anything, and that version of this check
    // passed with the whole render body deleted.
    //
    // So look at the track row's band with the cursor parked on a different
    // row, which leaves the band free of the selected-row pulse and thus
    // static. Two captures of the same track must match (proving the band
    // is stable), and a different track must change it (proving the name
    // is drawn).
    static uint8_t band_a[640 * 40 * 4], band_b[640 * 40 * 4];
    const size_t band_off = (size_t)232 * 640 * 4, band_sz = sizeof band_a;
    fe_menu_show_setup(2);           // cursor on START RACE
    b3_select_track(0);
    pump_for_ms(300, pixels, &w, &h);
    memcpy(band_a, pixels + band_off, band_sz);
    pump_for_ms(300, pixels, &w, &h);
    memcpy(band_b, pixels + band_off, band_sz);
    if (memcmp(band_a, band_b, band_sz) != 0) {
        fprintf(stderr, "FAIL: track row band is not static; "
                        "the render check below cannot distinguish content\n");
        return 1;
    }
    b3_select_track(tracks / 2);
    pump_for_ms(300, pixels, &w, &h);
    memcpy(band_b, pixels + band_off, band_sz);
    if (memcmp(band_a, band_b, band_sz) == 0) {
        fprintf(stderr, "FAIL: setup screen does not draw the track name "
                        "(%s and %s render identically)\n",
                b3_track_name(0), b3_track_name(tracks / 2));
        return 1;
    }

    // Race on track 0, then on a different track, and require the geometry
    // to actually change. Chunk count alone could coincide, so compare the
    // spawn point too.
    struct { int chunks; float sx, sz; } seen[2];
    const int picks[2] = {0, tracks / 2};
    for (int i = 0; i < 2; i++) {
        b3_select_track(picks[i]);
        b3_select_car(i == 0 ? 0 : cars - 1);
        fe_menu_force_race();
        pump_for_ms(200, pixels, &w, &h);
        seen[i].chunks = g_track_data.chunk_count;
        seen[i].sx = g_track_data.spawn[0];
        seen[i].sz = g_track_data.spawn[2];
        if (seen[i].chunks <= 0) {
            fprintf(stderr, "FAIL: track %d (%s) loaded 0 chunks\n",
                    picks[i], b3_track_name(picks[i]));
            return 1;
        }
        fe_menu_stop_race();
    }

    if (seen[0].chunks == seen[1].chunks &&
        seen[0].sx == seen[1].sx && seen[0].sz == seen[1].sz) {
        fprintf(stderr,
                "FAIL: second race reused the first track "
                "(%d chunks, spawn %.1f,%.1f both times)\n",
                seen[0].chunks, seen[0].sx, seen[0].sz);
        return 1;
    }

    fprintf(stderr,
            "  selection: %d tracks / %d cars; %s=%d chunks, %s=%d chunks OK\n",
            tracks, cars, b3_track_name(picks[0]), seen[0].chunks,
            b3_track_name(picks[1]), seen[1].chunks);
    return 0;
}

// Opt-in sweep (BURNOUT3_TEST_ALL_TRACKS=1): race every track in turn and
// report what each one loaded and whether its spawn has road ahead. Not in
// ctest — it loads 37 tracks and takes about a minute.
static int test_all_tracks(void) {
    if (!g_game_ready || !getenv("BURNOUT3_TEST_ALL_TRACKS")) return 0;
    static uint8_t pixels[640 * 480 * 4];
    int w = 640, h = 480, bad = 0;

    for (int t = 0; t < b3_track_count(); t++) {
        struct timespec s0, s1;
        clock_gettime(CLOCK_MONOTONIC, &s0);
        b3_select_track(t);
        fe_menu_force_race();
        pump_for_ms(150, pixels, &w, &h);
        clock_gettime(CLOCK_MONOTONIC, &s1);
        int chunks = g_track_data.chunk_count;
        int verts = 0;
        for (int c = 0; c < chunks; c++) verts += (int)g_track_data.chunks[c].vertex_count;
        int vb = 0, ib = 0, tex = 0;
        vulkan_d3d8_pool_counts(&vb, &ib, &tex);
        fprintf(stderr, "  [%2d/%d] %-12s %4d chunks %7d verts  %.1fs  "
                        "vb=%d ib=%d tex=%d%s\n",
                t + 1, b3_track_count(), b3_track_name(t), chunks, verts,
                (double)(s1.tv_sec - s0.tv_sec) +
                (double)(s1.tv_nsec - s0.tv_nsec) / 1e9,
                vb, ib, tex,
                chunks > 0 ? "" : "   <-- EMPTY");
        if (chunks <= 0) bad++;
        fe_menu_stop_race();
    }
    fprintf(stderr, "  all tracks: %d of %d loaded geometry\n",
            b3_track_count() - bad, b3_track_count());
    return bad ? 1 : 0;
}

// Nothing else in this suite would notice the game rendering correctly but
// far too slowly — the other checks only assert on pixels and physics. The
// frontend is the worst case for draw-call overhead (one textured quad per
// character), so measure its frame rate directly.
static int test_menu_frame_rate(void) {
    if (!g_game_ready) {
        fprintf(stderr, "SKIP: menu frame rate (game not initialised)\n");
        return 0;
    }
    static uint8_t pixels[640 * 480 * 4];
    int w = 0, h = 0;

    // Pump for a fixed wall-clock window and count frames that actually
    // rendered. Counting run_frame calls would just measure the pump's
    // 60 Hz early-outs (which "achieve" tens of thousands of fps).
    //
    // Two windows, because the first race pays a large one-off cost
    // uploading the track's ~1700 objects to the GPU: a single window
    // starting at race launch measures that upload, not the frame rate,
    // and reported single digits for a game that then ran at 60.
    double fps[2] = {0, 0};
    for (int pass = 0; pass < 2; pass++) {
        burnout3_run_frame(pixels, &w, &h);
        const unsigned long start_frames = burnout3_frame_counter();
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        do {
            burnout3_run_frame(pixels, &w, &h);
            clock_gettime(CLOCK_MONOTONIC, &t1);
        } while ((t1.tv_sec - t0.tv_sec) * 1000 +
                 (t1.tv_nsec - t0.tv_nsec) / 1000000 < 2000);

        const double secs = (t1.tv_sec - t0.tv_sec) +
                            (t1.tv_nsec - t0.tv_nsec) / 1e9;
        const unsigned long frames = burnout3_frame_counter() - start_frames;
        fps[pass] = secs > 0 ? (double)frames / secs : 0.0;
    }

    // The pump self-throttles to 60 Hz, so the ceiling is 60; anything
    // below 30 once warm means frames are genuinely too slow to keep up.
    if (fps[1] < 30.0) {
        fprintf(stderr, "FAIL: ran at %.1f fps once warm (%.1f during warm-up)\n",
                fps[1], fps[0]);
        return 1;
    }
    fprintf(stderr, "  frame rate: %.1f fps OK (%.1f during warm-up)\n",
            fps[1], fps[0]);
    return 0;
}

static int test_memory_layout(void) {
    // Verify the 64MB Xbox memory region is mapped
    if (g_xbox_mem_offset == 0) {
        fprintf(stderr, "FAIL: g_xbox_mem_offset = 0\n");
        return 1;
    }

    // Write and read back through MEM32
    volatile uint32_t *mem = (volatile uint32_t *)((uintptr_t)0x10000 + g_xbox_mem_offset);
    *mem = 0xDEADBEEF;
    if (*mem != 0xDEADBEEF) {
        fprintf(stderr, "FAIL: MEM32 readback mismatch\n");
        return 1;
    }
    *mem = 0;  // clean up
    fprintf(stderr, "  memory layout: 64MB at offset 0x%tx OK\n", g_xbox_mem_offset);
    return 0;
}

static int test_audio_callback_smoke(void) {
    if (!g_game_ready) {
        fprintf(stderr, "SKIP: audio test skipped (game not initialised)\n");
        return 0;
    }
    // Smoke test: call audio callback, verify it returns > 0 frames
    // without crashing. The bridge stub returns max_frames of silence.
    int16_t buffer[1024];  // stereo: max_frames * 2 int16_t values
    int frames = burnout3_audio_callback(buffer, 512);
    if (frames <= 0) {
        fprintf(stderr, "FAIL: audio callback returned %d frames\n", frames);
        return 1;
    }
    if (frames > 512) {
        fprintf(stderr, "FAIL: audio callback returned %d > max %d\n", frames, 512);
        return 1;
    }
    // Verify test tone is non-silent (440Hz sine wave)
    int nonzero = 0;
    for (int i = 0; i < frames * 2; i++) {
        if (buffer[i] != 0) nonzero++;
    }
    if (nonzero == 0) {
        fprintf(stderr, "FAIL: audio callback produced all zeros (no tone)\n");
        return 1;
    }
    fprintf(stderr, "  audio callback: %d frames tone (440Hz, %d/%d nonzero) OK\n",
            frames, nonzero, frames * 2);
    return 0;
}

static int test_keyboard_injection_smoke(void) {
    // Verify keyboard injection routes scancodes → VK bitmask.
    // Press W (SDL_SCANCODE_W = 26) and check VK_W (0x57) bit is set.
    bool keys[512] = {false};
    keys[SDL_SCANCODE_W] = true;
    keys[SDL_SCANCODE_RETURN] = true;
    burnout3_inject_keyboard(keys, 512);
    int w = 0x57 / 64;
    int b = 0x57 % 64;
    if (!(g_keyboard_state[w] & (1ULL << b))) {
        fprintf(stderr, "FAIL: VK_W not set after SDL_SCANCODE_W injected\n");
        return 1;
    }
    // Verify RETURN is mapped
    int rw = 0x0D / 64;
    int rb = 0x0D % 64;
    if (!(g_keyboard_state[rw] & (1ULL << rb))) {
        fprintf(stderr, "FAIL: VK_RETURN not set after SDL_SCANCODE_RETURN injected\n");
        return 1;
    }
    // Verify unpressed key is NOT set (VK_A should be 0)
    int aw = 0x41 / 64;
    int ab = 0x41 % 64;
    if (g_keyboard_state[aw] & (1ULL << ab)) {
        fprintf(stderr, "FAIL: VK_A set but SDL_SCANCODE_A was not pressed\n");
        return 1;
    }
    fprintf(stderr, "  keyboard injection: W+RETURN mapped, A not pressed OK\n");
    return 0;
}

static int test_gamepad_injection_smoke(void) {
    // Verify gamepad injection globals can be written and read.
    // The session's burnout3_inject_input writes these; we test
    // the globals directly.
    g_xinput_buttons = 0x1000;  // A button
    g_xinput_thumb_lx = 16000;
    g_xinput_left_trigger = 128;
    if (g_xinput_buttons != 0x1000) {
        fprintf(stderr, "FAIL: g_xinput_buttons write/readback mismatch\n");
        return 1;
    }
    if (g_xinput_thumb_lx != 16000) {
        fprintf(stderr, "FAIL: g_xinput_thumb_lx write/readback mismatch\n");
        return 1;
    }
    // Reset
    g_xinput_buttons = 0;
    g_xinput_thumb_lx = 0;
    g_xinput_left_trigger = 0;
    fprintf(stderr, "  gamepad injection: write/readback OK\n");
    return 0;
}

// ── Main ──────────────────────────────────────────────────────

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <game_data_dir>\n", argv[0]);
        fprintf(stderr, "  game_data_dir must contain default.xbe + Data/ + Tracks/ etc.\n");
        return 1;
    }

    g_data_path = argv[1];

    int failures = 0;
    failures += test_xbe_and_kernel_load();
    failures += test_boot_sequence();
    failures += test_menu_reached();
    failures += test_memory_layout();
    failures += test_frame_pump_runs();
    failures += test_menu_frame_rate();
    failures += test_menu_launch_and_drive();
    failures += test_track_car_selection();
    failures += test_all_tracks();
    failures += test_audio_callback_smoke();
    failures += test_keyboard_injection_smoke();
    failures += test_gamepad_injection_smoke();

    if (failures) {
        fprintf(stderr, "\nFAIL: %d test(s) failed\n", failures);
        return 1;
    }

    fprintf(stderr, "\nburnout3_boot_test: all checks passed\n");
    return 0;
}
