/*
 * Visible Burnout 3 retail-XBE runner.
 *
 * This is intentionally the same narrow path as burnout3_intro_test:
 * Criterion's recompiled WinMain/frontend, the Linux-native Xbox kernel and
 * file boundary, and DXVK for the game's RenderWare/D3D8 submissions.  There
 * is no host-authored menu, substitute game loop, network layer, or Windows
 * compatibility kernel in this executable. Portable builds use the correctly
 * paired H.264/AAC movie assets imported from the retail XMV and movie.xwb;
 * the original video-only XMV remains the fallback.
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <SDL3/SDL.h>

#include "b3_native_env.h"
#include "b3_native_runtime.h"
#include "manx_fmv.h"

#define FB_W   640
#define FB_H   480
#define WIN_W  960
#define WIN_H  720
#define STK_VA 0x005F0000u

extern uint32_t g_esp;
extern void sub_00156400(void);
extern void xbox_kernel_init(void);
extern void xbox_path_init(const char *game_dir, const char *save_dir);
extern void burnout3_kernel_service_resource_worker(void);

extern int b3_dxvk_init(int width, int height);
extern void b3_dxvk_shutdown(void);
extern unsigned long b3_dxvk_copy_published(void *destination, size_t capacity,
                                            int *width, int *height);
extern void b3_dxvk_set_movie_frame(const void *bgra, int width, int height);
extern void b3_geo_reset(void);
extern unsigned long b3_geo_draws;
extern unsigned long b3_geo_primitives;
extern uint16_t g_xinput_buttons;
extern volatile uint32_t g_xinput_activity_latched;
extern volatile uint32_t g_b3_movie_finished_latched;

#ifndef B3_BUILD_MEDIA_DIR
#define B3_BUILD_MEDIA_DIR ""
#endif

static const char *s_boot_movie_stems[] = {
    "cri_rw30",
    "englis30",
    "Titles30",
};

typedef struct boot_movie_player {
    const char *game_dir;
    int index;
    manx_fmv *movie;
    SDL_AudioStream *audio;
} boot_movie_player;

static void SDLCALL boot_movie_audio_pull(void *userdata,
                                          SDL_AudioStream *stream,
                                          int additional_amount,
                                          int total_amount)
{
    (void)total_amount;
    boot_movie_player *boot = (boot_movie_player *)userdata;
    int16_t pcm[1024 * 2];
    const int bytes_per_frame = 2 * (int)sizeof(int16_t);
    int frames_needed = (additional_amount + bytes_per_frame - 1) /
                        bytes_per_frame;

    while (frames_needed > 0) {
        const int frames = frames_needed < 1024 ? frames_needed : 1024;
        const int got = boot->movie
            ? manx_fmv_read_audio(boot->movie, pcm, frames) : 0;
        if (got < frames)
            SDL_memset(pcm + got * 2, 0,
                       (size_t)(frames - got) * bytes_per_frame);
        SDL_PutAudioStreamData(stream, pcm, frames * bytes_per_frame);
        frames_needed -= frames;
    }
}

static int file_exists(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    fclose(fp);
    return 1;
}

static uint16_t native_button_for_key(SDL_Keycode key)
{
    if (key == SDLK_RETURN || key == SDLK_SPACE) return 0x1010u; /* A + Start */
    if (key == SDLK_ESCAPE || key == SDLK_BACKSPACE) return 0x2020u; /* B + Back */
    if (key == SDLK_UP) return 0x0001u;
    if (key == SDLK_DOWN) return 0x0002u;
    if (key == SDLK_LEFT) return 0x0004u;
    if (key == SDLK_RIGHT) return 0x0008u;
    return 0;
}

static void boot_movie_close(boot_movie_player *boot)
{
    if (!boot->movie) return;

    if (boot->audio) SDL_LockAudioStream(boot->audio);
    manx_fmv *movie = boot->movie;
    boot->movie = NULL;
    if (boot->audio) SDL_UnlockAudioStream(boot->audio);

    manx_fmv_close(movie);
    if (boot->audio) SDL_ClearAudioStream(boot->audio);
}

static int boot_movie_open(boot_movie_player *boot)
{
    const int movie_count = (int)(sizeof(s_boot_movie_stems) /
                                  sizeof(s_boot_movie_stems[0]));

    boot_movie_close(boot);
    while (boot->index < movie_count) {
        const char *stem = s_boot_movie_stems[boot->index];
        const char *portable = getenv("B3_PORTABLE_MEDIA");
        char candidates[4][1200];
        int count = 0;

        /* Prefer the portable asset built from the confirmed movie.xwb
         * pairing. It carries standard AAC audio and needs no XACT plugin. */
        if (portable && *portable)
            snprintf(candidates[count++], sizeof(candidates[0]),
                     "%s/movies/%s.mp4", portable, stem);
        if (B3_BUILD_MEDIA_DIR[0])
            snprintf(candidates[count++], sizeof(candidates[0]),
                     "%s/movies/%s.mp4", B3_BUILD_MEDIA_DIR, stem);
        snprintf(candidates[count++], sizeof(candidates[0]),
                 "%s/portable/movies/%s.mp4", boot->game_dir, stem);
        snprintf(candidates[count++], sizeof(candidates[0]),
                 "%s/ovid/%s.xmv", boot->game_dir, stem);
        for (int candidate = 0; candidate < count; candidate++) {
            if (!file_exists(candidates[candidate])) continue;
            manx_fmv *movie = manx_fmv_open(candidates[candidate], FB_W, FB_H,
                                            manx_fmv_format_bgra);
            if (!movie) continue;

            if (boot->audio) SDL_LockAudioStream(boot->audio);
            boot->movie = movie;
            if (boot->audio) SDL_UnlockAudioStream(boot->audio);

            fprintf(stderr, "Burnout 3: retail boot movie %d/3: %s (%s)\n",
                    boot->index + 1, candidates[candidate],
                    manx_fmv_has_audio(movie) ? "native audio" : "silent");
            return 1;
        }

        fprintf(stderr, "Burnout 3: boot movie missing: %s\n", stem);
        boot->index++;
    }
    return 0;
}

static int boot_movie_advance(boot_movie_player *boot)
{
    boot_movie_close(boot);
    boot->index++;
    return boot_movie_open(boot);
}

static volatile int s_game_returned;

static void *retail_game_thread(void *unused)
{
    (void)unused;
    sub_00156400();
    s_game_returned = 1;
    return NULL;
}

static int start_retail_xbe(const char *game_dir)
{
    char xbe_path[1024];
    snprintf(xbe_path, sizeof(xbe_path), "%s/default.xbe", game_dir);

    if (b3_env_init(xbe_path) != 0) {
        fprintf(stderr, "Burnout 3: cannot load %s\n", xbe_path);
        return 0;
    }

    xbox_kernel_init();
    xbox_path_init(game_dir, game_dir);

    if (!b3_dxvk_init(FB_W, FB_H)) {
        fprintf(stderr, "Burnout 3: DXVK D3D8 initialization failed\n");
        return 0;
    }

    b3_geo_reset();
    b3_call_reset();

    /* Skip only the repeated loading-screen draw pass. The original state
     * machine, resource worker, frontend builders and menu frame remain. */
    setenv("B3_SKIP_BOOT_DRAWS", "1", 0);
    setenv("B3_PUBLISH_FRAMES", "1", 0);

    g_esp = STK_VA;
#define PUSH_GUEST(value) do {                                           \
        g_esp -= 4;                                                       \
        *(uint32_t *)b3_env_ptr(g_esp) = (uint32_t)(value);              \
    } while (0)
    PUSH_GUEST(0);
    PUSH_GUEST(0);
    PUSH_GUEST(0);
    PUSH_GUEST(0xDEADBEEF);
#undef PUSH_GUEST

    pthread_t thread;
    if (pthread_create(&thread, NULL, retail_game_thread, NULL) != 0) {
        fprintf(stderr, "Burnout 3: cannot start retail game thread\n");
        return 0;
    }
    pthread_detach(thread);
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <Burnout 3 game directory>\n", argv[0]);
        return 1;
    }

    if (!getenv("DXVK_WSI_DRIVER"))
        setenv("DXVK_WSI_DRIVER", "SDL3", 0);

    if (!start_retail_xbe(argv[1]))
        return 1;

    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        fprintf(stderr, "Burnout 3: SDL video initialization failed: %s\n",
                SDL_GetError());
        return 1;
    }

    SDL_Window *window = SDL_CreateWindow(
        "Burnout 3: Takedown — retail XBE recompilation",
        WIN_W, WIN_H, SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE);
    SDL_Renderer *renderer = window ? SDL_CreateRenderer(window, NULL) : NULL;
    SDL_Texture *texture = renderer ? SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING,
        FB_W, FB_H) : NULL;

    if (!window || !renderer || !texture) {
        fprintf(stderr, "Burnout 3: cannot create visible output: %s\n",
                SDL_GetError());
        return 1;
    }

    fprintf(stderr, "Burnout 3: retail XBE frontend running\n");

    boot_movie_player boot = { .game_dir = argv[1] };
    if (SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        SDL_AudioSpec audio_spec = { SDL_AUDIO_S16, 2, 48000 };
        boot.audio = SDL_OpenAudioDeviceStream(
            SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &audio_spec,
            boot_movie_audio_pull, &boot);
        if (!boot.audio)
            fprintf(stderr, "Burnout 3: audio output unavailable: %s\n",
                    SDL_GetError());
        else if (!SDL_ResumeAudioStreamDevice(boot.audio))
            fprintf(stderr, "Burnout 3: cannot start audio: %s\n",
                    SDL_GetError());
    } else {
        fprintf(stderr, "Burnout 3: SDL audio initialization failed: %s\n",
                SDL_GetError());
    }
    int booting = getenv("B3_SKIP_BOOT_MOVIES") ? 0 : boot_movie_open(&boot);
    int menu_movie = 0;
    if (!booting) {
        boot.index = 2;
        menu_movie = boot_movie_open(&boot);
    }

    int running = 1;
    int frontend_enabled = 0;
    uint32_t pending_frontend_latch = 0;
    uint32_t previous_screen_state = 0xFFFFFFFFu;
    uint64_t next_title = 0;
    unsigned char *present_frame = malloc((size_t)FB_W * FB_H * 4);
    unsigned long presented_generation = 0;

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (!booting && (event.type == SDL_EVENT_KEY_DOWN ||
                             event.type == SDL_EVENT_KEY_UP)) {
                const uint16_t button = native_button_for_key(event.key.key);
                if (event.type == SDL_EVENT_KEY_DOWN) {
                    g_xinput_buttons |= button;
                    if (button)
                        g_xinput_activity_latched = 1;
                } else
                    g_xinput_buttons &= (uint16_t)~button;
            }
            if (event.type == SDL_EVENT_QUIT ||
                (event.type == SDL_EVENT_KEY_DOWN &&
                 event.key.key == SDLK_F10))
                running = 0;
            else if (booting && event.type == SDL_EVENT_KEY_DOWN &&
                     (event.key.key == SDLK_RETURN ||
                      event.key.key == SDLK_SPACE)) {
                booting = boot_movie_advance(&boot);
                /* Skipping through the reel must still leave the title
                 * attract movie looping behind the retail frontend, the
                 * same as when the reel ends naturally. */
                if (!booting) {
                    boot_movie_close(&boot);
                    boot.index = 2;
                    menu_movie = boot_movie_open(&boot);
                }
            }
            else if (!booting && event.type == SDL_EVENT_KEY_DOWN &&
                     !event.key.repeat) {
                /* Directional one-shot latches consumed by sub_00013F10.
                 * 0x4A1C74/78 are the dpad up/down pair and 76/77 left/
                 * right. The neighbouring 75/79 bytes are the SECOND
                 * up/down source (stick), NOT A/Start — writing 75 flips
                 * the frontend nav block at 0x4A4B90 to "backward" and
                 * gates off the flow machine in sub_000636D0, which
                 * presented as a hard freeze after pressing Start.
                 * Start/A/B reach the game through the pad-button path
                 * (g_xinput_buttons via XInputGetState) alone. The title
                 * movie also stays host-owned and looping: the retail
                 * screens decide when to leave it. */
                if (event.key.key == SDLK_UP)
                    pending_frontend_latch = 0x004A1C74u;
                else if (event.key.key == SDLK_DOWN)
                    pending_frontend_latch = 0x004A1C78u;
                else if (event.key.key == SDLK_LEFT)
                    pending_frontend_latch = 0x004A1C76u;
                else if (event.key.key == SDLK_RIGHT)
                    pending_frontend_latch = 0x004A1C77u;
            }
        }

        /* B3_AUTO_START_MS=<n>: scripted menu walk for headless capture.
         * Press A+Start through the same pad-button path as Enter for 1.5 s
         * once the attract movie is running, then release. */
        static uint32_t auto_start_at = 0;
        if (getenv("B3_AUTO_START_MS")) {
            uint32_t first = (uint32_t)atoi(getenv("B3_AUTO_START_MS"));
            if (!auto_start_at)
                auto_start_at = first ? first : 12000u;
            uint64_t ticks = SDL_GetTicks();
            if (!booting && ticks >= auto_start_at &&
                ticks < auto_start_at + 1500u) {
                g_xinput_buttons |= 0x1010u;
                g_xinput_activity_latched = 1;
            } else {
                g_xinput_buttons &= (uint16_t)~0x1010u;
            }
        }

        uint32_t init_state = *(uint32_t *)b3_env_ptr(
            0x004A71A0u + 0x2E1E8u);
        uint32_t screen_state = *(uint32_t *)b3_env_ptr(0x0055CB88u + 4);
        if (screen_state != previous_screen_state) {
            fprintf(stderr, "Burnout 3: retail screen state %u -> %u\n",
                    previous_screen_state, screen_state);
            previous_screen_state = screen_state;
        }
        if (init_state >= 1u && init_state <= 0x17u)
            burnout3_kernel_service_resource_worker();

        if (!frontend_enabled && init_state == 0x17u) {
            unsetenv("B3_SKIP_BOOT_DRAWS");
            frontend_enabled = 1;
            fprintf(stderr, "Burnout 3: retail frontend ready\n");
        }

        const uint64_t now = SDL_GetTicks();
        if (booting) {
            int new_frame = 0;
            if (!manx_fmv_update(boot.movie, &new_frame)) {
                if (boot.index == 2) {
                    boot_movie_close(&boot);
                    boot.index = 2;
                    menu_movie = boot_movie_open(&boot);
                    booting = 0;
                } else {
                    booting = boot_movie_advance(&boot);
                }
                new_frame = 0;
            }
            if (booting && new_frame) {
                b3_dxvk_set_movie_frame(manx_fmv_frame(boot.movie),
                                        FB_W, FB_H);
                SDL_UpdateTexture(texture, NULL,
                                  manx_fmv_frame(boot.movie), FB_W * 4);
                SDL_RenderClear(renderer);
                SDL_RenderTexture(renderer, texture, NULL, NULL);
                SDL_RenderPresent(renderer);
            }
        } else {
            if (menu_movie) {
                int new_frame = 0;
                if (!manx_fmv_update(boot.movie, &new_frame)) {
                    /* Retail loops the attract reel: report this pass
                     * finished to the XBE (its FMV screen cycles on that
                     * byte) and start the movie again so the background
                     * never freezes on the last decoded frame. */
                    boot_movie_close(&boot);
                    boot.index = 2;
                    menu_movie = boot_movie_open(&boot);
                    g_b3_movie_finished_latched = 1;
                    new_frame = 0;
                }
                if (menu_movie && new_frame)
                    b3_dxvk_set_movie_frame(manx_fmv_frame(boot.movie),
                                            FB_W, FB_H);
            }
            int frame_width = 0;
            int frame_height = 0;
            unsigned long generation = b3_dxvk_copy_published(
                present_frame, (size_t)FB_W * FB_H * 4,
                &frame_width, &frame_height);
            if (generation && generation != presented_generation) {
                presented_generation = generation;
                SDL_UpdateTexture(texture, NULL, present_frame,
                                  frame_width * 4);
                SDL_RenderClear(renderer);
                SDL_RenderTexture(renderer, texture, NULL, NULL);
                SDL_RenderPresent(renderer);
                /* Deliver immediately after a completed retail frame. The
                 * XBE consumes and clears this byte in sub_00013F10. */
                if (pending_frontend_latch) {
                    *(uint8_t *)b3_env_ptr(pending_frontend_latch) = 1;
                    fprintf(stderr,
                            "Burnout 3: retail input latch %#x delivered\n",
                            pending_frontend_latch);
                    pending_frontend_latch = 0;
                }
            }
        }

        if (now >= next_title) {
            char title[192];
            if (booting)
                snprintf(title, sizeof(title),
                         "Burnout 3 retail boot FMV — %d/3", boot.index + 1);
            else
                snprintf(title, sizeof(title),
                         "Burnout 3 retail XBE — init %u / screen %u — %lu draws / %lu prims",
                         init_state, screen_state,
                         b3_geo_draws, b3_geo_primitives);
            SDL_SetWindowTitle(window, title);
            next_title = now + 1000;
        }

        if (s_game_returned && !frontend_enabled) {
            fprintf(stderr, "Burnout 3: retail game thread returned early\n");
            running = 0;
        }

        SDL_Delay(8);
    }

    SDL_DestroyTexture(texture);
    boot_movie_close(&boot);
    if (boot.audio) SDL_DestroyAudioStream(boot.audio);
    free(present_frame);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    /* The detached retail game thread never returns (WinMain's frame loop
     * runs forever) and touches guest memory every frame. Tearing down the
     * DXVK device or unmapping the guest space here made every window
     * close look like a SIGSEGV "crash" when the game thread faulted an
     * instant later. The game thread owns those mappings; leave process
     * exit to reclaim everything, exactly like burnout3_intro_test. */
    fflush(NULL);
    _exit(0);
}
