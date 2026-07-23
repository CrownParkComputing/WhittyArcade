/* m6809f_ops_alu16.c -- 16-bit ALU family.
 *
 * Ops:
 *   page1: ADDD (0xC3/0xD3/0xE3/0xF3), SUBD (0x83/0x93/0xA3/0xB3),
 *          CMPX (0x8C/0x9C/0xAC/0xBC), MUL (0x3D), SEX (0x1D), ABX (0x3A)
 *   page2: CMPD (0x83/0x93/0xA3/0xB3), CMPY (0x8C/0x9C/0xAC/0xBC)
 *   page3: CMPU (0x83/0x93/0xA3/0xB3), CMPS (0x8C/0x9C/0xAC/0xBC)
 *
 * SEMANTIC + CYCLE ORACLE: src/cores/m6809.c (op_sub16/op_add16/op_cmp16 and the
 * per-opcode cycle adds). Flag math copied verbatim:
 *   ADDD/SUBD set N,Z,V,C   CMPx set N,Z,V,C   (16-bit, N = res>0x7FFF)
 *   MUL sets Z and C(=bit7 of result low byte/B)   SEX sets N,Z and clears V
 *   ABX sets no flags.
 *
 * Cycle model (mc6809_step adds NO base in this interp, the oracle adds 1 in
 * mc6809_step before dispatch, and page2()/page3() add NO base of their own):
 * each handler adds the FULL oracle total = 1 (oracle base) + per-case add.
 *   page1: imm=4 dir=6 idx=6(+ea) ext=7
 *   page2 CMPD/CMPY: imm=5 dir=7 idx=7(+ea) ext=7
 *   page3 CMPU/CMPS: imm=5 dir=7 idx=7(+ea) ext=8  (note: oracle ext differs page2 vs page3)
 * Indexed: m6809f_ea_indexed() adds the addressing-mode cycles on top.
 * 16-bit operands are big-endian via m6809f_rd16 / m6809f_fetch16.
 */
#include "m6809f.h"

/* ===== flag-setting 16-bit ALU primitives (verbatim from m6809.c op_*16) ===== */

static inline u16 alu_sub16(M6809F *c, u16 dest, u16 src)
{
    u16 res = (u16)(dest - src);
    u16 ci  = (u16)(res ^ dest ^ src);
    int cf  = src > dest;
    u8  cc  = (u8)(c->cc & ~(CC_N|CC_Z|CC_V|CC_C));
    if (res > 0x7FFF) cc |= CC_N;
    if (res == 0)     cc |= CC_Z;
    if (cf)           cc |= CC_C;
    if (((ci & 0x8000) != 0) ^ cf) cc |= CC_V;
    c->cc = cc;
    return res;
}

static inline void alu_cmp16(M6809F *c, u16 dest, u16 src)
{
    (void)alu_sub16(c, dest, src);
}

static inline u16 alu_add16(M6809F *c, u16 dest, u16 src)
{
    u16 res = (u16)(dest + src);
    u16 ci  = (u16)(res ^ dest ^ src);
    int cf  = (res < dest);
    u8  cc  = (u8)(c->cc & ~(CC_N|CC_Z|CC_V|CC_C));
    if (res > 0x7FFF) cc |= CC_N;
    if (res == 0)     cc |= CC_Z;
    if (cf)           cc |= CC_C;
    if (((ci & 0x8000) != 0) ^ cf) cc |= CC_V;
    c->cc = cc;
    return res;
}

/* ===== operand fetch per addressing mode (big-endian; NO cycles here) =====
 * Cycles are added by each handler so page1/page2/page3 can differ. The indexed
 * EA decoder still adds its own addressing-mode cycles. */
static inline u16 g_imm(M6809F *c){ return m6809f_fetch16(c); }
static inline u16 g_dir(M6809F *c){ return m6809f_rd16(c, m6809f_ea_direct(c)); }
static inline u16 g_idx(M6809F *c){ return m6809f_rd16(c, m6809f_ea_indexed(c)); }
static inline u16 g_ext(M6809F *c){ return m6809f_rd16(c, m6809f_ea_extended(c)); }

/* ===== handler generators ===== */
/* store-to-D ops (ADDD/SUBD) */
#define HD(nm, MODE, CYC, FN) \
    static void nm(M6809F *c){ c->cycles += (CYC); u16 m = g_##MODE(c); m6809f_sd(c, FN(c, m6809f_gd(c), m)); }
/* compare ops (CMPx); REG is the lvalue/expr providing the dest register */
#define HC(nm, REG, MODE, CYC) \
    static void nm(M6809F *c){ c->cycles += (CYC); u16 m = g_##MODE(c); alu_cmp16(c, (REG), m); }

/* ---- page1: ADDD / SUBD / CMPX ---- */
HD(addd_imm, imm, 4, alu_add16)  HD(addd_dir, dir, 6, alu_add16)  HD(addd_idx, idx, 6, alu_add16)  HD(addd_ext, ext, 7, alu_add16)
HD(subd_imm, imm, 4, alu_sub16)  HD(subd_dir, dir, 6, alu_sub16)  HD(subd_idx, idx, 6, alu_sub16)  HD(subd_ext, ext, 7, alu_sub16)
HC(cmpx_imm, c->x, imm, 4)       HC(cmpx_dir, c->x, dir, 6)       HC(cmpx_idx, c->x, idx, 6)       HC(cmpx_ext, c->x, ext, 7)

/* ---- page2: CMPD / CMPY ---- */
HC(cmpd_imm, m6809f_gd(c), imm, 5) HC(cmpd_dir, m6809f_gd(c), dir, 7) HC(cmpd_idx, m6809f_gd(c), idx, 7) HC(cmpd_ext, m6809f_gd(c), ext, 7)
HC(cmpy_imm, c->y, imm, 5)         HC(cmpy_dir, c->y, dir, 7)         HC(cmpy_idx, c->y, idx, 7)         HC(cmpy_ext, c->y, ext, 7)

/* ---- page3: CMPU / CMPS  (note: extended is 8 here, vs 7 on page2) ---- */
HC(cmpu_imm, c->u, imm, 5)         HC(cmpu_dir, c->u, dir, 7)         HC(cmpu_idx, c->u, idx, 7)         HC(cmpu_ext, c->u, ext, 8)
HC(cmps_imm, c->s, imm, 5)         HC(cmps_dir, c->s, dir, 7)         HC(cmps_idx, c->s, idx, 7)         HC(cmps_ext, c->s, ext, 8)

/* ---- inherent ops ---- */
static void op_mul(M6809F *c)       /* 0x3D: D = A*B; Z, C(=bit7 of low byte) ; N,V,H untouched */
{
    c->cycles += 11;
    u16 r = (u16)c->a * (u16)c->b;
    m6809f_sd(c, r);
    u8 cc = (u8)(c->cc & ~(CC_Z|CC_C));
    if (r & 0x0080) cc |= CC_C;
    if (r == 0)     cc |= CC_Z;
    c->cc = cc;
}

static void op_sex(M6809F *c)       /* 0x1D: sign-extend B into A; N,Z from D, V cleared */
{
    c->cycles += 2;
    c->a = (c->b > 0x7F) ? 0xFF : 0x00;
    u16 d = m6809f_gd(c);
    u8  cc = (u8)(c->cc & ~(CC_N|CC_Z|CC_V));
    if (d > 0x7FFF) cc |= CC_N;
    if (d == 0)     cc |= CC_Z;
    c->cc = cc;
}

static void op_abx(M6809F *c)       /* 0x3A: X += B (unsigned); no flags */
{
    c->cycles += 3;
    c->x = (u16)(c->x + (u16)c->b);
}

void m6809f_install_alu16(void)
{
    /* page1 */
    m6809f_p1[0xC3] = addd_imm; m6809f_p1[0xD3] = addd_dir; m6809f_p1[0xE3] = addd_idx; m6809f_p1[0xF3] = addd_ext;
    m6809f_p1[0x83] = subd_imm; m6809f_p1[0x93] = subd_dir; m6809f_p1[0xA3] = subd_idx; m6809f_p1[0xB3] = subd_ext;
    m6809f_p1[0x8C] = cmpx_imm; m6809f_p1[0x9C] = cmpx_dir; m6809f_p1[0xAC] = cmpx_idx; m6809f_p1[0xBC] = cmpx_ext;
    m6809f_p1[0x3D] = op_mul;
    m6809f_p1[0x1D] = op_sex;
    m6809f_p1[0x3A] = op_abx;

    /* page2 (0x10 prefix) */
    m6809f_p2[0x83] = cmpd_imm; m6809f_p2[0x93] = cmpd_dir; m6809f_p2[0xA3] = cmpd_idx; m6809f_p2[0xB3] = cmpd_ext;
    m6809f_p2[0x8C] = cmpy_imm; m6809f_p2[0x9C] = cmpy_dir; m6809f_p2[0xAC] = cmpy_idx; m6809f_p2[0xBC] = cmpy_ext;

    /* page3 (0x11 prefix) */
    m6809f_p3[0x83] = cmpu_imm; m6809f_p3[0x93] = cmpu_dir; m6809f_p3[0xA3] = cmpu_idx; m6809f_p3[0xB3] = cmpu_ext;
    m6809f_p3[0x8C] = cmps_imm; m6809f_p3[0x9C] = cmps_dir; m6809f_p3[0xAC] = cmps_idx; m6809f_p3[0xBC] = cmps_ext;
}
