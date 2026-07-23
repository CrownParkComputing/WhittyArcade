/* m6809f_ops_branch.c -- branch family (short/long conditional branches + BSR/LBSR).
 *
 * SEMANTIC + CYCLE ORACLE: src/cores/m6809.c
 *   - relative target = (int8)offset + pc-after-offset-fetch          (mc6809_relative)
 *   - long target     = (int16)offset16 + pc-after-offset-fetch       (mc6809_lrelative)
 *   - BSR/LBSR push the return pc (LSB first to --S, then MSB to --S), exactly like
 *     mc6809.c case 0x8D / 0x17.
 *
 * Cycle totals (m6809f_step adds NO base cycle, so handlers add the FULL count;
 * the oracle's "cpu->cycles++" base of 1 is folded in here):
 *   short Bcc 0x20-0x2F : 3                 (oracle base 1 + 2)
 *   LBRA  0x16          : 5                 (1 + 4)
 *   LBSR  0x17          : 9                 (1 + 8)
 *   BSR   0x8D          : 7                 (1 + 6)
 *   long  Lcc 0x21-0x2F : 5 not-taken / 6 taken   (1 + 4, +1 when taken)
 *
 * Condition tests mirror m6809.c's bhi/bls/... using the packed CC byte (CC_N/Z/V/C).
 */
#include "m6809f.h"

/* ---- relative targets (offset fetch advances pc; target = pc + sign-extended off) ---- */
static inline u16 rel8 (M6809F *c){ int8_t o = (int8_t)m6809f_fetch(c);   return (u16)(c->pc + (u16)(int16_t)o); }
static inline u16 rel16(M6809F *c){ u16   o = m6809f_fetch16(c);          return (u16)(c->pc + o); }

/* ---- condition predicates from the packed CC byte ---- */
#define F_C (c->cc & CC_C)
#define F_Z (c->cc & CC_Z)
#define F_N (c->cc & CC_N)
#define F_V (c->cc & CC_V)
#define NB(x)  (!!(x))
#define NV_EQ  (NB(F_N) == NB(F_V))      /* N == V */
#define NV_NE  (NB(F_N) != NB(F_V))      /* N != V */

/* ===== short branches (page1 0x20-0x2F): 3 cycles, taken => pc = target ===== */
#define SB(nm, COND) \
    static void nm(M6809F *c){ u16 t = rel8(c); c->cycles += 3; if (COND) c->pc = t; }

SB(sb_bra, 1)                 /* 0x20 BRA  : always           */
SB(sb_brn, 0)                 /* 0x21 BRN  : never (consumes) */
SB(sb_bhi, !F_C && !F_Z)      /* 0x22 BHI                     */
SB(sb_bls, F_C || F_Z)        /* 0x23 BLS                     */
SB(sb_bcc, !F_C)              /* 0x24 BCC/BHS                 */
SB(sb_bcs, F_C)               /* 0x25 BCS/BLO                 */
SB(sb_bne, !F_Z)              /* 0x26 BNE                     */
SB(sb_beq, F_Z)               /* 0x27 BEQ                     */
SB(sb_bvc, !F_V)              /* 0x28 BVC                     */
SB(sb_bvs, F_V)               /* 0x29 BVS                     */
SB(sb_bpl, !F_N)              /* 0x2A BPL                     */
SB(sb_bmi, F_N)               /* 0x2B BMI                     */
SB(sb_bge, NV_EQ)             /* 0x2C BGE                     */
SB(sb_blt, NV_NE)             /* 0x2D BLT                     */
SB(sb_bgt, NV_EQ && !F_Z)     /* 0x2E BGT                     */
SB(sb_ble, F_Z || NV_NE)      /* 0x2F BLE                     */

/* ===== long branches (page2 0x21-0x2F): 5 not-taken / 6 taken ===== */
#define LB(nm, COND) \
    static void nm(M6809F *c){ u16 t = rel16(c); c->cycles += 5; if (COND){ c->cycles += 1; c->pc = t; } }

LB(lb_lbrn, 0)                /* 0x21 LBRN : never (consumes) */
LB(lb_lbhi, !F_C && !F_Z)     /* 0x22 LBHI                    */
LB(lb_lbls, F_C || F_Z)       /* 0x23 LBLS                    */
LB(lb_lbcc, !F_C)             /* 0x24 LBCC/LBHS               */
LB(lb_lbcs, F_C)              /* 0x25 LBCS/LBLO               */
LB(lb_lbne, !F_Z)             /* 0x26 LBNE                    */
LB(lb_lbeq, F_Z)              /* 0x27 LBEQ                    */
LB(lb_lbvc, !F_V)             /* 0x28 LBVC                    */
LB(lb_lbvs, F_V)              /* 0x29 LBVS                    */
LB(lb_lbpl, !F_N)             /* 0x2A LBPL                    */
LB(lb_lbmi, F_N)              /* 0x2B LBMI                    */
LB(lb_lbge, NV_EQ)            /* 0x2C LBGE                    */
LB(lb_lblt, NV_NE)            /* 0x2D LBLT                    */
LB(lb_lbgt, NV_EQ && !F_Z)    /* 0x2E LBGT                    */
LB(lb_lble, F_Z || NV_NE)     /* 0x2F LBLE                    */

/* ===== always-long branch + subroutine branches (page1) ===== */
static void p1_lbra(M6809F *c)            /* 0x16 LBRA : 5 cycles, always */
{
    u16 t = rel16(c);
    c->pc = t;
    c->cycles += 5;
}

static void p1_lbsr(M6809F *c)            /* 0x17 LBSR : push pc, branch, 9 cycles */
{
    u16 t = rel16(c);
    m6809f_wr(c, --c->s, (u8)c->pc);        /* LSB */
    m6809f_wr(c, --c->s, (u8)(c->pc >> 8)); /* MSB */
    c->pc = t;
    c->cycles += 9;
}

static void p1_bsr(M6809F *c)             /* 0x8D BSR  : push pc, branch, 7 cycles */
{
    u16 t = rel8(c);
    m6809f_wr(c, --c->s, (u8)c->pc);        /* LSB */
    m6809f_wr(c, --c->s, (u8)(c->pc >> 8)); /* MSB */
    c->pc = t;
    c->cycles += 7;
}

void m6809f_install_branch(void)
{
    /* page1 always-long / subroutine branches */
    m6809f_p1[0x16] = p1_lbra;
    m6809f_p1[0x17] = p1_lbsr;
    m6809f_p1[0x8D] = p1_bsr;

    /* page1 short conditional branches 0x20-0x2F */
    m6809f_p1[0x20] = sb_bra; m6809f_p1[0x21] = sb_brn; m6809f_p1[0x22] = sb_bhi; m6809f_p1[0x23] = sb_bls;
    m6809f_p1[0x24] = sb_bcc; m6809f_p1[0x25] = sb_bcs; m6809f_p1[0x26] = sb_bne; m6809f_p1[0x27] = sb_beq;
    m6809f_p1[0x28] = sb_bvc; m6809f_p1[0x29] = sb_bvs; m6809f_p1[0x2A] = sb_bpl; m6809f_p1[0x2B] = sb_bmi;
    m6809f_p1[0x2C] = sb_bge; m6809f_p1[0x2D] = sb_blt; m6809f_p1[0x2E] = sb_bgt; m6809f_p1[0x2F] = sb_ble;

    /* page2 long conditional branches 0x21-0x2F (LBRA/LBSR are page1 0x16/0x17) */
    m6809f_p2[0x21] = lb_lbrn; m6809f_p2[0x22] = lb_lbhi; m6809f_p2[0x23] = lb_lbls;
    m6809f_p2[0x24] = lb_lbcc; m6809f_p2[0x25] = lb_lbcs; m6809f_p2[0x26] = lb_lbne; m6809f_p2[0x27] = lb_lbeq;
    m6809f_p2[0x28] = lb_lbvc; m6809f_p2[0x29] = lb_lbvs; m6809f_p2[0x2A] = lb_lbpl; m6809f_p2[0x2B] = lb_lbmi;
    m6809f_p2[0x2C] = lb_lbge; m6809f_p2[0x2D] = lb_lblt; m6809f_p2[0x2E] = lb_lbgt; m6809f_p2[0x2F] = lb_lble;
}
