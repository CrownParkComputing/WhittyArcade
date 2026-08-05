/**
 * burnout3_collision_unit_test.c — unit tests for track_collision.
 *
 * Builds synthetic tracks (a floor, a wall, a corner, an overpass) rather
 * than loading a real one, so each check isolates one behaviour and the
 * expected answer is known exactly. Real-track coverage lives in the boot
 * test, which drives the car into the harbour.
 *
 * No Vulkan, no game data — safe for headless CI.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "track_loader.h"
#include "track_collision.h"

/* Step height used by the caller; kerbs below this are mountable. */
#define B3_STEP 1.2f

static int g_failures = 0;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: ", __func__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
        g_failures++; \
    } \
} while (0)

/* ── Synthetic track construction ────────────────────────────────
 * One chunk per quad. Each quad is a 4-vertex triangle strip, which is
 * exactly how the shipped geometry stores flat surfaces. */

#define MAX_QUADS 8

typedef struct {
    TrackData    track;
    TrackChunk   chunks[MAX_QUADS];
    TrackVertex  verts[MAX_QUADS][4];
    uint16_t     idx[MAX_QUADS][4];
    uint32_t     breaks[MAX_QUADS];
    uint32_t     lens[MAX_QUADS];
    uint8_t      prims[MAX_QUADS];
    float        bbox[MAX_QUADS][6];
    int          count;
} FakeTrack;

/* Add a quad from its four corners, in strip order (a,b,c,d). */
static void add_quad(FakeTrack *ft, const float c0[3], const float c1[3],
                     const float c2[3], const float c3[3])
{
    if (ft->count >= MAX_QUADS) { fprintf(stderr, "add_quad: full\n"); return; }
    const int q = ft->count++;
    const float *src[4] = {c0, c1, c2, c3};

    float *bb = ft->bbox[q];
    bb[0] = bb[1] = bb[2] =  1e30f;
    bb[3] = bb[4] = bb[5] = -1e30f;

    for (int i = 0; i < 4; i++) {
        memset(&ft->verts[q][i], 0, sizeof(TrackVertex));
        ft->verts[q][i].x = src[i][0];
        ft->verts[q][i].y = src[i][1];
        ft->verts[q][i].z = src[i][2];
        ft->idx[q][i] = (uint16_t)i;
        for (int a = 0; a < 3; a++) {
            if (src[i][a] < bb[a])     bb[a]     = src[i][a];
            if (src[i][a] > bb[3 + a]) bb[3 + a] = src[i][a];
        }
    }
    ft->breaks[q] = 0;
    ft->lens[q]   = 4;
    ft->prims[q]  = 6;   /* triangle strip */

    TrackChunk *ch = &ft->chunks[q];
    memset(ch, 0, sizeof(*ch));
    ch->vertices     = ft->verts[q];
    ch->vertex_count = 4;
    ch->indices      = ft->idx[q];
    ch->index_count  = 4;
    ch->strip_breaks = &ft->breaks[q];
    ch->strip_lens   = &ft->lens[q];
    ch->strip_prims  = &ft->prims[q];
    ch->strip_break_count = 1;
    ch->obj_bbox     = ft->bbox[q];
    ch->center[0] = (bb[0] + bb[3]) * 0.5f;
    ch->center[1] = (bb[1] + bb[4]) * 0.5f;
    ch->center[2] = (bb[2] + bb[5]) * 0.5f;
}

/* Horizontal quad at height y spanning [x0,x1] × [z0,z1]. */
static void add_floor(FakeTrack *ft, float y, float x0, float z0,
                      float x1, float z1)
{
    const float a[3] = {x0, y, z0}, b[3] = {x1, y, z0};
    const float c[3] = {x0, y, z1}, d[3] = {x1, y, z1};
    add_quad(ft, a, b, c, d);
}

/* Vertical quad along Z at x, spanning [z0,z1] and [y0,y1]. */
static void add_wall_x(FakeTrack *ft, float x, float z0, float z1,
                       float y0, float y1)
{
    const float a[3] = {x, y0, z0}, b[3] = {x, y1, z0};
    const float c[3] = {x, y0, z1}, d[3] = {x, y1, z1};
    add_quad(ft, a, b, c, d);
}

/* Vertical quad along X at z. */
static void add_wall_z(FakeTrack *ft, float z, float x0, float x1,
                       float y0, float y1)
{
    const float a[3] = {x0, y0, z}, b[3] = {x0, y1, z};
    const float c[3] = {x1, y0, z}, d[3] = {x1, y1, z};
    add_quad(ft, a, b, c, d);
}

static void finish(FakeTrack *ft)
{
    memset(&ft->track, 0, sizeof(ft->track));
    ft->track.chunks = ft->chunks;
    ft->track.chunk_count = ft->count;
}

/* ── Tests ───────────────────────────────────────────────────────── */

static void test_build_rejects_empty(void)
{
    TrackData empty;
    memset(&empty, 0, sizeof empty);
    CHECK(track_collision_build(&empty) == NULL, "built a grid from no chunks");
    CHECK(track_collision_build(NULL) == NULL, "built a grid from NULL");
}

static void test_ground_height(void)
{
    FakeTrack ft; memset(&ft, 0, sizeof ft);
    add_floor(&ft, 10.0f, -50, -50, 50, 50);
    finish(&ft);
    TrackCollision *tc = track_collision_build(&ft.track);
    CHECK(tc != NULL, "no grid for a floor");
    if (!tc) return;

    float y = -999;
    CHECK(track_collision_ground(tc, 0, 0, 10.0f, 20, 20, &y),
          "no ground over the floor");
    CHECK(fabsf(y - 10.0f) < 0.01f, "ground y = %.3f, expected 10", y);

    /* Off the edge of the quad there is nothing to stand on. */
    CHECK(!track_collision_ground(tc, 500, 500, 10.0f, 20, 20, &y),
          "found ground 500 units off the floor");

    /* Outside the search window it must not be reported either. */
    CHECK(!track_collision_ground(tc, 0, 0, 200.0f, 20, 20, &y),
          "reported a floor 190 units below the search window");

    track_collision_free(tc);
}

static void test_ground_picks_the_surface_above(void)
{
    /* An overpass over a road: standing on the deck must report the deck. */
    FakeTrack ft; memset(&ft, 0, sizeof ft);
    add_floor(&ft, 0.0f,  -50, -50, 50, 50);
    add_floor(&ft, 30.0f, -50, -50, 50, 50);
    finish(&ft);
    TrackCollision *tc = track_collision_build(&ft.track);
    CHECK(tc != NULL, "no grid for the overpass");
    if (!tc) return;

    float y = -999;
    CHECK(track_collision_ground(tc, 0, 0, 30.0f, 5, 5, &y) &&
          fabsf(y - 30.0f) < 0.01f, "on the deck, got y=%.2f", y);
    CHECK(track_collision_ground(tc, 0, 0, 0.0f, 5, 5, &y) &&
          fabsf(y) < 0.01f, "under the deck, got y=%.2f", y);

    track_collision_free(tc);
}

static void test_wall_blocks(void)
{
    FakeTrack ft; memset(&ft, 0, sizeof ft);
    add_floor(&ft, 0.0f, -50, -50, 50, 50);
    add_wall_x(&ft, 20.0f, -50, 50, 0, 10);   /* wall at x = 20 */
    finish(&ft);
    TrackCollision *tc = track_collision_build(&ft.track);
    CHECK(tc != NULL, "no grid for the wall");
    if (!tc) return;

    const float from[3] = {0, 2, 0};
    const float to[3]   = {40, 2, 0};         /* straight through the wall */
    float out[3], n[3];
    int hit = track_collision_move(tc, from, to, 2.0f, B3_STEP, out, n);
    CHECK(hit, "drove through the wall unimpeded");
    CHECK(out[0] < 19.0f, "ended at x=%.2f, past the wall at 20", out[0]);
    CHECK(out[0] > 16.0f, "pushed back to x=%.2f, far short of the wall", out[0]);
    CHECK(fabsf(n[1]) < 0.01f, "wall normal has vertical component %.3f", n[1]);

    track_collision_free(tc);
}

static void test_floor_is_not_a_wall(void)
{
    /* The bug this guards: treating an object's bounds as solid makes the
     * road itself block the car, because the box around a road strip is a
     * flat slab the car is permanently inside. */
    FakeTrack ft; memset(&ft, 0, sizeof ft);
    add_floor(&ft, 0.0f, -200, -200, 200, 200);
    finish(&ft);
    TrackCollision *tc = track_collision_build(&ft.track);
    CHECK(tc != NULL, "no grid for the open floor");
    if (!tc) return;

    const float from[3] = {0, 1, 0};
    const float to[3]   = {0, 1, 150};
    float out[3];
    CHECK(!track_collision_move(tc, from, to, 2.0f, B3_STEP, out, NULL),
          "the floor blocked the car");
    CHECK(fabsf(out[2] - 150.0f) < 0.01f,
          "driving over open floor ended at z=%.2f, expected 150", out[2]);

    track_collision_free(tc);
}

static void test_kerb_is_mountable_but_barrier_is_not(void)
{
    /* Kerbs and pavement risers about a metre tall line the city roads and
     * are modelled as vertical strips at road level. Without a step height
     * the sphere — whose underside sits on the tarmac — is stopped by every
     * one of them, which walls the car in on open road. */
    FakeTrack ft; memset(&ft, 0, sizeof ft);
    add_floor(&ft, 0.0f, -100, -100, 100, 100);
    add_wall_x(&ft, 10.0f, -100, 100, 0.0f, 1.0f);    /* kerb, below step */
    add_wall_x(&ft, 40.0f, -100, 100, 0.0f, 5.0f);    /* barrier, above it */
    finish(&ft);
    TrackCollision *tc = track_collision_build(&ft.track);
    CHECK(tc != NULL, "no grid");
    if (!tc) return;

    const float from[3] = {0, 2, 0};
    const float to[3]   = {30, 2, 0};       /* over the kerb, short of the wall */
    float out[3];
    CHECK(!track_collision_move(tc, from, to, 2.0f, B3_STEP, out, NULL),
          "the 1-unit kerb blocked the car");
    CHECK(fabsf(out[0] - 30.0f) < 0.01f,
          "kerb stopped the car at x=%.2f, expected 30", out[0]);

    const float far_to[3] = {60, 2, 0};     /* now into the barrier */
    CHECK(track_collision_move(tc, from, far_to, 2.0f, B3_STEP, out, NULL),
          "the 5-unit barrier did not block the car");
    CHECK(out[0] < 39.0f, "ended at x=%.2f, past the barrier at 40", out[0]);

    track_collision_free(tc);
}

static void test_no_tunnelling_at_speed(void)
{
    /* One frame at 0.25 s (the physics dt clamp) and 75 units/s moves 19
     * units — more than a wall is thick, so a single unswept test would
     * step straight past it. */
    FakeTrack ft; memset(&ft, 0, sizeof ft);
    add_floor(&ft, 0.0f, -100, -100, 100, 100);
    add_wall_z(&ft, 0.0f, -100, 100, 0, 10);
    finish(&ft);
    TrackCollision *tc = track_collision_build(&ft.track);
    CHECK(tc != NULL, "no grid");
    if (!tc) return;

    const float from[3] = {0, 2, -20};
    const float to[3]   = {0, 2,  20};        /* 40 units in one call */
    float out[3];
    CHECK(track_collision_move(tc, from, to, 2.0f, B3_STEP, out, NULL),
          "tunnelled through the wall");
    CHECK(out[2] < 0.0f, "ended at z=%.2f, past the wall at 0", out[2]);

    track_collision_free(tc);
}

static void test_corner_does_not_launch(void)
{
    /* Two walls at once must separate the car by what one wall needs, not
     * by the sum — the naive version fires it out of the corner. */
    FakeTrack ft; memset(&ft, 0, sizeof ft);
    add_floor(&ft, 0.0f, -100, -100, 100, 100);
    add_wall_x(&ft, 10.0f, -100, 100, 0, 10);
    add_wall_z(&ft, 10.0f, -100, 100, 0, 10);
    finish(&ft);
    TrackCollision *tc = track_collision_build(&ft.track);
    CHECK(tc != NULL, "no grid");
    if (!tc) return;

    const float from[3] = {5, 2, 5};
    const float to[3]   = {9.5f, 2, 9.5f};    /* into the corner */
    float out[3];
    track_collision_move(tc, from, to, 2.0f, B3_STEP, out, NULL);
    float dx = out[0] - to[0], dz = out[2] - to[2];
    float pushed = sqrtf(dx * dx + dz * dz);
    CHECK(pushed < 6.0f, "corner pushed the car %.2f units", pushed);
    CHECK(out[0] < 10.0f && out[2] < 10.0f,
          "ended outside the corner at (%.2f, %.2f)", out[0], out[2]);

    track_collision_free(tc);
}

static void test_grid_narrows_the_search(void)
{
    /* The point of the grid: a query must examine a handful of objects,
     * not the whole track. The scan it replaces was O(chunks x verts)
     * every tick. */
    FakeTrack ft; memset(&ft, 0, sizeof ft);
    for (int i = 0; i < MAX_QUADS; i++)
        add_floor(&ft, 0.0f, (float)(i * 400), -50, (float)(i * 400 + 100), 50);
    finish(&ft);
    TrackCollision *tc = track_collision_build(&ft.track);
    CHECK(tc != NULL, "no grid");
    if (!tc) return;

    float y;
    track_collision_ground(tc, 50, 0, 0, 5, 5, &y);
    int cells = 0, refs = 0, probed = 0;
    track_collision_stats(tc, &cells, &refs, &probed);
    CHECK(probed <= 2, "ground query probed %d of %d objects",
          probed, ft.count);
    CHECK(cells > 0 && refs > 0, "empty grid: %d cells, %d refs", cells, refs);

    track_collision_free(tc);
}

int main(void)
{
    test_build_rejects_empty();
    test_ground_height();
    test_ground_picks_the_surface_above();
    test_wall_blocks();
    test_floor_is_not_a_wall();
    test_kerb_is_mountable_but_barrier_is_not();
    test_no_tunnelling_at_speed();
    test_corner_does_not_launch();
    test_grid_narrows_the_search();

    if (g_failures) {
        fprintf(stderr, "burnout3_collision_unit_test: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    fprintf(stderr, "burnout3_collision_unit_test: all checks passed\n");
    return 0;
}
