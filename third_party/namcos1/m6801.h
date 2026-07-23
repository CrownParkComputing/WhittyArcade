/* src/cores/m6801.h
 * Compact HD6301/HD63701 (6801-family) CPU core for the Pac-Land MCU.
 * Callback-driven external memory (like mc6809.h). The HD63701 on-chip RAM,
 * I/O ports and timer are modelled by the machine layer via the read/write
 * callbacks for addresses < 0x20 / the internal RAM window -- the core itself
 * is just the CPU. Clean implementation; MAME m6801.cpp used only as a
 * semantic reference (vectors, flags, 6301 opcodes), not copied.
 */
#ifndef M6801_CORE_H
#define M6801_CORE_H
#include <stdint.h>
#include <stdbool.h>

/* 6801/6301 interrupt vectors (top of memory) */
#define M6801_VEC_SCI   0xfff0
#define M6801_VEC_TOF   0xfff2   /* timer overflow            */
#define M6801_VEC_OCF   0xfff4   /* output compare            */
#define M6801_VEC_ICF   0xfff6   /* input capture             */
#define M6801_VEC_IRQ1  0xfff8   /* external IRQ1 (here: vblank) */
#define M6801_VEC_SWI   0xfffa
#define M6801_VEC_NMI   0xfffc
#define M6801_VEC_RESET 0xfffe

typedef struct m6801 m6801_t;
struct m6801 {
    uint16_t pc, x, sp;
    uint8_t  a, b;                 /* D = A:B */
    uint8_t  cc;                   /* 11HINZVC */
    unsigned long cycles;
    bool     irq1;                 /* external IRQ1 line (level)  */
    bool     wai, halted;
    /* HD63701 on-chip free-running timer (regs 0x08-0x0c) */
    uint16_t frc;                  /* free-running counter        */
    uint16_t ocr;                  /* output-compare register     */
    uint8_t  tcsr;                 /* timer control/status        */
    bool     tcsr_read;            /* for OCF/TOF clear sequence   */
    int      trap_op;              /* nonzero = hit an unimplemented opcode */
    uint16_t trap_pc;
    void    *user;
    uint8_t (*read )(m6801_t *, uint16_t);
    void    (*write)(m6801_t *, uint16_t, uint8_t);
    /* Optional direct-read page table (256 pages x 256 bytes). A non-NULL entry
     * is a base pointer for plain RAM/ROM at that 256-byte page -> reads skip the
     * `read` callback entirely (the hot opcode-fetch + handshake-poll path).
     * NULL -> fall back to the callback (MMIO: timer/ports/inputs). The machine
     * layer fills this after reset; left all-NULL it behaves exactly as before.
     * Writes always go through `write` (side effects). */
    const uint8_t *rdpage[256];
};

/* CC flag bits */
#define M6801_H 0x20
#define M6801_I 0x10
#define M6801_N 0x08
#define M6801_Z 0x04
#define M6801_V 0x02
#define M6801_C 0x01

void m6801_reset(m6801_t *);
int  m6801_step (m6801_t *);   /* run one instruction; returns trap_op (0 = ok) */

#endif
