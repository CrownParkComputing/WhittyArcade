/**
 * burnout3_car_render_test.c — integration test: load a Burnout 3 car
 * model (.bgv), render it through the Vulkan D3D8 backend, and display
 * it in a live SDL3 window with keyboard controls.
 *
 * Usage:
 *   burnout3_car_render_test <bgv_path>
 *
 * Controls:
 *   ← →  rotate car around Y axis
 *   ↑ ↓  tilt camera pitch
 *   +/-  zoom in/out
 *   Esc  quit
 *   R    reset view
 *
 * Builds on the pattern of burnout3_render_unit_test.c (GPU backend)
 * and the SDL3 present path in launcher_menu.cpp.
 *
 * No game thread, no menu, no boot sequence — just one car, live.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <SDL3/SDL.h>

#define D3D8_USE_PORTABLE
#define BURNOUT3_NATIVE_LINUX
#include "d3d8_xbox.h"
#include "vulkan_d3d8.h"
#include "bgv_loader.h"

/* ── Xbox memory offset (needed by vulkan_d3d8 internals) ───── */
ptrdiff_t g_xbox_mem_offset = 0;

/* ── Window constants ───────────────────────────────────────── */
#define WIN_W  960
#define WIN_H  720

/* ── 4×4 matrix helpers ─────────────────────────────────────── */

static void mat4_identity(float m[16]) {
    memset(m, 0, 64);
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void mat4_perspective(float m[16], float fov_y, float aspect,
                             float znear, float zfar) {
    memset(m, 0, 64);
    float f = 1.0f / tanf(fov_y * 0.5f);
    m[0] = f / aspect;
    m[5] = f;
    m[10] = zfar / (znear - zfar);
    m[11] = -1.0f;
    m[14] = znear * zfar / (znear - zfar);
}

static void mat4_lookat(float m[16],
                        float ex, float ey, float ez,
                        float tx, float ty, float tz,
                        float ux, float uy, float uz) {
    float fx = tx - ex, fy = ty - ey, fz = tz - ez;
    float fl = sqrtf(fx*fx + fy*fy + fz*fz);
    fx /= fl; fy /= fl; fz /= fl;

    float rx = uy*fz - uz*fy;
    float ry = uz*fx - ux*fz;
    float rz = ux*fy - uy*fx;
    float rl = sqrtf(rx*rx + ry*ry + rz*rz);
    rx /= rl; ry /= rl; rz /= rl;

    float ux2 = fy*rz - fz*ry;
    float uy2 = fz*rx - fx*rz;
    float uz2 = fx*ry - fy*rx;

    m[0] = rx;   m[1] = ux2;  m[2] = -fx;  m[3] = 0;
    m[4] = ry;   m[5] = uy2;  m[6] = -fy;  m[7] = 0;
    m[8] = rz;   m[9] = uz2;  m[10] = -fz; m[11] = 0;
    m[12] = -(rx*ex + ry*ey + rz*ez);
    m[13] = -(ux2*ex + uy2*ey + uz2*ez);
    m[14] =  (fx*ex + fy*ey + fz*ez);
    m[15] = 1;
}

/* Rotation about world Y (heading). Row-vector convention. */
static void mat4_rotate_y(float m[16], float rad) {
    float c = cosf(rad), s = sinf(rad);
    mat4_identity(m);
    m[0] = c;  m[2] = s;
    m[8] = -s; m[10] = c;
}

/* ── Rendering helpers ──────────────────────────────────────── */

static void render_one_frame(IDirect3DDevice8 *dev,
                             IDirect3DVertexBuffer8 *vb,
                             IDirect3DIndexBuffer8 *ib,
                             const BGV_Model *model,
                             float pitch_rad, float yaw_rad,
                             float zoom) {
    float r = model->bounding_radius > 0.1f ? model->bounding_radius : 2.0f;
    float dist = r * zoom;

    /* Camera orbits around origin at distance dist */
    float cx = sinf(yaw_rad) * dist;
    float cy = sinf(pitch_rad) * dist * 0.6f + r * 0.3f;
    float cz = cosf(yaw_rad) * dist;

    D3DMATRIX view_mat, proj_mat, world_mat;
    mat4_lookat((float *)&view_mat,
                cx, cy, cz,                   /* eye: orbiting */
                0.0f, r * 0.3f, 0.0f,         /* target: car centre */
                0.0f, 1.0f, 0.0f);             /* up */
    mat4_perspective((float *)&proj_mat,
                     45.0f * 3.14159265f / 180.0f,
                     (float)WIN_W / WIN_H,
                     r * 0.1f, r * 20.0f);
    /* World matrix: rotate car by -yaw so it always faces camera */
    mat4_rotate_y((float *)&world_mat, -yaw_rad);

    dev->lpVtbl->BeginScene(dev);

    D3DVIEWPORT8 vp = { 0, 0, WIN_W, WIN_H, 0.0f, 1.0f };
    dev->lpVtbl->SetViewport(dev, &vp);

    dev->lpVtbl->Clear(dev, 0, NULL,
                       D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                       0xFF203040, 1.0f, 0);

    dev->lpVtbl->SetRenderState(dev, D3DRS_ZENABLE, TRUE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_ZWRITEENABLE, TRUE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_LIGHTING, FALSE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHABLENDENABLE, FALSE);

    dev->lpVtbl->SetTransform(dev, D3DTS_VIEW, &view_mat);
    dev->lpVtbl->SetTransform(dev, D3DTS_PROJECTION, &proj_mat);
    dev->lpVtbl->SetTransform(dev, D3DTS_WORLD, &world_mat);

    dev->lpVtbl->SetVertexShader(dev,
        D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    dev->lpVtbl->SetStreamSource(dev, 0, vb, sizeof(BGV_Vertex));
    dev->lpVtbl->SetIndices(dev, ib, 0);

    dev->lpVtbl->SetTexture(dev, 0, NULL);
    dev->lpVtbl->SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_DISABLE);

    dev->lpVtbl->DrawIndexedPrimitive(dev, D3DPT_TRIANGLELIST,
                                      0, model->vertex_count,
                                      0, model->index_count / 3);

    dev->lpVtbl->EndScene(dev);
    dev->lpVtbl->Present(dev, NULL, NULL, NULL, NULL);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <bgv_path>\n"
                "  e.g.: %s game_data/xbox/burnout3/pveh/COMP/Car1.bgv\n"
                "Controls: arrows=rotate, +/-=zoom, R=reset, Esc=quit\n",
                argv[0], argv[0]);
        return 1;
    }

    const char *bgv_path = argv[1];

    /* ── 1. Init Xbox memory ────────────────────────────────── */
    #define XBOX_RAM (64 * 1024 * 1024)
    void *xbox_ram = calloc(1, XBOX_RAM);
    if (!xbox_ram) { fprintf(stderr, "OOM for Xbox RAM\n"); return 1; }
    g_xbox_mem_offset = (ptrdiff_t)xbox_ram;

    /* ── 2. Init Vulkan D3D8 backend ────────────────────────── */
    if (!vulkan_d3d8_init(WIN_W, WIN_H)) {
        fprintf(stderr, "Vulkan D3D8 init FAILED (no GPU?)\n");
        free(xbox_ram);
        return 1;
    }

    IDirect3DDevice8 *dev = vulkan_d3d8_get_device();
    if (!dev) {
        fprintf(stderr, "No D3D8 device\n");
        vulkan_d3d8_shutdown();
        free(xbox_ram);
        return 1;
    }

    /* ── 3. Init SDL3 window ────────────────────────────────── */
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        fprintf(stderr, "SDL_InitSubSystem: %s\n", SDL_GetError());
        vulkan_d3d8_shutdown();
        free(xbox_ram);
        return 1;
    }

    SDL_Window *window = SDL_CreateWindow(
        "Burnout 3 — Car Viewer",
        WIN_W, WIN_H,
        SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        vulkan_d3d8_shutdown();
        free(xbox_ram);
        return 1;
    }

    SDL_Renderer *renderer = SDL_CreateRenderer(window, NULL);
    if (!renderer) {
        renderer = SDL_CreateRenderer(window, "software");
    }
    if (!renderer) {
        fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        vulkan_d3d8_shutdown();
        free(xbox_ram);
        return 1;
    }

    SDL_Texture *tex = SDL_CreateTexture(
        renderer,
        SDL_PIXELFORMAT_XRGB8888,
        SDL_TEXTUREACCESS_STREAMING,
        WIN_W, WIN_H);
    if (!tex) {
        fprintf(stderr, "SDL_CreateTexture: %s\n", SDL_GetError());
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        vulkan_d3d8_shutdown();
        free(xbox_ram);
        return 1;
    }

    /* ── 4. Load car model ──────────────────────────────────── */
    BGV_Model model;
    memset(&model, 0, sizeof(model));
    if (bgv_load(bgv_path, &model) != 0) {
        fprintf(stderr, "BGV load FAILED: %s\n", bgv_path);
        SDL_DestroyTexture(tex);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        vulkan_d3d8_shutdown();
        free(xbox_ram);
        return 1;
    }
    fprintf(stderr, "Car: %u verts, %u idxs, radius=%.1f\n",
            model.vertex_count, model.index_count, model.bounding_radius);

    /* ── 5. Create GPU buffers ──────────────────────────────── */
    IDirect3DVertexBuffer8 *vb = NULL;
    {
        UINT vb_size = model.vertex_count * sizeof(BGV_Vertex);
        dev->lpVtbl->CreateVertexBuffer(
            dev, vb_size, 0,
            D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1,
            D3DPOOL_MANAGED, &vb);
        BYTE *data = NULL;
        vb->lpVtbl->Lock(vb, 0, vb_size, &data, 0);
        memcpy(data, model.vertices, vb_size);
        vb->lpVtbl->Unlock(vb);
    }

    IDirect3DIndexBuffer8 *ib = NULL;
    {
        UINT ib_size = model.index_count * sizeof(uint16_t);
        dev->lpVtbl->CreateIndexBuffer(
            dev, ib_size, 0, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib);
        BYTE *data = NULL;
        ib->lpVtbl->Lock(ib, 0, ib_size, &data, 0);
        memcpy(data, model.indices, ib_size);
        ib->lpVtbl->Unlock(ib);
    }

    /* ── 6. Main loop ───────────────────────────────────────── */
    float yaw = 0.0f;         /* camera orbit angle (radians) */
    float pitch = 0.3f;       /* camera elevation */
    float zoom = 3.5f;        /* camera distance factor */
    bool running = true;
    uint64_t last_tick = SDL_GetTicks();
    int frame_count = 0;
    float fps = 0.0f;

    fprintf(stderr, "Window open — arrows rotate, +/- zoom, Esc quit\n");

    while (running) {
        /* ── Input ──────────────────────────────────────────── */
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (ev.type == SDL_EVENT_KEY_DOWN) {
                switch (ev.key.key) {
                case SDLK_ESCAPE:  running = false; break;
                case SDLK_R:       yaw = 0; pitch = 0.3f; zoom = 3.5f; break;
                default: break;
                }
            }
        }

        /* Continuous key state for held keys */
        const bool *keys = SDL_GetKeyboardState(NULL);
        if (keys[SDL_SCANCODE_LEFT])  yaw -= 0.04f;
        if (keys[SDL_SCANCODE_RIGHT]) yaw += 0.04f;
        if (keys[SDL_SCANCODE_UP])    pitch += 0.02f;
        if (keys[SDL_SCANCODE_DOWN])  pitch -= 0.02f;
        if (keys[SDL_SCANCODE_EQUALS] || keys[SDL_SCANCODE_KP_PLUS])
            zoom = fmaxf(1.5f, zoom - 0.1f);
        if (keys[SDL_SCANCODE_MINUS] || keys[SDL_SCANCODE_KP_MINUS])
            zoom = fminf(12.0f, zoom + 0.1f);

        /* ── Render ─────────────────────────────────────────── */
        render_one_frame(dev, vb, ib, &model, pitch, yaw, zoom);

        /* ── Read back and blit to window ───────────────────── */
        int fw = 0, fh = 0;
        const uint8_t *frame = vulkan_d3d8_present(&fw, &fh);
        if (frame && fw > 0 && fh > 0) {
            SDL_UpdateTexture(tex, NULL, frame, fw * 4);
            SDL_RenderClear(renderer);
            SDL_RenderTexture(renderer, tex, NULL, NULL);
            SDL_RenderPresent(renderer);
        }

        /* ── FPS counter ────────────────────────────────────── */
        frame_count++;
        uint64_t now = SDL_GetTicks();
        if (now - last_tick >= 2000) {
            fps = frame_count * 1000.0f / (float)(now - last_tick);
            char title[128];
            snprintf(title, sizeof(title),
                     "Burnout 3 — Car Viewer  |  %u tris  |  %.0f fps",
                     model.index_count / 3, fps);
            SDL_SetWindowTitle(window, title);
            last_tick = now;
            frame_count = 0;
        }
    }

    /* ── 7. Cleanup ─────────────────────────────────────────── */
    ib->lpVtbl->Release(ib);
    vb->lpVtbl->Release(vb);
    bgv_free(&model);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
    vulkan_d3d8_shutdown();
    free(xbox_ram);

    fprintf(stderr, "burnout3_car_render_test: done\n");
    return 0;
}
