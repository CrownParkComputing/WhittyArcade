// MIPS R4600 CPU interpreter (MIPS III ISA subset).
//
// Clean-room implementation targeting Killer Instinct on the Midway
// Wolf Unit. KI runs in 32-bit kernel mode, LITTLE-endian (MAME: R4600LE).
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
//   - Little-endian byte order

#include "midway/mips_cpu.h"

#include <cstdlib>
#include <cstring>
#include <cstdio>

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
    uint32_t count;    // Count register value (increments at HALF cycle rate)
    uint32_t count_frac; // carries the odd cycle so Count advances every 2
    uint32_t compare;  // Timer compare
    bool compare_armed; // fire IP7 once per Compare write (edge-triggered)
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

// ---- Memory access (little-endian; the bus layer does the byte order) ----

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
    case  9: cpu->count = val; cpu->count_frac = 0; break;
    case 11:  // Compare: arm the timer, clear any pending IP7 (the ack)
        cpu->compare = val;
        cpu->compare_armed = true;
        cpu->cause &= ~(1u << 15);
        break;
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
    static const bool trace_exc = getenv("KI_TRACE_EXC") != nullptr;
    if (trace_exc) {
        static int n = 0;
        if (n < 40) {
            fprintf(stderr, "[EXC] #%d code=%u epc=0x%08X sr=0x%08X "
                    "cause=0x%08X ip&im=0x%X\n", n, code, cpu->epc, cpu->sr,
                    cpu->cause, (cpu->cause & cpu->sr) & 0xFC00);
            ++n;
        }
    }
    // Vector to the handler. A TLB/XTLB refill (codes 2/3) uses the dedicated
    // refill vector; everything else - interrupts included - uses the GENERAL
    // exception vector, which for BEV=0 is 0x80000180, NOT 0x80000080
    // (0x80000080 is the 64-bit XTLB-refill slot and the game leaves it zero,
    // so interrupts vectored there ran off into nop-filled RAM).
    const bool bev = (cpu->sr >> 22) & 1;
    const bool refill = (code == 2 || code == 3);
    if (bev)
        cpu->pc = refill ? 0xBFC00200 : 0xBFC00380;
    else
        cpu->pc = refill ? 0x80000000 : 0x80000180;
    cpu->next_pc = cpu->pc + 4;
    // Enter exception level: masks further interrupts until the handler's
    // ERET clears it. Without this the handler is re-interrupted on its very
    // first instruction and the CPU storms the vector.
    cpu->sr |= 0x2;  // SR.EXL
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
    // Status at reset: BEV=1 (bootstrap vectors) and ERL=1 (error level), as
    // MAME's mips3 sets it - interrupts are held off until the game clears ERL.
    cpu->sr      = 0x00400004;
    cpu->cause   = 0;
    cpu->epc     = 0;
    cpu->count   = 0;
    cpu->count_frac = 0;
    cpu->compare = 0xFFFFFFFF;  // MAME resets Compare high so it never matches
    cpu->compare_armed = false;
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

    // Optional one-shot code dump at KI_WATCH=<hexpc>, for boot bring-up. The
    // env is read once (getenv per instruction would dominate the frame).
    static const char* watch_env = getenv("KI_WATCH");
    if (watch_env) {
        static const uint32_t tw =
            static_cast<uint32_t>(strtoul(watch_env, nullptr, 16));
        if (addr == tw) {
            static bool done = false;
            if (!done) { done = true;
                for (int i = -6; i < 30; ++i)
                    fprintf(stderr, "[W] %08X: %08X\n", addr + i * 4,
                            mem_read32(cpu, addr + i * 4));
            }
        }
    }

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

    // GPR values (r0 is always 0). GPR/GPRU are the 32-bit views used by the
    // word instructions; GPRD/GPRDU are the full 64-bit register for the
    // MIPS III doubleword instructions.
    #define GPR(n) ((n) == 0 ? 0 : static_cast<int32_t>(cpu->r[n]))
    #define GPRU(n) ((n) == 0 ? 0 : cpu->r[n])
    #define GPRD(n) ((n) == 0 ? int64_t(0) : static_cast<int64_t>(cpu->r[n]))
    #define GPRDU(n) ((n) == 0 ? uint64_t(0) : cpu->r[n])

    uint32_t cycles = 1;

    // ---- SPECIAL (opcode 0x00) --------------------------------------------
    if (opcode == 0x00) {
        switch (func) {
        // Shift
        // The word shifts operate on the LOW 32 bits and sign-extend the
        // 32-bit result. GPRU(rt) is the full 64-bit register, so a value that
        // was sign-extended (upper 32 bits all ones) would otherwise feed
        // those ones into a right shift - which left KI's DCS byte-uploader
        // shifting 0xFF0055AA and never reaching zero, spinning forever.
        case 0x00: // SLL
            cpu->r[rd] = sign_ext32(static_cast<uint32_t>(GPRU(rt)) << sa);
            break;
        case 0x02: // SRL
            cpu->r[rd] = sign_ext32(static_cast<uint32_t>(GPRU(rt)) >> sa);
            break;
        case 0x03: // SRA
            cpu->r[rd] = static_cast<int64_t>(GPR(rt)) >> sa;
            break;
        case 0x04: // SLLV
            cpu->r[rd] = sign_ext32(static_cast<uint32_t>(GPRU(rt))
                                    << (GPRU(rs) & 0x1F));
            break;
        case 0x06: // SRLV
            cpu->r[rd] = sign_ext32(static_cast<uint32_t>(GPRU(rt))
                                    >> (GPRU(rs) & 0x1F));
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
        // 32-bit mul/div results are 32-bit values held sign-extended in
        // HI/LO, so the full-width MFHI/MFLO read them back correctly.
        case 0x18: // MULT
            {
                const int64_t result = static_cast<int64_t>(GPR(rs)) *
                                       static_cast<int64_t>(GPR(rt));
                cpu->lo = sign_ext32(static_cast<uint32_t>(result));
                cpu->hi = sign_ext32(static_cast<uint32_t>(result >> 32));
                cycles = 5;
            }
            break;
        case 0x19: // MULTU
            {
                const uint64_t result =
                    (GPRU(rs) & 0xFFFFFFFFu) * (GPRU(rt) & 0xFFFFFFFFu);
                cpu->lo = sign_ext32(static_cast<uint32_t>(result));
                cpu->hi = sign_ext32(static_cast<uint32_t>(result >> 32));
                cycles = 5;
            }
            break;
        case 0x1A: // DIV
            {
                const int32_t a = GPR(rs);
                const int32_t b = GPR(rt);
                if (b != 0) {
                    cpu->lo = sign_ext32(static_cast<uint32_t>(a / b));
                    cpu->hi = sign_ext32(static_cast<uint32_t>(a % b));
                }
                cycles = 36;
            }
            break;
        case 0x1B: // DIVU
            {
                const uint32_t a = static_cast<uint32_t>(GPRU(rs));
                const uint32_t b = static_cast<uint32_t>(GPRU(rt));
                if (b != 0) {
                    cpu->lo = sign_ext32(a / b);
                    cpu->hi = sign_ext32(a % b);
                }
                cycles = 36;
            }
            break;
        case 0x1C: // DMULT (signed 64x64 -> 128, low half in LO, high in HI)
        case 0x1D: { // DMULTU
            const bool sgn = (func == 0x1C);
            unsigned __int128 a = sgn
                ? (unsigned __int128)(__int128)GPRD(rs)
                : (unsigned __int128)GPRDU(rs);
            unsigned __int128 b = sgn
                ? (unsigned __int128)(__int128)GPRD(rt)
                : (unsigned __int128)GPRDU(rt);
            const unsigned __int128 r = a * b;
            cpu->lo = static_cast<uint64_t>(r);
            cpu->hi = static_cast<uint64_t>(r >> 64);
            cycles = 8;
            break;
        }
        case 0x1E: // DDIV
            if (GPRD(rt) != 0) {
                cpu->lo = static_cast<uint64_t>(GPRD(rs) / GPRD(rt));
                cpu->hi = static_cast<uint64_t>(GPRD(rs) % GPRD(rt));
            }
            cycles = 68;
            break;
        case 0x1F: // DDIVU
            if (GPRDU(rt) != 0) {
                cpu->lo = GPRDU(rs) / GPRDU(rt);
                cpu->hi = GPRDU(rs) % GPRDU(rt);
            }
            cycles = 68;
            break;
        case 0x0F: // SYNC - memory barrier, nothing to do in an interpreter
            break;
        case 0x10: // MFHI (full 64-bit)
            cpu->r[rd] = cpu->hi;
            break;
        case 0x12: // MFLO (full 64-bit)
            cpu->r[rd] = cpu->lo;
            break;
        case 0x11: // MTHI
            cpu->hi = GPRDU(rs);
            break;
        case 0x13: // MTLO
            cpu->lo = GPRDU(rs);
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

        // ---- MIPS III doubleword (64-bit) ---------------------------------
        // Variable shifts use the low 6 bits of rs; DADD/DSUB keep the full
        // 64-bit result (overflow traps are omitted - KI never relies on them
        // and the plain forms differ from DADDU/DSUBU only in that trap).
        case 0x14: // DSLLV
            cpu->r[rd] = GPRDU(rt) << (GPRU(rs) & 0x3F);
            break;
        case 0x16: // DSRLV
            cpu->r[rd] = GPRDU(rt) >> (GPRU(rs) & 0x3F);
            break;
        case 0x17: // DSRAV
            cpu->r[rd] = static_cast<uint64_t>(GPRD(rt) >> (GPRU(rs) & 0x3F));
            break;
        case 0x2C: // DADD
        case 0x2D: // DADDU
            cpu->r[rd] = GPRDU(rs) + GPRDU(rt);
            break;
        case 0x2E: // DSUB
        case 0x2F: // DSUBU
            cpu->r[rd] = GPRDU(rs) - GPRDU(rt);
            break;
        case 0x38: // DSLL
            cpu->r[rd] = GPRDU(rt) << sa;
            break;
        case 0x3A: // DSRL
            cpu->r[rd] = GPRDU(rt) >> sa;
            break;
        case 0x3B: // DSRA
            cpu->r[rd] = static_cast<uint64_t>(GPRD(rt) >> sa);
            break;
        case 0x3C: // DSLL32
            cpu->r[rd] = GPRDU(rt) << (sa + 32);
            break;
        case 0x3E: // DSRL32
            cpu->r[rd] = GPRDU(rt) >> (sa + 32);
            break;
        case 0x3F: // DSRA32
            cpu->r[rd] = static_cast<uint64_t>(GPRD(rt) >> (sa + 32));
            break;

        default: {
            static const bool trap = getenv("KI_TRAP_OP") != nullptr;
            if (trap) {
                static uint64_t seen = 0;
                if (!(seen & (1ull << func))) {
                    seen |= 1ull << func;
                    fprintf(stderr, "[UNIMPL] special fn 0x%02X at pc=0x%08X\n",
                            func, cpu->last_pc);
                }
            }
            break;  // Unimplemented: NOP
        }
        }
    }
    // ---- REGIMM (opcode 0x01) --------------------------------------------
    else if (opcode == 0x01) {
        switch (rt) {
        case 0x00: // BLTZ
            if (GPR(rs) < 0) cpu->next_pc = cpu->last_pc + 4 + (simm << 2);
            break;
        case 0x01: // BGEZ
            if (GPR(rs) >= 0) cpu->next_pc = cpu->last_pc + 4 + (simm << 2);
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
        // Return address is the instruction after the delay slot. cpu->pc is
        // ALREADY the delay-slot address (last_pc+4) at execute time, so the
        // link is last_pc+8 - the old cpu->pc+8 was last_pc+12, one instruction
        // too far, so every jal returned PAST the instruction after it. JALR
        // (cpu->pc+4) and BLTZAL/BGEZAL (last_pc+8) already had it right.
        cpu->r[31] = static_cast<int64_t>(cpu->last_pc + 8);
        cpu->next_pc = (cpu->pc & 0xF0000000) | (target << 2);
    }
    // ---- BEQ / BNE (opcodes 0x04, 0x05) --------------------------------
    else if (opcode == 0x04) { // BEQ
        if (GPRU(rs) == GPRU(rt))
            cpu->next_pc = cpu->last_pc + 4 + (simm << 2);
    }
    else if (opcode == 0x05) { // BNE
        if (GPRU(rs) != GPRU(rt))
            cpu->next_pc = cpu->last_pc + 4 + (simm << 2);
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
            cpu->next_pc = cpu->last_pc + 4 + (simm << 2);
    }
    // ---- BGTZ (opcode 0x07) --------------------------------------------
    else if (opcode == 0x07) { // BGTZ
        if (GPR(rs) > 0)
            cpu->next_pc = cpu->last_pc + 4 + (simm << 2);
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
        // Signed-overflow test for ADDITION: both operands' sign differs from
        // the result's. The previous form was the SUBTRACTION pattern
        // ((a^b)&(a^r)), which fired whenever the operands merely had opposite
        // signs - so `addi $x,$x,-1` crossing zero (0 -> -1) threw a spurious
        // overflow. KI's boot ROM counts its 48-entry TLB-init loop down with
        // exactly that instruction, so the bogus trap restarted the loop
        // forever and the machine never left CPU init.
        if (((GPR(rs) ^ result) & (simm ^ result)) >> 31)
            exception(cpu, 12, 0);
        else
            cpu->r[rt] = sign_ext32(static_cast<uint32_t>(result));
    }
    else if (opcode == 0x09) { // ADDIU
        cpu->r[rt] = sign_ext32(GPRU(rs) + static_cast<uint32_t>(simm));
    }
    // ---- DADDI / DADDIU (opcodes 0x18, 0x19), MIPS III 64-bit add-imm ----
    else if (opcode == 0x18 || opcode == 0x19) { // DADDI / DADDIU
        cpu->r[rt] = GPRDU(rs) + static_cast<uint64_t>(static_cast<int64_t>(simm));
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
        } else if ((op & 0x3F) == 0x18 && (rs & 0x10)) { // ERET (CO=1, fn 0x18)
            // Return from exception: resume at EPC and drop the level bit.
            // Without this the ISR can never return and interrupts stay
            // masked (EXL) forever. We keep a single EPC, so ERET clears both
            // EXL and ERL and jumps there.
            cpu->pc = cpu->epc;
            cpu->next_pc = cpu->epc + 4;
            cpu->sr &= ~0x6u;  // clear EXL | ERL
        }
        // Other CO=1 ops (TLBWI/TLBWR/TLBR/TLBP) are no-ops: kseg0/1 is
        // unmapped so the TLB never resolves an address here.
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
    // ---- Doubleword load/store (MIPS III, little-endian) ----------------
    else if (opcode == 0x37) { // LD
        const uint32_t addr = GPRU(rs) + static_cast<uint32_t>(simm);
        const uint64_t lo = mem_read32(cpu, addr);
        const uint64_t hi = mem_read32(cpu, addr + 4);
        cpu->r[rt] = lo | (hi << 32);
    }
    else if (opcode == 0x3F) { // SD
        const uint32_t addr = GPRU(rs) + static_cast<uint32_t>(simm);
        mem_write32(cpu, addr, static_cast<uint32_t>(GPRDU(rt)));
        mem_write32(cpu, addr + 4, static_cast<uint32_t>(GPRDU(rt) >> 32));
    }
    // Unaligned doubleword loads. Done byte-wise, which is unambiguous for
    // little-endian: LDL fills from the addressed byte down into the high end
    // of the register, LDR fills from the addressed byte up into the low end.
    // The pair `ldl $x,7(b); ldr $x,0(b)` therefore loads a full unaligned
    // doubleword - which is exactly what KI's boot self-check does.
    else if (opcode == 0x1A) { // LDL
        const uint32_t addr = GPRU(rs) + static_cast<uint32_t>(simm);
        const int b = addr & 7;
        uint64_t r = GPRDU(rt);
        for (int i = 0; i <= b; ++i) {
            const uint8_t byte = mem_read8(cpu, addr - i);
            const int pos = 7 - i;
            r = (r & ~(0xFFull << (pos * 8))) |
                (static_cast<uint64_t>(byte) << (pos * 8));
        }
        cpu->r[rt] = r;
    }
    else if (opcode == 0x1B) { // LDR
        const uint32_t addr = GPRU(rs) + static_cast<uint32_t>(simm);
        const int b = addr & 7;
        uint64_t r = GPRDU(rt);
        for (int i = 0; i < 8 - b; ++i) {
            const uint8_t byte = mem_read8(cpu, addr + i);
            r = (r & ~(0xFFull << (i * 8))) |
                (static_cast<uint64_t>(byte) << (i * 8));
        }
        cpu->r[rt] = r;
    }
    else if (opcode == 0x27) { // LWU - load word, zero-extended to 64 bits
        cpu->r[rt] = mem_read32(cpu, GPRU(rs) + static_cast<uint32_t>(simm));
    }
    // LL/LLD/SC/SCD: no multiprocessor here, so load-linked is a plain load
    // and store-conditional always succeeds (writes and returns 1).
    else if (opcode == 0x30) { // LL
        cpu->r[rt] = sign_ext32(
            mem_read32(cpu, GPRU(rs) + static_cast<uint32_t>(simm)));
    }
    else if (opcode == 0x34) { // LLD
        const uint32_t a = GPRU(rs) + static_cast<uint32_t>(simm);
        cpu->r[rt] = static_cast<uint64_t>(mem_read32(cpu, a)) |
                     (static_cast<uint64_t>(mem_read32(cpu, a + 4)) << 32);
    }
    else if (opcode == 0x38) { // SC
        mem_write32(cpu, GPRU(rs) + static_cast<uint32_t>(simm),
                    static_cast<uint32_t>(GPRU(rt)));
        if (rt != 0) cpu->r[rt] = 1;
    }
    else if (opcode == 0x3C) { // SCD
        const uint32_t a = GPRU(rs) + static_cast<uint32_t>(simm);
        mem_write32(cpu, a, static_cast<uint32_t>(GPRDU(rt)));
        mem_write32(cpu, a + 4, static_cast<uint32_t>(GPRDU(rt) >> 32));
        if (rt != 0) cpu->r[rt] = 1;
    }
    // Unaligned doubleword stores, byte-wise (little-endian), mirroring LDL/LDR.
    else if (opcode == 0x2C) { // SDL
        const uint32_t addr = GPRU(rs) + static_cast<uint32_t>(simm);
        const int b = addr & 7;
        for (int i = 0; i <= b; ++i)
            mem_write8(cpu, addr - i,
                       static_cast<uint8_t>(GPRDU(rt) >> ((7 - i) * 8)));
    }
    else if (opcode == 0x2D) { // SDR
        const uint32_t addr = GPRU(rs) + static_cast<uint32_t>(simm);
        const int b = addr & 7;
        for (int i = 0; i < 8 - b; ++i)
            mem_write8(cpu, addr + i,
                       static_cast<uint8_t>(GPRDU(rt) >> (i * 8)));
    }
    else {
        static const bool trap = getenv("KI_TRAP_OP") != nullptr;
        if (trap) {
            static uint64_t seen = 0;
            if (opcode < 64 && !(seen & (1ull << opcode))) {
                seen |= 1ull << opcode;
                fprintf(stderr, "[UNIMPL] primary op 0x%02X at pc=0x%08X\n",
                        opcode, cpu->last_pc);
            }
        }
    }

    #undef GPR
    #undef GPRU
    #undef GPRD
    #undef GPRDU

    // Count advances at HALF the cycle rate (MAME: Count = cycles/2), and the
    // odd cycle is carried so it still ticks once every two.
    const uint32_t prev_count = cpu->count;
    cpu->count_frac += cycles;
    cpu->count += cpu->count_frac >> 1;
    cpu->count_frac &= 1;

    // Timer interrupt: fire IP7 exactly once when Count reaches Compare, and
    // only while armed (armed is set when the game writes Compare). This is
    // MAME's compare_armed edge behaviour, not a level that re-asserts every
    // step - which used to storm the handler.
    if (cpu->compare_armed &&
        static_cast<int32_t>(cpu->count - cpu->compare) >= 0 &&
        static_cast<int32_t>(prev_count - cpu->compare) < 0) {
        cpu->compare_armed = false;
        cpu->cause |= (1u << 15);  // IP7: timer interrupt pending
    }

    // Take an interrupt only when MAME's mips3 check_irqs would: a hardware
    // interrupt is pending AND unmasked (CAUSE & SR & 0xFC00 - bits 10..15,
    // the six hardware IPs), interrupts are enabled (IE, SR bit 0), and the
    // CPU is not already at exception OR ERROR level (EXL bit 1, ERL bit 2).
    // The missing ERL test was the bug: reset leaves ERL set, and while the
    // game runs at error level it POLLS the Cause bit directly and installs no
    // vector - so firing interrupts anyway sent it into the zero-filled
    // exception vectors.
    if ((cpu->cause & cpu->sr & 0xFC00) && (cpu->sr & 0x1) &&
        !(cpu->sr & 0x6) &&
        // Don't vector into a handler the game has not installed yet. The
        // general exception slot (BEV=0: 0x80000180, BEV=1: boot ROM) sits in
        // zero-filled RAM until the game writes its ISR there; the R4600
        // count/compare timer is armed early - before that ISR exists - so
        // delivering it would run the CPU off through nop-filled RAM. Once a
        // real handler is present this gate opens on its own.
        mem_read32(cpu, ((cpu->sr >> 22) & 1) ? 0xBFC00380u : 0x80000180u)
            != 0) {
        exception(cpu, 0, 0);  // External interrupt
        // Deliver the VBLANK line (IP3) as a single edge per assertion. It is
        // level-set for a slice of each frame; without clearing it here the
        // handler's ERET would re-take it immediately and the CPU would storm
        // the vector, never returning to the main loop.
        cpu->cause &= ~(1u << 11);
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
