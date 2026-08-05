/**
 * burnout3_native_boot_test.c — the game's own main().
 *
 * sub_00156400 is what the CRT startup calls with (0,0,0): Burnout 3's
 * entry point. Everything up to now ran individual subsystems against
 * hand-supplied state. This runs the game's own initialisation and asks
 * whether it builds its own objects — specifically the global at
 * 0x4AED9C that the frame tick reads and, finding null, returns from.
 *
 * The game will not get far without its XDK services, and that is the
 * point: this test measures how far, and moves as they are bound.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>

#include "b3_native_env.h"
#include "b3_native_runtime.h"

extern uint32_t g_esp;

void sub_00156400(void);

static void *boot_thread(void *a) { (void)a; sub_00156400(); return NULL; }   /* the game's main(0,0,0) */

static int g_failures = 0;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: ", __func__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
        g_failures++; \
    } \
} while (0)

/* The object pointer the frame tick needs. Non-zero means the game has
 * constructed the thing it ticks. */
#define TICK_OBJECT 0x004AED9Cu
#define STK_VA      0x005F0000u

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "SKIP: no XBE path given\n"); return 0; }
    if (b3_env_init(argv[1]) != 0) {
        fprintf(stderr, "FAIL: b3_env_init(%s)\n", argv[1]);
        return 1;
    }

    const uint32_t before = *(uint32_t *)b3_env_ptr(TICK_OBJECT);
    b3_call_reset();
    b3_icall_reset();

    g_esp = STK_VA;
    #define PUSHV(v) do { g_esp -= 4; *(uint32_t *)b3_env_ptr(g_esp) = (v); } while (0)
    PUSHV(0); PUSHV(0); PUSHV(0);     /* main(0, 0, 0) */
    PUSHV(0xDEADBEEF);                /* return address slot */
    #undef PUSHV

    /* main() is a game loop and is not expected to return. Run it on a
     * thread, give it time, then look at what it built. */
    pthread_t th;
    fprintf(stderr, "calling the game's main (sub_00156400)...\n");
    pthread_create(&th, NULL, boot_thread, NULL);
    for (int i = 0; i < 50; i++) {
        usleep(100000);
        if (*(uint32_t *)b3_env_ptr(TICK_OBJECT)) break;
    }
    fprintf(stderr, "main ran for up to 5s\n");

    const uint32_t after = *(uint32_t *)b3_env_ptr(TICK_OBJECT);
    fprintf(stderr, "  tick object 0x4AED9C: %#x -> %#x\n", before, after);
    fprintf(stderr, "  %d distinct external calls, %d indirect targets requested\n",
            b3_call_distinct(), b3_icall_distinct());
    b3_call_dump();

    CHECK(after != 0,
          "the game's main did not construct the object its frame tick "
          "needs — boot did not get far enough");

    /* The boot thread is still inside the game loop and owns the Xbox
     * mapping. Tearing it down here would pull the ground out from under
     * it, so report and leave without unmapping — _exit skips atexit and
     * the destructors that would race the running thread. */
    if (g_failures) {
        fprintf(stderr, "burnout3_native_boot_test: %d FAILURE(S)\n", g_failures);
        fflush(stderr);
        _exit(1);
    }
    fprintf(stderr, "burnout3_native_boot_test: all checks passed\n");
    fflush(stderr);
    _exit(0);
}
