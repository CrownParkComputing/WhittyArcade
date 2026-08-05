// KI MIPS CPU boot test — runs the actual MIPS interpreter through the
// first few hundred instructions of the boot ROM.
//
// Build: g++ -std=c++17 -Iinclude -o ki_cpu_test tests/ki_cpu_test.cpp \
//          src/midway/mips_cpu.cpp src/midway/midway_rom.cpp \
//          src/midway/midway_chd.cpp -lminizip -lz

#include "midway/mips_cpu.h"
#include "midway/midway_rom.h"
#include "midway/midway_chd.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>
#include <string>

// Minimal RAM + boot ROM buffers.
static std::vector<uint8_t> g_ram(512ULL * 1024 * 1024, 0);
static std::vector<uint8_t> g_boot_rom(512 * 1024, 0);
static std::vector<uint8_t> g_game_roms(4 * 1024 * 1024, 0xFF);  // u10-u36 at 0x1F800000
static std::vector<uint8_t> g_io(8 * 1024 * 1024, 0);  // dummy I/O
static std::string g_disc_path;
static chd_info g_chd_info;
static bool g_raw_disk = true;  // use raw file reads
static int g_ide_read_count = 0;

constexpr uint32_t BOOT_ROM_BASE = 0x1FC00000;
constexpr uint32_t GAME_ROM_BASE = 0x1F800000;
constexpr uint32_t IO_BASE = 0x1F000000;
constexpr uint32_t IDE_DATA_OFFSET = 0x500000;

// ---- Raw disk reader ----

static bool raw_read_sectors(uint64_t lba, void* dst, size_t count) {
    std::ifstream f(g_disc_path, std::ios::binary);
    if (!f) return false;
    f.seekg(static_cast<std::streamoff>(lba * 512), std::ios::beg);
    f.read(reinterpret_cast<char*>(dst),
           static_cast<std::streamsize>(count * 512));
    return f.gcount() == static_cast<std::streamsize>(count * 512);
}

// ---- Bus callbacks ----

static uint32_t mem_read32(void*, uint32_t addr) {
    if (addr < g_ram.size())
        return (static_cast<uint32_t>(g_ram[addr]) << 24) |
               (static_cast<uint32_t>(g_ram[addr + 1]) << 16) |
               (static_cast<uint32_t>(g_ram[addr + 2]) << 8) |
               static_cast<uint32_t>(g_ram[addr + 3]);
    if (addr >= BOOT_ROM_BASE && addr < BOOT_ROM_BASE + g_boot_rom.size()) {
        uint32_t off = addr - BOOT_ROM_BASE;
        return (static_cast<uint32_t>(g_boot_rom[off]) << 24) |
               (static_cast<uint32_t>(g_boot_rom[off + 1]) << 16) |
               (static_cast<uint32_t>(g_boot_rom[off + 2]) << 8) |
               static_cast<uint32_t>(g_boot_rom[off + 3]);
    }
    if (addr >= GAME_ROM_BASE && addr < GAME_ROM_BASE + g_game_roms.size()) {
        uint32_t off = addr - GAME_ROM_BASE;
        return (static_cast<uint32_t>(g_game_roms[off]) << 24) |
               (static_cast<uint32_t>(g_game_roms[off + 1]) << 16) |
               (static_cast<uint32_t>(g_game_roms[off + 2]) << 8) |
               static_cast<uint32_t>(g_game_roms[off + 3]);
    }
    if (addr >= IO_BASE && addr < IO_BASE + g_io.size()) {
        uint32_t off = addr - IO_BASE;
        // If this is an IDE data read, serve from raw disk.
        if (off >= IDE_DATA_OFFSET && off < IDE_DATA_OFFSET + 0x20) {
            // Don't actually read - just return 0 for now to avoid complexity.
            // The IDE subsystem is tested separately.
        }
        return (static_cast<uint32_t>(g_io[off]) << 24) |
               (static_cast<uint32_t>(g_io[off + 1]) << 16) |
               (static_cast<uint32_t>(g_io[off + 2]) << 8) |
               static_cast<uint32_t>(g_io[off + 3]);
    }
    return 0;
}

static int g_write_count = 0;

static void mem_write32(void*, uint32_t addr, uint32_t val) {
    if (g_write_count < 40) {
        fprintf(stderr, "[WR32] 0x%08X = 0x%08X\n", addr, val);
        ++g_write_count;
    }
    if (addr < g_ram.size()) {
        g_ram[addr] = static_cast<uint8_t>(val >> 24);
        g_ram[addr + 1] = static_cast<uint8_t>(val >> 16);
        g_ram[addr + 2] = static_cast<uint8_t>(val >> 8);
        g_ram[addr + 3] = static_cast<uint8_t>(val);
        return;
    }
    if (addr >= IO_BASE && addr < IO_BASE + g_io.size()) {
        uint32_t off = addr - IO_BASE;
        g_io[off] = static_cast<uint8_t>(val >> 24);
        g_io[off + 1] = static_cast<uint8_t>(val >> 16);
        g_io[off + 2] = static_cast<uint8_t>(val >> 8);
        g_io[off + 3] = static_cast<uint8_t>(val);
    }
}

static uint16_t mem_read16(void*, uint32_t addr) {
    uint32_t aligned = addr & ~3u;
    uint32_t word = mem_read32(nullptr, aligned);
    return static_cast<uint16_t>((addr & 2) ? (word & 0xFFFF) : (word >> 16));
}

static void mem_write16(void*, uint32_t addr, uint16_t val) {
    uint32_t aligned = addr & ~3u;
    uint32_t current = mem_read32(nullptr, aligned);
    uint32_t newval = (addr & 2) ? ((current & 0xFFFF0000u) | val)
                                 : (static_cast<uint32_t>(val) << 16);
    mem_write32(nullptr, aligned, newval);
}

static uint8_t mem_read8(void*, uint32_t addr) {
    uint32_t aligned = addr & ~3u;
    uint32_t word = mem_read32(nullptr, aligned);
    uint8_t shift = static_cast<uint8_t>((3 - (addr & 3)) * 8);
    return static_cast<uint8_t>(word >> shift);
}

static void mem_write8(void*, uint32_t addr, uint8_t val) {
    uint32_t aligned = addr & ~3u;
    uint32_t current = mem_read32(nullptr, aligned);
    uint8_t shift = static_cast<uint8_t>((3 - (addr & 3)) * 8);
    uint32_t mask = ~(0xFFu << shift);
    mem_write32(nullptr, aligned,
                (current & mask) | (static_cast<uint32_t>(val) << shift));
}

// ---- Decode MIPS instruction for logging ----

static void decode_instruction(uint32_t instr, uint32_t pc, char* buf, size_t bufsz) {
    uint32_t op = instr >> 26;
    switch (op) {
    case 0x00: {  // SPECIAL
        uint32_t funct = instr & 0x3F;
        uint32_t rs = (instr >> 21) & 0x1F;
        uint32_t rt = (instr >> 16) & 0x1F;
        uint32_t rd = (instr >> 11) & 0x1F;
        uint32_t sa = (instr >> 6) & 0x1F;
        switch (funct) {
        case 0x00: snprintf(buf, bufsz, "SLL    r%d,r%d,%u", rd, rt, sa); break;
        case 0x02: snprintf(buf, bufsz, "SRL    r%d,r%d,%u", rd, rt, sa); break;
        case 0x03: snprintf(buf, bufsz, "SRA    r%d,r%d,%u", rd, rt, sa); break;
        case 0x04: snprintf(buf, bufsz, "SLLV   r%d,r%d,r%d", rd, rt, rs); break;
        case 0x08: snprintf(buf, bufsz, "JR     r%d", rs); break;
        case 0x09: snprintf(buf, bufsz, "JALR   r%d,r%d", rd, rs); break;
        case 0x0C: snprintf(buf, bufsz, "SYSCALL"); break;
        case 0x0D: snprintf(buf, bufsz, "BREAK"); break;
        case 0x10: snprintf(buf, bufsz, "MFHI   r%d", rd); break;
        case 0x11: snprintf(buf, bufsz, "MTHI   r%d", rs); break;
        case 0x12: snprintf(buf, bufsz, "MFLO   r%d", rd); break;
        case 0x13: snprintf(buf, bufsz, "MTLO   r%d", rs); break;
        case 0x18: snprintf(buf, bufsz, "MULT   r%d,r%d", rs, rt); break;
        case 0x19: snprintf(buf, bufsz, "MULTU  r%d,r%d", rs, rt); break;
        case 0x1A: snprintf(buf, bufsz, "DIV    r%d,r%d", rs, rt); break;
        case 0x1B: snprintf(buf, bufsz, "DIVU   r%d,r%d", rs, rt); break;
        case 0x20: snprintf(buf, bufsz, "ADD    r%d,r%d,r%d", rd, rs, rt); break;
        case 0x21: snprintf(buf, bufsz, "ADDU   r%d,r%d,r%d", rd, rs, rt); break;
        case 0x22: snprintf(buf, bufsz, "SUB    r%d,r%d,r%d", rd, rs, rt); break;
        case 0x23: snprintf(buf, bufsz, "SUBU   r%d,r%d,r%d", rd, rs, rt); break;
        case 0x24: snprintf(buf, bufsz, "AND    r%d,r%d,r%d", rd, rs, rt); break;
        case 0x25: snprintf(buf, bufsz, "OR     r%d,r%d,r%d", rd, rs, rt); break;
        case 0x26: snprintf(buf, bufsz, "XOR    r%d,r%d,r%d", rd, rs, rt); break;
        case 0x27: snprintf(buf, bufsz, "NOR    r%d,r%d,r%d", rd, rs, rt); break;
        case 0x2A: snprintf(buf, bufsz, "SLT    r%d,r%d,r%d", rd, rs, rt); break;
        case 0x2B: snprintf(buf, bufsz, "SLTU   r%d,r%d,r%d", rd, rs, rt); break;
        default:   snprintf(buf, bufsz, "SPECIAL(0x%02X)", funct); break;
        }
        break;
    }
    case 0x01: {  // REGIMM
        uint32_t rt = (instr >> 16) & 0x1F;
        uint32_t rs = (instr >> 21) & 0x1F;
        int16_t imm = static_cast<int16_t>(instr & 0xFFFF);
        switch (rt) {
        case 0x00: snprintf(buf, bufsz, "BLTZ   r%d,%+d", rs, imm); break;
        case 0x01: snprintf(buf, bufsz, "BGEZ   r%d,%+d", rs, imm); break;
        case 0x10: snprintf(buf, bufsz, "BLTZAL r%d,%+d", rs, imm); break;
        case 0x11: snprintf(buf, bufsz, "BGEZAL r%d,%+d", rs, imm); break;
        default:   snprintf(buf, bufsz, "REGIMM(0x%02X)", rt); break;
        }
        break;
    }
    case 0x02: { uint32_t target = instr & 0x3FFFFFF; snprintf(buf, bufsz, "J      0x%08X", (pc & 0xF0000000) | (target << 2)); break; }
    case 0x03: { uint32_t target = instr & 0x3FFFFFF; snprintf(buf, bufsz, "JAL    0x%08X", (pc & 0xF0000000) | (target << 2)); break; }
    case 0x04: { uint32_t rs = (instr >> 21) & 0x1F; uint32_t rt = (instr >> 16) & 0x1F; int16_t imm = instr & 0xFFFF; snprintf(buf, bufsz, "BEQ    r%d,r%d,%+d", rs, rt, imm); break; }
    case 0x05: { uint32_t rs = (instr >> 21) & 0x1F; uint32_t rt = (instr >> 16) & 0x1F; int16_t imm = instr & 0xFFFF; snprintf(buf, bufsz, "BNE    r%d,r%d,%+d", rs, rt, imm); break; }
    case 0x06: { uint32_t rs = (instr >> 21) & 0x1F; int16_t imm = instr & 0xFFFF; snprintf(buf, bufsz, "BLEZ   r%d,%+d", rs, imm); break; }
    case 0x07: { uint32_t rs = (instr >> 21) & 0x1F; int16_t imm = instr & 0xFFFF; snprintf(buf, bufsz, "BGTZ   r%d,%+d", rs, imm); break; }
    case 0x08: { uint32_t rs = (instr >> 21) & 0x1F; uint32_t rt = (instr >> 16) & 0x1F; int16_t imm = instr & 0xFFFF; snprintf(buf, bufsz, "ADDI   r%d,r%d,%d", rt, rs, imm); break; }
    case 0x09: { uint32_t rs = (instr >> 21) & 0x1F; uint32_t rt = (instr >> 16) & 0x1F; int16_t imm = instr & 0xFFFF; snprintf(buf, bufsz, "ADDIU  r%d,r%d,%d", rt, rs, imm); break; }
    case 0x0A: { uint32_t rs = (instr >> 21) & 0x1F; uint32_t rt = (instr >> 16) & 0x1F; uint16_t imm = instr & 0xFFFF; snprintf(buf, bufsz, "SLTI   r%d,r%d,%d", rt, rs, (int16_t)imm); break; }
    case 0x0B: { uint32_t rs = (instr >> 21) & 0x1F; uint32_t rt = (instr >> 16) & 0x1F; uint16_t imm = instr & 0xFFFF; snprintf(buf, bufsz, "SLTIU  r%d,r%d,%u", rt, rs, imm); break; }
    case 0x0C: { uint32_t rs = (instr >> 21) & 0x1F; uint32_t rt = (instr >> 16) & 0x1F; uint16_t imm = instr & 0xFFFF; snprintf(buf, bufsz, "ANDI   r%d,r%d,0x%04X", rt, rs, imm); break; }
    case 0x0D: { uint32_t rs = (instr >> 21) & 0x1F; uint32_t rt = (instr >> 16) & 0x1F; uint16_t imm = instr & 0xFFFF; snprintf(buf, bufsz, "ORI    r%d,r%d,0x%04X", rt, rs, imm); break; }
    case 0x0E: { uint32_t rs = (instr >> 21) & 0x1F; uint32_t rt = (instr >> 16) & 0x1F; uint16_t imm = instr & 0xFFFF; snprintf(buf, bufsz, "XORI   r%d,r%d,0x%04X", rt, rs, imm); break; }
    case 0x0F: { uint32_t rt = (instr >> 16) & 0x1F; uint16_t imm = instr & 0xFFFF; snprintf(buf, bufsz, "LUI    r%d,0x%04X", rt, imm); break; }
    case 0x10: { uint32_t rs = (instr >> 21) & 0x1F; uint32_t rt = (instr >> 16) & 0x1F; uint32_t fmt = (instr >> 21) & 0x1F; uint32_t ft = (instr >> 16) & 0x1F; snprintf(buf, bufsz, "MFC0   r%d,cop0r%d", rt, (instr >> 11) & 0x1F); break; }
    case 0x14: { uint32_t rs = (instr >> 21) & 0x1F; uint32_t rt = (instr >> 16) & 0x1F; int16_t imm = instr & 0xFFFF; snprintf(buf, bufsz, "BEQL   r%d,r%d,%+d", rs, rt, imm); break; }
    case 0x20: { uint32_t rs = (instr >> 21) & 0x1F; uint32_t rt = (instr >> 16) & 0x1F; int16_t imm = instr & 0xFFFF; snprintf(buf, bufsz, "LB     r%d,%d(r%d)", rt, imm, rs); break; }
    case 0x24: { uint32_t rs = (instr >> 21) & 0x1F; uint32_t rt = (instr >> 16) & 0x1F; int16_t imm = instr & 0xFFFF; snprintf(buf, bufsz, "LBU    r%d,%d(r%d)", rt, imm, rs); break; }
    case 0x21: { uint32_t rs = (instr >> 21) & 0x1F; uint32_t rt = (instr >> 16) & 0x1F; int16_t imm = instr & 0xFFFF; snprintf(buf, bufsz, "LH     r%d,%d(r%d)", rt, imm, rs); break; }
    case 0x25: { uint32_t rs = (instr >> 21) & 0x1F; uint32_t rt = (instr >> 16) & 0x1F; int16_t imm = instr & 0xFFFF; snprintf(buf, bufsz, "LHU    r%d,%d(r%d)", rt, imm, rs); break; }
    case 0x23: { uint32_t rs = (instr >> 21) & 0x1F; uint32_t rt = (instr >> 16) & 0x1F; int16_t imm = instr & 0xFFFF; snprintf(buf, bufsz, "LW     r%d,%d(r%d)", rt, imm, rs); break; }
    case 0x28: { uint32_t rs = (instr >> 21) & 0x1F; uint32_t rt = (instr >> 16) & 0x1F; int16_t imm = instr & 0xFFFF; snprintf(buf, bufsz, "SB     r%d,%d(r%d)", rt, imm, rs); break; }
    case 0x29: { uint32_t rs = (instr >> 21) & 0x1F; uint32_t rt = (instr >> 16) & 0x1F; int16_t imm = instr & 0xFFFF; snprintf(buf, bufsz, "SH     r%d,%d(r%d)", rt, imm, rs); break; }
    case 0x2B: { uint32_t rs = (instr >> 21) & 0x1F; uint32_t rt = (instr >> 16) & 0x1F; int16_t imm = instr & 0xFFFF; snprintf(buf, bufsz, "SW     r%d,%d(r%d)", rt, imm, rs); break; }
    case 0x30: { uint32_t rs = (instr >> 21) & 0x1F; uint32_t rt = (instr >> 16) & 0x1F; int16_t imm = instr & 0xFFFF; snprintf(buf, bufsz, "LL     r%d,%d(r%d)", rt, imm, rs); break; }
    case 0x38: { uint32_t rs = (instr >> 21) & 0x1F; uint32_t rt = (instr >> 16) & 0x1F; int16_t imm = instr & 0xFFFF; snprintf(buf, bufsz, "SC     r%d,%d(r%d)", rt, imm, rs); break; }
    default:   snprintf(buf, bufsz, "???    (op=0x%02X)", op); break;
    }
}

int main(int argc, char** argv) {
    const char* rom_path = argc > 1 ? argv[1] : "/home/jon/.local/share/MANX/roms/midway/kinst.zip";
    const char* raw_path = argc > 2 ? argv[2] : "/home/jon/.local/share/MANX/chd/kinst.raw";

    fprintf(stderr, "=== KI CPU Boot Test ===\n");
    fprintf(stderr, "ROM: %s\n", rom_path);
    fprintf(stderr, "RAW: %s\n", raw_path);

    // Load ROM
    auto loaded = midway_rom_loader::load(rom_path, "");
    if (!loaded) {
        fprintf(stderr, "FAILED: %s\n", loaded.error.c_str());
        return 1;
    }
    fprintf(stderr, "ROM set: %d boot_rom: %zu bytes\n",
            (int)loaded.set, loaded.boot_rom.size());
    fprintf(stderr, "Disc path from loader: %s\n", loaded.disc_path.c_str());

    // Copy boot ROM
    size_t copy_size = std::min(loaded.boot_rom.size(), g_boot_rom.size());
    memcpy(g_boot_rom.data(), loaded.boot_rom.data(), copy_size);

    // Dump first 64 bytes of boot ROM
    fprintf(stderr, "\n--- Boot ROM first 64 bytes ---\n");
    for (int i = 0; i < 64; i += 16) {
        fprintf(stderr, "%04X: ", i);
        for (int j = 0; j < 16 && i + j < 64; ++j)
            fprintf(stderr, "%02X ", g_boot_rom[i + j]);
        fprintf(stderr, " ");
        for (int j = 0; j < 16 && i + j < 64; ++j) {
            char c = g_boot_rom[i + j];
            fprintf(stderr, "%c", (c >= 32 && c < 127) ? c : '.');
        }
        fprintf(stderr, "\n");
    }

    // What's the first instruction the CPU will fetch?
    uint32_t reset_pc = 0xBFC00000;
    uint32_t phys_reset = reset_pc & 0x1FFFFFFF;
    uint32_t reset_offset = phys_reset - BOOT_ROM_BASE;  // should be 0
    uint32_t first_instr = (static_cast<uint32_t>(g_boot_rom[reset_offset]) << 24) |
                           (static_cast<uint32_t>(g_boot_rom[reset_offset + 1]) << 16) |
                           (static_cast<uint32_t>(g_boot_rom[reset_offset + 2]) << 8) |
                           static_cast<uint32_t>(g_boot_rom[reset_offset + 3]);

    char disasm[128];
    decode_instruction(first_instr, reset_pc, disasm, sizeof(disasm));
    fprintf(stderr, "\nReset vector 0x%08X: 0x%08X  %s\n",
            reset_pc, first_instr, disasm);

    // Copy game ROMs if loaded
    if (!loaded.game_roms.empty()) {
        size_t copy_size = std::min(loaded.game_roms.size(), g_game_roms.size());
        memcpy(g_game_roms.data(), loaded.game_roms.data(), copy_size);
        fprintf(stderr, "Game ROMs loaded: %zu bytes\n", loaded.game_roms.size());
    } else {
        fprintf(stderr, "WARNING: No game ROMs loaded (u10-u36 not found)\n");
    }

    // Set up disc path for raw reads
    g_disc_path = raw_path;

    // Create MIPS CPU
    mips_cpu* cpu = mips_create(nullptr,
                                mem_read32, mem_write32,
                                mem_read16, mem_write16,
                                mem_read8, mem_write8);
    if (!cpu) {
        fprintf(stderr, "FAILED: mips_create returned null\n");
        return 1;
    }
    fprintf(stderr, "CPU created, PC=0x%08X\n", mips_last_pc(cpu));

    // Run instructions
    fprintf(stderr, "\n--- Running instructions ---\n");
    static const int MAX_INSTRS = 200;
    for (int i = 0; i < MAX_INSTRS; ++i) {
        uint32_t pc = mips_last_pc(cpu);
        uint32_t phys = pc & 0x1FFFFFFF;

        // Fetch the instruction for logging
        uint32_t instr = 0;
        if (phys >= BOOT_ROM_BASE && phys < BOOT_ROM_BASE + g_boot_rom.size()) {
            uint32_t off = phys - BOOT_ROM_BASE;
            instr = (static_cast<uint32_t>(g_boot_rom[off]) << 24) |
                    (static_cast<uint32_t>(g_boot_rom[off + 1]) << 16) |
                    (static_cast<uint32_t>(g_boot_rom[off + 2]) << 8) |
                    static_cast<uint32_t>(g_boot_rom[off + 3]);
        } else if (phys < g_ram.size()) {
            uint32_t off = phys;
            instr = (static_cast<uint32_t>(g_ram[off]) << 24) |
                    (static_cast<uint32_t>(g_ram[off + 1]) << 16) |
                    (static_cast<uint32_t>(g_ram[off + 2]) << 8) |
                    static_cast<uint32_t>(g_ram[off + 3]);
        }

        decode_instruction(instr, pc, disasm, sizeof(disasm));
        fprintf(stderr, "[%3d] PC=0x%08X   0x%08X  %s\n", i, pc, instr, disasm);

        uint32_t cycles = mips_step(cpu);

        // Check if PC hasn't changed (infinite loop or halt)
        if (i > 0 && mips_last_pc(cpu) == pc && i > 5) {
            fprintf(stderr, "[%3d] *** PC stuck at 0x%08X - possible halt or infinite loop ***\n",
                    i + 1, pc);
            // Run 5 more to be sure
            if (i > 10) {
                fprintf(stderr, "*** Stopping: PC is not advancing ***\n");
                break;
            }
        }
    }

    uint32_t final_pc = mips_last_pc(cpu);
    fprintf(stderr, "\n--- Final state ---\n");
    fprintf(stderr, "Final PC: 0x%08X\n", final_pc);
    fprintf(stderr, "After %d instructions\n", MAX_INSTRS);

    mips_destroy(cpu);
    fprintf(stderr, "=== Test complete ===\n");
    return 0;
}
