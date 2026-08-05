/**
 * burnout3_native_pixels_test.c — pixels produced by the game's own code.
 *
 * Brings up the Vulkan backend, runs Criterion's RW frame render, and
 * reads the framebuffer back. The clear the game asks for has to appear
 * in the pixels. That is the whole point: not "the transformed code runs"
 * and not "it called Clear", but that the frame on screen was decided by
 * the game rather than by anything hand-written here.
 *
 * Needs a GPU. Skips cleanly when Vulkan is unavailable.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "b3_native_env.h"
#include "b3_native_runtime.h"

extern uint32_t g_esp;

struct IDirect3DDevice8;
struct IDirect3DDevice8 *vulkan_d3d8_get_device(void);
int  vulkan_d3d8_init(int width, int height);
void vulkan_d3d8_shutdown(void);
const uint8_t *vulkan_d3d8_present(int *out_width, int *out_height);
/* The D3D8 Present() that flushes the render target into the host-visible
 * readback buffer. Without it vulkan_d3d8_present() hands back an
 * untouched buffer — which is why the first version of this test saw
 * black no matter what anyone drew. */
void vulkan_d3d8_trigger_present(void);

void sub_0003FEE0(void);
void b3_scene_begin(void);
void b3_scene_end(void);
void b3_device_clear(uint32_t colour);

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

static void run_game_frame(void)
{
    memset(b3_env_ptr(DEV_VA), 0, 0x4000);
    memset(b3_env_ptr(OBJ_VA), 0, 0x4000);
    memset(b3_env_ptr(CAM_VA), 0, 0x4000);
    for (int m = 0; m < 8; m++) {
        float *M = (float *)((char *)b3_env_ptr(OBJ_VA) + 0x500 + m * 0x40);
        for (int i = 0; i < 4; i++) M[i * 5] = 1.0f;
    }
    *(uint32_t *)b3_env_ptr(D3D_G_PDEVICE) = DEV_VA;

    g_esp = STK_VA;
    #define PUSHV(v) do { g_esp -= 4; *(uint32_t *)b3_env_ptr(g_esp) = (v); } while (0)
    PUSHV(CAM_VA); PUSHV(OBJ_VA); PUSHV(0xDEADBEEF);
    #undef PUSHV

    /* The frame has to be inside a scene or nothing is submitted and the
     * readback returns an untouched buffer — which is how the first
     * version of this test passed while doing nothing at all. */
    b3_scene_begin();
    sub_0003FEE0();
    b3_scene_end();
    vulkan_d3d8_trigger_present();
}

/* Before trusting anything about the game's frame: prove the backend can
 * clear to a colour and hand it back. The game clears to black, so a
 * readback that is black for any reason at all would make the real test
 * pass while doing nothing — which is exactly what happened the first
 * time this file was written. */
static void test_backend_clear_actually_works(void)
{
    struct IDirect3DDevice8 *dev = vulkan_d3d8_get_device();
    CHECK(dev != NULL, "no D3D8 device from the backend");
    if (!dev) return;

    b3_scene_begin();
    b3_device_clear(0x00FF7F00u);      /* orange: never a default */
    b3_scene_end();
    vulkan_d3d8_trigger_present();

    int w = 0, h = 0;
    const uint8_t *px = vulkan_d3d8_present(&w, &h);
    CHECK(px && w > 0 && h > 0, "no framebuffer read back");
    if (!px || w <= 0) return;

    const uint8_t *p = px + ((size_t)(h / 2) * w + w / 2) * 4;
    CHECK(!(p[0] == 0 && p[1] == 0 && p[2] == 0),
          "backend cleared to #FF7F00 but the readback is black — the "
          "clear/present path is not delivering pixels, so nothing can be "
          "concluded about the game's own frame");
}

/* The frame the game cleared must come back with the colour it asked for. */
static void test_game_clear_reaches_the_framebuffer(void)
{
    memset(&g_b3_clear, 0, sizeof g_b3_clear);
    run_game_frame();

    CHECK(g_b3_clear.valid, "the game never issued a Clear");
    if (!g_b3_clear.valid) return;

    int w = 0, h = 0;
    const uint8_t *px = vulkan_d3d8_present(&w, &h);
    CHECK(px != NULL, "no framebuffer read back");
    CHECK(w == 640 && h == 480, "framebuffer is %dx%d", w, h);
    if (!px || w <= 0 || h <= 0) return;

    /* The colour the game passed, as the backend stores it (BGRA). */
    const uint8_t want_b = (uint8_t)( g_b3_clear.colour        & 0xFF);
    const uint8_t want_g = (uint8_t)((g_b3_clear.colour >>  8) & 0xFF);
    const uint8_t want_r = (uint8_t)((g_b3_clear.colour >> 16) & 0xFF);

    /* Sample well inside the frame, away from any border. */
    long matched = 0, total = 0;
    for (int y = h / 4; y < h * 3 / 4; y += 8)
        for (int x = w / 4; x < w * 3 / 4; x += 8) {
            const uint8_t *p = px + ((size_t)y * w + x) * 4;
            total++;
            if (p[0] == want_r && p[1] == want_g && p[2] == want_b) matched++;
        }
    CHECK(total > 0, "sampled no pixels");
    CHECK(matched == total,
          "%ld of %ld sampled pixels carry the game's clear colour "
          "#%02x%02x%02x (first pixel is #%02x%02x%02x)",
          matched, total, want_r, want_g, want_b,
          px[0], px[1], px[2]);

    fprintf(stderr, "  the game cleared %dx%d to #%02x%02x%02x, Z=%.1f — "
                    "%ld/%ld sampled pixels match\n",
            w, h, want_r, want_g, want_b, (double)g_b3_clear.z, matched, total);
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "SKIP: no XBE path given\n"); return 0; }
    if (b3_env_init(argv[1]) != 0) {
        fprintf(stderr, "FAIL: b3_env_init(%s)\n", argv[1]);
        return 1;
    }
    if (!vulkan_d3d8_init(640, 480)) {
        fprintf(stderr, "SKIP: no Vulkan device available\n");
        b3_env_shutdown();
        return 0;
    }

    test_backend_clear_actually_works();
    test_game_clear_reaches_the_framebuffer();

    vulkan_d3d8_shutdown();
    b3_env_shutdown();

    if (g_failures) {
        fprintf(stderr, "burnout3_native_pixels_test: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    fprintf(stderr, "burnout3_native_pixels_test: all checks passed\n");
    return 0;
}
