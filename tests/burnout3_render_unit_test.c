/**
 * burnout3_render_unit_test.c — unit tests for the D3D8→Vulkan backend's
 * CPU-side vertex machinery: FVF layout/stride derivation, vertex
 * canonicalization, triangle-list expansion (strip/fan/quad), indexed
 * expansion, and the fixed-function matrix product.
 *
 * The functions under test are static inside vulkan_d3d8.c, so the source
 * file is included directly (kernel-style). No Vulkan device is created —
 * only pure functions run, so this passes on headless CI.
 *
 * Checks are explicit fprintf+fail (never assert(): Release -DNDEBUG has
 * silently vacated assert-based tests in this repo before).
 */

#include "vulkan_d3d8.c"

static int g_failures = 0;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: ", __func__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
        g_failures++; \
    } \
} while (0)

#define FVF_3D_TEST     (D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1)
#define FVF_2D_TEX      (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)
#define FVF_2D_PLAIN    (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)

/* 36-byte BGV-style source vertex */
typedef struct {
    float x, y, z, nx, ny, nz;
    uint32_t color;
    float u, v;
} SrcV3D;

/* canonical 24-byte 3D vertex */
typedef struct { float x, y, z; uint32_t color; float u, v; } Canon3D;
/* canonical 28-byte 2D vertex */
typedef struct { float x, y, z, rhw; uint32_t color; float u, v; } Canon2D;

static void test_fvf_stride(void) {
    CHECK(fvf_src_stride(FVF_3D_TEST) == 36, "FVF_3D stride %u != 36",
          fvf_src_stride(FVF_3D_TEST));
    CHECK(fvf_src_stride(FVF_2D_TEX) == 28, "FEVertex stride %u != 28",
          fvf_src_stride(FVF_2D_TEX));
    CHECK(fvf_src_stride(FVF_2D_PLAIN) == 20, "ScreenVert stride %u != 20",
          fvf_src_stride(FVF_2D_PLAIN));
}

static void test_fvf_layout(void) {
    FVF_Layout L = fvf_layout(FVF_3D_TEST);
    CHECK(L.is3d == 1, "FVF_3D is3d");
    CHECK(L.pos_floats == 3, "FVF_3D pos_floats %d", L.pos_floats);
    CHECK(L.color_off == 24, "FVF_3D color_off %d != 24 (normals skipped)", L.color_off);
    CHECK(L.uv_off == 28, "FVF_3D uv_off %d != 28", L.uv_off);

    L = fvf_layout(FVF_2D_TEX);
    CHECK(L.is3d == 0, "FEVertex is3d");
    CHECK(L.pos_floats == 4, "FEVertex pos_floats %d", L.pos_floats);
    CHECK(L.color_off == 16, "FEVertex color_off %d", L.color_off);
    CHECK(L.uv_off == 20, "FEVertex uv_off %d", L.uv_off);

    L = fvf_layout(FVF_2D_PLAIN);
    CHECK(L.color_off == 16, "ScreenVert color_off %d", L.color_off);
    CHECK(L.uv_off == -1, "ScreenVert uv_off %d != -1", L.uv_off);
}

static void test_fvf_canon_3d(void) {
    SrcV3D src = { 1.f, 2.f, 3.f, 0.f, 1.f, 0.f, 0xAABBCCDD, 0.25f, 0.75f };
    Canon3D out;
    FVF_Layout L = fvf_layout(FVF_3D_TEST);
    fvf_canon(&L, (const uint8_t *)&src, (uint8_t *)&out);
    CHECK(out.x == 1.f && out.y == 2.f && out.z == 3.f,
          "3D canon pos (%f,%f,%f)", out.x, out.y, out.z);
    CHECK(out.color == 0xAABBCCDD, "3D canon color %08X (normals must be skipped)", out.color);
    CHECK(out.u == 0.25f && out.v == 0.75f, "3D canon uv (%f,%f)", out.u, out.v);
}

static void test_fvf_canon_2d_no_uv(void) {
    /* 20-byte ScreenVert: uv must default to 0, color preserved */
    struct { float x, y, z, rhw; uint32_t color; } src =
        { 10.f, 20.f, 0.5f, 1.f, 0x80FF8040 };
    Canon2D out;
    FVF_Layout L = fvf_layout(FVF_2D_PLAIN);
    fvf_canon(&L, (const uint8_t *)&src, (uint8_t *)&out);
    CHECK(out.x == 10.f && out.rhw == 1.f, "2D canon pos");
    CHECK(out.color == 0x80FF8040, "2D canon color %08X", out.color);
    CHECK(out.u == 0.f && out.v == 0.f, "missing uv must canon to 0 (got %f,%f)", out.u, out.v);
}

static void test_fvf_canon_no_diffuse(void) {
    /* No DIFFUSE in FVF → canonical color must default to opaque white */
    struct { float x, y, z; float u, v; } src = { 1, 2, 3, 4, 5 };
    Canon3D out;
    FVF_Layout L = fvf_layout(D3DFVF_XYZ | D3DFVF_TEX1);
    fvf_canon(&L, (const uint8_t *)&src, (uint8_t *)&out);
    CHECK(out.color == 0xFFFFFFFFu, "no-diffuse color %08X != white", out.color);
    CHECK(out.u == 4.f && out.v == 5.f, "no-diffuse uv");
}

static void test_mat_mul44(void) {
    float I[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    float A[16] = { 1,2,3,4, 5,6,7,8, 9,10,11,12, 13,14,15,16 };
    float out[16];

    mat_mul44(I, A, out);
    CHECK(memcmp(out, A, sizeof(A)) == 0, "I*A != A");
    mat_mul44(A, I, out);
    CHECK(memcmp(out, A, sizeof(A)) == 0, "A*I != A");

    /* D3D row-vector convention: translations live in row 3 and compose
     * additively: T(a)*T(b) = T(a+b). */
    float Ta[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 10,20,30,1 };
    float Tb[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 1,2,3,1 };
    mat_mul44(Ta, Tb, out);
    CHECK(out[12] == 11.f && out[13] == 22.f && out[14] == 33.f,
          "T*T translation row (%f,%f,%f)", out[12], out[13], out[14]);
    CHECK(out[0] == 1.f && out[5] == 1.f && out[15] == 1.f, "T*T diagonal");
}

/* Build N distinct 36-byte 3D source vertices with x = index. */
static void fill_src(SrcV3D *v, int n) {
    for (int i = 0; i < n; i++) {
        memset(&v[i], 0, sizeof(v[i]));
        v[i].x = (float)i; v[i].color = 0xFF000000u | (uint32_t)i;
        v[i].u = (float)i * 0.1f;
    }
}

static float canon_x(const uint8_t *buf, int i) {
    float x; memcpy(&x, buf + (size_t)i * 24, 4); return x;
}

static void test_expand_strip(void) {
    SrcV3D src[4]; fill_src(src, 4);
    uint8_t out[6 * 24];
    struct emit_up_ctx c;
    c.L = fvf_layout(FVF_3D_TEST);
    c.src = (const uint8_t *)src; c.stride = 36;
    c.pt = D3DPT_TRIANGLESTRIP; c.prim_count = 2;
    emit_up(out, &c);
    /* tri0 = v0,v1,v2  tri1 = v1,v2,v3 */
    const float want[6] = { 0, 1, 2, 1, 2, 3 };
    for (int i = 0; i < 6; i++)
        CHECK(canon_x(out, i) == want[i], "strip out[%d]=%f want %f",
              i, canon_x(out, i), want[i]);
}

static void test_expand_fan(void) {
    SrcV3D src[4]; fill_src(src, 4);
    uint8_t out[6 * 24];
    struct emit_up_ctx c;
    c.L = fvf_layout(FVF_3D_TEST);
    c.src = (const uint8_t *)src; c.stride = 36;
    c.pt = D3DPT_TRIANGLEFAN; c.prim_count = 2;
    emit_up(out, &c);
    /* tri0 = v0,v1,v2  tri1 = v0,v2,v3 */
    const float want[6] = { 0, 1, 2, 0, 2, 3 };
    for (int i = 0; i < 6; i++)
        CHECK(canon_x(out, i) == want[i], "fan out[%d]=%f want %f",
              i, canon_x(out, i), want[i]);
}

static void test_expand_quad(void) {
    SrcV3D src[4]; fill_src(src, 4);
    uint8_t out[6 * 24];
    struct emit_up_ctx c;
    c.L = fvf_layout(FVF_3D_TEST);
    c.src = (const uint8_t *)src; c.stride = 36;
    c.pt = D3DPT_QUADLIST; c.prim_count = 1;
    emit_up(out, &c);
    /* quad q0..q3 → (q0,q1,q2) + (q0,q2,q3) */
    const float want[6] = { 0, 1, 2, 0, 2, 3 };
    for (int i = 0; i < 6; i++)
        CHECK(canon_x(out, i) == want[i], "quad out[%d]=%f want %f",
              i, canon_x(out, i), want[i]);
}

static void test_expand_indexed(void) {
    SrcV3D src[8]; fill_src(src, 8);
    const uint16_t idx[6] = { 2, 3, 4, 4, 3, 5 };
    uint8_t out[6 * 24];
    struct emit_indexed_ctx c;
    c.L = fvf_layout(FVF_3D_TEST);
    c.verts = (const uint8_t *)src; c.stride = 36;
    c.indices = idx; c.index_count = 6; c.base = 1;   /* base_vertex offset */
    emit_indexed(out, &c);
    const float want[6] = { 3, 4, 5, 5, 4, 6 };       /* idx + base */
    for (int i = 0; i < 6; i++)
        CHECK(canon_x(out, i) == want[i], "indexed out[%d]=%f want %f",
              i, canon_x(out, i), want[i]);
}

int main(void) {
    test_fvf_stride();
    test_fvf_layout();
    test_fvf_canon_3d();
    test_fvf_canon_2d_no_uv();
    test_fvf_canon_no_diffuse();
    test_mat_mul44();
    test_expand_strip();
    test_expand_fan();
    test_expand_quad();
    test_expand_indexed();

    if (g_failures) {
        fprintf(stderr, "burnout3_render_unit_test: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    fprintf(stderr, "burnout3_render_unit_test: all checks passed\n");
    return 0;
}
