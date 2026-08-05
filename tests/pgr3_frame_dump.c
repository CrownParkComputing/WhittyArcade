// Renders a PGR3 city through the SAME D3D8-on-Vulkan backend Burnout 3 uses,
// headless, and writes the frame as a PPM.
//
// This is the integration point, and it lives in MANX on purpose.
// PGR3Native supplies geometry and knows nothing about any device; the backend
// currently lives in Burnout3Recomp; only the framework is allowed to know about
// both. When the backend is promoted out of Burnout 3 into the framework proper,
// this file is what moves into the game tree and becomes its renderer.
//
//   pgr3_frame_dump <router-dir> <city> <out.ppm> [--chase|--eye]
#include "pgr3/pgr3_mesh.h"
#include "vulkan_d3d8.h"
#include "d3d8_xbox.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The mesh's own vertex is already the backend's canonical 3D layout: 12 bytes
// of position, a packed D3DCOLOR, and one texture coordinate = 24 bytes. Saying
// so as a static assertion rather than a comment means a change to either side
// stops the build instead of quietly feeding the pipeline the wrong stride.
#define PGR3_FVF (D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1)
typedef char pgr3_vertex_is_canonical[(sizeof(pgr3_vertex) == 24) ? 1 : -1];

static void mat_identity(D3DMATRIX* m) {
    memset(m, 0, sizeof(*m));
    m->m[0][0] = m->m[1][1] = m->m[2][2] = m->m[3][3] = 1.0f;
}

// D3D is row-vector (v * M), left-handed. The backend feeds these bytes to the
// shader raw and does the handedness fix itself, so they must be genuine D3D
// matrices rather than GL ones transposed.
static void mat_look_at_lh(D3DMATRIX* m, pgr3_vec3 eye, pgr3_vec3 at,
                           pgr3_vec3 up) {
    float zx = at.x - eye.x, zy = at.y - eye.y, zz = at.z - eye.z;
    float zl = sqrtf(zx * zx + zy * zy + zz * zz);
    zx /= zl; zy /= zl; zz /= zl;
    float xx = up.y * zz - up.z * zy;
    float xy = up.z * zx - up.x * zz;
    float xz = up.x * zy - up.y * zx;
    float xl = sqrtf(xx * xx + xy * xy + xz * xz);
    xx /= xl; xy /= xl; xz /= xl;
    float yx = zy * xz - zz * xy;
    float yy = zz * xx - zx * xz;
    float yz = zx * xy - zy * xx;

    mat_identity(m);
    m->m[0][0] = xx; m->m[0][1] = yx; m->m[0][2] = zx;
    m->m[1][0] = xy; m->m[1][1] = yy; m->m[1][2] = zy;
    m->m[2][0] = xz; m->m[2][1] = yz; m->m[2][2] = zz;
    m->m[3][0] = -(xx * eye.x + xy * eye.y + xz * eye.z);
    m->m[3][1] = -(yx * eye.x + yy * eye.y + yz * eye.z);
    m->m[3][2] = -(zx * eye.x + zy * eye.y + zz * eye.z);
}

static void mat_perspective_lh(D3DMATRIX* m, float fov_y, float aspect,
                               float zn, float zf) {
    float ys = 1.0f / tanf(fov_y * 0.5f);
    memset(m, 0, sizeof(*m));
    m->m[0][0] = ys / aspect;
    m->m[1][1] = ys;
    m->m[2][2] = zf / (zf - zn);
    m->m[2][3] = 1.0f;
    m->m[3][2] = -zn * zf / (zf - zn);
}

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr,
                "usage: pgr3_frame_dump <router-dir> <city> <out.ppm> "
                "[--chase|--eye]\n");
        return 2;
    }
    const char* dir = argv[1];
    const char* city = argv[2];
    const char* out = argv[3];
    float cam_height = 26.0f, cam_back = 55.0f;
    for (int i = 4; i < argc; ++i) {
        if (strcmp(argv[i], "--eye") == 0) { cam_height = 1.6f; cam_back = 8.0f; }
        else if (strcmp(argv[i], "--chase") == 0) { cam_height = 26.0f; cam_back = 55.0f; }
    }

    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.txt", dir, city);
    pgr3_route route;
    char err[256] = {0};
    if (pgr3_route_load(path, &route, err, sizeof(err)) != 0) {
        fprintf(stderr, "pgr3_frame_dump: %s\n", err);
        return 1;
    }
    pgr3_mesh_options opt = {3.0f, 0.0f, 0.0f};
    pgr3_road_mesh mesh;
    if (pgr3_road_mesh_build(&route, &opt, &mesh) != 0) {
        fprintf(stderr, "pgr3_frame_dump: mesh build failed\n");
        return 1;
    }
    // The backend's index buffers are 16-bit. Every shipped city is far under
    // that, but say so out loud rather than truncating silently - a wrapped
    // index draws a plausible-looking wrong road.
    if (mesh.vertex_count > 65535) {
        fprintf(stderr, "pgr3_frame_dump: %zu vertices exceeds 16-bit indices\n",
                mesh.vertex_count);
        return 1;
    }

    const int W = 1280, H = 720;
    if (!vulkan_d3d8_init(W, H)) {
        fprintf(stderr, "pgr3_frame_dump: vulkan_d3d8_init failed\n");
        return 1;
    }
    IDirect3DDevice8* dev = vulkan_d3d8_get_device();
    if (!dev) {
        fprintf(stderr, "pgr3_frame_dump: no device\n");
        return 1;
    }

    IDirect3DVertexBuffer8* vb = NULL;
    IDirect3DIndexBuffer8* ib = NULL;
    UINT vb_size = (UINT)(mesh.vertex_count * sizeof(pgr3_vertex));
    UINT ib_size = (UINT)(mesh.index_count * sizeof(uint16_t));

    if (FAILED(dev->lpVtbl->CreateVertexBuffer(dev, vb_size, 0, PGR3_FVF,
                                               D3DPOOL_MANAGED, &vb))) {
        fprintf(stderr, "pgr3_frame_dump: CreateVertexBuffer failed\n");
        return 1;
    }
    BYTE* dst = NULL;
    if (SUCCEEDED(vb->lpVtbl->Lock(vb, 0, vb_size, &dst, 0))) {
        memcpy(dst, mesh.vertices, vb_size);
        vb->lpVtbl->Unlock(vb);
    }

    if (FAILED(dev->lpVtbl->CreateIndexBuffer(dev, ib_size, 0, D3DFMT_INDEX16,
                                              D3DPOOL_MANAGED, &ib))) {
        fprintf(stderr, "pgr3_frame_dump: CreateIndexBuffer failed\n");
        return 1;
    }
    if (SUCCEEDED(ib->lpVtbl->Lock(ib, 0, ib_size, &dst, 0))) {
        uint16_t* d16 = (uint16_t*)dst;
        for (size_t i = 0; i < mesh.index_count; ++i)
            d16[i] = (uint16_t)mesh.indices[i];
        ib->lpVtbl->Unlock(ib);
    }

    // Stand on the longest section, as the software harness does, so the two
    // renderers can be compared from the same place.
    int best = 0, best_points = -1;
    for (size_t s = 0; s < route.section_count; ++s)
        if (route.sections[s].point_count > best_points) {
            best_points = route.sections[s].point_count;
            best = (int)s;
        }
    const pgr3_section* sec = &route.sections[best];
    pgr3_vec3 a, b;
    if (sec->point_count >= 2) {
        a = route.points[sec->first_point];
        b = route.points[sec->first_point + 1];
    } else {
        a = pgr3_gate_centre(&route.gates[sec->gate_a]);
        b = pgr3_gate_centre(&route.gates[sec->gate_b]);
    }
    float fx = b.x - a.x, fz = b.z - a.z;
    float fl = sqrtf(fx * fx + fz * fz);
    if (fl > 1e-6f) { fx /= fl; fz /= fl; }
    float look = 60.0f + cam_height * 4.0f;
    pgr3_vec3 eye = {a.x - fx * cam_back, a.y + cam_height, a.z - fz * cam_back};
    pgr3_vec3 at = {a.x + fx * look, a.y + cam_height * 0.25f, a.z + fz * look};
    pgr3_vec3 up = {0.0f, 1.0f, 0.0f};

    D3DMATRIX world, view, proj;
    mat_identity(&world);
    mat_look_at_lh(&view, eye, at, up);
    mat_perspective_lh(&proj, 65.0f * 3.14159265f / 180.0f, (float)W / (float)H,
                       0.5f, 6000.0f);

    dev->lpVtbl->BeginScene(dev);
    dev->lpVtbl->Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                       0xff9fb0c8, 1.0f, 0);
    dev->lpVtbl->SetTransform(dev, D3DTS_WORLD, &world);
    dev->lpVtbl->SetTransform(dev, D3DTS_VIEW, &view);
    dev->lpVtbl->SetTransform(dev, D3DTS_PROJECTION, &proj);
    dev->lpVtbl->SetRenderState(dev, D3DRS_ZENABLE, TRUE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_ZWRITEENABLE, TRUE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_LIGHTING, FALSE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_CULLMODE, 1 /*D3DCULL_NONE*/);
    dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHABLENDENABLE, FALSE);
    dev->lpVtbl->SetTexture(dev, 0, NULL);
    dev->lpVtbl->SetVertexShader(dev, PGR3_FVF);
    dev->lpVtbl->SetStreamSource(dev, 0, vb, sizeof(pgr3_vertex));
    dev->lpVtbl->SetIndices(dev, ib, 0);
    dev->lpVtbl->DrawIndexedPrimitive(dev, D3DPT_TRIANGLELIST, 0,
                                      (UINT)mesh.vertex_count, 0,
                                      (UINT)(mesh.index_count / 3));
    dev->lpVtbl->EndScene(dev);
    dev->lpVtbl->Present(dev, NULL, NULL, NULL, NULL);

    int pw = 0, ph = 0;
    const uint8_t* frame = vulkan_d3d8_present(&pw, &ph);
    if (!frame || pw <= 0 || ph <= 0) {
        fprintf(stderr, "pgr3_frame_dump: present returned nothing\n");
        return 1;
    }

    FILE* f = fopen(out, "wb");
    if (!f) {
        fprintf(stderr, "pgr3_frame_dump: cannot write %s\n", out);
        return 1;
    }
    fprintf(f, "P6\n%d %d\n255\n", pw, ph);
    for (int i = 0; i < pw * ph; ++i) fwrite(frame + (size_t)i * 4, 1, 3, f);
    fclose(f);

    // Report how much of the frame is not the clear colour. A frame that is
    // entirely sky means the draw did not land, and that is indistinguishable
    // from success in an exit status.
    size_t drawn = 0;
    for (int i = 0; i < pw * ph; ++i) {
        const uint8_t* p = frame + (size_t)i * 4;
        if (!(p[0] > 0x90 && p[0] < 0xd8 && p[2] > 0xb0)) drawn++;
    }
    printf("%s: %zu verts, %zu tris, %.1f%% of frame drawn -> %s\n", city,
           mesh.vertex_count, mesh.index_count / 3,
           100.0 * (double)drawn / (double)(pw * ph), out);

    vulkan_d3d8_shutdown();
    pgr3_road_mesh_free(&mesh);
    pgr3_route_free(&route);
    return drawn == 0 ? 1 : 0;
}
