// MIPS R4600 CPU interpreter (MIPS III ISA subset).
//
// Clean-room implementation of the MIPS III instruction set needed
// to boot Killer Instinct. KI runs in 32-bit kernel mode on a
// big-endian R4600, using unmapped kseg0/kseg1 regions so the TLB
// is not required for initial boot.
//
// Instructions implemented:
//   ALU:    ADD/ADDU/SUB/SUBU/AND/OR/XOR/NOR/SLT/SLTU
//   Imm:    ADDI/ADDIU/ANDI/ORI/XORI/LUI
//   Shift:  SLL/SRL/SRA/SLLV/SRLV/SRAV
//   MulDiv: MULT/MULTU/DIV/DIVU/MFHI/MFLO/MTHI/MTLO
//   Load:   LB/LBU/LH/LHU/LW/LWL/LWR
//   Store:  SB/SH/SW/SWL/SWR
//   Branch: BEQ/BNE/BLEZ/BGTZ/BLTZ/BGEZ/BLTZAL/BGEZAL
//   Jump:   J/JAL/JR/JALR
//   Cop0:   MFC0/MTC0
//   System: SYSCALL/BREAK/ERET
//   Trap:   TGE/TGEU/TLT/TLTU/TEQ/TNE
#pragma once

#include <cstdint>
#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mips_cpu mips_cpu;

// Bus callbacks. addr is a physical address (no kseg mapping).
typedef uint32_t (*mips_read32_fn)(void* user, uint32_t addr);
typedef void     (*mips_write32_fn)(void* user, uint32_t addr, uint32_t val);
typedef uint16_t (*mips_read16_fn)(void* user, uint32_t addr);
typedef void     (*mips_write16_fn)(void* user, uint32_t addr, uint16_t val);
typedef uint8_t  (*mips_read8_fn)(void* user, uint32_t addr);
typedef void     (*mips_write8_fn)(void* user, uint32_t addr, uint8_t val);

mips_cpu* mips_create(void* user_data,
                      mips_read32_fn  read32,
                      mips_write32_fn write32,
                      mips_read16_fn  read16,
                      mips_write16_fn write16,
                      mips_read8_fn   read8,
                      mips_write8_fn  write8);

void mips_destroy(mips_cpu* cpu);

// Reset the CPU to its power-on state. PC = 0xBFC00000 (kseg1 reset
// vector), BEV=1, all GPRs zeroed.
void mips_reset(mips_cpu* cpu);

// Execute one instruction. Returns the number of cycles consumed
// (typically 1 for simple ops; multiply/divide take more).
uint32_t mips_step(mips_cpu* cpu);

// Read/write coprocessor 0 registers by index. For the session to
// inject interrupts, read the Count/Compare registers, etc.
uint32_t mips_cop0_read(mips_cpu* cpu, uint8_t reg);
void     mips_cop0_write(mips_cpu* cpu, uint8_t reg, uint32_t val);

// Cause an interrupt (IP[7:0] bits set in Cop0 Cause).
void mips_set_interrupt(mips_cpu* cpu, uint8_t ip_bits);
void mips_clear_interrupt(mips_cpu* cpu, uint8_t ip_bits);

// Last PC that was executed (for debugging).
uint32_t mips_last_pc(mips_cpu* cpu);

// Returns non-zero if the CPU is in a branch delay slot.
int mips_in_delay_slot(mips_cpu* cpu);

#ifdef __cplusplus
}
#endif
