// Headless Killer Instinct boot test.  Creates the full bus environment
// (RAM, boot ROM, game ROMs, PCI, I/O, IDE) and runs the MIPS CPU
// for several frames, logging key events to stdout.

#include "midway/mips_cpu.h"
#include "midway/midway_rom.h"
#include "midway/midway_chd.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

// ---- Mirror of wolfunit_session constants ----
static constexpr uint32_t RAM_SIZE       = 16 * 1024 * 1024;
static constexpr uint32_t BOOT_ROM_BASE  = 0x1FC00000;
static constexpr uint32_t BOOT_ROM_SIZE  = 0x80000;
static constexpr uint32_t IO_BASE        = 0x1F000000;
static constexpr uint32_t IO_SIZE        = 0x00C00000;
static constexpr uint32_t GAME_ROM_BASE  = 0x1F800000;
static constexpr uint32_t GAME_ROM_SIZE  = 0x400000;

// PCI / Voodoo
static constexpr uint32_t GPU_JAL_TARGET = 0x14036BFC;  // phys
static constexpr uint32_t GPU_DELAY_SLOT = 0x14036C00;

// ---- Global state ----
static std::vector<uint8_t> g_ram(RAM_SIZE);
static std::vector<uint8_t> g_boot(BOOT_ROM_SIZE);
static std::vector<uint8_t> g_game(GAME_ROM_SIZE, 0xFF);
static mips_cpu* g_cpu = nullptr;

// IDE stubs (minimal)
static bool g_ide_busy = false;

// Stats
static int g_pci_reads = 0;
static int g_io_reads = 0;
static int g_boot_patch_hits = 0;
static uint32_t g_last_pc = 0;

// ---- Bus callbacks ----

static uint32_t read32_be(const uint8_t* p) {
    return (uint32_t(p[0])<<24)|(uint32_t(p[1])<<16)|(uint32_t(p[2])<<8)|p[3];
}

static uint32_t bus_read32(void* user, uint32_t addr) {
    (void)user;
    uint32_t phys = addr & 0x1FFFFFFF;
    g_last_pc = mips_last_pc(g_cpu);

    // PCI / Voodoo
    if (phys >= 0x10000000 && phys < 0x1F000000) {
        ++g_pci_reads;
        if (g_pci_reads <= 8)
            printf("[PCI-R] PC=0x%08X phys=0x%08X\n", g_last_pc, phys);
        if (phys == GPU_JAL_TARGET) return 0x03E00008;  // JR $ra
        if (phys == GPU_DELAY_SLOT || phys == GPU_DELAY_SLOT + 4) return 0x00000000;
        return 1;  // status: ready
    }

    // RAM
    if (addr < RAM_SIZE) return read32_be(&g_ram[addr]);

    // Boot ROM
    if (addr >= BOOT_ROM_BASE && addr < BOOT_ROM_BASE + BOOT_ROM_SIZE) {
        uint32_t off = addr - BOOT_ROM_BASE;
        // Patch: NOP out JAL to GPU at 0xBFC0260C
        if (off == 0x260C) {
            ++g_boot_patch_hits;
            return 0x00000000;  // NOP
        }
        return read32_be(&g_boot[off]);
    }

    // Game ROMs
    if (addr >= GAME_ROM_BASE && addr < GAME_ROM_BASE + GAME_ROM_SIZE)
        return read32_be(&g_game[addr - GAME_ROM_BASE]);

    // I/O region
    if (addr >= IO_BASE && addr < IO_BASE + IO_SIZE) {
        ++g_io_reads;
        if (g_io_reads <= 10)
            printf("[IO-R] PC=0x%08X off=0x%06X\n", g_last_pc, addr - IO_BASE);
        uint32_t off = addr - IO_BASE;
        // IDE status
        if (off == 0x50001C || off == 0x500020)
            return g_ide_busy ? 0x80000000u : 0x40000000u;  // RDY
        // VSYNC
        if (off == 0x40000C)
            return 1;  // vsync toggled
        // DCS ready
        if (off == 0x200008)
            return 0x01000000u;  // transfer done
        return 0;
    }

    return 0;
}

static void write32_be(uint8_t* p, uint32_t v) {
    p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v;
}

static void bus_write32(void* user, uint32_t addr, uint32_t val) {
    (void)user;
    if (addr < RAM_SIZE) {
        write32_be(&g_ram[addr], val);
        return;
    }
    if (addr >= IO_BASE && addr < IO_BASE + IO_SIZE) {
        uint32_t off = addr - IO_BASE;
        // IDE command
        if (off == 0x50001C) {
            uint8_t cmd = val >> 24;
            if (cmd == 0xEC) {  // IDENTIFY
                printf("[IDE] IDENTIFY DEVICE\n");
            } else if (cmd == 0x20 || cmd == 0x21) {
                printf("[IDE] READ SECTORS\n");
            } else {
                printf("[IDE] CMD 0x%02X at PC=0x%08X\n", cmd, g_last_pc);
            }
        }
        // ASIC control
        if (off == 0x400008) {
            printf("[VIDEO] ASIC control write 0x%08X\n", val);
        }
        return;
    }
}

static uint16_t bus_read16(void* user, uint32_t addr) {
    return (uint16_t)(bus_read32(user, addr & ~3u) >> ((addr & 2) ? 0 : 16));
}
static void bus_write16(void* user, uint32_t addr, uint16_t val) {
    bus_write32(user, addr & ~3u, (addr & 2) ? val : (uint32_t(val) << 16));
}
static uint8_t bus_read8(void* user, uint32_t addr) {
    return (uint8_t)(bus_read32(user, addr & ~3u) >> ((3-(addr&3))*8));
}
static void bus_write8(void* user, uint32_t addr, uint8_t val) {
    bus_write32(user, addr & ~3u, (uint32_t(val) << ((3-(addr&3))*8)));
}

int main() {
    printf("=== KI Headless Boot Test ===\n");

    // Load ROMs
    auto loaded = midway_rom_loader::load(
        "/home/jon/.local/share/MANX/roms/midway/kinst.zip", "");
    if (!loaded) {
        printf("ROM LOAD FAILED: %s\n", loaded.error.c_str());
        return 1;
    }
    printf("ROM set: %s\n", midway_rom_loader::set_short_name(loaded.set));
    printf("Boot ROM: %zu bytes\n", loaded.boot_rom.size());
    printf("Game ROMs: %zu bytes\n", loaded.game_roms.size());

    memcpy(g_boot.data(), loaded.boot_rom.data(),
           std::min(loaded.boot_rom.size(), g_boot.size()));
    if (!loaded.game_roms.empty())
        memcpy(g_game.data(), loaded.game_roms.data(),
               std::min(loaded.game_roms.size(), g_game.size()));

    // Verify boot ROM patch location
    printf("Boot ROM at 0x260C: 0x%08X (original) → 0x00000000 (NOP patched)\n",
           read32_be(&g_boot[0x260C]));

    // Create CPU
    g_cpu = mips_create(nullptr, bus_read32, bus_write32,
                        bus_read16, bus_write16,
                        bus_read8, bus_write8);
    if (!g_cpu) { printf("CPU creation failed\n"); return 1; }
    printf("Reset PC: 0x%08X\n", mips_last_pc(g_cpu));

    // Run 10 frames (2M cycles each)
    constexpr uint32_t CYCLES_PER_FRAME = 2000000;
    for (int frame = 0; frame < 10; ++frame) {
        uint32_t ran = 0;
        uint32_t last_pc_log = 0;
        while (ran < CYCLES_PER_FRAME) {
            uint32_t pc_before = mips_last_pc(g_cpu);
            uint32_t cycles = mips_step(g_cpu);
            if (cycles == 0) {
                printf("[HALT] mips_step returned 0 at PC=0x%08X\n", pc_before);
                break;
            }
            ran += cycles;
            if (ran > 10000000) break;

            // Log PC every ~500K cycles
            if (ran - last_pc_log >= 500000) {
                uint32_t phys = pc_before & 0x1FFFFFFF;
                const char* region = "?";
                if (phys < RAM_SIZE) region = "RAM";
                else if (phys >= 0x1F000000 && phys < 0x1F800000) region = "IO";
                else if (phys >= 0x1F800000 && phys < 0x1FC00000) region = "GAME";
                else if (phys >= 0x1FC00000) region = "BOOT";
                last_pc_log = ran;
                printf("  [frame %d @%uK] PC=0x%08X (%s)\n",
                       frame, ran/1000, pc_before, region);
            }
        }
        printf("Frame %d: %u cycles, PC=0x%08X, PCI=%d IO=%d patch=%d\n",
               frame, ran, mips_last_pc(g_cpu),
               g_pci_reads, g_io_reads, g_boot_patch_hits);
    }

    printf("\n=== SUMMARY ===\n");
    printf("PCI reads: %d\n", g_pci_reads);
    printf("I/O reads: %d\n", g_io_reads);
    printf("Boot patch hits: %d (JAL at 0x260C NOPped)\n", g_boot_patch_hits);
    printf("Final PC: 0x%08X\n", mips_last_pc(g_cpu));

    mips_destroy(g_cpu);
    return 0;
}
