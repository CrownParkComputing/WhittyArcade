/* m6809f_ops_misc.c -- MISC / inherent family.
 *
 * Ops: NOP, SYNC, DAA, ORCC, ANDCC, EXG, TFR, LEAX/Y/S/U, CWAI, SWI (page1),
 * SWI2 (page2), SWI3 (page3).
 *
 * SEMANTIC + CYCLE ORACLE: src/cores/m6809.c (cases 0x12,0x13,0x19,0x1A,0x1C,
 * 0x1E,0x1F,0x30..0x33,0x3C,0x3F and page2/page3 0x3F). Mirrored byte-exactly.
 *
 * Notes:
 *  - EXG/TFR postbyte nibbles select 16-bit regs (0=D,1=X,2=Y,3=U,4=S,5=PC) when
 *    (pb&0x88)==0x00, or 8-bit regs (8=A,9=B,10=CC,11=DP) when (pb&0x88)==0x88.
 *    Any other postbyte FAULTS in the oracle -> we set c->stop so the diff harness
 *    skips the case (matching the "oracle faulted" path).
 *  - CC is already the packed cctobyte image, so EXG/TFR of CC and the push of CC
 *    are direct byte copies.
 *  - LEAX/LEAY set Z = (result==0); LEAU/LEAS set no flags.
 */
#include "m6809f.h"

/* ---- 16-bit / 8-bit register access by EXG/TFR field code ---- */
static u16 rd16reg(M6809F *c, int i)
{
    switch (i) {
        case 0: return m6809f_gd(c);
        case 1: return c->x;
        case 2: return c->y;
        case 3: return c->u;
        case 4: return c->s;
        default: return c->pc;       /* 5 */
    }
}
static void wr16reg(M6809F *c, int i, u16 v)
{
    switch (i) {
        case 0: m6809f_sd(c, v); break;
        case 1: c->x = v; break;
        case 2: c->y = v; break;
        case 3: c->u = v; break;
        case 4: c->s = v; c->nmi_armed = 1; break;
        case 5: c->pc = v; break;
    }
}
static u8 rd8reg(M6809F *c, int i)
{
    switch (i) {
        case 8:  return c->a;
        case 9:  return c->b;
        case 10: return c->cc;
        default: return c->dp;       /* 11 */
    }
}
static void wr8reg(M6809F *c, int i, u8 v)
{
    switch (i) {
        case 8:  c->a = v; break;
        case 9:  c->b = v; break;
        case 10: c->cc = v; break;
        case 11: c->dp = v; break;
    }
}

/* ---- full-state push (12 bytes), order matches mc6809.c SWI/CWAI ---- */
static void push_state(M6809F *c)
{
    m6809f_wr(c, --c->s, (u8)c->pc);
    m6809f_wr(c, --c->s, (u8)(c->pc >> 8));
    m6809f_wr(c, --c->s, (u8)c->u);
    m6809f_wr(c, --c->s, (u8)(c->u >> 8));
    m6809f_wr(c, --c->s, (u8)c->y);
    m6809f_wr(c, --c->s, (u8)(c->y >> 8));
    m6809f_wr(c, --c->s, (u8)c->x);
    m6809f_wr(c, --c->s, (u8)(c->x >> 8));
    m6809f_wr(c, --c->s, c->dp);
    m6809f_wr(c, --c->s, c->b);
    m6809f_wr(c, --c->s, c->a);
    m6809f_wr(c, --c->s, c->cc);
}

/* ============================ handlers ============================ */

static void op_nop(M6809F *c)   { c->cycles += 2; }

static void op_sync(M6809F *c)  { c->cycles += 2; c->sync = 1; }

static void op_daa(M6809F *c)
{
    c->cycles += 2;
    u8 A   = c->a;
    u8 msn = (u8)(A >> 4);
    u8 lsn = (u8)(A & 0x0F);
    int h  = (c->cc & CC_H) != 0;
    int oc = (c->cc & CC_C) != 0;
    u8 cf  = 0;
    if (oc || msn > 9 || (msn > 8 && lsn > 9)) cf |= 0x60;
    if (h  || lsn > 9)                          cf |= 0x06;

    u8 res  = (u8)(A + cf);
    int newc = (res < A);                       /* op_add carry */
    u8 cc = (u8)(c->cc & ~(CC_H | CC_N | CC_Z | CC_V | CC_C));
    if (res & 0x80)  cc |= CC_N;
    if (res == 0)    cc |= CC_Z;
    if (newc | oc)   cc |= CC_C;                 /* C = newcarry | oldcarry */
    if (h)           cc |= CC_H;                 /* H restored to its old value */
    /* V cleared */
    c->cc = cc;
    c->a  = res;
}

static void op_orcc(M6809F *c)  { c->cycles += 3; c->cc |= m6809f_fetch(c); }
static void op_andcc(M6809F *c) { c->cycles += 3; c->cc &= m6809f_fetch(c); }

static void op_exg(M6809F *c)
{
    c->cycles += 8;
    u8 pb = m6809f_fetch(c);
    if ((pb & 0x88) == 0x00) {
        int s = (pb >> 4) & 0x0F, d = pb & 0x0F;
        if (s > 5 || d > 5) { c->stop = 4; return; }
        u16 vs = rd16reg(c, s), vd = rd16reg(c, d);
        wr16reg(c, s, vd);
        wr16reg(c, d, vs);
    } else if ((pb & 0x88) == 0x88) {
        int s = (pb >> 4) & 0x0F, d = pb & 0x0F;
        if (s < 8 || s > 11 || d < 8 || d > 11) { c->stop = 4; return; }
        u8 vs = rd8reg(c, s), vd = rd8reg(c, d);
        wr8reg(c, s, vd);
        wr8reg(c, d, vs);
    } else {
        c->stop = 4;
    }
}

static void op_tfr(M6809F *c)
{
    c->cycles += 7;
    u8 pb = m6809f_fetch(c);
    if ((pb & 0x88) == 0x00) {
        int s = (pb >> 4) & 0x0F, d = pb & 0x0F;
        if (s > 5 || d > 5) { c->stop = 4; return; }
        wr16reg(c, d, rd16reg(c, s));
    } else if ((pb & 0x88) == 0x88) {
        int s = (pb >> 4) & 0x0F, d = pb & 0x0F;
        if (s < 8 || s > 11 || d < 8 || d > 11) { c->stop = 4; return; }
        wr8reg(c, d, rd8reg(c, s));
    } else {
        c->stop = 4;
    }
}

static void op_leax(M6809F *c) { c->cycles += 4; u16 ea = m6809f_ea_indexed(c); c->x = ea; m6809f_setf(c, CC_Z, ea == 0); }
static void op_leay(M6809F *c) { c->cycles += 4; u16 ea = m6809f_ea_indexed(c); c->y = ea; m6809f_setf(c, CC_Z, ea == 0); }
static void op_leas(M6809F *c) { c->cycles += 4; u16 ea = m6809f_ea_indexed(c); c->s = ea; c->nmi_armed = 1; }
static void op_leau(M6809F *c) { c->cycles += 4; u16 ea = m6809f_ea_indexed(c); c->u = ea; }

static void op_cwai(M6809F *c)
{
    c->cycles += 21;
    u8 data = m6809f_fetch(c);
    c->cc &= data;
    c->cc |= CC_E;
    push_state(c);
    c->cwai = 1;
}

static void op_swi(M6809F *c)   /* page1 */
{
    c->cycles += 19;
    c->cc |= CC_E;
    push_state(c);
    c->cc |= CC_F | CC_I;
    c->pc = m6809f_rd16(c, 0xfffa);
}

static void op_swi2(M6809F *c)  /* page2 */
{
    c->cycles += 20;
    c->cc |= CC_E;
    push_state(c);
    c->pc = m6809f_rd16(c, 0xfff4);
}

static void op_swi3(M6809F *c)  /* page3 */
{
    c->cycles += 20;
    c->cc |= CC_E;
    push_state(c);
    c->pc = m6809f_rd16(c, 0xfff2);
}

void m6809f_install_misc(void)
{
    m6809f_p1[0x12] = op_nop;
    m6809f_p1[0x13] = op_sync;
    m6809f_p1[0x19] = op_daa;
    m6809f_p1[0x1A] = op_orcc;
    m6809f_p1[0x1C] = op_andcc;
    m6809f_p1[0x1E] = op_exg;
    m6809f_p1[0x1F] = op_tfr;
    m6809f_p1[0x30] = op_leax;
    m6809f_p1[0x31] = op_leay;
    m6809f_p1[0x32] = op_leas;
    m6809f_p1[0x33] = op_leau;
    m6809f_p1[0x3C] = op_cwai;
    m6809f_p1[0x3F] = op_swi;

    m6809f_p2[0x3F] = op_swi2;
    m6809f_p3[0x3F] = op_swi3;
}
