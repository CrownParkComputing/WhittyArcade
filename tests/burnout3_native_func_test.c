/**
 * burnout3_native_func_test.c — individual transformed game functions.
 *
 * These are Criterion's own functions, translated from the XBE. A leaf
 * function with no game state is exactly checkable: set up the machine
 * registers the caller would, run it, assert on memory. If the transform
 * were wrong these would fail, which is what separates "the code runs"
 * from "the code is right".
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "b3_native_env.h"

extern uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi;

/* sub_0003FE10(dst = eax, src = edx): copies FOUR consecutive 4x4 float
 * matrices, at +0x00, +0x40, +0x80 and +0xC0. The render path calls it to
 * move a matrix set out of the game object. (Written first expecting
 * three, which is what the surrounding code reads; the test caught it.) */
void sub_0003FE10(void);

static int g_failures = 0;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: ", __func__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
        g_failures++; \
    } \
} while (0)

#define SRC_VA 0x00A00000u
#define DST_VA 0x00A10000u
#define STK_VA 0x005F0000u

static void test_matrix_copy(void)
{
    /* The real shape, read off the transformed function: four 4x4
     * matrices, then six dwords, then five 16-byte vectors, then one
     * dword — 0x174 bytes with an 8-byte hole at 0x118 that it skips.
     * Written twice from a guess before checking; the test is byte-exact
     * now so it cannot drift again. */
    static const struct { uint32_t off, len; } copied[] = {
        { 0x000, 0x100 },   /* 4 matrices */
        { 0x100, 0x018 },   /* 6 dwords   */
        { 0x120, 0x050 },   /* 5 vectors — the movaps ones */
        { 0x170, 0x004 },   /* trailing dword */
    };
    const uint32_t STRUCT_END = 0x174;

    uint8_t *src = (uint8_t *)b3_env_ptr(SRC_VA);
    uint8_t *dst = (uint8_t *)b3_env_ptr(DST_VA);
    CHECK(src && dst, "no host pointers for the test buffers");
    if (!src || !dst) return;

    for (uint32_t i = 0; i < 0x200; i++) src[i] = (uint8_t)(i * 7 + 1);
    memset(dst, 0, 0x200);

    g_eax = DST_VA;
    g_edx = SRC_VA;
    g_esp = STK_VA;
    sub_0003FE10();

    /* Everything in a copied range must arrive intact. Before the lifter
     * lowered movaps as a real 16-byte move, only the first 4 bytes of
     * each vector did. */
    for (unsigned r = 0; r < sizeof copied / sizeof *copied; r++)
        for (uint32_t i = 0; i < copied[r].len; i++) {
            const uint32_t o = copied[r].off + i;
            CHECK(dst[o] == src[o], "byte %#x: got %02x want %02x",
                  o, dst[o], src[o]);
        }

    /* And nothing past the structure may be touched. */
    for (uint32_t o = STRUCT_END; o < 0x200; o++)
        CHECK(dst[o] == 0, "wrote past the structure at %#x (%02x)", o, dst[o]);
}

/* Copying onto itself must be harmless — the render path does this when
 * source and destination are the same slot. */
static void test_matrix_copy_in_place(void)
{
    float *src = (float *)b3_env_ptr(SRC_VA);
    if (!src) return;
    for (int i = 0; i < 96; i++) src[i] = (float)(i + 1);

    g_eax = SRC_VA;
    g_edx = SRC_VA;
    g_esp = STK_VA;
    sub_0003FE10();

    for (int i = 0; i < 96; i++)
        CHECK(src[i] == (float)(i + 1),
              "in-place copy corrupted element %d: %.1f", i, src[i]);
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "SKIP: no XBE path given\n"); return 0; }
    if (b3_env_init(argv[1]) != 0) {
        fprintf(stderr, "FAIL: b3_env_init(%s)\n", argv[1]);
        return 1;
    }

    test_matrix_copy();
    test_matrix_copy_in_place();

    b3_env_shutdown();

    if (g_failures) {
        fprintf(stderr, "burnout3_native_func_test: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    fprintf(stderr, "burnout3_native_func_test: all checks passed\n");
    return 0;
}
