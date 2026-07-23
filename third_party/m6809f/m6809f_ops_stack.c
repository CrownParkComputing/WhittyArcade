/* m6809f_ops_stack.c -- STACK / subroutine family.
 *
 * Ops: PSHS 0x34 / PULS 0x35 / PSHU 0x36 / PULU 0x37,
 *      RTS 0x39, RTI 0x3B, JSR 0x9D(direct)/0xAD(indexed)/0xBD(extended).
 *
 * SEMANTIC + CYCLE ORACLE: src/cores/m6809.c (cases 0x34..0x3B, 0x9D/0xAD/0xBD).
 *
 * Cycle model: m6809f_step() adds NO base cycle, so each handler adds the FULL
 * oracle total = (mc6809_step base 1) + (per-case add). The oracle cases are:
 *   PSH/PUL : case +4  -> base 5, +2 per 16-bit reg, +1 per 8-bit reg.
 *   RTS     : case +4  -> 5.
 *   RTI     : case +5  -> 6; +9 more (=15) when the pulled CC has E set.
 *   JSR dir : case +6  -> 7.   JSR idx : case +6 -> 7 (+ ea-mode cycles).
 *   JSR ext : case +7  -> 8.
 *
 * PUSH order (high stack -> low): PC,U/S,Y,X,DP,B,A,CC, each 16-bit reg pushed
 * LSB-then-MSB via --stack so memory holds MSB at the lower address. PULL is the
 * exact reverse (CC,A,B,DP,X,Y,U/S,PC), MSB-then-LSB. PSHS pushes U for bit6,
 * PSHU pushes S; PULU pulls S for bit6 (and arms NMI, mirroring the oracle). */
#include "m6809f.h"

/* ---- PUSH (stack reg *sp; r6 is the bit6 register: U for PSHS, S for PSHU) ---- */
static inline void do_psh(M6809F *c, u16 *sp, u16 r6)
{
    u8 d = m6809f_fetch(c);
    c->cycles += 5;
    if (d & 0x80) { c->cycles += 2; m6809f_wr(c, --(*sp), (u8)c->pc);  m6809f_wr(c, --(*sp), (u8)(c->pc >> 8)); }
    if (d & 0x40) { c->cycles += 2; m6809f_wr(c, --(*sp), (u8)r6);      m6809f_wr(c, --(*sp), (u8)(r6 >> 8));    }
    if (d & 0x20) { c->cycles += 2; m6809f_wr(c, --(*sp), (u8)c->y);    m6809f_wr(c, --(*sp), (u8)(c->y >> 8));  }
    if (d & 0x10) { c->cycles += 2; m6809f_wr(c, --(*sp), (u8)c->x);    m6809f_wr(c, --(*sp), (u8)(c->x >> 8));  }
    if (d & 0x08) { c->cycles++; m6809f_wr(c, --(*sp), c->dp); }
    if (d & 0x04) { c->cycles++; m6809f_wr(c, --(*sp), c->b);  }
    if (d & 0x02) { c->cycles++; m6809f_wr(c, --(*sp), c->a);  }
    if (d & 0x01) { c->cycles++; m6809f_wr(c, --(*sp), c->cc); }
}

/* ---- PULL (stack reg *sp; r6 = bit6 dest: U for PULS, S for PULU) ----
 * arm=1 (PULU) arms NMI when S is pulled, mirroring the oracle's LDS side-effect. */
static inline void do_pul(M6809F *c, u16 *sp, u16 *r6, int arm)
{
    u8 d = m6809f_fetch(c);
    c->cycles += 5;
    if (d & 0x01) { c->cycles++; c->cc = m6809f_rd(c, (*sp)++); }
    if (d & 0x02) { c->cycles++; c->a  = m6809f_rd(c, (*sp)++); }
    if (d & 0x04) { c->cycles++; c->b  = m6809f_rd(c, (*sp)++); }
    if (d & 0x08) { c->cycles++; c->dp = m6809f_rd(c, (*sp)++); }
    if (d & 0x10) { c->cycles += 2; u8 hi = m6809f_rd(c, (*sp)++); u8 lo = m6809f_rd(c, (*sp)++); c->x  = ((u16)hi << 8) | lo; }
    if (d & 0x20) { c->cycles += 2; u8 hi = m6809f_rd(c, (*sp)++); u8 lo = m6809f_rd(c, (*sp)++); c->y  = ((u16)hi << 8) | lo; }
    if (d & 0x40) { c->cycles += 2; u8 hi = m6809f_rd(c, (*sp)++); u8 lo = m6809f_rd(c, (*sp)++); *r6   = ((u16)hi << 8) | lo; if (arm) c->nmi_armed = 1; }
    if (d & 0x80) { c->cycles += 2; u8 hi = m6809f_rd(c, (*sp)++); u8 lo = m6809f_rd(c, (*sp)++); c->pc = ((u16)hi << 8) | lo; }
}

static void op_pshs(M6809F *c){ do_psh(c, &c->s, c->u); }
static void op_pshu(M6809F *c){ do_psh(c, &c->u, c->s); }
static void op_puls(M6809F *c){ do_pul(c, &c->s, &c->u, 0); }
static void op_pulu(M6809F *c){ do_pul(c, &c->u, &c->s, 1); }

/* ---- RTS: pull 16-bit PC off S ---- */
static void op_rts(M6809F *c)
{
    c->cycles += 5;
    u8 hi = m6809f_rd(c, c->s++);
    u8 lo = m6809f_rd(c, c->s++);
    c->pc = ((u16)hi << 8) | lo;
}

/* ---- RTI: pull CC; if E set pull full state, else just PC ---- */
static void op_rti(M6809F *c)
{
    c->cycles += 6;
    c->cc = m6809f_rd(c, c->s++);
    if (c->cc & CC_E) {
        c->cycles += 9;
        c->a  = m6809f_rd(c, c->s++);
        c->b  = m6809f_rd(c, c->s++);
        c->dp = m6809f_rd(c, c->s++);
        { u8 hi = m6809f_rd(c, c->s++); u8 lo = m6809f_rd(c, c->s++); c->x = ((u16)hi << 8) | lo; }
        { u8 hi = m6809f_rd(c, c->s++); u8 lo = m6809f_rd(c, c->s++); c->y = ((u16)hi << 8) | lo; }
        { u8 hi = m6809f_rd(c, c->s++); u8 lo = m6809f_rd(c, c->s++); c->u = ((u16)hi << 8) | lo; }
    }
    { u8 hi = m6809f_rd(c, c->s++); u8 lo = m6809f_rd(c, c->s++); c->pc = ((u16)hi << 8) | lo; }
}

/* ---- JSR: compute EA, push 16-bit return PC onto S, jump ---- */
static inline void jsr_to(M6809F *c, u16 ea)
{
    m6809f_wr(c, --c->s, (u8)c->pc);
    m6809f_wr(c, --c->s, (u8)(c->pc >> 8));
    c->pc = ea;
}
static void op_jsr_dir(M6809F *c){ c->cycles += 7; u16 ea = m6809f_ea_direct(c);   jsr_to(c, ea); }
static void op_jsr_idx(M6809F *c){ c->cycles += 7; u16 ea = m6809f_ea_indexed(c);  jsr_to(c, ea); }
static void op_jsr_ext(M6809F *c){ c->cycles += 8; u16 ea = m6809f_ea_extended(c); jsr_to(c, ea); }

void m6809f_install_stack(void)
{
    m6809f_p1[0x34] = op_pshs;
    m6809f_p1[0x35] = op_puls;
    m6809f_p1[0x36] = op_pshu;
    m6809f_p1[0x37] = op_pulu;
    m6809f_p1[0x39] = op_rts;
    m6809f_p1[0x3B] = op_rti;
    m6809f_p1[0x9D] = op_jsr_dir;
    m6809f_p1[0xAD] = op_jsr_idx;
    m6809f_p1[0xBD] = op_jsr_ext;
}
