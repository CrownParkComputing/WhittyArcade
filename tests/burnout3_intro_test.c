/**
 * burnout3_intro_test.c — the intro, driven by the game's own native code.
 *
 * The retail intro is three FMVs followed by the menu. This test boots the
 * statically recompiled game and asks, of the real code rather than of a
 * stand-in, how far along that sequence it gets and what it puts on screen.
 *
 * It deliberately asserts on stage rather than on a finished picture. The
 * port is mid-bring-up: pinning "the intro looks right" would fail for
 * years, while pinning "the game reaches stage N and draws M primitives"
 * fails the moment a change moves it backwards. That is the regression
 * this catches, and it has already caught several.
 *
 * Set B3_INTRO_DUMP=<path> to write the frame as a PPM and look at it.
 *
 * Needs a GPU and a display server; skips cleanly without them.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#include "b3_native_env.h"
#include "b3_native_runtime.h"

extern uint32_t g_esp;
extern volatile uint32_t g_b3_movie_finished_latched;
void xbox_kernel_init(void);
void xbox_path_init(const char *game_dir, const char *save_dir);
void burnout3_kernel_service_resource_worker(void);

int   b3_dxvk_init(int w, int h);
const unsigned char *b3_dxvk_readback(int *w, int *h);

void sub_00156400(void);      /* the game's main */
void sub_000165F0(void);      /* one retail game/frontend frame */

extern unsigned long b3_geo_draws, b3_geo_primitives, b3_geo_buffers_created;
extern unsigned long b3_geo_lit_max;
extern unsigned char *b3_geo_best_frame;
extern int b3_geo_best_w, b3_geo_best_h;
void b3_geo_reset(void);

static int g_failures = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: ", __func__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); g_failures++; \
    } \
} while (0)

#define STK_VA 0x005F0000u

static volatile int s_boot_complete;
static void *boot_thread(void *a) {
    (void)a;
    sub_00156400();
    s_boot_complete = 1;
    return NULL;
}

/* Which of the intro's stages has the game actually reached? Each is
 * evidenced by calls only that stage makes, so the answer comes from the
 * game's behaviour rather than from a flag we set ourselves. */
static const char *intro_stage(void)
{
    /* The FMVs decode through the XBE's own XMV section. No call into it
     * means playback has not started, whatever else is on screen. */
    const int fmv   = b3_call_count("XMV_Play") > 0;
    const int draws = b3_geo_draws > 0;
    const int clear = b3_call_count("D3DDevice_Clear") > 0;

    if (fmv)   return "FMV playback";
    if (draws) return "pre-FMV geometry (loading/boot screen)";
    if (clear) return "clearing only, no geometry";
    return "no rendering reached";
}

static void write_ppm(const char *path)
{
    if (!path || !b3_geo_best_frame) return;
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", b3_geo_best_w, b3_geo_best_h);
    for (long i = 0; i < (long)b3_geo_best_w * b3_geo_best_h; i++) {
        const unsigned char *q = b3_geo_best_frame + i * 4;
        fputc(q[2], f); fputc(q[1], f); fputc(q[0], f);   /* BGRA -> RGB */
    }
    fclose(f);
    fprintf(stderr, "  wrote %s (%dx%d)\n", path, b3_geo_best_w, b3_geo_best_h);
}

static void dump_resource_worker(void)
{
    const uint32_t worker = 0x003F9040u;
    uint32_t count = *(uint32_t *)b3_env_ptr(worker + 0x0Cu);
    uint32_t slots = *(uint32_t *)b3_env_ptr(worker + 0x1Cu);
    uint32_t cursor = *(uint32_t *)b3_env_ptr(worker + 0x20u);
    fprintf(stderr,
            "  resource worker: slots=0x%08X count=%u cursor=%u event=0x%08X\n",
            slots, count, cursor,
            *(uint32_t *)b3_env_ptr(worker + 0x10u));
    if (slots && count <= 256u) {
        for (uint32_t i = 0; i < count; i++) {
            uint32_t slot = slots + i * 0x160u;
            uint32_t state = *(uint32_t *)b3_env_ptr(slot + 0x140u);
            if (state || *(uint32_t *)b3_env_ptr(slot + 0x20u)) {
                fprintf(stderr,
                        "    slot %u: state=%u status=%u file=0x%08X "
                        "remaining=%u position=0x%08X\n",
                        i, state, *(uint32_t *)b3_env_ptr(slot + 0x20u),
                        *(uint32_t *)b3_env_ptr(slot + 0x30u),
                        *(uint32_t *)b3_env_ptr(slot + 0x38u),
                        *(uint32_t *)b3_env_ptr(slot + 0x3Cu));
            }
        }
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "SKIP: no XBE path given\n"); return 0; }
    if (b3_env_init(argv[1]) != 0) {
        fprintf(stderr, "FAIL: b3_env_init(%s)\n", argv[1]);
        return 1;
    }
    /* Resolve the XBE's 147 raw xboxkrnl ordinals to native synthetic VAs
     * before WinMain makes its first allocation. */
    xbox_kernel_init();
    {
        char game_dir[1024];
        snprintf(game_dir, sizeof(game_dir), "%s", argv[1]);
        char *slash = strrchr(game_dir, '/');
        if (slash) *slash = '\0';
        else snprintf(game_dir, sizeof(game_dir), ".");
        xbox_path_init(game_dir, game_dir);
    }
    if (!b3_dxvk_init(640, 480)) {
        fprintf(stderr, "SKIP: no GPU or no display\n");
        return 0;
    }

    b3_geo_reset();
    b3_call_reset();
    /* Readback is synchronous. Sampling every draw throttles the original
     * boot so heavily that the harness expires before frontend init. */
    /* Capture the first proof-of-render frame only. Even one DXVK back-buffer
     * lock costs several seconds on this path; periodic locks still prevent
     * the retail initializer from reaching the frontend within the test. */
    setenv("B3_DRAW_PROBE", "1000000", 0);
    setenv("B3_SKIP_BOOT_DRAWS", "1", 0);

    g_esp = STK_VA;
    #define PUSHV(v) do { g_esp -= 4; *(uint32_t *)b3_env_ptr(g_esp) = (v); } while (0)
    PUSHV(0); PUSHV(0); PUSHV(0); PUSHV(0xDEADBEEF);
    #undef PUSHV

    pthread_t th;
    pthread_create(&th, NULL, boot_thread, NULL);

    /* Run until the draw count stops moving. The game clears and redraws
     * continuously, so "settled" is the only meaningful stopping point. */
    unsigned long prev = 0;
    int quiet = 0;
    int fmv_latched = 0;
    int frontend_draws_enabled = 0;
    uint32_t prior_init_state = UINT32_MAX;
    int max_polls = getenv("B3_MAX_POLLS") ? atoi(getenv("B3_MAX_POLLS")) : 600;
    for (int i = 0; i < max_polls; i++) {
        uint32_t init_state =
            *(uint32_t *)b3_env_ptr(0x004A71A0u + 0x2E1E8u);
        if (init_state >= 1u && init_state <= 0x17u)
            burnout3_kernel_service_resource_worker();
        usleep(100000);
        init_state = *(uint32_t *)b3_env_ptr(0x004A71A0u + 0x2E1E8u);
        if (init_state != prior_init_state) {
            fprintf(stderr, "  retail init transition: %u -> %u\n",
                    prior_init_state, init_state);
            prior_init_state = init_state;
        }
        /* 0x17 is sub_00015F10's retail ready state. The same XBE thread
         * then enters its own 0x165F0 frame loop, so WinMain is not expected
         * to return. Re-enable GPU work exactly at that boundary. */
        if (!frontend_draws_enabled && init_state == 0x17u) {
            unsetenv("B3_SKIP_BOOT_DRAWS");
            frontend_draws_enabled = 1;
            prev = b3_geo_draws;
            quiet = 0;
        }
        if (b3_geo_draws != prev) { prev = b3_geo_draws; quiet = 0; }
        else if (frontend_draws_enabled) quiet++;
        if (frontend_draws_enabled && quiet >= 20) break;
        /* B3_FMV_DONE: stand in for the host movie player and the player's
         * Start press. The retail flow is FMV screen (boot movies) -> exit
         * -> same screen re-activated as the title/attract (id 0xF90) ->
         * Start -> event handler 0x67880 requests the menu screen. Stage 0
         * reports the boot movie finished exactly as Burnout3Standalone
         * does; stage 2 holds the frontend's one-shot Start latch (guest
         * 0x4A1C75, consumed by sub_00013F10 each frame) until the title
         * screen leaves its wait state. */
        if (getenv("B3_FMV_DONE")) {
            uint32_t scr = *(uint32_t *)b3_env_ptr(0x0055CB88u + 4);
            extern volatile uint32_t g_xinput_activity_latched;
            switch (fmv_latched) {
            case 0:
                if (scr == 2u) {
                    g_b3_movie_finished_latched = 1;
                    fmv_latched = 1;
                    fprintf(stderr, "  test: FMV waiting; movie-finished "
                                    "latch set\n");
                }
                break;
            case 1:
                if (scr != 2u) fmv_latched = 2;
                break;
            case 2:
                if (scr == 2u) {
                    *(uint8_t *)b3_env_ptr(0x004A1C75u) = 1;
                    g_xinput_activity_latched = 1;
                    g_b3_movie_finished_latched = 1;
                    fmv_latched = 3;
                    fprintf(stderr, "  test: title screen waiting; "
                                    "pressing Start\n");
                }
                break;
            default:
                /* One Start press per wait state, at most three presses
                 * total: enough to leave the title/attract and confirm a
                 * profile prompt, without also driving selections inside
                 * the menu itself. */
                if (scr == 2u && fmv_latched < 9) {
                    if (!(fmv_latched & 1)) {
                        *(uint8_t *)b3_env_ptr(0x004A1C75u) = 1;
                        g_xinput_activity_latched = 1;
                        fmv_latched++;
                        fprintf(stderr, "  test: pressing Start (%d)\n",
                                (fmv_latched - 3) / 2 + 1);
                    }
                } else if (scr != 2u && (fmv_latched & 1)) {
                    fmv_latched++;
                    fprintf(stderr, "  test: screen advanced\n");
                }
                break;
            }
        }
    }

    /* WinMain performs one-shot setup. Xbox's outer runtime calls 0x165F0
     * once per frame; exercise that retail frontend path after setup rather
     * than judging the menu from setup-time geometry alone. */
    if (s_boot_complete && !getenv("B3_SKIP_RETAIL_FRAMES")) {
        for (int frame = 0; frame < 120; frame++) {
            burnout3_kernel_service_resource_worker();
            g_esp = STK_VA;
            #define PUSHF(v) do { g_esp -= 4; *(uint32_t *)b3_env_ptr(g_esp) = (v); } while (0)
            PUSHF(0x004A71A0u);
            PUSHF(0xDEADBEEF);
            #undef PUSHF
            sub_000165F0();
        }
    }

    fprintf(stderr,
            "  retail state: boot=%d init=%u frame=%u pending=%u "
            "frontend=0x%08X screen=0x%08X phase=%d\n",
            s_boot_complete,
            *(uint32_t *)b3_env_ptr(0x004A71A0u + 0x2E1E8u),
            *(uint32_t *)b3_env_ptr(0x004A71A0u + 0x2E218u),
            *(uint32_t *)b3_env_ptr(0x004A71A0u + 0x2E214u),
            *(uint32_t *)b3_env_ptr(0x004A71A0u + 0x2E1D0u),
            *(uint32_t *)b3_env_ptr(0x00557A70u),
            (int8_t)*(uint8_t *)b3_env_ptr(0x0055609Eu));

    fprintf(stderr, "  intro stage: %s\n", intro_stage());
    fprintf(stderr, "  %lu draws, %lu primitives, %lu vertex buffers\n",
            b3_geo_draws, b3_geo_primitives, b3_geo_buffers_created);
    fprintf(stderr, "  geometry covers %lu sampled pixels\n", b3_geo_lit_max);
    dump_resource_worker();
    write_ppm(getenv("B3_INTRO_DUMP"));

    /* The game's own code must reach its renderer and put geometry up.
     * These are the floors reached on 2026-08-01, not targets: the retail
     * intro is three FMVs then a menu, and a real menu frame is about 30
     * draws, so passing here is a long way from correct. It is a ratchet
     * against going backwards. */
    CHECK(b3_geo_draws > 0,
          "the game's boot produced no draws at all");
    CHECK(b3_geo_primitives >= 500,
          "only %lu primitives; the boot used to reach ~1600",
          b3_geo_primitives);
    CHECK(b3_geo_lit_max > 0,
          "%lu draws but nothing reached the framebuffer",
          b3_geo_draws);

    fflush(stderr);
    if (g_failures) {
        fprintf(stderr, "burnout3_intro_test: %d FAILURE(S)\n", g_failures);
        _exit(1);
    }
    fprintf(stderr, "burnout3_intro_test: all checks passed\n");
    _exit(0);   /* the boot thread still owns the mapping */
}
