/* m6809f_ops_alu8.c -- 8-bit ALU family for accumulators A and B.
 *
 * Ops: SUB CMP SBC AND BIT EOR ADC OR ADD, in all 4 addressing modes
 * (immediate / direct / indexed / extended), for both A and B.
 *
 * SEMANTIC + CYCLE ORACLE: src/cores/m6809.c (the op_sub/op_add/... helpers and
 * the per-opcode cycle adds). This file mirrors those byte-exactly:
 *
 *  - ADD/ADC set H,N,Z,V,C   (H = half-carry from bit3->bit4)
 *  - SUB/SBC/CMP set N,Z,V,C  (H untouched)
 *  - AND/OR/EOR/BIT set N,Z,V(=0)  (C,H untouched)
 *
 * The flag math is copied verbatim from m6809.c's op_* so the carry/overflow
 * edge cases (esp. SBC/ADC where the OLD carry feeds the C computation) match.
 *
 * --------------------------------------------------------------------------
 * FAMILY-FILE PATTERN (for the agents writing alu16/load/rmw/branch/...):
 *   * A handler has signature  void name(M6809F *c).
 *     The core has ALREADY fetched & consumed the opcode byte; pc points at the
 *     first operand byte. The handler fetches its own operand bytes via the EA
 *     helpers, does the work, sets flags, and adds the instruction's cycles.
 *   * Read operands:
 *       immediate : m6809f_fetch(c)                       (1 byte, pc++)
 *       direct    : m6809f_rd(c, m6809f_ea_direct(c))     (dp:byte)
 *       indexed   : m6809f_rd(c, m6809f_ea_indexed(c))    (postbyte; helper adds its own cycles)
 *       extended  : m6809f_rd(c, m6809f_ea_extended(c))   (16-bit address)
 *   * Cycles: m6809f_step() does NOT add a base cycle, so each handler must add
 *     the FULL count = (oracle base 1) + (oracle per-case add). For indexed the
 *     EA helper adds the addressing-mode cycles on top automatically.
 *   * Flags: CC is the packed byte (CC_E..CC_C). Clear the bits the op writes,
 *     then OR in the new ones; never disturb bits the op leaves alone.
 *   * Register slots: install_<fam>() writes the page-1 (or page-2/3) table
 *     entry for each opcode:  m6809f_p1[0x8B] = a_add_imm;  etc.
 * -------------------------------------------------------------------------- */
#include "m6809f.h"

/* ===== flag-setting ALU primitives (verbatim from m6809.c op_*) ===== */

static inline u8 alu_sub(M6809F *c, u8 dest, u8 src)
{
    u8 res = (u8)(dest - src);
    u8 ci  = (u8)(res ^ dest ^ src);
    int cf = src > dest;
    u8 cc  = (u8)(c->cc & ~(CC_N|CC_Z|CC_V|CC_C));
    if (res & 0x80) cc |= CC_N;
    if (res == 0)   cc |= CC_Z;
    if (cf)         cc |= CC_C;
    if (((ci & 0x80) != 0) ^ cf) cc |= CC_V;
    c->cc = cc;
    return res;
}

static inline void alu_cmp(M6809F *c, u8 dest, u8 src)
{
    (void)alu_sub(c, dest, src);
}

static inline u8 alu_sbc(M6809F *c, u8 dest, u8 src)
{
    int oldc = (c->cc & CC_C) ? 1 : 0;
    u8  res  = (u8)(dest - src - oldc);
    u8  ci   = (u8)(res ^ dest ^ src);
    int cf   = (src + oldc) > dest;            /* m6809.c: src + cc.c > dest */
    u8  cc   = (u8)(c->cc & ~(CC_N|CC_Z|CC_V|CC_C));
    if (res & 0x80) cc |= CC_N;
    if (res == 0)   cc |= CC_Z;
    if (cf)         cc |= CC_C;
    if (((ci & 0x80) != 0) ^ cf) cc |= CC_V;
    c->cc = cc;
    return res;
}

static inline u8 alu_and(M6809F *c, u8 dest, u8 src)
{
    u8 res = (u8)(dest & src);
    u8 cc  = (u8)(c->cc & ~(CC_N|CC_Z|CC_V));   /* V cleared; C,H untouched */
    if (res & 0x80) cc |= CC_N;
    if (res == 0)   cc |= CC_Z;
    c->cc = cc;
    return res;
}

static inline void alu_bit(M6809F *c, u8 dest, u8 src)
{
    (void)alu_and(c, dest, src);
}

static inline u8 alu_eor(M6809F *c, u8 dest, u8 src)
{
    u8 res = (u8)(dest ^ src);
    u8 cc  = (u8)(c->cc & ~(CC_N|CC_Z|CC_V));
    if (res & 0x80) cc |= CC_N;
    if (res == 0)   cc |= CC_Z;
    c->cc = cc;
    return res;
}

static inline u8 alu_or(M6809F *c, u8 dest, u8 src)
{
    u8 res = (u8)(dest | src);
    u8 cc  = (u8)(c->cc & ~(CC_N|CC_Z|CC_V));
    if (res & 0x80) cc |= CC_N;
    if (res == 0)   cc |= CC_Z;
    c->cc = cc;
    return res;
}

static inline u8 alu_adc(M6809F *c, u8 dest, u8 src)
{
    int oldc = (c->cc & CC_C) ? 1 : 0;
    u8  res  = (u8)(dest + src + oldc);
    u8  ci   = (u8)(res ^ dest ^ src);
    int cf   = ((int)res < (int)dest + oldc);  /* m6809.c: res < dest + cc.c */
    u8  cc   = (u8)(c->cc & ~(CC_H|CC_N|CC_Z|CC_V|CC_C));
    if (ci & 0x10)  cc |= CC_H;
    if (res & 0x80) cc |= CC_N;
    if (res == 0)   cc |= CC_Z;
    if (cf)         cc |= CC_C;
    if (((ci & 0x80) != 0) ^ cf) cc |= CC_V;
    c->cc = cc;
    return res;
}

static inline u8 alu_add(M6809F *c, u8 dest, u8 src)
{
    u8  res = (u8)(dest + src);
    u8  ci  = (u8)(res ^ dest ^ src);
    int cf  = (res < dest);
    u8  cc  = (u8)(c->cc & ~(CC_H|CC_N|CC_Z|CC_V|CC_C));
    if (ci & 0x10)  cc |= CC_H;
    if (res & 0x80) cc |= CC_N;
    if (res == 0)   cc |= CC_Z;
    if (cf)         cc |= CC_C;
    if (((ci & 0x80) != 0) ^ cf) cc |= CC_V;
    c->cc = cc;
    return res;
}

/* ===== operand fetch per addressing mode (adds the full base instr cycles) =====
 * Oracle totals = (base 1 from mc6809_step) + (per-case add):
 *   immediate = 2, direct = 4, indexed = 4 (+ ea-mode cycles), extended = 5.  */
static inline u8 rd_imm(M6809F *c){ c->cycles += 2; return m6809f_fetch(c); }
static inline u8 rd_dir(M6809F *c){ c->cycles += 4; return m6809f_rd(c, m6809f_ea_direct(c)); }
static inline u8 rd_idx(M6809F *c){ c->cycles += 4; u16 ea = m6809f_ea_indexed(c); return m6809f_rd(c, ea); }
static inline u8 rd_ext(M6809F *c){ c->cycles += 5; return m6809f_rd(c, m6809f_ea_extended(c)); }

/* ===== handler generators ===== */
/* store-result ops (sub/sbc/and/eor/adc/or/add): write back to accumulator */
#define HS(nm, ACC, MODE, FN) \
    static void nm(M6809F *c){ u8 m = rd_##MODE(c); c->ACC = FN(c, c->ACC, m); }
/* flags-only ops (cmp/bit): no write-back */
#define HV(nm, ACC, MODE, FN) \
    static void nm(M6809F *c){ u8 m = rd_##MODE(c); FN(c, c->ACC, m); }

/* ---- A accumulator ---- */
HS(a_sub_imm, a, imm, alu_sub)  HS(a_sub_dir, a, dir, alu_sub)  HS(a_sub_idx, a, idx, alu_sub)  HS(a_sub_ext, a, ext, alu_sub)
HV(a_cmp_imm, a, imm, alu_cmp)  HV(a_cmp_dir, a, dir, alu_cmp)  HV(a_cmp_idx, a, idx, alu_cmp)  HV(a_cmp_ext, a, ext, alu_cmp)
HS(a_sbc_imm, a, imm, alu_sbc)  HS(a_sbc_dir, a, dir, alu_sbc)  HS(a_sbc_idx, a, idx, alu_sbc)  HS(a_sbc_ext, a, ext, alu_sbc)
HS(a_and_imm, a, imm, alu_and)  HS(a_and_dir, a, dir, alu_and)  HS(a_and_idx, a, idx, alu_and)  HS(a_and_ext, a, ext, alu_and)
HV(a_bit_imm, a, imm, alu_bit)  HV(a_bit_dir, a, dir, alu_bit)  HV(a_bit_idx, a, idx, alu_bit)  HV(a_bit_ext, a, ext, alu_bit)
HS(a_eor_imm, a, imm, alu_eor)  HS(a_eor_dir, a, dir, alu_eor)  HS(a_eor_idx, a, idx, alu_eor)  HS(a_eor_ext, a, ext, alu_eor)
HS(a_adc_imm, a, imm, alu_adc)  HS(a_adc_dir, a, dir, alu_adc)  HS(a_adc_idx, a, idx, alu_adc)  HS(a_adc_ext, a, ext, alu_adc)
HS(a_or_imm,  a, imm, alu_or)   HS(a_or_dir,  a, dir, alu_or)   HS(a_or_idx,  a, idx, alu_or)   HS(a_or_ext,  a, ext, alu_or)
HS(a_add_imm, a, imm, alu_add)  HS(a_add_dir, a, dir, alu_add)  HS(a_add_idx, a, idx, alu_add)  HS(a_add_ext, a, ext, alu_add)

/* ---- B accumulator ---- */
HS(b_sub_imm, b, imm, alu_sub)  HS(b_sub_dir, b, dir, alu_sub)  HS(b_sub_idx, b, idx, alu_sub)  HS(b_sub_ext, b, ext, alu_sub)
HV(b_cmp_imm, b, imm, alu_cmp)  HV(b_cmp_dir, b, dir, alu_cmp)  HV(b_cmp_idx, b, idx, alu_cmp)  HV(b_cmp_ext, b, ext, alu_cmp)
HS(b_sbc_imm, b, imm, alu_sbc)  HS(b_sbc_dir, b, dir, alu_sbc)  HS(b_sbc_idx, b, idx, alu_sbc)  HS(b_sbc_ext, b, ext, alu_sbc)
HS(b_and_imm, b, imm, alu_and)  HS(b_and_dir, b, dir, alu_and)  HS(b_and_idx, b, idx, alu_and)  HS(b_and_ext, b, ext, alu_and)
HV(b_bit_imm, b, imm, alu_bit)  HV(b_bit_dir, b, dir, alu_bit)  HV(b_bit_idx, b, idx, alu_bit)  HV(b_bit_ext, b, ext, alu_bit)
HS(b_eor_imm, b, imm, alu_eor)  HS(b_eor_dir, b, dir, alu_eor)  HS(b_eor_idx, b, idx, alu_eor)  HS(b_eor_ext, b, ext, alu_eor)
HS(b_adc_imm, b, imm, alu_adc)  HS(b_adc_dir, b, dir, alu_adc)  HS(b_adc_idx, b, idx, alu_adc)  HS(b_adc_ext, b, ext, alu_adc)
HS(b_or_imm,  b, imm, alu_or)   HS(b_or_dir,  b, dir, alu_or)   HS(b_or_idx,  b, idx, alu_or)   HS(b_or_ext,  b, ext, alu_or)
HS(b_add_imm, b, imm, alu_add)  HS(b_add_dir, b, dir, alu_add)  HS(b_add_idx, b, idx, alu_add)  HS(b_add_ext, b, ext, alu_add)

void m6809f_install_alu8(void)
{
    /* A accumulator: immediate 0x8x, direct 0x9x, indexed 0xAx, extended 0xBx */
    m6809f_p1[0x80] = a_sub_imm; m6809f_p1[0x90] = a_sub_dir; m6809f_p1[0xA0] = a_sub_idx; m6809f_p1[0xB0] = a_sub_ext;
    m6809f_p1[0x81] = a_cmp_imm; m6809f_p1[0x91] = a_cmp_dir; m6809f_p1[0xA1] = a_cmp_idx; m6809f_p1[0xB1] = a_cmp_ext;
    m6809f_p1[0x82] = a_sbc_imm; m6809f_p1[0x92] = a_sbc_dir; m6809f_p1[0xA2] = a_sbc_idx; m6809f_p1[0xB2] = a_sbc_ext;
    m6809f_p1[0x84] = a_and_imm; m6809f_p1[0x94] = a_and_dir; m6809f_p1[0xA4] = a_and_idx; m6809f_p1[0xB4] = a_and_ext;
    m6809f_p1[0x85] = a_bit_imm; m6809f_p1[0x95] = a_bit_dir; m6809f_p1[0xA5] = a_bit_idx; m6809f_p1[0xB5] = a_bit_ext;
    m6809f_p1[0x88] = a_eor_imm; m6809f_p1[0x98] = a_eor_dir; m6809f_p1[0xA8] = a_eor_idx; m6809f_p1[0xB8] = a_eor_ext;
    m6809f_p1[0x89] = a_adc_imm; m6809f_p1[0x99] = a_adc_dir; m6809f_p1[0xA9] = a_adc_idx; m6809f_p1[0xB9] = a_adc_ext;
    m6809f_p1[0x8A] = a_or_imm;  m6809f_p1[0x9A] = a_or_dir;  m6809f_p1[0xAA] = a_or_idx;  m6809f_p1[0xBA] = a_or_ext;
    m6809f_p1[0x8B] = a_add_imm; m6809f_p1[0x9B] = a_add_dir; m6809f_p1[0xAB] = a_add_idx; m6809f_p1[0xBB] = a_add_ext;

    /* B accumulator: immediate 0xCx, direct 0xDx, indexed 0xEx, extended 0xFx */
    m6809f_p1[0xC0] = b_sub_imm; m6809f_p1[0xD0] = b_sub_dir; m6809f_p1[0xE0] = b_sub_idx; m6809f_p1[0xF0] = b_sub_ext;
    m6809f_p1[0xC1] = b_cmp_imm; m6809f_p1[0xD1] = b_cmp_dir; m6809f_p1[0xE1] = b_cmp_idx; m6809f_p1[0xF1] = b_cmp_ext;
    m6809f_p1[0xC2] = b_sbc_imm; m6809f_p1[0xD2] = b_sbc_dir; m6809f_p1[0xE2] = b_sbc_idx; m6809f_p1[0xF2] = b_sbc_ext;
    m6809f_p1[0xC4] = b_and_imm; m6809f_p1[0xD4] = b_and_dir; m6809f_p1[0xE4] = b_and_idx; m6809f_p1[0xF4] = b_and_ext;
    m6809f_p1[0xC5] = b_bit_imm; m6809f_p1[0xD5] = b_bit_dir; m6809f_p1[0xE5] = b_bit_idx; m6809f_p1[0xF5] = b_bit_ext;
    m6809f_p1[0xC8] = b_eor_imm; m6809f_p1[0xD8] = b_eor_dir; m6809f_p1[0xE8] = b_eor_idx; m6809f_p1[0xF8] = b_eor_ext;
    m6809f_p1[0xC9] = b_adc_imm; m6809f_p1[0xD9] = b_adc_dir; m6809f_p1[0xE9] = b_adc_idx; m6809f_p1[0xF9] = b_adc_ext;
    m6809f_p1[0xCA] = b_or_imm;  m6809f_p1[0xDA] = b_or_dir;  m6809f_p1[0xEA] = b_or_idx;  m6809f_p1[0xFA] = b_or_ext;
    m6809f_p1[0xCB] = b_add_imm; m6809f_p1[0xDB] = b_add_dir; m6809f_p1[0xEB] = b_add_idx; m6809f_p1[0xFB] = b_add_ext;
}
