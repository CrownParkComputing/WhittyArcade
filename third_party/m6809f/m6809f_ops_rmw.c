/* m6809f_ops_rmw.c -- read-modify-write / unary family (page 1).
 *
 * Ops (low nibble of each column):
 *   0 NEG  3 COM  4 LSR  6 ROR  7 ASR  8 LSL/ASL  9 ROL
 *   a DEC  c INC  d TST  e JMP  f CLR
 * Modes (high nibble):
 *   0x00-0x0F DIRECT mem, 0x40-0x4F inherent A, 0x50-0x5F inherent B,
 *   0x60-0x6F INDEXED mem, 0x70-0x7F EXTENDED mem.
 *
 * SEMANTIC + CYCLE ORACLE: src/cores/m6809.c (op_neg/com/lsr/ror/asr/lsl/rol/
 * dec/inc/tst/clr + the per-opcode cycle adds). Flag math copied verbatim.
 *
 * Cycles: m6809f_step() adds no base cycle, so each handler adds the FULL total
 * = (oracle base 1 from mc6809_step) + (oracle per-case add). For indexed, the
 * shared m6809f_ea_indexed() helper adds the addressing-mode cycles on top.
 *   direct  RMW/CLR/TST = 6, direct  JMP = 4
 *   inherent (A/B)      = 2
 *   indexed RMW/CLR/TST = 6, indexed JMP = 4  (+ ea-mode cycles)
 *   extended RMW/CLR/TST= 7, extended JMP = 5
 */
#include "m6809f.h"

/* ===== flag-setting unary primitives (verbatim from m6809.c op_*) ===== */

static inline u8 rmw_neg(M6809F *c, u8 src)
{
    u8 res = (u8)(-src);
    u8 cc  = (u8)(c->cc & ~(CC_N|CC_Z|CC_V|CC_C));
    if (res > 0x7F)   cc |= CC_N;
    if (res == 0x00)  cc |= CC_Z;
    if (src == 0x80)  cc |= CC_V;
    if (src != 0x00)  cc |= CC_C;
    c->cc = cc;
    return res;
}

static inline u8 rmw_com(M6809F *c, u8 src)
{
    u8 res = (u8)(~src);
    u8 cc  = (u8)(c->cc & ~(CC_N|CC_Z|CC_V|CC_C));
    if (res > 0x7F)   cc |= CC_N;
    if (res == 0x00)  cc |= CC_Z;
    cc |= CC_C;                                 /* C = true, V = false */
    c->cc = cc;
    return res;
}

static inline u8 rmw_lsr(M6809F *c, u8 src)
{
    u8 res = (u8)(src >> 1);
    u8 cc  = (u8)(c->cc & ~(CC_N|CC_Z|CC_C));   /* N = false; V untouched */
    if (res == 0x00)  cc |= CC_Z;
    if (src & 0x01)   cc |= CC_C;
    c->cc = cc;
    return res;
}

static inline u8 rmw_ror(M6809F *c, u8 src)
{
    u8 res = (u8)(src >> 1);
    res |= (c->cc & CC_C) ? 0x80 : 0x00;        /* old carry into bit7 */
    u8 cc = (u8)(c->cc & ~(CC_N|CC_Z|CC_C));    /* V untouched */
    if (res > 0x7F)   cc |= CC_N;
    if (res == 0x00)  cc |= CC_Z;
    if (src & 0x01)   cc |= CC_C;
    c->cc = cc;
    return res;
}

static inline u8 rmw_asr(M6809F *c, u8 src)
{
    u8 res = (u8)(src >> 1);
    res |= (src > 0x7F) ? 0x80 : 0x00;          /* sign extend */
    u8 cc = (u8)(c->cc & ~(CC_N|CC_Z|CC_C));    /* V untouched */
    if (res > 0x7F)   cc |= CC_N;
    if (res == 0x00)  cc |= CC_Z;
    if (src & 0x01)   cc |= CC_C;
    c->cc = cc;
    return res;
}

static inline u8 rmw_lsl(M6809F *c, u8 src)
{
    u8 res = (u8)(src << 1);
    u8 cc  = (u8)(c->cc & ~(CC_N|CC_Z|CC_V|CC_C));
    if (res > 0x7F)            cc |= CC_N;
    if (res == 0x00)           cc |= CC_Z;
    if ((src ^ res) & 0x80)    cc |= CC_V;
    if (src > 0x7F)            cc |= CC_C;
    c->cc = cc;
    return res;
}

static inline u8 rmw_rol(M6809F *c, u8 src)
{
    u8 res = (u8)(src << 1);
    res |= (c->cc & CC_C) ? 0x01 : 0x00;        /* old carry into bit0 */
    u8 cc = (u8)(c->cc & ~(CC_N|CC_Z|CC_V|CC_C));
    if (res > 0x7F)            cc |= CC_N;
    if (res == 0x00)           cc |= CC_Z;
    if ((src ^ res) & 0x80)    cc |= CC_V;
    if (src > 0x7F)            cc |= CC_C;
    c->cc = cc;
    return res;
}

static inline u8 rmw_dec(M6809F *c, u8 src)
{
    u8 res = (u8)(src - 1);
    u8 cc  = (u8)(c->cc & ~(CC_N|CC_Z|CC_V));   /* C untouched */
    if (res > 0x7F)   cc |= CC_N;
    if (res == 0x00)  cc |= CC_Z;
    if (src == 0x80)  cc |= CC_V;
    c->cc = cc;
    return res;
}

static inline u8 rmw_inc(M6809F *c, u8 src)
{
    u8 res = (u8)(src + 1);
    u8 cc  = (u8)(c->cc & ~(CC_N|CC_Z|CC_V));   /* C untouched */
    if (res > 0x7F)   cc |= CC_N;
    if (res == 0x00)  cc |= CC_Z;
    if (src == 0x7F)  cc |= CC_V;
    c->cc = cc;
    return res;
}

static inline void rmw_tst(M6809F *c, u8 src)
{
    u8 cc = (u8)(c->cc & ~(CC_N|CC_Z|CC_V));    /* C untouched; V = false */
    if (src > 0x7F)   cc |= CC_N;
    if (src == 0x00)  cc |= CC_Z;
    c->cc = cc;
}

static inline u8 rmw_clr(M6809F *c)
{
    c->cc = (u8)(c->cc & ~(CC_N|CC_V|CC_C)) | CC_Z;  /* N=0 V=0 C=0 Z=1 */
    return 0;
}

/* ===== handler generators ===== */

/* memory RMW: read EA, op, write back. CYC = full instruction cycles. */
#define MEMOP(nm, EA, CYC, FN) \
    static void nm(M6809F *c){ c->cycles += CYC; u16 ea = EA(c); m6809f_wr(c, ea, FN(c, m6809f_rd(c, ea))); }

#define MEM3(op, FN) \
    MEMOP(d_##op,   m6809f_ea_direct,   6, FN) \
    MEMOP(idx_##op, m6809f_ea_indexed,  6, FN) \
    MEMOP(ext_##op, m6809f_ea_extended, 7, FN)

/* inherent RMW on accumulator */
#define INH2(op, FN) \
    static void ina_##op(M6809F *c){ c->cycles += 2; c->a = FN(c, c->a); } \
    static void inb_##op(M6809F *c){ c->cycles += 2; c->b = FN(c, c->b); }

MEM3(neg, rmw_neg)  INH2(neg, rmw_neg)
MEM3(com, rmw_com)  INH2(com, rmw_com)
MEM3(lsr, rmw_lsr)  INH2(lsr, rmw_lsr)
MEM3(ror, rmw_ror)  INH2(ror, rmw_ror)
MEM3(asr, rmw_asr)  INH2(asr, rmw_asr)
MEM3(lsl, rmw_lsl)  INH2(lsl, rmw_lsl)
MEM3(rol, rmw_rol)  INH2(rol, rmw_rol)
MEM3(dec, rmw_dec)  INH2(dec, rmw_dec)
MEM3(inc, rmw_inc)  INH2(inc, rmw_inc)

/* TST: flags only, no write-back (oracle still reads memory) */
#define MTST(nm, EA, CYC) \
    static void nm(M6809F *c){ c->cycles += CYC; u16 ea = EA(c); rmw_tst(c, m6809f_rd(c, ea)); }
MTST(d_tst,   m6809f_ea_direct,   6)
MTST(idx_tst, m6809f_ea_indexed,  6)
MTST(ext_tst, m6809f_ea_extended, 7)
static void ina_tst(M6809F *c){ c->cycles += 2; rmw_tst(c, c->a); }
static void inb_tst(M6809F *c){ c->cycles += 2; rmw_tst(c, c->b); }

/* CLR: oracle reads EA then writes 0 */
#define MCLR(nm, EA, CYC) \
    static void nm(M6809F *c){ c->cycles += CYC; u16 ea = EA(c); (void)m6809f_rd(c, ea); m6809f_wr(c, ea, rmw_clr(c)); }
MCLR(d_clr,   m6809f_ea_direct,   6)
MCLR(idx_clr, m6809f_ea_indexed,  6)
MCLR(ext_clr, m6809f_ea_extended, 7)
static void ina_clr(M6809F *c){ c->cycles += 2; c->a = rmw_clr(c); }
static void inb_clr(M6809F *c){ c->cycles += 2; c->b = rmw_clr(c); }

/* JMP: pc = EA, no flags (memory modes only) */
#define MJMP(nm, EA, CYC) \
    static void nm(M6809F *c){ c->cycles += CYC; c->pc = EA(c); }
MJMP(d_jmp,   m6809f_ea_direct,   4)
MJMP(idx_jmp, m6809f_ea_indexed,  4)
MJMP(ext_jmp, m6809f_ea_extended, 5)

void m6809f_install_rmw(void)
{
    /* ---- DIRECT 0x00-0x0F ---- */
    m6809f_p1[0x00] = d_neg; m6809f_p1[0x03] = d_com; m6809f_p1[0x04] = d_lsr;
    m6809f_p1[0x06] = d_ror; m6809f_p1[0x07] = d_asr; m6809f_p1[0x08] = d_lsl;
    m6809f_p1[0x09] = d_rol; m6809f_p1[0x0A] = d_dec; m6809f_p1[0x0C] = d_inc;
    m6809f_p1[0x0D] = d_tst; m6809f_p1[0x0E] = d_jmp; m6809f_p1[0x0F] = d_clr;

    /* ---- INHERENT A 0x40-0x4F (no 0x41/0x42/0x45/0x4B/0x4E) ---- */
    m6809f_p1[0x40] = ina_neg; m6809f_p1[0x43] = ina_com; m6809f_p1[0x44] = ina_lsr;
    m6809f_p1[0x46] = ina_ror; m6809f_p1[0x47] = ina_asr; m6809f_p1[0x48] = ina_lsl;
    m6809f_p1[0x49] = ina_rol; m6809f_p1[0x4A] = ina_dec; m6809f_p1[0x4C] = ina_inc;
    m6809f_p1[0x4D] = ina_tst; m6809f_p1[0x4F] = ina_clr;

    /* ---- INHERENT B 0x50-0x5F ---- */
    m6809f_p1[0x50] = inb_neg; m6809f_p1[0x53] = inb_com; m6809f_p1[0x54] = inb_lsr;
    m6809f_p1[0x56] = inb_ror; m6809f_p1[0x57] = inb_asr; m6809f_p1[0x58] = inb_lsl;
    m6809f_p1[0x59] = inb_rol; m6809f_p1[0x5A] = inb_dec; m6809f_p1[0x5C] = inb_inc;
    m6809f_p1[0x5D] = inb_tst; m6809f_p1[0x5F] = inb_clr;

    /* ---- INDEXED 0x60-0x6F ---- */
    m6809f_p1[0x60] = idx_neg; m6809f_p1[0x63] = idx_com; m6809f_p1[0x64] = idx_lsr;
    m6809f_p1[0x66] = idx_ror; m6809f_p1[0x67] = idx_asr; m6809f_p1[0x68] = idx_lsl;
    m6809f_p1[0x69] = idx_rol; m6809f_p1[0x6A] = idx_dec; m6809f_p1[0x6C] = idx_inc;
    m6809f_p1[0x6D] = idx_tst; m6809f_p1[0x6E] = idx_jmp; m6809f_p1[0x6F] = idx_clr;

    /* ---- EXTENDED 0x70-0x7F ---- */
    m6809f_p1[0x70] = ext_neg; m6809f_p1[0x73] = ext_com; m6809f_p1[0x74] = ext_lsr;
    m6809f_p1[0x76] = ext_ror; m6809f_p1[0x77] = ext_asr; m6809f_p1[0x78] = ext_lsl;
    m6809f_p1[0x79] = ext_rol; m6809f_p1[0x7A] = ext_dec; m6809f_p1[0x7C] = ext_inc;
    m6809f_p1[0x7D] = ext_tst; m6809f_p1[0x7E] = ext_jmp; m6809f_p1[0x7F] = ext_clr;
}
