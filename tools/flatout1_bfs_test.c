// flatout1_bfs_test.c — BFS archive diagnostic tool.
//
// Lists BFS contents by type (DDS textures, FSB audio, Lua scripts, etc.),
// extracts sample files, and verifies zlib decompression.
//
// Classification uses flatout1_bfs_peek() which reads from the in-memory
// peek cache (no seeks), so it runs in seconds instead of minutes.
//
// Usage: ./FlatOut1BFSTest [flatout.bfs] [extract_dir] [extract_one_file]

#include "flatout1_bfs_vfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>

/* ── Magic-byte classification ───────────────────────────────
 * Each entry's peek bytes might start with a metadata/path prefix;
 * we search WITHIN the buffer for these magic signatures. */

typedef struct {
    const char *name;
    const char *ext;
    uint8_t     magic[8];
    uint8_t     magic_len;
} magic_entry;

static const magic_entry MAGICS[] = {
    {"DDS texture",       "dds",  {'D','D','S',' '}, 4},
    {"WAV/AVI (RIFF)",    "wav",  {'R','I','F','F'}, 4},
    {"OGG audio",         "ogg",  {'O','g','g','S'}, 4},
    {"FSB4/5 audio bank", "fsb",  {'F','S','B',0},   4},
    {"PNG image",         "png",  {0x89,'P','N','G'}, 4},
    {"Bink video",        "bik",  {'B','I','K',0},   3},
    {"XMV video",         "xmv",  {'X','M','V',0},   4},
    {"Bugbear IMAG",      "img",  {'I','M','A','G'}, 4},
    {"Bugbear BMAP",      "bmp",  {'B','M','A','P'}, 4},
    {"SQLite DB",         "sql",  {'S','Q','L','i'}, 4},
    {"Lua script (text)", "lua",  {0},               0},
    {"zlib compressed",   "zz",   {0x78, 0},         1},
};

static int counts[32];
static int extras[32];   // entries beyond first 5000

/* Search for magic bytes anywhere in the buffer. */
static int find_magic(const uint8_t *buf, uint32_t len,
                      const uint8_t *magic, int magic_len) {
    if (magic_len <= 0 || magic_len > (int)len) return 0;
    for (uint32_t off = 0; off <= len - (uint32_t)magic_len; off++) {
        if (memcmp(buf + off, magic, magic_len) == 0)
            return 1;
    }
    return 0;
}

/* Extract a readable path string from peek data.  Returns 0 if none found. */
static int extract_path(const uint8_t *data, uint32_t len,
                        char *out, size_t out_sz) {
    for (uint32_t off = 0; off + 4 < len; off++) {
        if (data[off] != '/') continue;
        if (off == 0 || data[off-1] < 'a' || data[off-1] > 'z') continue;
        int start = (int)off;
        while (start > 0 && data[start-1] >= 32 && data[start-1] <= 126
               && data[start-1] != ' ')
            start--;
        int end = (int)off;
        while (end < (int)len && data[end] >= 32 && data[end] <= 126
               && data[end] != ' ')
            end++;
        if (end - start <= 4) continue;
        if (!memchr(data + start, '/', end - start)) continue;
        size_t n = (size_t)(end - start);
        if (n >= out_sz) n = out_sz - 1;
        memcpy(out, data + start, n);
        out[n] = 0;
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *bfs_path = argc > 1 ? argv[1]
        : "flatout1_extracted/FlatOut.1.USA.XBOX-ZTM/flatout.bfs";
    const char *extract_dir = argc > 2 ? argv[2] : NULL;
    const char *extract_one = argc > 3 ? argv[3] : NULL;

    printf("═══════════════════════════════════════════════════════\n");
    printf("  FlatOut 1 — BFS Archive Diagnostic\n");
    printf("═══════════════════════════════════════════════════════\n");
    printf("Archive: %s\n", bfs_path);

    flatout1_bfs *bfs = flatout1_bfs_open(bfs_path);
    if (!bfs) {
        fprintf(stderr, "ERROR: cannot open BFS\n");
        return 1;
    }

    uint32_t entry_count = flatout1_bfs_entry_count(bfs);
    printf("Entries: %u\n\n", entry_count);

    memset(counts, 0, sizeof(counts));
    memset(extras, 0, sizeof(extras));

    /* ── 1. Classify ALL entries by magic (using peek cache — no I/O) ── */
    printf("── Classification (scanning all %u entries via peek cache) ──\n",
           entry_count);

    for (uint32_t i = 0; i < entry_count; i++) {
        uint32_t peek_len;
        const uint8_t *data = flatout1_bfs_peek(bfs, i, &peek_len);
        if (!data || peek_len < 4) continue;

        int *bucket = (i < 5000) ? counts : extras;

        /* Try each magic type, searching anywhere in the peek. */
        int found = 0;
        for (int m = 0; m < (int)(sizeof(MAGICS)/sizeof(MAGICS[0])); m++) {
            if (MAGICS[m].magic_len == 0) continue;
            if (find_magic(data, peek_len, MAGICS[m].magic, MAGICS[m].magic_len)) {
                bucket[m]++;
                found = 1;
                break;
            }
        }

        /* zlib header (0x78) in raw peek data */
        if (!found && data[0] == 0x78) {
            bucket[11]++;  /* zlib index */
            found = 1;
        }

        /* Text/Lua: mostly printable and contains '=' */
        if (!found && peek_len > 20) {
            int printable = 0;
            for (uint32_t j = 0; j < peek_len && j < 100; j++)
                if (data[j] >= 32 && data[j] <= 126) printable++;
            if (printable >= 20 && memchr(data, '=', peek_len < 100 ? peek_len : 100)) {
                bucket[10]++;  /* Lua index */
                found = 1;
            }
        }

        if (!found) bucket[31]++;  /* other */
    }

    printf("First 5000 entries:\n");
    for (int m = 0; m < (int)(sizeof(MAGICS)/sizeof(MAGICS[0])); m++) {
        if (counts[m] > 0)
            printf("  %-22s (.%s): %d\n", MAGICS[m].name, MAGICS[m].ext,
                   counts[m]);
    }
    if (counts[31] > 0)
        printf("  %-22s      : %d\n", "Other/unclassified", counts[31]);

    if (entry_count > 5000) {
        printf("\nRemainder (%u entries):\n", entry_count - 5000);
        for (int m = 0; m < (int)(sizeof(MAGICS)/sizeof(MAGICS[0])); m++) {
            if (extras[m] > 0)
                printf("  %-22s (.%s): %d\n", MAGICS[m].name,
                       MAGICS[m].ext, extras[m]);
        }
        if (extras[31] > 0)
            printf("  %-22s      : %d\n", "Other/unclassified", extras[31]);
    }

    /* ── 2. Sample paths from the peek cache ─────────────────── */
    printf("\n── Sample Paths (from peek cache) ──\n");
    int sample = 0;
    char path[256];
    for (uint32_t i = 0; i < entry_count && sample < 30; i++) {
        uint32_t peek_len;
        const uint8_t *data = flatout1_bfs_peek(bfs, i, &peek_len);
        if (!data || peek_len < 4) continue;

        if (extract_path(data, peek_len, path, sizeof(path))) {
            printf("  [%5u] %s\n", i, path);
            sample++;
        }
    }

    /* ── 3. Path-based lookup (uses lazy hashtable built from peek cache) ── */
    printf("\n── Path Lookup Test ──\n");
    static const char *test_paths[] = {
        "data/cars/car_4/skin3.dds",
        "data/cars/car_2/dashboard.dds",
        "data/tracks/town/textures/farmhouse_b.dds",
        NULL
    };
    for (int p = 0; test_paths[p]; p++) {
        const uint8_t *data; uint32_t len;
        const uint8_t *raw; uint32_t raw_len;
        if (flatout1_bfs_find_by_path(bfs, test_paths[p],
                                      &data, &len, &raw, &raw_len)) {
            printf("  FOUND: %s (%u bytes", test_paths[p], len);
            if (len != raw_len) printf(", %u compressed", raw_len);
            printf(")\n");
            printf("    Magic: %02X %02X %02X %02X %02X %02X %02X %02X\n",
                   data[0], data[1], data[2], data[3],
                   data[4], data[5], data[6], data[7]);
            /* Find DDS magic offset */
            for (uint32_t off = 0; off + 4 <= len; off++)
                if (memcmp(data + off, "DDS ", 4) == 0) {
                    printf("    DDS magic at offset %u\n", off);
                    break;
                }
            free((void *)raw);
        } else {
            printf("  NOT FOUND: %s\n", test_paths[p]);
        }
    }

    /* ── 4. Extract a single file if requested ──────────────── */
    if (extract_one) {
        printf("\n── Extract: %s ──\n", extract_one);
        const uint8_t *data; uint32_t len;
        const uint8_t *raw; uint32_t raw_len;
        if (flatout1_bfs_find_by_path(bfs, extract_one,
                                      &data, &len, &raw, &raw_len)) {
            char outpath[512];
            const char *basename = strrchr(extract_one, '/');
            basename = basename ? basename + 1 : extract_one;
            if (extract_dir) {
                snprintf(outpath, sizeof(outpath), "%s/%s",
                         extract_dir, basename);
            } else {
                snprintf(outpath, sizeof(outpath), "/tmp/%s", basename);
            }
            FILE *out = fopen(outpath, "wb");
            if (out) {
                fwrite(data, 1, len, out);
                fclose(out);
                printf("  Wrote %u bytes to %s\n", len, outpath);
            } else {
                fprintf(stderr, "  Cannot write %s\n", outpath);
            }
            free((void *)raw);
        } else {
            printf("  NOT FOUND\n");
        }
    }

    /* ── 5. Quick extract: first 20 DDS textures ────────────── */
    if (extract_dir && !extract_one) {
        printf("\n── Extract first 20 DDS textures to %s ──\n", extract_dir);
        mkdir(extract_dir, 0755);
        int extracted = 0;
        for (uint32_t i = 0; i < entry_count && extracted < 20; i++) {
            uint32_t peek_len;
            const uint8_t *data = flatout1_bfs_peek(bfs, i, &peek_len);
            if (!data || peek_len < 4) continue;

            /* Check if the path string in peek data ends with .dds.
             * Cannot use DDS magic directly — zlib entries have 0x78
             * in peek, not "DDS ". */
            char fpath[256];
            if (!extract_path(data, peek_len, fpath, sizeof(fpath)))
                continue;
            size_t plen = strlen(fpath);
            if (plen < 4 || strcasecmp(fpath + plen - 4, ".dds") != 0)
                continue;

            /* Now actually load the full entry (seek + decompress).
             * Skip entries with absurd claimed sizes. */
            const uint8_t *fulldata; uint32_t fulllen;
            if (!flatout1_bfs_find_by_index(bfs, i, &fulldata, &fulllen))
                continue;
            if (fulllen > 100 * 1024 * 1024) continue;  /* >100 MB is corrupt */
            if (fulllen < 128) continue;  /* way too small for DDS */

            /* Find DDS magic in full data and skip the path prefix. */
            int dds_off = -1;
            for (uint32_t off = 0; off + 4 <= fulllen; off++) {
                if (memcmp(fulldata + off, "DDS ", 4) == 0) {
                    dds_off = (int)off;
                    break;
                }
            }
            if (dds_off < 0) continue;

            char outpath[1024];
            snprintf(outpath, sizeof(outpath), "%s/%s", extract_dir, fpath);
            /* Ensure parent dirs exist. */
            for (char *s = outpath + strlen(extract_dir) + 1; *s; s++) {
                if (*s == '/') { *s = 0; mkdir(outpath, 0755); *s = '/'; }
            }
            char *last = strrchr(outpath, '/');
            if (last) { *last = 0; mkdir(outpath, 0755); *last = '/'; }

            FILE *out = fopen(outpath, "wb");
            if (out) {
                fwrite(fulldata + dds_off, 1, fulllen - (uint32_t)dds_off, out);
                fclose(out);
                extracted++;
                printf("  [%3d] %s (%u bytes, DDS at +%d)\n",
                       extracted, fpath, fulllen, dds_off);
            }
        }
        printf("  Extracted %d DDS textures\n", extracted);
    }

    flatout1_bfs_close(bfs);
    printf("\nDone.\n");
    return 0;
}
