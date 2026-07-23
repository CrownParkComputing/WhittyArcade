/* m6809f.h -- FAST hand-optimized MC6809 interpreter for native m68k (gcc, NO LLVM).
 * Drop-in replacement for the Rust transcode: same flat-mem + io-write HAL model as
 * pl_native_rust.c, but a plain-C interpreter that m68k-amigaos-gcc compiles reliably
 * (no LLVM-m68k codegen hazards). SEMANTIC ORACLE: src/cores/mc6809.c (byte-exact).
 *
 * Architecture for SPEED + parallel development:
 *  - State is a compact struct; D = A:B (A is MSB), the four 16-bit index/stack regs
 *    and PC are native u16.
 *  - CC is a packed byte in architectural push/pull order (E F H I N Z V C, bit7..bit0)
 *    so RTI/PSH/PUL/TFR are trivial; helpers set N/Z/V/C without per-flag branches.
 *  - Dispatch is a function-pointer table: page1[256], plus page2 (0x10 prefix) and
 *    page3 (0x11 prefix) sub-tables. Each OPCODE FAMILY lives in its own file and
 *    installs its handlers via an install_*() fn (no shared-file edit conflicts).
 *  - Indexed addressing is one shared ea_indexed(); inherent/imm/direct/extended EAs
 *    are tiny inline helpers. Memory fast-path reads/writes the flat mem[] inline.
 *
 * A handler reads its own operand bytes via the pc, does the work, sets flags, and adds
 * the instruction's cycle count. The core fetches the opcode and dispatches.
 */
#ifndef M6809F_H
#define M6809F_H
#include <stdint.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;

struct M6809F;
typedef void (*m6809f_op)(struct M6809F *c);

typedef struct M6809F {
    u8  a, b;              /* D = (A<<8)|B */
    u8  dp;                /* direct page  */
    u8  cc;                /* E F H I N Z V C (bit7..bit0)            */
    u16 x, y, u, s, pc;
    u32 cycles;            /* total elapsed 6809 cycles               */
    /* control / interrupt lines (set by the HAL each frame) */
    u8  irq, firq, nmi, nmi_armed, cwai, sync, stop;
    u16 badpc;            /* set with stop when an illegal op is hit  */
    u8 *mem;             /* flat 64K view; HAL keeps banked window + read-only regions correct */
    void *userdata;      /* board instance supplied to the I/O callback */
    u16 writable_limit;  /* exclusive end of directly writable RAM */
    /* control-register WRITE side-effects (scroll/bank/irq-mask/mcu/cus30) -> HAL */
    void (*iowrite)(struct M6809F *c, u16 addr, u8 val);
} M6809F;

/* ---- CC flag bits (architectural order, == mc6809_cctobyte / the prelude F_*) ---- */
#define CC_C 0x01
#define CC_V 0x02
#define CC_Z 0x04
#define CC_N 0x08
#define CC_I 0x10
#define CC_H 0x20
#define CC_F 0x40
#define CC_E 0x80

/* ---- memory: flat fast-path read; write splits RAM store + io side-effects ----
 * Reads come straight from the HAL-maintained flat view (RAM, the live banked ROM
 * window at 0x4000-0x5fff, CUS30 at 0x6800, watchdog 0x7800->0xff, fixed ROM, etc.).
 * Writes store the RAM/CUS30 bytes and ALWAYS notify the HAL hook for control regs. */
#ifdef GAPLUS_DBG
extern unsigned g_rdcount[16];
extern unsigned char g_rdlast[16];
#endif
static inline u8  m6809f_rd (M6809F *c, u16 a) {
#ifdef GAPLUS_DBG
    if (a >= 0x6800 && a < 0x6810) { g_rdcount[a-0x6800]++; g_rdlast[a-0x6800]=c->mem[a]; }
#endif
    return c->mem[a];
}
static inline u16 m6809f_rd16(M6809F *c, u16 a)          { return ((u16)c->mem[a] << 8) | c->mem[(u16)(a+1)]; }
#ifdef GAPLUS_DBG
extern void wr_idx_log(u16 a, u8 v, u16 pc);
#endif
static inline void m6809f_wr(M6809F *c, u16 a, u8 v) {
#ifdef GAPLUS_DBG
    if (a==0x0070 || a==0x1070) wr_idx_log(a, v, c->pc);
#endif
    if (a < c->writable_limit) c->mem[a] = v;
    if (c->iowrite) c->iowrite(c, a, v);    /* I/O 0x6800+, irq/sreset/starfield control, ROM-overlay writes */
}
static inline void m6809f_wr16(M6809F *c, u16 a, u16 v) {
    m6809f_wr(c, a, (u8)(v >> 8));
    m6809f_wr(c, (u16)(a+1), (u8)v);
}

/* ---- D accessor ---- */
static inline u16 m6809f_gd(M6809F *c)        { return ((u16)c->a << 8) | c->b; }
static inline void m6809f_sd(M6809F *c, u16 v){ c->a = (u8)(v >> 8); c->b = (u8)v; }

/* ---- operand-fetch helpers (advance pc) ---- */
static inline u8  m6809f_fetch (M6809F *c)    { return c->mem[c->pc++]; }
static inline u16 m6809f_fetch16(M6809F *c)   { u16 v = m6809f_rd16(c, c->pc); c->pc += 2; return v; }

/* ---- flag helpers (branchless-ish; hand-optimize per family as needed) ---- */
static inline void m6809f_setf(M6809F *c, u8 m, int cond){ if (cond) c->cc |= m; else c->cc &= (u8)~m; }
static inline void m6809f_nz8 (M6809F *c, u8 r){ c->cc = (c->cc & ~(CC_N|CC_Z)) | (r ? (r & 0x80 ? CC_N : 0) : CC_Z); }
static inline void m6809f_nz16(M6809F *c, u16 r){ c->cc = (c->cc & ~(CC_N|CC_Z)) | (r ? (r & 0x8000 ? CC_N : 0) : CC_Z); }

/* ---- effective-address helpers ---- */
u16 m6809f_ea_indexed(M6809F *c);                       /* reads postbyte, all index modes + auto inc/dec */
static inline u16 m6809f_ea_direct(M6809F *c)  { return ((u16)c->dp << 8) | m6809f_fetch(c); }
static inline u16 m6809f_ea_extended(M6809F *c){ return m6809f_fetch16(c); }

/* ---- dispatch tables (filled by the family install_*() fns) ---- */
extern m6809f_op m6809f_p1[256];   /* page 1 (direct opcodes)        */
extern m6809f_op m6809f_p2[256];   /* page 2 (after 0x10 prefix)     */
extern m6809f_op m6809f_p3[256];   /* page 3 (after 0x11 prefix)     */

/* ---- core API ---- */
void m6809f_reset(M6809F *c);      /* pc=[fffe], cc=F|I, dp=0, etc.  */
void m6809f_step (M6809F *c);      /* execute ONE instruction        */
void m6809f_irq  (M6809F *c);      /* take a pending IRQ if unmasked  */

/* families register their handlers here (called once by m6809f_init/reset) */
void m6809f_install_alu8  (void);
void m6809f_install_alu16 (void);
void m6809f_install_load  (void);
void m6809f_install_rmw    (void);
void m6809f_install_branch(void);
void m6809f_install_stack (void);
void m6809f_install_misc  (void);
void m6809f_install_page2 (void);
void m6809f_install_page3 (void);

#endif /* M6809F_H */
