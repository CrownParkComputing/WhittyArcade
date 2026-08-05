// flatout1_crt_test.c — FlatOut 1 CRT init table diagnostic.
//
// Loads the XBE, inits kernel + thunks, walks the init table, and
// prints every entry with its kernel thunk dependencies.  Confirms
// the boot sequence before any recompilation happens.
//
// Usage: ./FlatOut1CRTTest [flatout1_extracted/FlatOut.1.USA.XBOX-ZTM]

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Kernel shim API */
extern int       xbox_MemoryLayoutInit(const void *xbe, unsigned long sz);
extern void      xbox_MemoryLayoutShutdown(void);
extern void      flatout1_kernel_init(void);
extern void      flatout1_kernel_shutdown(void);
extern int       flatout1_crt_walk_init_table(uint32_t table_va);
extern void      flatout1_crt_dispatch(void);
extern int       flatout1_crt_set_init_args(int index, const uint32_t *args, int count);
extern uint32_t  flatout1_kernel_heap_alloc(uint32_t size, uint32_t align);
extern ptrdiff_t g_xbox_mem_offset;
extern uint32_t  g_eax;

#define MEM32(addr) (*(volatile uint32_t *)((uintptr_t)(addr) + (uintptr_t)g_xbox_mem_offset))

/* Types from kernel shim */
typedef struct {
    uint32_t xbox_va;
    uint32_t type_id;
    uint32_t size_bytes;
    uint32_t thunk_slots[16];
    int      thunk_count;
    uint32_t thunk_ordinals[16];
    void    (*native_stub)(void);
    uint32_t args[4];
    int      arg_count;
} fo1_init_entry;

extern const fo1_init_entry *flatout1_crt_get_init_entries(int *out_count);

static const char *ordinal_name(uint32_t ordinal) {
    switch (ordinal) {
    case  15: return "ExAllocatePool";
    case  16: return "ExAllocatePoolWithTag";
    case  24: return "ExQueryPoolBlockSize";
    case 165: return "MmAllocateContiguousMemory";
    case 166: return "MmAllocateContiguousMemoryEx";
    case 171: return "ExFreePool";
    case 187: return "NtClose";
    case 190: return "NtCreateFile";
    case 197: return "NtDuplicateObject";
    case 202: return "NtOpenFile";
    case 210: return "NtReadFile";
    case 218: return "NtQueryVolumeInformationFile";
    case 277: return "RtlEnterCriticalSection";
    case 289: return "RtlInitAnsiString";
    case 291: return "RtlInitializeCriticalSection";
    case 294: return "RtlLeaveCriticalSection";
    case 164: return "KeTickCount";
    default:  return "";
    }
}

int main(int argc, char **argv) {
    const char *data_dir = argc > 1 ? argv[1]
        : "flatout1_extracted/FlatOut.1.USA.XBOX-ZTM";

    printf("═══════════════════════════════════════════════════════\n");
    printf("  FlatOut 1 — CRT Init Table Diagnostic\n");
    printf("═══════════════════════════════════════════════════════\n");

    /* 1. Load XBE */
    char xbe_path[1024];
    snprintf(xbe_path, sizeof(xbe_path), "%s/default.xbe", data_dir);
    FILE *f = fopen(xbe_path, "rb");
    if (!f) { fprintf(stderr, "ERROR: cannot open %s\n", xbe_path); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *xbe = malloc((size_t)sz);
    if (!xbe || fread(xbe, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "ERROR: XBE read failed\n"); free(xbe); fclose(f); return 1;
    }
    fclose(f);

    /* 2. Decode XBE entry point */
    {
        const uint8_t *xb = (const uint8_t *)xbe;
        uint32_t raw  = *(const uint32_t *)(xb + 0x10C);
        uint32_t key  = *(const uint32_t *)(xb + 0x118);
        uint32_t entry = raw ^ key;
        printf("XBE entry: 0x%08X (raw=0x%08X key=0x%08X)\n", entry, raw, key);
    }

    /* 3. Init Xbox memory + kernel */
    if (!xbox_MemoryLayoutInit(xbe, (unsigned long)sz)) {
        fprintf(stderr, "ERROR: memory init failed\n"); free(xbe); return 1;
    }
    flatout1_kernel_init();

    /* 4. Walk init table */
    {
        const uint8_t *xb = (const uint8_t *)xbe;
        uint32_t raw  = *(const uint32_t *)(xb + 0x10C);
        uint32_t key  = *(const uint32_t *)(xb + 0x118);
        uint32_t entry = raw ^ key;

        flatout1_crt_walk_init_table(entry);

        int count;
        const fo1_init_entry *entries = flatout1_crt_get_init_entries(&count);

        /* ── Recompiled stub verification: fo1_init_06 (INIT_SYSTEM) ── */
        (void)entries;  /* used only in the block below */
        printf("\n── Recompiled Stub Test (fo1_init_06 @ 0x0026BEAB) ──\n");
        {
            int target = -1;
            for (int i = 0; i < count; i++)
                if (entries[i].xbox_va == 0x0026BEABu) target = i;
            if (target < 0) {
                printf("  ERROR: fo1_init_06 entry not found!\n");
            } else if (!entries[target].native_stub) {
                printf("  ERROR: fo1_init_06 has no native_stub wired!\n");
            } else {
                printf("  Entry %d has a recompiled stub — dispatching...\n",
                       target);

                /* Allocate a 3-dword state struct + two output dwords
                 * in Xbox memory for the stub to operate on. */
                uint32_t state_va = flatout1_kernel_heap_alloc(12, 4);
                uint32_t out1_va  = flatout1_kernel_heap_alloc(4, 4);
                uint32_t out2_va  = flatout1_kernel_heap_alloc(4, 4);
                if (!state_va || !out1_va || !out2_va) {
                    printf("  ERROR: heap alloc failed\n");
                } else {
                    /* Main path: f4 != 0.  Pick values that exercise
                     * both the ~f8&f0 and ~f0&f8 halves plus common. */
                    MEM32(state_va + 0) = 0xFF000000u;   /* f0 */
                    MEM32(state_va + 4) = 0x000000FFu;   /* f4 */
                    MEM32(state_va + 8) = 0x00FF0000u;   /* f8 */

                    uint32_t args[3] = { state_va, out1_va, out2_va };
                    flatout1_crt_set_init_args(target, args, 3);

                    g_eax = 0xDEADBEEFu;
                    flatout1_crt_dispatch();

                    uint32_t out1 = MEM32(out1_va);
                    uint32_t out2 = MEM32(out2_va);
                    uint32_t f4_after = MEM32(state_va + 4);
                    uint32_t f8_after = MEM32(state_va + 8);

                    /* Expected: o1 = ~f8&f0 | (f4&f8&f0)
                     *           o2 = ~f0&f8 | (f4&f8&f0)
                     * f0=FF000000 f4=000000FF f8=00FF0000
                     * ~f8&f0 = FF000000, ~f0&f8 = 00FF0000
                     * f4&f8&f0 = 0, so o1=FF000000 o2=00FF0000 */
                    printf("  out1=0x%08X (expect 0xFF000000) %s\n",
                           out1, out1 == 0xFF000000u ? "✓" : "✗");
                    printf("  out2=0x%08X (expect 0x00FF0000) %s\n",
                           out2, out2 == 0x00FF0000u ? "✓" : "✗");
                    printf("  state->f4=0x%08X (expect 0) %s\n",
                           f4_after, f4_after == 0 ? "✓" : "✗");
                    printf("  state->f8=0x%08X (expect 0xFF000000) %s\n",
                           f8_after, f8_after == 0xFF000000u ? "✓" : "✗");
                    printf("  return eax=0x%08X (expect 1) %s\n",
                           g_eax, g_eax == 1 ? "✓" : "✗");

                    /* Idle path: f4 == 0 → both outputs zeroed, return 0. */
                    MEM32(state_va + 4) = 0;
                    MEM32(out1_va) = 0xAAAAAAAAu;
                    MEM32(out2_va) = 0xBBBBBBBBu;
                    flatout1_crt_set_init_args(target, args, 3);
                    flatout1_crt_dispatch();
                    printf("  [idle] out1=0x%08X out2=0x%08X eax=0x%08X "
                           "(expect 0,0,0) %s\n",
                           MEM32(out1_va), MEM32(out2_va), g_eax,
                           (MEM32(out1_va) == 0 && MEM32(out2_va) == 0 &&
                            g_eax == 0) ? "✓" : "✗");
                }
            }
        }

        printf("\n── Init Sequence Summary ──\n");
        printf("Total entries: %d\n\n", count);

        for (int i = 0; i < count; i++) {
            const fo1_init_entry *e = &entries[i];
            printf("[%2d] VA=0x%08X  type=0x%08X  size=%4u bytes\n",
                   i, e->xbox_va, e->type_id, e->size_bytes);
            if (e->thunk_count == 0) {
                printf("     No kernel thunk calls detected. "
                       "(stub/BSS/zero-fill — skip)\n");
            } else {
                printf("     Kernel calls (%d):\n", e->thunk_count);
                for (int t = 0; t < e->thunk_count; t++) {
                    uint32_t ord = e->thunk_ordinals[t];
                    const char *nm = ordinal_name(ord);
                    printf("       slot %3u → ordinal %3u  %s\n",
                           e->thunk_slots[t], ord,
                           nm[0] ? nm : "(unknown)");
                }
            }

            /* Show what we need for recompilation */
            if (e->thunk_count > 0) {
                printf("     Recompile as: void fo1_init_%02d(void); "
                       "/* → calls: ", i);
                for (int t = 0; t < e->thunk_count; t++) {
                    const char *nm = ordinal_name(e->thunk_ordinals[t]);
                    printf("%s%s", t ? ", " : "",
                           nm[0] ? nm : "ord_?");
                }
                printf(" */\n");
            }
        }

        printf("\n── Recompilation Roadmap ──\n");
        printf("Functions needing recompilation: ");
        int need = 0;
        for (int i = 0; i < count; i++)
            if (entries[i].thunk_count > 0) need++;
        printf("%d / %d\n", need, count);

        printf("\nOrdered recompilation priority:\n");
        for (int i = 0; i < count; i++) {
            if (entries[i].thunk_count > 0) {
                printf("  %2d. fo1_init_%02d()  @ 0x%08X  size=%u  "
                       "thunks=%d  type=%s\n",
                       i+1, i, entries[i].xbox_va, entries[i].size_bytes,
                       entries[i].thunk_count,
                       entries[i].type_id == 0x00070009 ? "INIT_SUBSYSTEM" :
                       entries[i].type_id == 0x00070008 ? "INIT_SYSTEM" :
                       entries[i].type_id == 0x00010038 ? "INIT_GAME" :
                       "other");
            }
        }
    }

    /* 5. Cleanup */
    flatout1_kernel_shutdown();
    xbox_MemoryLayoutShutdown();
    free(xbe);

    printf("\n═══════════════════════════════════════════════════════\n");
    printf("  Diagnostic complete\n");
    printf("═══════════════════════════════════════════════════════\n");
    return 0;
}
