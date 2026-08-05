/**
 * burnout3_native_env_test.c — the host environment the transformed game
 * code runs in.
 *
 * This is the smallest piece of the "run Criterion's own code" path, and
 * it is where both of the bugs that stopped the render path lived: the
 * build was 32-bit when the runtime is written for 64, and the Xbox
 * mapping landed at an address whose low 32 bits made xbox_ptr_resolve()
 * hand back raw Xbox addresses. Neither was in the game code. Both are
 * pinned here.
 *
 * Needs the XBE, so it is skipped when the game data is not configured.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "b3_native_env.h"

extern ptrdiff_t g_xbox_mem_offset;

static int g_failures = 0;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: ", __func__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
        g_failures++; \
    } \
} while (0)

static const char *g_xbe = NULL;

/* The runtime resolves Xbox addresses with 64-bit arithmetic. A 32-bit
 * build silently truncates the offset and every access lands nowhere. */
static void test_host_is_64_bit(void)
{
    CHECK(sizeof(void *) == 8, "host pointers are %zu bytes; the recomp "
          "runtime requires a 64-bit build", sizeof(void *));
    CHECK(sizeof(ptrdiff_t) == 8, "ptrdiff_t is %zu bytes", sizeof(ptrdiff_t));
}

static void test_mapping_is_4gb_aligned(void)
{
    const uintptr_t base = b3_env_base();
    CHECK(base != 0, "no mapping");
    /* The load-bearing property: with low 32 bits zero, xbox_ptr_resolve()
     * skips its is-this-already-native guess and always rebases. With any
     * other alignment a small Xbox address can be mistaken for a host
     * pointer and returned unmapped. */
    CHECK((base & 0xFFFFFFFFu) == 0,
          "mapping at %#tx is not 4GB-aligned — xbox_ptr_resolve() will "
          "return raw Xbox addresses for low VAs", (ptrdiff_t)base);
    CHECK(g_xbox_mem_offset == (ptrdiff_t)base,
          "g_xbox_mem_offset %#tx != mapping base %#tx",
          g_xbox_mem_offset, (ptrdiff_t)base);
}

/* Every XBE section must be readable at its own virtual address. */
static void test_sections_loaded(void)
{
    CHECK(b3_env_section_count() == 17,
          "loaded %d sections, expected 17", b3_env_section_count());

    /* .text begins at 0x11000 and must not be blank. */
    const unsigned char *text = (const unsigned char *)b3_env_ptr(0x11000);
    CHECK(text != NULL, "no pointer for .text");
    if (text) {
        int nonzero = 0;
        for (int i = 0; i < 4096; i++) if (text[i]) nonzero++;
        CHECK(nonzero > 2000, ".text at 0x11000 is mostly zero (%d/4096) — "
              "sections did not load", nonzero);
    }
}

/* Addresses the whole port depends on, checked against the file rather
 * than trusted. D3D_g_pDevice is the one that cost this project the most:
 * it is a pointer, and the render path dereferences it immediately. */
static void test_known_addresses_are_mapped(void)
{
    const uint32_t probes[] = {
        0x11000,      /* .text */
        0x35FB48,     /* D3D8LTCG__D3D_g_pDevice */
        0x35FB58,     /* D3D8LTCG__D3D_g_RenderState */
        0x3B2360,     /* .data */
    };
    for (unsigned i = 0; i < sizeof probes / sizeof *probes; i++) {
        void *p = b3_env_ptr(probes[i]);
        CHECK(p != NULL, "no host pointer for %#x", probes[i]);
        if (p) {
            volatile uint32_t v = *(volatile uint32_t *)p;   /* must not fault */
            (void)v;
        }
    }
}

/* A null pointer inside the game's not-yet-built object graph must read
 * as zero rather than kill the process — the render path walks several
 * before the game has booted. */
static void test_null_reads_are_safe(void)
{
    void *p = b3_env_ptr(0);
    CHECK(p != NULL, "Xbox address 0 is not mapped");
    if (p) {
        volatile uint32_t v = *(volatile uint32_t *)p;
        CHECK(v == 0, "Xbox address 0 reads %#x, expected 0", v);
    }
}

int main(int argc, char **argv)
{
    g_xbe = argc > 1 ? argv[1] : NULL;
    if (!g_xbe) {
        fprintf(stderr, "SKIP: no XBE path given\n");
        return 0;
    }

    test_host_is_64_bit();
    if (g_failures) {           /* nothing else can work on a 32-bit host */
        fprintf(stderr, "burnout3_native_env_test: %d FAILURE(S)\n", g_failures);
        return 1;
    }

    if (b3_env_init(g_xbe) != 0) {
        fprintf(stderr, "FAIL: b3_env_init(%s)\n", g_xbe);
        return 1;
    }

    test_mapping_is_4gb_aligned();
    test_sections_loaded();
    test_known_addresses_are_mapped();
    test_null_reads_are_safe();

    b3_env_shutdown();

    if (g_failures) {
        fprintf(stderr, "burnout3_native_env_test: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    fprintf(stderr, "burnout3_native_env_test: all checks passed\n");
    return 0;
}
