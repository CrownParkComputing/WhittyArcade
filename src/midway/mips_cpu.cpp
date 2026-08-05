// MIPS R4600 CPU interpreter (MIPS III ISA subset).
//
// Clean-room implementation targeting Killer Instinct on the Midway
// Wolf Unit. KI runs in 32-bit kernel mode with big-endian byte order.
//
// The CPU uses unmapped kseg0 (0x80000000) and kseg1 (0xA0000000)
// regions, which are identity-mapped to physical addresses. This means
// the TLB is not required for the initial boot sequence.
//
// Architecture notes:
//   - 32 x 64-bit GPRs (r0 is hardwired to zero)
//   - HI/LO multiply-divide registers
//   - Coprocessor 0: Status, Cause, EPC, Count, Compare, etc.
//   - Branch delay slots: the instruction after a branch always executes
//   - Big-endian byte order

#include "midway/mips_cpu.h"

#include <cstdlib>
#include <cstring>

// ---- Internal state --------------------------------------------------------

struct mips_cpu {
    // General-purpose registers (r0 always reads as 0).
    uint64_t r[32];
    uint64_t hi, lo;

    // Program counter tracking.
    uint32_t pc;
    uint32_t next_pc;

    // Coprocessor 0 registers.
    uint32_t sr;       // Status register
    uint32_t cause;    // Cause register (exception code + IP bits)
    uint32_t epc;      // Exception PC
    uint32_t count;    // Free-running counter
    uint32_t compare;  // Timer compare
    uint32_t config;   // Configuration

    // Bus callbacks.
    void* user_data;
    mips_read32_fn  read32;
    mips_write32_fn write32;
    mips_read16_fn  read16;
    mips_write16_fn write16;
    mips_read8_fn   read8;
    mips_write8_fn  write8;

    // Exceptions
    bool exception_pending;

    // Last PC for debugging.
    uint32_t last_pc;
};

// ---- Address translation ---------------------------------------------------
//
// KI runs in kernel mode with kseg0/kseg1 unmapped. The TLB is
// unnecessary for the initial boot; we simply strip the top 3 bits.
//
//   kuseg:  0x00000000-0x7FFFFFFF  (user, TLB-mapped)
//   kseg0:  0x80000000-0x9FFFFFFF  (kernel, cached, unmapped → phys 0x00000000)
//   kseg1:  0xA0000000-0xBFFFFFFF  (kernel, uncached, unmapped → phys 0x00000000)
//   kseg2:  0xC0000000-0xFFFFFFFF  (kernel, TLB-mapped)

static uint32_t to_physical(uint32_t addr) {
    if (addr >= 0x80000000 && addr < 0xC0000000)
        return addr & 0x1FFFFFFF;
    if (addr >= 0xC0000000)
        return addr & 0x1FFFFFFF;  // kseg2: assume identity for now
    return addr;
}

// ---- Memory access (big-endian) --------------------------------------------

static uint32_t mem_read32(mips_cpu* cpu, uint32_t addr) {
    addr = to_physical(addr) & ~3u;
    return cpu->read32(cpu->user_data, addr);
}

static void mem_write32(mips_cpu* cpu, uint32_t addr, uint32_t val) {
    addr = to_physical(addr) & ~3u;
    cpu->write32(cpu->user_data, addr, val);
}

static uint16_t mem_read16(mips_cpu* cpu, uint32_t addr) {
    addr = to_physical(addr) & ~1u;
    return cpu->read16(cpu->user_data, addr);
}

static void mem_write16(mips_cpu* cpu, uint32_t addr, uint16_t val) {
    addr = to_physical(addr) & ~1u;
    cpu->write16(cpu->user_data, addr, val);
}

static uint8_t mem_read8(mips_cpu* cpu, uint32_t addr) {
    return cpu->read8(cpu->user_data, to_physical(addr));
}

static void mem_write8(mips_cpu* cpu, uint32_t addr, uint8_t val) {
    cpu->write8(cpu->user_data, to_physical(addr), val);
}

// ---- Sign extension helpers ------------------------------------------------

static inline int32_t sign_ext16(uint16_t v) {
    return static_cast<int32_t>(static_cast<int16_t>(v));
}

static inline int64_t sign_ext32(uint32_t v) {
    return static_cast<int64_t>(static_cast<int32_t>(v));
}

// ---- Coprocessor 0 ---------------------------------------------------------

static uint32_t cop0_read(mips_cpu* cpu, uint8_t reg) {
    switch (reg) {
    case  0: return 0;                        // Index
    case  8: return 0;                        // BadVAddr
    case  9: return cpu->count;               // Count
    case 11: return cpu->compare;             // Compare
    case 12: return cpu->sr;                  // Status
    case 13: return cpu->cause;               // Cause
    case 14: return cpu->epc;                 // EPC
    case 15: return 0x00000200;               // PRId (R4600)
    case 16: return cpu->config;              // Config
    default: return 0;
    }
}

static void cop0_write(mips_cpu* cpu, uint8_t reg, uint32_t val) {
    switch (reg) {
    case  9: cpu->count   = val; break;
    case 11: cpu->compare = val; break;
    case 12: cpu->sr      = val; break;
    case 13: cpu->cause   = (cpu->cause & ~0x00000300) | (val & 0x00000300); break;
    case 14: cpu->epc     = val; break;
    case 16: cpu->config  = val; break;
    default: break;
    }
}

// Exception: save PC in EPC, set exception code in Cause, jump to handler.
static void exception(mips_cpu* cpu, uint8_t code, uint32_t bad_addr) {
    // Determine if the faulting instruction was in a branch delay slot.
    // After mips_step advances pc=next_pc, if pc != last_pc+4 then a
    // branch was just taken — the delay slot was at last_pc.
    const bool in_delay = (cpu->pc != cpu->last_pc + 4) && cpu->last_pc != 0;
    if (in_delay) {
        cpu->epc = cpu->last_pc - 4;  // The branch instruction
        cpu->cause |= (1u << 31);     // BD (branch delay) bit
    } else {
        cpu->epc = cpu->last_pc;
        cpu->cause &= ~(1u << 31);
    }
    cpu->cause = (cpu->cause & ~0x7C) | ((code & 0x1F) << 2);
    // Jump to exception handler (BEV=1: 0xBFC00380, else 0x80000080).
    const bool bev = (cpu->sr >> 22) & 1;
    cpu->pc = bev ? 0xBFC00380 : 0x80000080;
    cpu->next_pc = cpu->pc + 4;
    (void)bad_addr;
}

// ---- Public API ------------------------------------------------------------

mips_cpu* mips_create(void* user_data,
                      mips_read32_fn  read32,
                      mips_write32_fn write32,
                      mips_read16_fn  read16,
                      mips_write16_fn write16,
                      mips_read8_fn   read8,
                      mips_write8_fn  write8) {
    mips_cpu* cpu = static_cast<mips_cpu*>(calloc(1, sizeof(mips_cpu)));
    cpu->user_data = user_data;
    cpu->read32    = read32;
    cpu->write32   = write32;
    cpu->read16    = read16;
    cpu->write16   = write16;
    cpu->read8     = read8;
    cpu->write8    = write8;
    mips_reset(cpu);
    return cpu;
}

void mips_destroy(mips_cpu* cpu) {
    free(cpu);
}

void mips_reset(mips_cpu* cpu) {
    std::memset(cpu->r, 0, sizeof(cpu->r));
    cpu->hi = 0;
    cpu->lo = 0;
    cpu->pc      = 0xBFC00000;  // Reset vector
    cpu->next_pc = 0xBFC00004;
    // Status: BEV=1 (bootstrap exception vectors), kernel mode, interrupts off.
    cpu->sr      = 0x00400000;
    cpu->cause   = 0;
    cpu->epc     = 0;
    cpu->count   = 0;
    cpu->compare = 0;
    cpu->config  = 0;
    cpu->exception_pending = false;
    cpu->last_pc = 0;
}

uint32_t mips_last_pc(mips_cpu* cpu) {
    return cpu->last_pc;
}

int mips_in_delay_slot(mips_cpu* cpu) {
    return cpu->last_pc != 0 && (cpu->pc - cpu->last_pc) != 4;
}

// ---- Instruction execution ------------------------------------------------

uint32_t mips_step(mips_cpu* cpu) {
    cpu->last_pc = cpu->pc;

    const uint32_t addr = cpu->pc;
    cpu->pc = cpu->next_pc;
    cpu->next_pc += 4;

    const uint32_t op = mem_read32(cpu, addr);

    // R-type fields
    const uint8_t  rs    = (op >> 21) & 0x1F;
    const uint8_t  rt    = (op >> 16) & 0x1F;
    const uint8_t  rd    = (op >> 11) & 0x1F;
    const uint8_t  sa    = (op >>  6) & 0x1F;
    const uint8_t  func  = op & 0x3F;
    const uint8_t  opcode = (op >> 26) & 0x3F;

    // I-type immediate
    const int32_t  simm  = sign_ext16(static_cast<uint16_t>(op & 0xFFFF));
    const uint32_t uimm  = op & 0xFFFF;
    const uint32_t target = op & 0x03FFFFFF;

    // For trap instructions, the code is in bits [15:6] of rs.
    // For branch likely, rt is the condition code.

    // GPR values (r0 is always 0)
    #define GPR(n) ((n) == 0 ? 0 : static_cast<int32_t>(cpu->r[n]))
    #define GPRU(n) ((n) == 0 ? 0 : cpu->r[n])

    uint32_t cycles = 1;

    // ---- SPECIAL (opcode 0x00) --------------------------------------------
    if (opcode == 0x00) {
        switch (func) {
        // Shift
        case 0x00: // SLL
            cpu->r[rd] = sign_ext32(GPRU(rt) << sa);
            break;
        case 0x02: // SRL
            cpu->r[rd] = sign_ext32(GPRU(rt) >> sa);
            break;
        case 0x03: // SRA
            cpu->r[rd] = static_cast<int64_t>(GPR(rt)) >> sa;
            break;
        case 0x04: // SLLV
            cpu->r[rd] = sign_ext32(GPRU(rt) << (GPRU(rs) & 0x1F));
            break;
        case 0x06: // SRLV
            cpu->r[rd] = sign_ext32(GPRU(rt) >> (GPRU(rs) & 0x1F));
            break;
        case 0x07: // SRAV
            cpu->r[rd] = static_cast<int64_t>(GPR(rt)) >> (GPRU(rs) & 0x1F);
            break;

        // Jump register
        case 0x08: // JR
            cpu->next_pc = GPRU(rs);
            break;
        case 0x09: // JALR
            cpu->r[rd] = static_cast<int64_t>(cpu->pc + 4);
            cpu->next_pc = GPRU(rs);
            break;

        // System
        case 0x0C: // SYSCALL
            exception(cpu, 8, 0);  // Syscall exception
            cycles = 10;
            break;
        case 0x0D: // BREAK
            exception(cpu, 9, 0);  // Breakpoint exception
            cycles = 10;
            break;

        // Multiply/Divide
        case 0x18: // MULT
            {
                const int64_t result = static_cast<int64_t>(GPR(rs)) *
                                       static_cast<int64_t>(GPR(rt));
                cpu->lo = static_cast<uint64_t>(result) & 0xFFFFFFFFu;
                cpu->hi = static_cast<uint64_t>(result >> 32) & 0xFFFFFFFFu;
                cycles = 5;
            }
            break;
        case 0x19: // MULTU
            {
                const uint64_t a = GPRU(rs) & 0xFFFFFFFFu;
                const uint64_t b = GPRU(rt) & 0xFFFFFFFFu;
                const uint64_t result = a * b;
                cpu->lo = result & 0xFFFFFFFFu;
                cpu->hi = (result >> 32) & 0xFFFFFFFFu;
                cycles = 5;
            }
            break;
        case 0x1A: // DIV
            {
                const int32_t a = GPR(rs);
                const int32_t b = GPR(rt);
                if (b != 0) {
                    cpu->lo = static_cast<uint32_t>(a / b);
                    cpu->hi = static_cast<uint32_t>(a % b);
                }
                cycles = 36;
            }
            break;
        case 0x1B: // DIVU
            {
                const uint32_t a = static_cast<uint32_t>(GPRU(rs));
                const uint32_t b = static_cast<uint32_t>(GPRU(rt));
                if (b != 0) {
                    cpu->lo = a / b;
                    cpu->hi = a % b;
                }
                cycles = 36;
            }
            break;
        case 0x10: // MFHI
            cpu->r[rd] = sign_ext32(static_cast<uint32_t>(cpu->hi));
            break;
        case 0x12: // MFLO
            cpu->r[rd] = sign_ext32(static_cast<uint32_t>(cpu->lo));
            break;
        case 0x11: // MTHI
            cpu->hi = static_cast<uint64_t>(GPRU(rs));
            break;
        case 0x13: // MTLO
            cpu->lo = static_cast<uint64_t>(GPRU(rs));
            break;

        // ALU
        case 0x20: // ADD
            {
                const int32_t result = GPR(rs) + GPR(rt);
                // Check for signed overflow
                if (((GPR(rs) ^ result) & (GPR(rt) ^ result)) >> 31)
                    exception(cpu, 12, 0);  // Overflow
                else
                    cpu->r[rd] = sign_ext32(static_cast<uint32_t>(result));
            }
            break;
        case 0x21: // ADDU
            cpu->r[rd] = sign_ext32(GPRU(rs) + GPRU(rt));
            break;
        case 0x22: // SUB
            {
                const int32_t result = GPR(rs) - GPR(rt);
                if (((GPR(rs) ^ GPR(rt)) & (GPR(rs) ^ result)) >> 31)
                    exception(cpu, 12, 0);  // Overflow
                else
                    cpu->r[rd] = sign_ext32(static_cast<uint32_t>(result));
            }
            break;
        case 0x23: // SUBU
            cpu->r[rd] = sign_ext32(GPRU(rs) - GPRU(rt));
            break;
        case 0x24: // AND
            cpu->r[rd] = GPRU(rs) & GPRU(rt);
            break;
        case 0x25: // OR
            cpu->r[rd] = GPRU(rs) | GPRU(rt);
            break;
        case 0x26: // XOR
            cpu->r[rd] = GPRU(rs) ^ GPRU(rt);
            break;
        case 0x27: // NOR
            cpu->r[rd] = ~(GPRU(rs) | GPRU(rt));
            break;
        case 0x2A: // SLT
            cpu->r[rd] = (GPR(rs) < GPR(rt)) ? 1 : 0;
            break;
        case 0x2B: // SLTU
            cpu->r[rd] = (GPRU(rs) < GPRU(rt)) ? 1 : 0;
            break;

        // Trap (MIPS II)
        case 0x30: // TGE
            if (GPR(rs) >= GPR(rt)) exception(cpu, 13, 0);
            break;
        case 0x31: // TGEU
            if (GPRU(rs) >= GPRU(rt)) exception(cpu, 13, 0);
            break;
        case 0x32: // TLT
            if (GPR(rs) < GPR(rt)) exception(cpu, 13, 0);
            break;
        case 0x33: // TLTU
            if (GPRU(rs) < GPRU(rt)) exception(cpu, 13, 0);
            break;
        case 0x34: // TEQ
            if (GPRU(rs) == GPRU(rt)) exception(cpu, 13, 0);
            break;
        case 0x36: // TNE
            if (GPRU(rs) != GPRU(rt)) exception(cpu, 13, 0);
            break;

        default:
            break;  // Unimplemented: NOP
        }
    }
    // ---- REGIMM (opcode 0x01) --------------------------------------------
    else if (opcode == 0x01) {
        switch (rt) {
        case 0x00: // BLTZ
            if (GPR(rs) < 0) cpu->next_pc = cpu->pc + 4 + (simm << 2);
            break;
        case 0x01: // BGEZ
            if (GPR(rs) >= 0) cpu->next_pc = cpu->pc + 4 + (simm << 2);
            break;
        case 0x02: { // BLTZL (likely)
            if (GPR(rs) < 0) {
                uint32_t tgt = cpu->last_pc + 4 + (simm << 2);
                cpu->pc = tgt; cpu->next_pc = tgt + 4;
            }
            break;
        }
        case 0x03: { // BGEZL (likely)
            if (GPR(rs) >= 0) {
                uint32_t tgt = cpu->last_pc + 4 + (simm << 2);
                cpu->pc = tgt; cpu->next_pc = tgt + 4;
            }
            break;
        }
        case 0x10: // BLTZAL
            cpu->r[31] = static_cast<int64_t>(cpu->last_pc + 8);
            if (GPR(rs) < 0) cpu->next_pc = cpu->last_pc + 4 + (simm << 2);
            break;
        case 0x11: // BGEZAL
            cpu->r[31] = static_cast<int64_t>(cpu->last_pc + 8);
            if (GPR(rs) >= 0) cpu->next_pc = cpu->last_pc + 4 + (simm << 2);
            break;
        case 0x12: { // BLTZALL (likely)
            cpu->r[31] = static_cast<int64_t>(cpu->last_pc + 8);
            if (GPR(rs) < 0) {
                uint32_t tgt = cpu->last_pc + 4 + (simm << 2);
                cpu->pc = tgt; cpu->next_pc = tgt + 4;
            }
            break;
        }
        case 0x13: { // BGEZALL (likely)
            cpu->r[31] = static_cast<int64_t>(cpu->last_pc + 8);
            if (GPR(rs) >= 0) {
                uint32_t tgt = cpu->last_pc + 4 + (simm << 2);
                cpu->pc = tgt; cpu->next_pc = tgt + 4;
            }
            break;
        }
        default:
            break;
        }
    }
    // ---- J / JAL (opcodes 0x02, 0x03) ----------------------------------
    else if (opcode == 0x02) { // J
        cpu->next_pc = (cpu->pc & 0xF0000000) | (target << 2);
    }
    else if (opcode == 0x03) { // JAL
        cpu->r[31] = static_cast<int64_t>(cpu->pc + 8);
        cpu->next_pc = (cpu->pc & 0xF0000000) | (target << 2);
    }
    // ---- BEQ / BNE (opcodes 0x04, 0x05) --------------------------------
    else if (opcode == 0x04) { // BEQ
        if (GPRU(rs) == GPRU(rt))
            cpu->next_pc = cpu->pc + 4 + (simm << 2);
    }
    else if (opcode == 0x05) { // BNE
        if (GPRU(rs) != GPRU(rt))
            cpu->next_pc = cpu->pc + 4 + (simm << 2);
    }
    // ---- BEQL / BNEL / BLEZL / BGTZL (opcodes 0x14-0x17) -------------
    // Branch-likely: when taken, nullify the delay slot by overriding
    // cpu->pc (already advanced to delay slot address at top of mips_step).
    else if (opcode == 0x14) { // BEQL
        if (GPRU(rs) == GPRU(rt)) {
            uint32_t tgt = cpu->last_pc + 4 + (simm << 2);
            cpu->pc = tgt; cpu->next_pc = tgt + 4;
        }
    }
    else if (opcode == 0x15) { // BNEL
        if (GPRU(rs) != GPRU(rt)) {
            uint32_t tgt = cpu->last_pc + 4 + (simm << 2);
            cpu->pc = tgt; cpu->next_pc = tgt + 4;
        }
    }
    // ---- BLEZ (opcode 0x06) --------------------------------------------
    else if (opcode == 0x06) { // BLEZ
        if (GPR(rs) <= 0)
            cpu->next_pc = cpu->pc + 4 + (simm << 2);
    }
    // ---- BGTZ (opcode 0x07) --------------------------------------------
    else if (opcode == 0x07) { // BGTZ
        if (GPR(rs) > 0)
            cpu->next_pc = cpu->pc + 4 + (simm << 2);
    }
    else if (opcode == 0x16) { // BLEZL
        if (GPR(rs) <= 0) {
            uint32_t tgt = cpu->last_pc + 4 + (simm << 2);
            cpu->pc = tgt; cpu->next_pc = tgt + 4;
        }
    }
    else if (opcode == 0x17) { // BGTZL
        if (GPR(rs) > 0) {
            uint32_t tgt = cpu->last_pc + 4 + (simm << 2);
            cpu->pc = tgt; cpu->next_pc = tgt + 4;
        }
    }
    // ---- ADDI / ADDIU (opcodes 0x08, 0x09) ----------------------------
    else if (opcode == 0x08) { // ADDI
        const int32_t result = GPR(rs) + simm;
        if (((GPR(rs) ^ simm) & (GPR(rs) ^ result)) >> 31)
            exception(cpu, 12, 0);
        else
            cpu->r[rt] = sign_ext32(static_cast<uint32_t>(result));
    }
    else if (opcode == 0x09) { // ADDIU
        cpu->r[rt] = sign_ext32(GPRU(rs) + static_cast<uint32_t>(simm));
    }
    // ---- SLTI / SLTIU (opcodes 0x0A, 0x0B) -----------------------------
    else if (opcode == 0x0A) { // SLTI
        cpu->r[rt] = (GPR(rs) < simm) ? 1 : 0;
    }
    else if (opcode == 0x0B) { // SLTIU
        cpu->r[rt] = (GPRU(rs) < static_cast<uint64_t>(static_cast<uint32_t>(simm))) ? 1 : 0;
    }
    // ---- ANDI / ORI / XORI (opcodes 0x0C-0x0E) ------------------------
    else if (opcode == 0x0C) // ANDI
        cpu->r[rt] = GPRU(rs) & uimm;
    else if (opcode == 0x0D) // ORI
        cpu->r[rt] = GPRU(rs) | uimm;
    else if (opcode == 0x0E) // XORI
        cpu->r[rt] = GPRU(rs) ^ uimm;
    // ---- LUI (opcode 0x0F) ---------------------------------------------
    else if (opcode == 0x0F) // LUI
        cpu->r[rt] = sign_ext32(uimm << 16);
    // ---- Coprocessor 1/2/3 (opcodes 0x11-0x13) -------------------------
    // These trigger Coprocessor Unusable (CpU) exception if the
    // corresponding CU bit in the Status register is not set.
    else if (opcode == 0x11 || opcode == 0x12 || opcode == 0x13) {
        uint8_t cu_bit = static_cast<uint8_t>(opcode - 0x10 + 28);  // CU1=29, CU2=30, CU3=31
        if (!((cpu->sr >> cu_bit) & 1))
            exception(cpu, 11, 0);  // CpU exception
    }
    // ---- Coprocessor 0 (opcode 0x10) ------------------------------------
    else if (opcode == 0x10) {
        const uint8_t cop_fmt = rs;  // Format: rs field
        if (cop_fmt == 0x00) { // MFC0
            cpu->r[rt] = sign_ext32(cop0_read(cpu, rd));
        } else if (cop_fmt == 0x04) { // MTC0
            cop0_write(cpu, rd, GPRU(rt));
        }
    }
    // ---- Load instructions (opcodes 0x20-0x27) -------------------------
    else if (opcode == 0x20) { // LB
        cpu->r[rt] = static_cast<int64_t>(static_cast<int8_t>(
            mem_read8(cpu, GPRU(rs) + static_cast<uint32_t>(simm))));
    }
    else if (opcode == 0x21) { // LH
        cpu->r[rt] = static_cast<int64_t>(static_cast<int16_t>(
            mem_read16(cpu, GPRU(rs) + static_cast<uint32_t>(simm))));
    }
    else if (opcode == 0x23) { // LW
        cpu->r[rt] = sign_ext32(
            mem_read32(cpu, GPRU(rs) + static_cast<uint32_t>(simm)));
    }
    else if (opcode == 0x24) { // LBU
        cpu->r[rt] = static_cast<uint64_t>(
            mem_read8(cpu, GPRU(rs) + static_cast<uint32_t>(simm)));
    }
    else if (opcode == 0x25) { // LHU
        cpu->r[rt] = static_cast<uint64_t>(
            mem_read16(cpu, GPRU(rs) + static_cast<uint32_t>(simm)));
    }
    // ---- Store instructions (opcodes 0x28-0x2E) ------------------------
    else if (opcode == 0x28) { // SB
        mem_write8(cpu, GPRU(rs) + static_cast<uint32_t>(simm),
                   static_cast<uint8_t>(GPRU(rt)));
    }
    else if (opcode == 0x29) { // SH
        mem_write16(cpu, GPRU(rs) + static_cast<uint32_t>(simm),
                    static_cast<uint16_t>(GPRU(rt)));
    }
    else if (opcode == 0x2B) { // SW
        mem_write32(cpu, GPRU(rs) + static_cast<uint32_t>(simm),
                    static_cast<uint32_t>(GPRU(rt)));
    }
    // ---- CACHE (opcode 0x2F) -------------------------------------------
    else if (opcode == 0x2F) {
        // Cache operations are NOPs in our interpreter.
    }
    // ---- Unaligned loads (opcodes 0x22, 0x26) -------------------------
    else if (opcode == 0x22) { // LWL
        const uint32_t addr = GPRU(rs) + static_cast<uint32_t>(simm);
        const uint32_t aligned = addr & ~3u;
        const uint32_t word = mem_read32(cpu, aligned);
        const uint8_t shift = (addr & 3) * 8;
        const uint32_t mask = 0xFFFFFFFFu >> shift;
        cpu->r[rt] = sign_ext32((GPRU(rt) & ~mask) | ((word << (24 - (addr & 3) * 8)) & mask));
    }
    else if (opcode == 0x26) { // LWR
        const uint32_t addr = GPRU(rs) + static_cast<uint32_t>(simm);
        const uint32_t aligned = addr & ~3u;
        const uint32_t word = mem_read32(cpu, aligned);
        const uint8_t shift = (3 - (addr & 3)) * 8;
        const uint32_t mask = 0xFFFFFFFFu << shift;
        cpu->r[rt] = sign_ext32((GPRU(rt) & ~mask) | ((word >> ((addr & 3) * 8)) & mask));
    }
    // ---- Unaligned stores (opcodes 0x2A, 0x2E) ------------------------
    else if (opcode == 0x2A) { // SWL
        const uint32_t addr = GPRU(rs) + static_cast<uint32_t>(simm);
        const uint32_t aligned = addr & ~3u;
        const uint32_t word = mem_read32(cpu, aligned);
        const uint8_t shift = (addr & 3) * 8;
        const uint32_t mask = 0xFFFFFFFFu << shift;
        mem_write32(cpu, aligned, (word & ~mask) | ((GPRU(rt) >> (24 - (addr & 3) * 8)) & mask));
    }
    else if (opcode == 0x2E) { // SWR
        const uint32_t addr = GPRU(rs) + static_cast<uint32_t>(simm);
        const uint32_t aligned = addr & ~3u;
        const uint32_t word = mem_read32(cpu, aligned);
        const uint8_t shift = (3 - (addr & 3)) * 8;
        const uint32_t mask = 0xFFFFFFFFu >> shift;
        mem_write32(cpu, aligned, (word & ~mask) | ((GPRU(rt) << ((addr & 3) * 8)) & mask));
    }

    #undef GPR
    #undef GPRU

    // Timer: Count increments every cycle.
    cpu->count += cycles;

    // Check for timer interrupt.
    if (cpu->compare != 0 && cpu->count >= cpu->compare) {
        cpu->cause |= (1u << 15);  // IP7: timer interrupt pending
    }

    // Check for pending interrupts.
    const uint8_t im = (cpu->sr >> 8) & 0xFF;   // Interrupt mask
    const uint8_t ip = (cpu->cause >> 8) & 0xFF; // Pending interrupts
    const bool ie = (cpu->sr & 1) != 0;           // Interrupt enable (IEc)
    const bool exl = (cpu->sr & 2) != 0;          // Exception level

    if (ie && !exl && (ip & im)) {
        exception(cpu, 0, 0);  // External interrupt
    }

    return cycles;
}

uint32_t mips_cop0_read(mips_cpu* cpu, uint8_t reg) {
    return cop0_read(cpu, reg);
}

void mips_cop0_write(mips_cpu* cpu, uint8_t reg, uint32_t val) {
    cop0_write(cpu, reg, val);
}

void mips_set_interrupt(mips_cpu* cpu, uint8_t ip_bits) {
    cpu->cause |= (static_cast<uint32_t>(ip_bits) << 8);
}

void mips_clear_interrupt(mips_cpu* cpu, uint8_t ip_bits) {
    cpu->cause &= ~(static_cast<uint32_t>(ip_bits) << 8);
}
