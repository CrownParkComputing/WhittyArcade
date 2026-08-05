/**
 * burnout3_dxvk_pixels_test.c — the game's own frame, through DXVK.
 *
 * Same proof as burnout3_native_pixels_test, but the backend is DXVK's
 * native D3D8 rather than the hand-written Vulkan layer. If the clear the
 * game asks for comes back out of DXVK's back buffer, then the route
 * Xbox game code -> DXVK D3D8 -> Vulkan is real, and everything DXVK
 * already implements (shaders, states, formats) is available to the port.
 *
 * Needs a GPU and a display server; skips cleanly without them.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>

#include "b3_native_env.h"
#include "b3_native_runtime.h"

extern uint32_t g_esp;

int   b3_dxvk_init(int w, int h);
void  b3_dxvk_shutdown(void);
void *b3_dxvk_device(void);
void  b3_dxvk_begin(void);
void  b3_dxvk_end(void);
const unsigned char *b3_dxvk_readback(int *w, int *h);

void sub_0003FEE0(void);
void sub_00156400(void);   /* the game's main */

extern unsigned long b3_geo_buffers_created, b3_geo_buffers_reused, b3_geo_locks,
                     b3_geo_draws, b3_geo_primitives;
void b3_geo_reset(void);

static void *boot_thread(void *a) { (void)a; sub_00156400(); return NULL; }

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

    b3_dxvk_begin();
    sub_0003FEE0();
    b3_dxvk_end();
}

/* Prove the backend delivers pixels at all, before reading anything into
 * the game's frame. Without this the game's black clear would match a
 * black buffer for any reason whatsoever. */
static void test_dxvk_delivers_pixels(void)
{
    b3_dxvk_begin();
    extern void b3_dxvk_clear(unsigned, unsigned, float, unsigned);
    b3_dxvk_clear(0x83, 0x00FF7F00u, 1.0f, 0);   /* Xbox flags, orange */
    b3_dxvk_end();

    int w = 0, h = 0;
    const unsigned char *px = b3_dxvk_readback(&w, &h);
    CHECK(px != NULL, "no readback from DXVK");
    if (!px) return;
    const unsigned char *p = px + ((size_t)(h / 2) * w + w / 2) * 4;
    CHECK(!(p[0] == 0 && p[1] == 0 && p[2] == 0),
          "DXVK cleared to #FF7F00 but the read back centre pixel is black");
    fprintf(stderr, "  DXVK clear reads back #%02x%02x%02x\n", p[2], p[1], p[0]);
}

/* Now the game's own frame. */
static void test_game_frame_through_dxvk(void)
{
    memset(&g_b3_clear, 0, sizeof g_b3_clear);
    b3_call_reset();
    run_game_frame();

    CHECK(g_b3_clear.valid, "the game never issued a Clear");
    CHECK(b3_call_count("D3DDevice_SetTransform") > 0,
          "the game never set a transform");

    int w = 0, h = 0;
    const unsigned char *px = b3_dxvk_readback(&w, &h);
    CHECK(px != NULL, "no readback after the game's frame");
    if (!px || !g_b3_clear.valid) return;

    const uint8_t want_b = (uint8_t)( g_b3_clear.colour        & 0xFF);
    const uint8_t want_g = (uint8_t)((g_b3_clear.colour >>  8) & 0xFF);
    const uint8_t want_r = (uint8_t)((g_b3_clear.colour >> 16) & 0xFF);

    long matched = 0, total = 0;
    for (int y = h / 4; y < h * 3 / 4; y += 8)
        for (int x = w / 4; x < w * 3 / 4; x += 8) {
            const unsigned char *p = px + ((size_t)y * w + x) * 4;
            total++;
            if (p[2] == want_r && p[1] == want_g && p[0] == want_b) matched++;
        }
    CHECK(matched == total,
          "%ld of %ld pixels carry the game's clear #%02x%02x%02x",
          matched, total, want_r, want_g, want_b);

    fprintf(stderr, "  the game cleared %dx%d to #%02x%02x%02x through DXVK — "
                    "%ld/%ld pixels match\n",
            w, h, want_r, want_g, want_b, matched, total);
}

/* Boot the game with the DXVK backend live and see whether it submits
 * geometry. Everything before this drove the renderer with a hand-made
 * empty scene; this is the game building its own and drawing it. */
static void test_boot_submits_geometry(void)
{
    b3_geo_reset();
    b3_call_reset();

    g_esp = STK_VA;
    #define PUSHV(v) do { g_esp -= 4; *(uint32_t *)b3_env_ptr(g_esp) = (v); } while (0)
    PUSHV(0); PUSHV(0); PUSHV(0); PUSHV(0xDEADBEEF);
    #undef PUSHV

    /* Probe the frame on the drawing thread, right after each draw. The
     * game clears once per frame — 43 clears over this run — so a sampler
     * on another thread mostly catches cleared frames and reports black for
     * a renderer that is working. This is the measurement that answers
     * "does the game's geometry produce pixels". */
    setenv("B3_DRAW_PROBE", "1", 0);

    pthread_t th;
    pthread_create(&th, NULL, boot_thread, NULL);

    /* Wait for the first draw, then keep watching. Stopping at the first
     * draw measured nothing: it reported whatever had accumulated in the
     * same instant and called it the total. The real question is whether
     * draws keep coming — a game rendering menus should reach roughly 30
     * draws and 1200-1800 vertices per frame, so a handful that then stop
     * is a stall, not a working renderer. */
    for (int i = 0; i < 100 && b3_geo_draws == 0; i++) usleep(100000);
    const unsigned long first = b3_geo_draws;

    unsigned long prev = first;
    int quiet = 0;
    for (int i = 0; i < 100 && quiet < 20; i++) {
        usleep(100000);
        if (b3_geo_draws != prev) { prev = b3_geo_draws; quiet = 0; }
        else quiet++;
    }

    fprintf(stderr, "  boot: %lu vertex buffers (%lu reused), %lu locks, "
                    "%lu draws, %lu primitives\n",
            b3_geo_buffers_created, b3_geo_buffers_reused, b3_geo_locks,
            b3_geo_draws, b3_geo_primitives);
    fprintf(stderr, "  draws: %lu by first sample, %lu total%s\n",
            first, b3_geo_draws,
            b3_geo_draws == first && quiet >= 20
                ? " — STALLED, no further draws for 2s" : "");

    /* Submitting geometry is not the same as producing an image. Read the
     * back buffer and count what is not the clear colour: if the game's own
     * menu draws are landing, the frame cannot be uniformly black. */
    /* The boot thread clears and redraws continuously, so a single readback
     * can land between a clear and the draws that follow it and report
     * black for a renderer that is working. Sample repeatedly and keep the
     * best frame — the question is whether the game EVER puts pixels up. */
    long best_lit = 0, total = 0;
    double best_luma = 0.0;
    for (int s = 0; s < 24; s++) {
        int w = 0, h = 0;
        const unsigned char *px = b3_dxvk_readback(&w, &h);
        if (px) {
            long lit = 0; total = 0;
            unsigned long long acc = 0;
            for (int y = 0; y < h; y += 2)
                for (int x = 0; x < w; x += 2) {
                    const unsigned char *p = px + ((size_t)y * w + x) * 4;
                    total++;
                    unsigned v = p[0] + p[1] + p[2];
                    acc += v;
                    if (v > 24) lit++;
                }
            if (lit > best_lit) {
                best_lit = lit;
                best_luma = (double)acc / (3.0 * (total ? total : 1));
            }
        }
        usleep(40000);
    }
    { extern unsigned long b3_geo_lit_max;
      extern unsigned char *b3_geo_best_frame; extern int b3_geo_best_w, b3_geo_best_h;
      const char *dump = getenv("B3_FRAME_DUMP");
      if (dump && b3_geo_best_frame) {
          FILE *f = fopen(dump, "wb");
          if (f) {
              fprintf(f, "P6\n%d %d\n255\n", b3_geo_best_w, b3_geo_best_h);
              for (long i = 0; i < (long)b3_geo_best_w * b3_geo_best_h; i++) {
                  const unsigned char *q = b3_geo_best_frame + i * 4;
                  fputc(q[2], f); fputc(q[1], f); fputc(q[0], f);   /* BGRA -> RGB */
              }
              fclose(f);
              fprintf(stderr, "  wrote %s (%dx%d)\n", dump, b3_geo_best_w, b3_geo_best_h);
          }
      }
      fprintf(stderr, "  probe: best post-draw frame had %lu lit pixels\n",
              b3_geo_lit_max);
      CHECK(b3_geo_lit_max > 0,
            "the game issued %lu draws and every post-draw frame was black",
            b3_geo_draws); }
    fprintf(stderr, "  frame: best of 24 samples = %ld of %ld pixels non-black "
                    "(%.1f%%), mean luma %.1f\n",
            best_lit, total, total ? 100.0 * best_lit / total : 0.0, best_luma);
    /* Informational only. A zero here alongside a non-zero probe means the
     * frame was cleared between the draws and the sample, which is what the
     * game does every frame — not a rendering failure. */
    b3_call_dump();
    CHECK(b3_geo_buffers_created > 0,
          "the game created no vertex buffers while booting");
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "SKIP: no XBE path given\n"); return 0; }
    if (b3_env_init(argv[1]) != 0) {
        fprintf(stderr, "FAIL: b3_env_init(%s)\n", argv[1]);
        return 1;
    }
    if (!b3_dxvk_init(640, 480)) {
        fprintf(stderr, "SKIP: no DXVK device (no GPU or no display)\n");
        return 0;
    }

    test_dxvk_delivers_pixels();
    test_game_frame_through_dxvk();
    if (getenv("B3_DXVK_BOOT")) test_boot_submits_geometry();

    if (getenv("B3_DXVK_BOOT")) {
        /* The boot thread still owns the mapping; do not tear down. */
        fprintf(stderr, g_failures ? "burnout3_dxvk_pixels_test: FAILURES\n"
                                   : "burnout3_dxvk_pixels_test: all checks passed\n");
        fflush(stderr);
        _exit(g_failures ? 1 : 0);
    }
    b3_dxvk_shutdown();

    if (g_failures) {
        fprintf(stderr, "burnout3_dxvk_pixels_test: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    fprintf(stderr, "burnout3_dxvk_pixels_test: all checks passed\n");
    return 0;
}
