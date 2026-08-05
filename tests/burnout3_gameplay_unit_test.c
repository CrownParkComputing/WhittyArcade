/**
 * burnout3_gameplay_unit_test.c — unit tests for the native drive physics
 * (b3_drive_physics in burnout3_game_frame_pump.c): acceleration toward
 * the drag equilibrium, boost/normal speed caps, braking, speed-scaled
 * steering, position integration along the heading, and the road
 * constraint (revert-to-last-good when leaving road geometry).
 *
 * Links the real burnout3_recomp library; Xbox memory comes from the
 * kernel shim's mmap. No Vulkan, no game data — safe for headless CI.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "track_loader.h"

/* From burnout3_recomp */
extern int  xbox_MemoryLayoutInit(const void *xbe_data, unsigned long xbe_size);
extern void xbox_MemoryLayoutShutdown(void);
extern ptrdiff_t g_xbox_mem_offset;
extern TrackData g_track_data;
void b3_drive_physics(float dt, float throttle, float brake,
                      float steer, int boost);
/* Clears the teleport guard's baseline. Deliberately moving the car —
 * a respawn, or a test placing it for a scenario — must call this, or the
 * guard treats the move as an outside write and undoes it. */
void b3_physics_resync(void);

#define PHYS_BODY 0x5FFF00
#define PF(off) (*(volatile float *)((uintptr_t)(PHYS_BODY + (off)) + g_xbox_mem_offset))

static int g_failures = 0;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: ", __func__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
        g_failures++; \
    } \
} while (0)

static void reset_phys(float x, float z, float hdg) {
    PF(0x10) = x; PF(0x14) = z; PF(0x18) = hdg; PF(0x1C) = 0.0f;
    b3_physics_resync();   /* this placement is intentional */
}

static void ticks(int n, float thr, float brk, float steer, int boost) {
    for (int i = 0; i < n; i++)
        b3_drive_physics(1.0f / 60.0f, thr, brk, steer, boost);
}

static void test_accelerates_to_equilibrium(void) {
    reset_phys(0, 0, 0);
    ticks(600, 1.0f, 0, 0, 0);   /* 10 s full throttle */
    float spd = PF(0x1C);
    /* accel 35, drag 0.8/s → equilibrium ≈ 43.75, capped at 50 */
    CHECK(spd > 38.0f && spd <= 50.0f, "cruise speed %.1f not in (38..50]", spd);
    /* heading 0 → movement along +Z only */
    CHECK(fabsf(PF(0x10)) < 0.001f, "drifted in X: %.3f", PF(0x10));
    CHECK(PF(0x14) > 300.0f, "moved only %.1f in Z after 10s", PF(0x14));
}

static void test_boost_cap(void) {
    reset_phys(0, 0, 0);
    ticks(900, 1.0f, 0, 0, 1);   /* 15 s boosting */
    float spd = PF(0x1C);
    CHECK(spd > 50.0f && spd <= 75.01f, "boost speed %.1f not in (50..75]", spd);
}

static void test_brake_stops(void) {
    reset_phys(0, 0, 0);
    ticks(300, 1.0f, 0, 0, 0);
    float cruise = PF(0x1C);
    ticks(120, 0.0f, 1.0f, 0, 0);   /* 2 s hard braking */
    CHECK(PF(0x1C) < cruise * 0.25f,
          "brake left %.1f of %.1f", PF(0x1C), cruise);
    CHECK(PF(0x1C) >= 0.0f, "speed went negative: %.2f", PF(0x1C));
}

static void test_steering_scales_with_speed(void) {
    /* At rest, steering must have (almost) no authority */
    reset_phys(0, 0, 0);
    ticks(60, 0.0f, 0, 1.0f, 0);
    float hdg_at_rest = PF(0x18);
    CHECK(fabsf(hdg_at_rest) < 0.01f, "steered %.3f rad at rest", hdg_at_rest);

    /* At speed, one second of full lock turns substantially */
    reset_phys(0, 0, 0);
    ticks(300, 1.0f, 0, 0, 0);
    float hdg_before = PF(0x18);
    ticks(60, 1.0f, 0, 1.0f, 0);
    float turned = PF(0x18) - hdg_before;
    CHECK(turned > 0.8f && turned < 2.5f,
          "turned %.2f rad in 1s at speed", turned);
}

/* Build a fake track: a straight road of upward-facing vertices along +Z
 * at x=0, so the road constraint has real geometry to check against. */
static TrackVertex g_road_verts[64];
static TrackChunk  g_road_chunk;

static void install_fake_road(void) {
    for (int i = 0; i < 64; i++) {
        memset(&g_road_verts[i], 0, sizeof(TrackVertex));
        g_road_verts[i].x = (float)((i % 2) * 10 - 5);  /* two lanes: -5, +5 */
        g_road_verts[i].y = 0.0f;
        g_road_verts[i].z = (float)(i / 2) * 20.0f;     /* 0..620 */
        /* packed normal: ny bits [21:11]; 1023 = straight up */
        g_road_verts[i].packed_normal = (uint32_t)1023 << 11;
    }
    memset(&g_road_chunk, 0, sizeof(g_road_chunk));
    g_road_chunk.vertices = g_road_verts;
    g_road_chunk.vertex_count = 64;
    g_road_chunk.center[0] = 0; g_road_chunk.center[2] = 310;
    memset(&g_track_data, 0, sizeof(g_track_data));
    g_track_data.chunks = &g_road_chunk;
    g_track_data.chunk_count = 1;
}

static void remove_fake_road(void) {
    memset(&g_track_data, 0, sizeof(g_track_data));
}

static void test_road_constraint_allows_road(void) {
    install_fake_road();
    reset_phys(0, 10, 0);           /* on the road, driving along it */
    ticks(300, 1.0f, 0, 0, 0);
    CHECK(PF(0x14) > 100.0f, "on-road drive blocked at z=%.1f", PF(0x14));
    remove_fake_road();
}

static void test_road_constraint_blocks_offroad(void) {
    install_fake_road();
    reset_phys(0, 10, 0);
    ticks(60, 1.0f, 0, 0, 0);       /* get moving on the road */
    /* Turn hard toward +X — off the road — and keep driving */
    PF(0x18) = 1.5708f;             /* heading = due +X */
    ticks(300, 1.0f, 0, 0, 0);
    float x = PF(0x10);
    CHECK(x < 60.0f, "constraint let the car %f units off the road", x);
    remove_fake_road();
}

int main(void) {
    if (!xbox_MemoryLayoutInit(NULL, 0)) {
        fprintf(stderr, "FAIL: xbox_MemoryLayoutInit\n");
        return 1;
    }

    test_accelerates_to_equilibrium();
    test_boost_cap();
    test_brake_stops();
    test_steering_scales_with_speed();
    test_road_constraint_allows_road();
    test_road_constraint_blocks_offroad();

    xbox_MemoryLayoutShutdown();

    if (g_failures) {
        fprintf(stderr, "burnout3_gameplay_unit_test: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    fprintf(stderr, "burnout3_gameplay_unit_test: all checks passed\n");
    return 0;
}
