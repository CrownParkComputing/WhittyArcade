/**
 * burnout3_menu_test.c — Burnout 3 intro FMVs + menu, pump-only.
 *
 * Plays the EA/Criterion intro FMVs through the shared manx_fmv module,
 * then boots the game (XBE load, kernel, Vulkan, textures, RenderWare)
 * and pumps frames — all without spawning the game thread.
 *
 * Usage:
 *   burnout3_menu_test <game_data_dir>
 *
 * Controls: Esc to quit (skips FMVs too).
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <SDL3/SDL.h>

/* FMV module — plays Xbox XMV files through ffmpeg. */
#include "manx_fmv.h"

/* ── Burnout3Recomp kernel API ──────────────────────────────── */
extern int  xbox_MemoryLayoutInit(const void *xbe_data, unsigned long xbe_size);
extern void xbox_MemoryLayoutShutdown(void);
extern void xbox_kernel_init(void);
extern void xbox_kernel_shutdown(void);
extern void xbox_path_init(const char *game_dir, const char *save_dir);

/* ── Native game layer (Vulkan + textures + RenderWare) ──────── */
extern int  burnout3_game_native_init_no_thread(const char *game_data_path);
extern void burnout3_game_native_shutdown(void);

/* ── D3D8 / frame pump ──────────────────────────────────────── */
extern void game_frame_pump(void);
extern void vulkan_d3d8_trigger_present(void);
extern const uint8_t *vulkan_d3d8_present(int *out_width, int *out_height);
extern int  g_game_ready;
extern int  g_backbuffer_width;
extern int  g_backbuffer_height;
extern uint64_t g_keyboard_state[4];

/* ── Input injection ────────────────────────────────────────── */
/* Map SDL scancodes → virtual key codes in g_keyboard_state bitmask,
 * the same way burnout3_inject_keyboard does in burnout3_bridge.c.
 * That function lives in the bridge (not in burnout3_recomp), so the
 * test does the mapping inline. */

#define SET_KEY(vk, sdl_sc) do { \
    if ((sdl_sc) < num_keys && sdl_keys[(sdl_sc)]) { \
        int w = (vk) / 64; \
        int b = (vk) % 64; \
        g_keyboard_state[w] |= (1ULL << b); \
    } \
} while(0)

/* ── Load XBE from disk (copied from burnout3_bridge.c) ─────── */

static void *s_xbe_data = NULL;
static unsigned long s_xbe_size = 0;

static bool load_xbe(const char *game_dir) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/default.xbe", game_dir);

    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open %s\n", path);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz <= 0 || sz > 128 * 1024 * 1024) {
        fprintf(stderr, "Bad XBE size %ld\n", sz);
        fclose(f);
        return false;
    }

    s_xbe_data = malloc((size_t)sz);
    if (!s_xbe_data) { fclose(f); return false; }

    if (fread(s_xbe_data, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "Short XBE read\n");
        free(s_xbe_data); s_xbe_data = NULL;
        fclose(f);
        return false;
    }
    fclose(f);
    s_xbe_size = (unsigned long)sz;
    fprintf(stderr, "Loaded default.xbe (%lu bytes)\n", s_xbe_size);
    return true;
}

/* ── Init without spawning the game thread ──────────────────── */

static bool burnout3_init_pump_only(const char *game_data_path) {
    if (!load_xbe(game_data_path)) return false;

    if (!xbox_MemoryLayoutInit(s_xbe_data, s_xbe_size)) {
        fprintf(stderr, "xbox_MemoryLayoutInit failed\n");
        return false;
    }
    fprintf(stderr, "Memory layout initialised\n");

    xbox_kernel_init();
    fprintf(stderr, "Kernel thunks installed\n");

    xbox_path_init(game_data_path, game_data_path);
    fprintf(stderr, "Path translation initialised\n");

    /* Vulkan init + texture loading + RenderWare init, but skip the
     * game thread. We drive everything through game_frame_pump(). */
    if (!burnout3_game_native_init_no_thread(game_data_path)) {
        fprintf(stderr, "game_native_init failed\n");
        return false;
    }

    /* Boot pump: same 600-frame loop from burnout3_bridge.c.
     * The game's subsystems initialise through the frame pump's
     * D3D8 calls without needing sub_00156400. */
    fprintf(stderr, "Pumping %d frames to menu...\n", 600);
    for (int i = 0; i < 600; i++) {
        game_frame_pump();
        vulkan_d3d8_trigger_present();
    }

    g_game_ready = 1;
    fprintf(stderr, "Menu reached — %d frames pumped\n", 600);
    return true;
}

/* ── Main ──────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <game_data_dir>\n"
                "  e.g.: %s game_data/xbox/burnout3\n", argv[0], argv[0]);
        return 1;
    }

    /* ── 1. Init ─────────────────────────────────────────── */
    if (!burnout3_init_pump_only(argv[1])) {
        fprintf(stderr, "FAIL: burnout3_init_pump_only(%s)\n", argv[1]);
        return 1;
    }

    /* ── 2. SDL3 window for display ──────────────────────── */
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        fprintf(stderr, "SDL_InitSubSystem: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window *window = SDL_CreateWindow(
        "Burnout 3 — Menu", 960, 720,
        SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Renderer *renderer = SDL_CreateRenderer(window, NULL);
    if (!renderer) renderer = SDL_CreateRenderer(window, "software");
    if (!renderer) {
        fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        return 1;
    }

    SDL_Texture *tex = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_XRGB8888,
        SDL_TEXTUREACCESS_STREAMING, 640, 480);
    if (!tex) {
        fprintf(stderr, "SDL_CreateTexture: %s\n", SDL_GetError());
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        return 1;
    }

    /* ── 3. Play intro FMVs ──────────────────────────────── */
    /* The retail Xbox boots through EA + Criterion/RenderWare logos.
     * Play them through manx_fmv so the user sees Burnout 3 content
     * immediately, before the (currently blank) menu appears. */
    static const char *fmv_files[] = {
        "englis30.xmv",   /* EA / Criterion intro, WMV2 640x480 */
        "cri_rw30.xmv",   /* RenderWare logo */
    };
    int fmv_index = 0;
    manx_fmv *fmv = NULL;
    bool fmvs_done = false;

    /* ── 4. Main loop (FMV → pump) ───────────────────────── */
    bool running = true;
    uint64_t last_tick = SDL_GetTicks();
    int frame_count = 0;
    float fps = 0.0f;

    while (running) {
        /* Input */
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT)
                running = false;
            else if (ev.type == SDL_EVENT_KEY_DOWN &&
                     ev.key.key == SDLK_ESCAPE)
                running = false;
        }

        if (!fmvs_done) {
            /* Open next FMV if needed */
            if (!fmv && fmv_index < (int)(sizeof(fmv_files)/sizeof(fmv_files[0]))) {
                char fmv_path[1024];
                snprintf(fmv_path, sizeof(fmv_path), "%s/ovid/%s",
                         argv[1], fmv_files[fmv_index]);
                fmv = manx_fmv_open(fmv_path, 640, 480,
                                      manx_fmv_format_bgra);
                if (fmv)
                    fprintf(stderr, "[FMV] %s\n", fmv_files[fmv_index]);
                else
                    fprintf(stderr, "[FMV] SKIP %s (not found)\n",
                            fmv_files[fmv_index]);
            }

            /* Advance / advance-to-next */
            if (fmv) {
                int new_frame = 0;
                int alive = manx_fmv_update(fmv, &new_frame);
                if (alive && new_frame) {
                    SDL_UpdateTexture(tex, NULL, manx_fmv_frame(fmv),
                                      640 * 4);
                }
                if (!alive) {
                    manx_fmv_close(fmv);
                    fmv = NULL;
                    fmv_index++;
                }
            } else {
                fmv_index++;
            }

            /* All FMVs done — fall through to the pump loop */
            if (fmv_index >= (int)(sizeof(fmv_files)/sizeof(fmv_files[0])) && !fmv) {
                fmvs_done = true;
                fprintf(stderr, "[FMV] intro complete — entering menu pump\n");
            }

            /* Still in FMV phase — render and continue */
            if (!fmvs_done) {
                SDL_RenderClear(renderer);
                SDL_RenderTexture(renderer, tex, NULL, NULL);
                SDL_RenderPresent(renderer);
                continue;
            }
        }

        /* Inject keyboard state into Xbox memory */
        int num_keys = 0;
        const bool *sdl_keys = SDL_GetKeyboardState(&num_keys);
        memset(g_keyboard_state, 0, sizeof(g_keyboard_state));
        SET_KEY(0x0D, SDL_SCANCODE_RETURN);
        SET_KEY(0x1B, SDL_SCANCODE_ESCAPE);
        SET_KEY(0x20, SDL_SCANCODE_SPACE);
        SET_KEY(0x26, SDL_SCANCODE_UP);
        SET_KEY(0x28, SDL_SCANCODE_DOWN);
        SET_KEY(0x25, SDL_SCANCODE_LEFT);
        SET_KEY(0x27, SDL_SCANCODE_RIGHT);
        SET_KEY(0x10, SDL_SCANCODE_LSHIFT);
        SET_KEY(0x10, SDL_SCANCODE_RSHIFT);
        SET_KEY(0x57, SDL_SCANCODE_W);
        SET_KEY(0x41, SDL_SCANCODE_A);
        SET_KEY(0x53, SDL_SCANCODE_S);
        SET_KEY(0x44, SDL_SCANCODE_D);
        SET_KEY(0x52, SDL_SCANCODE_R);
        SET_KEY(0x47, SDL_SCANCODE_G);

        /* Pump one frame: advances the recompiled game + triggers
         * D3D8 Present to flush the GPU render target. */
        game_frame_pump();
        vulkan_d3d8_trigger_present();

        /* Read back and display */
        int fw = 0, fh = 0;
        const uint8_t *frame = vulkan_d3d8_present(&fw, &fh);
        if (frame && fw > 0 && fh > 0) {
            SDL_UpdateTexture(tex, NULL, frame, fw * 4);
            SDL_RenderClear(renderer);
            SDL_RenderTexture(renderer, tex, NULL, NULL);
            SDL_RenderPresent(renderer);
        }

        /* FPS counter in title */
        frame_count++;
        uint64_t now = SDL_GetTicks();
        if (now - last_tick >= 2000) {
            fps = frame_count * 1000.0f / (float)(now - last_tick);
            char title[128];
            snprintf(title, sizeof(title),
                     "Burnout 3 — Menu  |  %.0f fps", fps);
            SDL_SetWindowTitle(window, title);
            last_tick = now;
            frame_count = 0;
        }
    }

    /* ── 4. Cleanup ──────────────────────────────────────── */
    if (fmv) manx_fmv_close(fmv);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);

    burnout3_game_native_shutdown();
    xbox_kernel_shutdown();
    xbox_MemoryLayoutShutdown();
    free(s_xbe_data);

    fprintf(stderr, "Done.\n");
    return 0;
}
