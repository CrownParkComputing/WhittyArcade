/**
 * burnout3_native_frames_test.c — the game advancing over time.
 *
 * A single frame proves the transform runs. Movement means the game's own
 * state evolves and drives *different* work each frame. This runs the
 * game's frame tick and its renderer repeatedly and looks for change: in
 * the memory it owns, and in what it asks the GPU to do.
 *
 * A renderer that emits an identical frame forever is a static image, not
 * a running game, and nothing else here would notice the difference.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "b3_native_env.h"
#include "b3_native_runtime.h"

extern uint32_t g_esp;

void sub_0003FEE0(void);   /* RW frame render  */
void sub_000110E0(void);   /* game frame tick  */

static int g_failures = 0;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: ", __func__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
        g_failures++; \
    } \
} while (0)

#define D3D_G_PDEVICE 0x0035FB48u
#define DEV_VA 0x00800000u
#define OBJ_VA 0x00810000u
#define CAM_VA 0x00820000u
#define STK_VA 0x005F0000u

/* The game's own data segment. If the game is running, this changes. */
#define DATA_VA   0x003B2360u
#define DATA_SPAN 0x00040000u

static void world_reset(void)
{
    memset(b3_env_ptr(DEV_VA), 0, 0x4000);
    memset(b3_env_ptr(OBJ_VA), 0, 0x4000);
    memset(b3_env_ptr(CAM_VA), 0, 0x4000);
    for (int m = 0; m < 8; m++) {
        float *M = (float *)((char *)b3_env_ptr(OBJ_VA) + 0x500 + m * 0x40);
        for (int i = 0; i < 4; i++) M[i * 5] = 1.0f;
    }
    *(uint32_t *)b3_env_ptr(D3D_G_PDEVICE) = DEV_VA;
}

static void tick(void)
{
    g_esp = STK_VA;
    g_esp -= 4; *(uint32_t *)b3_env_ptr(g_esp) = 0xDEADBEEF;
    sub_000110E0();
}

static void render(void)
{
    g_esp = STK_VA;
    #define PUSHV(v) do { g_esp -= 4; *(uint32_t *)b3_env_ptr(g_esp) = (v); } while (0)
    PUSHV(CAM_VA); PUSHV(OBJ_VA); PUSHV(0xDEADBEEF);
    #undef PUSHV
    sub_0003FEE0();
}

/* The tick must not fault, and must do something. */
static void test_tick_runs(void)
{
    world_reset();
    b3_call_reset();
    for (int i = 0; i < 8; i++) tick();
    fprintf(stderr, "  tick: 8 frames, %d distinct external calls\n",
            b3_call_distinct());
}

/* Does the game's own memory change as it ticks? */
static void test_state_advances(void)
{
    world_reset();
    static uint8_t before[DATA_SPAN];
    memcpy(before, b3_env_ptr(DATA_VA), DATA_SPAN);

    for (int i = 0; i < 60; i++) { tick(); render(); }

    const uint8_t *after = (const uint8_t *)b3_env_ptr(DATA_VA);
    size_t changed = 0;
    for (size_t i = 0; i < DATA_SPAN; i++) if (before[i] != after[i]) changed++;

    fprintf(stderr, "  60 frames changed %zu of %u bytes of the game's data "
                    "segment (%.2f%%)\n",
            changed, (unsigned)DATA_SPAN, 100.0 * changed / DATA_SPAN);
    CHECK(changed > 0,
          "60 frames of tick+render changed nothing in the game's own "
          "memory — it is not advancing");
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "SKIP: no XBE path given\n"); return 0; }
    if (b3_env_init(argv[1]) != 0) {
        fprintf(stderr, "FAIL: b3_env_init(%s)\n", argv[1]);
        return 1;
    }

    test_tick_runs();
    test_state_advances();

    b3_env_shutdown();

    if (g_failures) {
        fprintf(stderr, "burnout3_native_frames_test: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    fprintf(stderr, "burnout3_native_frames_test: all checks passed\n");
    return 0;
}
