/**
 * burnout3_fmv_test.c — play a Burnout 3 XMV FMV through the shared
 * manx_fmv module in a live SDL3 window.
 *
 * The retail intro runs three FMVs before the menu.  This test lets us
 * see one of them without booting the game at all — just the FFmpeg
 * decode path plus an SDL3 texture upload.
 *
 * Usage:   burnout3_fmv_test <path/to/file.xmv-or-mp4>
 */

#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <SDL3/SDL.h>
#include "manx_fmv.h"

#define WIN_W  960
#define WIN_H  720

typedef struct audio_pull_state {
    manx_fmv *fmv;
    _Atomic uint64_t decoded_frames;
    _Atomic int saw_nonzero;
} audio_pull_state;

static void SDLCALL audio_pull(void *userdata, SDL_AudioStream *stream,
                               int additional_amount, int total_amount)
{
    (void)total_amount;
    audio_pull_state *state = (audio_pull_state *)userdata;
    int16_t pcm[1024 * 2];
    const int bytes_per_frame = (int)(2 * sizeof(int16_t));
    int frames_needed = (additional_amount + bytes_per_frame - 1) /
                        bytes_per_frame;
    while (frames_needed > 0) {
        int frames = frames_needed < 1024 ? frames_needed : 1024;
        int got = manx_fmv_read_audio(state->fmv, pcm, frames);
        atomic_fetch_add_explicit(&state->decoded_frames, (uint64_t)got,
                                  memory_order_relaxed);
        for (int i = 0; i < got * 2; i++) {
            if (pcm[i] != 0) {
                atomic_store_explicit(&state->saw_nonzero, 1,
                                      memory_order_relaxed);
                break;
            }
        }
        if (got < frames)
            SDL_memset(pcm + got * 2, 0,
                       (size_t)(frames - got) * bytes_per_frame);
        SDL_PutAudioStreamData(stream, pcm, frames * bytes_per_frame);
        frames_needed -= frames;
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <path/to/file.xmv-or-mp4>\n", argv[0]);
        return 1;
    }

    /* Open the XMV through ffmpeg, scaling to 640×480 BGRA (matches the
     * D3D8 back-buffer format used by the game). */
    manx_fmv *fmv = manx_fmv_open(argv[1], 640, 480,
                                      manx_fmv_format_bgra);
    if (!fmv) {
        fprintf(stderr, "FAIL: cannot open or decode %s\n", argv[1]);
        return 1;
    }

    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS)) {
        fprintf(stderr, "SDL_InitSubSystem: %s\n", SDL_GetError());
        manx_fmv_close(fmv);
        return 1;
    }

    SDL_Window *window = SDL_CreateWindow(
        "Burnout 3 — FMV", WIN_W, WIN_H,
        SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        manx_fmv_close(fmv);
        return 1;
    }

    SDL_Renderer *renderer = SDL_CreateRenderer(window, NULL);
    if (!renderer) renderer = SDL_CreateRenderer(window, "software");
    if (!renderer) {
        fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        manx_fmv_close(fmv);
        return 1;
    }

    SDL_Texture *tex = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_XRGB8888,
        SDL_TEXTUREACCESS_STREAMING, 640, 480);
    if (!tex) {
        fprintf(stderr, "SDL_CreateTexture: %s\n", SDL_GetError());
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        manx_fmv_close(fmv);
        return 1;
    }

    audio_pull_state audio_state = { .fmv = fmv };
    SDL_AudioStream *audio = NULL;
    if (manx_fmv_has_audio(fmv)) {
        SDL_AudioSpec spec = { SDL_AUDIO_S16, 2, 48000 };
        audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                          &spec, audio_pull, &audio_state);
        if (!audio) {
            fprintf(stderr, "SDL_OpenAudioDeviceStream: %s\n", SDL_GetError());
        } else if (!SDL_ResumeAudioStreamDevice(audio)) {
            fprintf(stderr, "SDL_ResumeAudioStreamDevice: %s\n", SDL_GetError());
        }
    }

    fprintf(stderr, "FMV playing — Esc to quit\n");

    int    running = 1;
    int    frame_count = 0;
    uint64_t last_tick = SDL_GetTicks();

    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT ||
                (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_ESCAPE))
                running = 0;
        }

        int new_frame = 0;
        int alive = manx_fmv_update(fmv, &new_frame);
        if (!alive) { running = 0; break; }  /* end of stream */

        if (new_frame) {
            const uint8_t *pixels = manx_fmv_frame(fmv);
            SDL_UpdateTexture(tex, NULL, pixels, 640 * 4);
        }

        SDL_RenderClear(renderer);
        SDL_RenderTexture(renderer, tex, NULL, NULL);
        SDL_RenderPresent(renderer);

        frame_count++;
        uint64_t now = SDL_GetTicks();
        if (now - last_tick >= 2000) {
            float fps = frame_count * 1000.0f / (float)(now - last_tick);
            char title[128];
            snprintf(title, sizeof(title),
                     "Burnout 3 — FMV  |  %.0f fps", fps);
            SDL_SetWindowTitle(window, title);
            last_tick = now;
            frame_count = 0;
        }
    }

    if (audio) SDL_DestroyAudioStream(audio);
    const uint64_t decoded_audio = atomic_load_explicit(
        &audio_state.decoded_frames, memory_order_relaxed);
    const int nonzero_audio = atomic_load_explicit(
        &audio_state.saw_nonzero, memory_order_relaxed);
    const int had_audio = manx_fmv_has_audio(fmv);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS);

    fprintf(stderr, "Done. decoded audio: %llu frames%s\n",
            (unsigned long long)decoded_audio,
            nonzero_audio ? " (non-silent)" : "");
    if (had_audio && (!decoded_audio || !nonzero_audio)) {
        fprintf(stderr, "FAIL: audio stream opened but no non-silent PCM played\n");
        manx_fmv_close(fmv);
        return 1;
    }
    manx_fmv_close(fmv);
    return 0;
}
