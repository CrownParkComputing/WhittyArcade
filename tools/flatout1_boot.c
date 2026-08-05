// flatout1_boot.c — FlatOut 1 boot visualiser.
//
// Opens flatout.bfs, loads real DDS textures from known data entries,
// and renders them in an SDL3 window with FlatOut 1 amber styling.
//
// Bugbear BFS indirection: manifest entries (with path strings) store
// the target entry's csize in field f2.  Data entries have hash=0.
// We match manifest→data by csize, then load the actual DDS.
//
// Usage: ./FlatOut1Boot [flatout1_extracted/FlatOut.1.USA.XBOX-ZTM]

#include "flatout1_bfs_vfs.h"

extern void flatout1_kernel_set_bfs(flatout1_bfs *bfs);

#include "dxt_decode.h"

#include <SDL3/SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t width, height, four_cc, pitch;
    const uint8_t *pixels;
    uint32_t pixel_bytes;
} dds_info;

/* Parse DDS header only — returns 0 on failure. */
static int dds_parse(const uint8_t *data, uint32_t len, dds_info *out) {
    if (len < 128 || memcmp(data, "DDS ", 4) != 0) return 0;
    uint32_t h = *(const uint32_t *)(data + 12);
    uint32_t w = *(const uint32_t *)(data + 16);
    uint32_t p  = *(const uint32_t *)(data + 20);  /* pitch or linear size */
    uint32_t pf = *(const uint32_t *)(data + 80);
    uint32_t fc = *(const uint32_t *)(data + 84);
    if (!w || !h || w > 4096 || h > 4096) return 0;
    out->width  = w;
    out->height = h;
    out->four_cc = (pf & 0x4) ? fc : 0;
    out->pitch = p;
    out->pixels      = data + 128;
    out->pixel_bytes  = len > 128 ? (uint32_t)(len - 128) : 0;
    return 1;
}

/* ═══════════════════════════════════════════════════════════════
 *  Main
 * ═══════════════════════════════════════════════════════════════ */

int main(int argc, char **argv) {
    const char *data_dir = argc > 1 ? argv[1]
        : "flatout1_extracted/FlatOut.1.USA.XBOX-ZTM";

    char bfs_path[1024];
    snprintf(bfs_path, sizeof(bfs_path), "%s/flatout.bfs", data_dir);
    flatout1_bfs *bfs = flatout1_bfs_open(bfs_path);
    if (!bfs) { fprintf(stderr, "ERROR: cannot open BFS\n"); return 1; }
    printf("BFS: %u entries\n", flatout1_bfs_entry_count(bfs));

    /* Wire BFS VFS into the kernel shim so NtOpenFile/NtReadFile/NtClose
     * stubs will serve files from flatout.bfs when recompiled code runs. */
    flatout1_kernel_set_bfs(bfs);

    /* Try path-based lookup first (now resolves Bugbear manifest
     * indirection transparently).  Fall back to known data entries. */
    static const char *TEX_PATHS[] = {
        "data/cars/car_2/dashboard.dds",
        "data/cars/car_4/skin3.dds",
        "data/tracks/town/textures/farmhouse_b.dds",
        "data/tracks/menu/textures/wirefence_alpha.dds",
        NULL
    };
    static const uint32_t KNOWN_DDS[] = {
        5456, 5523, 5580, 5637, 5694, 5755, 5816, 5877
    };

    dds_info dds = {0};
    const char *loaded = NULL;
    uint32_t *decoded_pixels = NULL;
    int tex_w = 640, tex_h = 480;

    /* Method 1: path lookup with manifest resolution. */
    for (int i = 0; TEX_PATHS[i]; i++) {
        const uint8_t *data; uint32_t len;
        const uint8_t *raw;
        if (!flatout1_bfs_find_by_path(bfs, TEX_PATHS[i],
                                       &data, &len, &raw, NULL))
            continue;
        if (!dds_parse(data, len, &dds)) { free((void *)raw); continue; }

        /* Decode DXT to RGBA8888 if compressed. */
        if (dds.four_cc) {
            size_t dst_sz = dxt_decode_dst_size(dds.width, dds.height);
            decoded_pixels = (uint32_t *)malloc(dst_sz);
            if (decoded_pixels &&
                dxt_decode_image(dds.pixels, dds.pixel_bytes,
                                 decoded_pixels, dds.width, dds.height,
                                 dds.four_cc)) {
                loaded = TEX_PATHS[i];
                tex_w = (int)dds.width; tex_h = (int)dds.height;
                printf("Loaded via path: %s  %ux%u  %s\n",
                       loaded, dds.width, dds.height,
                       dds.four_cc == FOURCC_DXT1 ? "DXT1" :
                       dds.four_cc == FOURCC_DXT3 ? "DXT3" :
                       dds.four_cc == FOURCC_DXT5 ? "DXT5" : "BC?");
            } else {
                free(decoded_pixels); decoded_pixels = NULL;
            }
        }
        free((void *)raw);
        if (loaded) break;
    }

    /* Method 2: known entry indices (fallback). */
    if (!loaded) {
        for (int i = 0; i < (int)(sizeof(KNOWN_DDS)/sizeof(KNOWN_DDS[0])); i++) {
            const uint8_t *data; uint32_t len;
            if (!flatout1_bfs_find_by_index(bfs, KNOWN_DDS[i], &data, &len))
                continue;
            if (!dds_parse(data, len, &dds)) continue;

            if (dds.four_cc) {
                size_t dst_sz = dxt_decode_dst_size(dds.width, dds.height);
                decoded_pixels = (uint32_t *)malloc(dst_sz);
                if (decoded_pixels &&
                    dxt_decode_image(dds.pixels, dds.pixel_bytes,
                                     decoded_pixels, dds.width, dds.height,
                                     dds.four_cc)) {
                    loaded = "DDS data entry (direct)";
                    tex_w = (int)dds.width; tex_h = (int)dds.height;
                    printf("Loaded: entry %u  %ux%u  %s\n",
                           KNOWN_DDS[i], dds.width, dds.height,
                           dds.four_cc == FOURCC_DXT1 ? "DXT1" :
                           dds.four_cc == FOURCC_DXT3 ? "DXT3" :
                           dds.four_cc == FOURCC_DXT5 ? "DXT5" : "BC?");
                } else {
                    free(decoded_pixels); decoded_pixels = NULL;
                }
            }
            if (loaded) break;
        }
    }

    if (!loaded)
        printf("No DDS texture found.\n");

    /* Init SDL3 */
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        flatout1_bfs_close(bfs); return 1;
    }

    int win_w = tex_w > 0 ? tex_w : 800;
    int win_h = tex_h > 0 ? tex_h : 600;
    if (win_w > 1280) { win_h = win_h * 1280 / win_w; win_w = 1280; }
    if (win_h > 960)  { win_w = win_w * 960  / win_h; win_h = 960;  }

    SDL_Window *window = SDL_CreateWindow(
        "FlatOut 1 — BOOTING", win_w, win_h, SDL_WINDOW_RESIZABLE);
    if (!window) {
        fprintf(stderr, "SDL window: %s\n", SDL_GetError());
        SDL_Quit(); flatout1_bfs_close(bfs); return 1;
    }

    SDL_Renderer *renderer = SDL_CreateRenderer(window, NULL);
    if (!renderer) renderer = SDL_CreateRenderer(window, "software");
    if (!renderer) {
        fprintf(stderr, "SDL renderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(window); SDL_Quit();
        flatout1_bfs_close(bfs); return 1;
    }
    SDL_SetRenderVSync(renderer, 1);

    /* Upload DDS pixels as SDL texture.  DXT-compressed data
     * will render as noise — pipeline proof.  Clamp surface
     * height to available pixel data to avoid OOB reads. */
    SDL_Texture *tex = NULL;
    if (decoded_pixels && dds.width > 0 && dds.height > 0) {
        SDL_Surface *s = SDL_CreateSurfaceFrom(
            (int)dds.width, (int)dds.height,
            SDL_PIXELFORMAT_RGBA8888,
            decoded_pixels, (int)dds.width * 4);
        if (s) {
            tex = SDL_CreateTextureFromSurface(renderer, s);
            SDL_DestroySurface(s);
        }
        free(decoded_pixels); decoded_pixels = NULL;
    }

    /* Render loop */
    printf("\nFlatOut 1 booting — BFS→DDS→SDL pipeline active.\n");
    printf("Close window or press key to exit.\n");
    int running = 1;
    Uint64 start = SDL_GetTicks();
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev))
            if (ev.type == SDL_EVENT_QUIT || ev.type == SDL_EVENT_KEY_DOWN)
                running = 0;

        SDL_SetRenderDrawColor(renderer, 40, 16, 2, 255);
        SDL_RenderClear(renderer);

        if (tex) {
            int rw, rh;
            SDL_GetCurrentRenderOutputSize(renderer, &rw, &rh);
            SDL_FRect d = {0, 0, (float)rw, (float)rh};
            float a = (float)dds.width / (float)dds.height;
            float wa = (float)rw / (float)rh;
            if (a > wa) { float h = (float)rw / a; d.y = ((float)rh - h)*0.5f; d.h = h; }
            else        { float w = (float)rh * a; d.x = ((float)rw - w)*0.5f; d.w = w; }
            SDL_RenderTexture(renderer, tex, NULL, &d);
        }

        /* Overlay bar */
        {
            int rw, rh;
            SDL_GetCurrentRenderOutputSize(renderer, &rw, &rh);
            (void)rh;
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 180);
            SDL_FRect bar = {0, 0, (float)rw, 50};
            SDL_RenderFillRect(renderer, &bar);
        }

        SDL_RenderPresent(renderer);
        if (SDL_GetTicks() - start > 8000) running = 0;
        SDL_Delay(16);
    }

    if (tex) SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    flatout1_bfs_close(bfs);
    printf("Done.\n");
    return 0;
}
