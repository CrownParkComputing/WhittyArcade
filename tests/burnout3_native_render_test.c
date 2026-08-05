/**
 * burnout3_native_render_test.c — the game's own frame render.
 *
 * sub_0003FEE0 is Criterion's RW frame render. This project previously
 * recorded it as an unsolvable blocker — "crashes on uninitialised device
 * context", with a circular dependency on the render-list dispatch — and
 * built an entire procedural renderer to avoid it. It is ordinary game
 * code. It faulted because D3D_g_pDevice was null, because the device was
 * assumed to be a static object at the wrong address.
 *
 * This runs it and asserts on the D3D8 sequence it issues. It is the
 * boundary between "the transform produces code that runs" and "the game
 * is driving the hardware layer".
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "b3_native_env.h"
#include "b3_native_runtime.h"

extern uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi;

void sub_0003FEE0(void);   /* RW frame render (game_obj, camera) */

static int g_failures = 0;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: ", __func__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
        g_failures++; \
    } \
} while (0)

/* Named by XbSymbolDatabase. The device is a *pointer* here, not an
 * object — the distinction that cost this project a 16 KB device snapshot
 * lifted out of xemu. */
#define D3D_G_PDEVICE  0x0035FB48u

#define DEV_VA  0x00800000u
#define OBJ_VA  0x00810000u
#define CAM_VA  0x00820000u
#define STK_VA  0x005F0000u

static void setup_world(void)
{
    memset(b3_env_ptr(DEV_VA), 0, 0x4000);
    memset(b3_env_ptr(OBJ_VA), 0, 0x4000);
    memset(b3_env_ptr(CAM_VA), 0, 0x4000);

    /* Identity matrices where the render path reads its set from. */
    for (int m = 0; m < 8; m++) {
        float *M = (float *)((char *)b3_env_ptr(OBJ_VA) + 0x500 + m * 0x40);
        for (int i = 0; i < 4; i++) M[i * 5] = 1.0f;
    }
    *(uint32_t *)b3_env_ptr(D3D_G_PDEVICE) = DEV_VA;
}

static void call_render(void)
{
    g_esp = STK_VA;
    /* The generated body pushes ebp itself, so lay down args then a
     * return-address slot exactly as x86 would. */
    #define PUSHV(v) do { g_esp -= 4; *(uint32_t *)b3_env_ptr(g_esp) = (v); } while (0)
    PUSHV(CAM_VA);          /* arg 2: camera   */
    PUSHV(OBJ_VA);          /* arg 1: game obj */
    PUSHV(0xDEADBEEF);      /* return address  */
    #undef PUSHV
    sub_0003FEE0();
}

/* It must run to completion. Returning at all is the thing that was
 * believed impossible. */
static void test_render_completes(void)
{
    setup_world();
    b3_call_reset();
    b3_icall_reset();
    call_render();
    CHECK(b3_call_distinct() > 0, "render made no external calls at all");
}

/* And it must drive the hardware layer coherently: reserve push-buffer
 * space, aim at a target, set the transform, clear. */
static void test_render_issues_d3d(void)
{
    setup_world();
    b3_call_reset();
    call_render();

    static const char *expected[] = {
        "D3D_MakeRequestedSpace",
        "D3DDevice_SetRenderTarget",
        "D3DDevice_SetTransform",
        "D3DDevice_Clear",
    };
    for (unsigned i = 0; i < sizeof expected / sizeof *expected; i++)
        CHECK(b3_call_count(expected[i]) > 0,
              "the game's renderer never called %s", expected[i]);

    if (g_failures) b3_call_dump();
}

/* The arguments, not just the call. Reading them wrong is the difference
 * between clearing the frame and clearing something else, and it cannot be
 * checked by counting calls. The convention is: the caller pushes
 * right-to-left then a return-address slot, so argument N sits at
 * esp + 4*(N+1). */
static void test_clear_arguments_decode(void)
{
    setup_world();
    b3_call_reset();
    memset(&g_b3_clear, 0, sizeof g_b3_clear);
    memset(&g_b3_xform, 0, sizeof g_b3_xform);
    call_render();

    CHECK(g_b3_clear.valid, "Clear was never reached");
    if (g_b3_clear.valid) {
        /* Whole-surface clear: no rect list. */
        CHECK(g_b3_clear.count == 0,
              "Clear count = %u, expected 0 (whole surface)", g_b3_clear.count);
        CHECK(g_b3_clear.rects == 0,
              "Clear pRects = %#x, expected NULL", g_b3_clear.rects);
        /* Flags must name at least one buffer; garbage here means the
         * stack offsets are wrong. */
        CHECK(g_b3_clear.flags != 0 && g_b3_clear.flags < 0x1000,
              "Clear flags = %#x — not a plausible D3DCLEAR mask",
              g_b3_clear.flags);
        /* Depth clears to the far plane. This is the strongest signal the
         * decode is right: 1.0f only appears if the float lands correctly. */
        CHECK(g_b3_clear.z == 1.0f,
              "Clear Z = %f, expected 1.0 (far plane)", (double)g_b3_clear.z);
        CHECK(g_b3_clear.stencil == 0,
              "Clear stencil = %u, expected 0", g_b3_clear.stencil);
    }

    CHECK(g_b3_xform.valid, "SetTransform was never reached");
    if (g_b3_xform.valid)
        CHECK(g_b3_xform.matrix != 0,
              "SetTransform was given a NULL matrix");
}

/* Running it twice must behave identically — a renderer that only works
 * on the first frame is not a renderer. */
static void test_render_is_repeatable(void)
{
    setup_world();
    b3_call_reset();
    call_render();
    const int first_clear = b3_call_count("D3DDevice_Clear");
    const int first_dist  = b3_call_distinct();

    b3_call_reset();
    call_render();
    CHECK(b3_call_count("D3DDevice_Clear") == first_clear,
          "second frame cleared %d times, first cleared %d",
          b3_call_count("D3DDevice_Clear"), first_clear);
    CHECK(b3_call_distinct() == first_dist,
          "second frame made %d distinct calls, first made %d",
          b3_call_distinct(), first_dist);
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "SKIP: no XBE path given\n"); return 0; }
    if (b3_env_init(argv[1]) != 0) {
        fprintf(stderr, "FAIL: b3_env_init(%s)\n", argv[1]);
        return 1;
    }

    test_render_completes();
    test_render_issues_d3d();
    test_clear_arguments_decode();
    test_render_is_repeatable();

    fprintf(stderr, "  render issued %d distinct calls, %d indirect targets\n",
            b3_call_distinct(), b3_icall_distinct());

    b3_env_shutdown();

    if (g_failures) {
        fprintf(stderr, "burnout3_native_render_test: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    fprintf(stderr, "burnout3_native_render_test: all checks passed\n");
    return 0;
}
