/* m6809f_ops_load.c -- LOAD / STORE family (LD/ST for A,B,D,X,Y,U,S).
 *
 * SEMANTIC + CYCLE ORACLE: src/cores/m6809.c (op_ldst / op_ldst16 + per-opcode
 * cycle adds). Both LD and ST set flags from the value moved:
 *   N = (val & sign-bit) != 0,  Z = (val == 0),  V = 0;  C,H untouched.
 *   - LD: flags from the value LOADED into the register.
 *   - ST: flags from the value STORED (i.e. the register's current contents).
 *
 * D is the A:B pair (m6809f_gd / m6809f_sd). 16-bit transfers are big-endian
 * (m6809f_rd16 / m6809f_wr16). LDS arms NMI (matches the oracle 0x10 0xCE/.. ).
 *
 * Cycle convention (m6809f adds the FULL count; no base cycle in the core):
 *   page1 8-bit  LD/ST : imm 2, dir 4, idx 4(+ea), ext 5
 *   page1 16-bit LD/ST : imm 3, dir 5, idx 5(+ea), ext 6
 *   page2 16-bit LD/ST : imm 4, dir 6, idx 6(+ea), ext 7   (+1 for the 0x10 prefix)
 * The indexed-EA helper (m6809f_ea_indexed) adds its own addressing-mode cycles.
 */
#include "m6809f.h"

/* ===== flag setters (verbatim semantics of op_ldst / op_ldst16) ===== */
static inline void ld_nz8(M6809F *c, u8 v)
{
    u8 cc = (u8)(c->cc & ~(CC_N|CC_Z|CC_V));   /* V cleared; C,H untouched */
    if (v & 0x80) cc |= CC_N;
    if (v == 0)   cc |= CC_Z;
    c->cc = cc;
}
static inline void ld_nz16(M6809F *c, u16 v)
{
    u8 cc = (u8)(c->cc & ~(CC_N|CC_Z|CC_V));
    if (v & 0x8000) cc |= CC_N;
    if (v == 0)     cc |= CC_Z;
    c->cc = cc;
}

/* ===== operand fetch / EA helpers (each adds the FULL instruction cycles) ===== */
/* 8-bit value reads (LD) */
static inline u8 rd8_imm(M6809F *c){ c->cycles += 2; return m6809f_fetch(c); }
static inline u8 rd8_dir(M6809F *c){ c->cycles += 4; return m6809f_rd(c, m6809f_ea_direct(c)); }
static inline u8 rd8_idx(M6809F *c){ c->cycles += 4; return m6809f_rd(c, m6809f_ea_indexed(c)); }
static inline u8 rd8_ext(M6809F *c){ c->cycles += 5; return m6809f_rd(c, m6809f_ea_extended(c)); }
/* 8-bit store EAs (ST) */
static inline u16 ea8_dir(M6809F *c){ c->cycles += 4; return m6809f_ea_direct(c); }
static inline u16 ea8_idx(M6809F *c){ c->cycles += 4; return m6809f_ea_indexed(c); }
static inline u16 ea8_ext(M6809F *c){ c->cycles += 5; return m6809f_ea_extended(c); }

/* 16-bit value reads (LD), page1 */
static inline u16 rd16_imm(M6809F *c){ c->cycles += 3; return m6809f_fetch16(c); }
static inline u16 rd16_dir(M6809F *c){ c->cycles += 5; return m6809f_rd16(c, m6809f_ea_direct(c)); }
static inline u16 rd16_idx(M6809F *c){ c->cycles += 5; return m6809f_rd16(c, m6809f_ea_indexed(c)); }
static inline u16 rd16_ext(M6809F *c){ c->cycles += 6; return m6809f_rd16(c, m6809f_ea_extended(c)); }
/* 16-bit store EAs (ST), page1 */
static inline u16 ea16_dir(M6809F *c){ c->cycles += 5; return m6809f_ea_direct(c); }
static inline u16 ea16_idx(M6809F *c){ c->cycles += 5; return m6809f_ea_indexed(c); }
static inline u16 ea16_ext(M6809F *c){ c->cycles += 6; return m6809f_ea_extended(c); }
/* STD is asymmetric in the oracle: direct/extended cost 7, indexed 5(+ea). */
static inline u16 eaStd_dir(M6809F *c){ c->cycles += 7; return m6809f_ea_direct(c); }
static inline u16 eaStd_ext(M6809F *c){ c->cycles += 7; return m6809f_ea_extended(c); }

/* 16-bit value reads (LD), page2 (+1 cycle for the 0x10 prefix) */
static inline u16 rd16p2_imm(M6809F *c){ c->cycles += 4; return m6809f_fetch16(c); }
static inline u16 rd16p2_dir(M6809F *c){ c->cycles += 6; return m6809f_rd16(c, m6809f_ea_direct(c)); }
static inline u16 rd16p2_idx(M6809F *c){ c->cycles += 6; return m6809f_rd16(c, m6809f_ea_indexed(c)); }
static inline u16 rd16p2_ext(M6809F *c){ c->cycles += 7; return m6809f_rd16(c, m6809f_ea_extended(c)); }
/* 16-bit store EAs (ST), page2 */
static inline u16 ea16p2_dir(M6809F *c){ c->cycles += 6; return m6809f_ea_direct(c); }
static inline u16 ea16p2_idx(M6809F *c){ c->cycles += 6; return m6809f_ea_indexed(c); }
static inline u16 ea16p2_ext(M6809F *c){ c->cycles += 7; return m6809f_ea_extended(c); }

/* ===== handler generators ===== */
/* 8-bit load into accumulator ACC; 8-bit store of ACC */
#define LD8(nm, ACC, RD) \
    static void nm(M6809F *c){ c->ACC = RD(c); ld_nz8(c, c->ACC); }
#define ST8(nm, ACC, EA) \
    static void nm(M6809F *c){ u16 ea = EA(c); m6809f_wr(c, ea, c->ACC); ld_nz8(c, c->ACC); }

/* 16-bit load/store of a native u16 register field (x/y/u/s) */
#define LD16(nm, REG, RD) \
    static void nm(M6809F *c){ c->REG = RD(c); ld_nz16(c, c->REG); }
#define ST16(nm, REG, EA) \
    static void nm(M6809F *c){ u16 ea = EA(c); m6809f_wr16(c, ea, c->REG); ld_nz16(c, c->REG); }

/* LDS variants also arm NMI (mirrors the oracle) */
#define LDS16(nm, RD) \
    static void nm(M6809F *c){ c->s = RD(c); ld_nz16(c, c->s); c->nmi_armed = 1; }

/* 16-bit load/store of D = A:B */
#define LDD16(nm, RD) \
    static void nm(M6809F *c){ u16 v = RD(c); m6809f_sd(c, v); ld_nz16(c, v); }
#define STD16(nm, EA) \
    static void nm(M6809F *c){ u16 ea = EA(c); u16 v = m6809f_gd(c); m6809f_wr16(c, ea, v); ld_nz16(c, v); }

/* ---- LDA / STA (A) ---- */
LD8(lda_imm, a, rd8_imm)  LD8(lda_dir, a, rd8_dir)  LD8(lda_idx, a, rd8_idx)  LD8(lda_ext, a, rd8_ext)
                          ST8(sta_dir, a, ea8_dir)  ST8(sta_idx, a, ea8_idx)  ST8(sta_ext, a, ea8_ext)
/* ---- LDB / STB (B) ---- */
LD8(ldb_imm, b, rd8_imm)  LD8(ldb_dir, b, rd8_dir)  LD8(ldb_idx, b, rd8_idx)  LD8(ldb_ext, b, rd8_ext)
                          ST8(stb_dir, b, ea8_dir)  ST8(stb_idx, b, ea8_idx)  ST8(stb_ext, b, ea8_ext)

/* ---- LDD / STD (D = A:B) ---- */
LDD16(ldd_imm, rd16_imm)  LDD16(ldd_dir, rd16_dir)  LDD16(ldd_idx, rd16_idx)  LDD16(ldd_ext, rd16_ext)
                          STD16(std_dir, eaStd_dir)  STD16(std_idx, ea16_idx)  STD16(std_ext, eaStd_ext)
/* ---- LDX / STX (X) ---- */
LD16(ldx_imm, x, rd16_imm)  LD16(ldx_dir, x, rd16_dir)  LD16(ldx_idx, x, rd16_idx)  LD16(ldx_ext, x, rd16_ext)
                            ST16(stx_dir, x, ea16_dir)  ST16(stx_idx, x, ea16_idx)  ST16(stx_ext, x, ea16_ext)
/* ---- LDU / STU (U) ---- */
LD16(ldu_imm, u, rd16_imm)  LD16(ldu_dir, u, rd16_dir)  LD16(ldu_idx, u, rd16_idx)  LD16(ldu_ext, u, rd16_ext)
                            ST16(stu_dir, u, ea16_dir)  ST16(stu_idx, u, ea16_idx)  ST16(stu_ext, u, ea16_ext)

/* ---- page2: LDY / STY (Y) ----
 * NB: per the oracle (m6809.c case 0xBE/0xBF) LDY/STY EXTENDED are 6 cycles (no page-2
 * prefix penalty), unlike LDS/STS ext = 7 -- a real oracle asymmetry. So ext uses the
 * page-1 (+6) helpers; imm/dir/idx keep the page-2 (+1) costs. */
LD16(ldy_imm, y, rd16p2_imm)  LD16(ldy_dir, y, rd16p2_dir)  LD16(ldy_idx, y, rd16p2_idx)  LD16(ldy_ext, y, rd16_ext)
                              ST16(sty_dir, y, ea16p2_dir)  ST16(sty_idx, y, ea16p2_idx)  ST16(sty_ext, y, ea16_ext)
/* ---- page2: LDS / STS (S) ---- */
LDS16(lds_imm, rd16p2_imm)  LDS16(lds_dir, rd16p2_dir)  LDS16(lds_idx, rd16p2_idx)  LDS16(lds_ext, rd16p2_ext)
                            ST16(sts_dir, s, ea16p2_dir)  ST16(sts_idx, s, ea16p2_idx)  ST16(sts_ext, s, ea16p2_ext)

void m6809f_install_load(void)
{
    /* ---- page 1, 8-bit ---- */
    m6809f_p1[0x86] = lda_imm; m6809f_p1[0x96] = lda_dir; m6809f_p1[0xA6] = lda_idx; m6809f_p1[0xB6] = lda_ext;
    m6809f_p1[0xC6] = ldb_imm; m6809f_p1[0xD6] = ldb_dir; m6809f_p1[0xE6] = ldb_idx; m6809f_p1[0xF6] = ldb_ext;
    m6809f_p1[0x97] = sta_dir; m6809f_p1[0xA7] = sta_idx; m6809f_p1[0xB7] = sta_ext;
    m6809f_p1[0xD7] = stb_dir; m6809f_p1[0xE7] = stb_idx; m6809f_p1[0xF7] = stb_ext;

    /* ---- page 1, 16-bit ---- */
    m6809f_p1[0xCC] = ldd_imm; m6809f_p1[0xDC] = ldd_dir; m6809f_p1[0xEC] = ldd_idx; m6809f_p1[0xFC] = ldd_ext;
    m6809f_p1[0xDD] = std_dir; m6809f_p1[0xED] = std_idx; m6809f_p1[0xFD] = std_ext;
    m6809f_p1[0x8E] = ldx_imm; m6809f_p1[0x9E] = ldx_dir; m6809f_p1[0xAE] = ldx_idx; m6809f_p1[0xBE] = ldx_ext;
    m6809f_p1[0x9F] = stx_dir; m6809f_p1[0xAF] = stx_idx; m6809f_p1[0xBF] = stx_ext;
    m6809f_p1[0xCE] = ldu_imm; m6809f_p1[0xDE] = ldu_dir; m6809f_p1[0xEE] = ldu_idx; m6809f_p1[0xFE] = ldu_ext;
    m6809f_p1[0xDF] = stu_dir; m6809f_p1[0xEF] = stu_idx; m6809f_p1[0xFF] = stu_ext;

    /* ---- page 2 (0x10 prefix), 16-bit ---- */
    m6809f_p2[0x8E] = ldy_imm; m6809f_p2[0x9E] = ldy_dir; m6809f_p2[0xAE] = ldy_idx; m6809f_p2[0xBE] = ldy_ext;
    m6809f_p2[0x9F] = sty_dir; m6809f_p2[0xAF] = sty_idx; m6809f_p2[0xBF] = sty_ext;
    m6809f_p2[0xCE] = lds_imm; m6809f_p2[0xDE] = lds_dir; m6809f_p2[0xEE] = lds_idx; m6809f_p2[0xFE] = lds_ext;
    m6809f_p2[0xDF] = sts_dir; m6809f_p2[0xEF] = sts_idx; m6809f_p2[0xFF] = sts_ext;
}
