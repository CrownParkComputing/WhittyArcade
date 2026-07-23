/* m6809f.c -- core of the fast MC6809 interpreter (see m6809f.h).
 * Owns the dispatch tables, step/reset/irq, the shared indexed-EA decoder, and the
 * illegal-op trap. Opcode handlers live in the per-family m6809f_ops_*.c files.
 * SEMANTIC ORACLE: src/cores/mc6809.c. Cycle counts mirror that core (cycle-exact). */
#include "m6809f.h"

m6809f_op m6809f_p1[256];
m6809f_op m6809f_p2[256];
m6809f_op m6809f_p3[256];

/* ---- illegal / unimplemented opcode: stop and record pc (oracle: mc6809 faults) ---- */
static void op_illegal(M6809F *c) { c->stop = 2; c->badpc = (u16)(c->pc - 1); }

/* page-2 / page-3 prefixes: fetch the 2nd opcode byte and dispatch the sub-table */
static void op_page2(M6809F *c) { u8 op = m6809f_fetch(c); m6809f_p2[op](c); }
static void op_page3(M6809F *c) { u8 op = m6809f_fetch(c); m6809f_p3[op](c); }

/* ---- shared indexed effective-address decode (all postbyte modes + auto inc/dec).
 * Ported from the byte-exact prelude/mc6809.c; the per-mode cycle adds match the oracle,
 * each handler adds only its base cost on top. ---- */
u16 m6809f_ea_indexed(M6809F *c)
{
    u8 mode = m6809f_fetch(c);
    int reg = (mode & 0x60) >> 5;                 /* 0=X 1=Y 2=U 3=S */
    u16 *R = (reg == 0) ? &c->x : (reg == 1) ? &c->y : (reg == 2) ? &c->u : &c->s;

    if (mode < 0x80) {                            /* 5-bit signed offset, no indirect */
        c->cycles += 1;
        u16 e = (u16)(mode & 0x1f);
        if (e > 15) e |= 0xffe0;                  /* sign-extend bit4 */
        return (u16)(e + *R);
    }

    u16 ea;
    switch (mode & 0x1f) {
        case 0x00: c->cycles += 2; ea = *R; *R = (u16)(*R + 1); break;            /* ,R+   */
        case 0x01: c->cycles += 3; ea = *R; *R = (u16)(*R + 2); break;            /* ,R++  */
        case 0x02: c->cycles += 2; *R = (u16)(*R - 1); ea = *R; break;            /* ,-R   */
        case 0x03: c->cycles += 3; *R = (u16)(*R - 2); ea = *R; break;            /* ,--R  */
        case 0x04: ea = *R; break;                                               /* ,R    */
        case 0x05: c->cycles += 1; ea = (u16)(*R + (u16)(int16_t)(int8_t)c->b); break;  /* B,R */
        case 0x06: c->cycles += 1; ea = (u16)(*R + (u16)(int16_t)(int8_t)c->a); break;  /* A,R */
        case 0x08: c->cycles += 1; { int8_t o = (int8_t)m6809f_fetch(c); ea = (u16)(*R + (u16)(int16_t)o); } break; /* n8,R  */
        case 0x09: c->cycles += 4; { u16 o = m6809f_fetch16(c); ea = (u16)(*R + o); } break;                        /* n16,R */
        case 0x0b: c->cycles += 4; ea = (u16)(*R + m6809f_gd(c)); break;          /* D,R   */
        case 0x0c: c->cycles += 1; { int8_t o = (int8_t)m6809f_fetch(c); ea = (u16)(c->pc + (u16)(int16_t)o); } break; /* n8,PC  */
        case 0x0d: c->cycles += 5; { u16 o = m6809f_fetch16(c); ea = (u16)(c->pc + o); } break;                        /* n16,PC */
        case 0x11: c->cycles += 6; { u16 r = *R; *R = (u16)(*R + 2); ea = m6809f_rd16(c, r); } break;   /* [,R++] */
        case 0x13: c->cycles += 6; { *R = (u16)(*R - 2); ea = m6809f_rd16(c, *R); } break;              /* [,--R] */
        case 0x14: c->cycles += 3; ea = m6809f_rd16(c, *R); break;                                      /* [,R]   */
        case 0x15: c->cycles += 4; ea = m6809f_rd16(c, (u16)(*R + (u16)(int16_t)(int8_t)c->b)); break;  /* [B,R]  */
        case 0x16: c->cycles += 4; ea = m6809f_rd16(c, (u16)(*R + (u16)(int16_t)(int8_t)c->a)); break;  /* [A,R]  */
        case 0x18: c->cycles += 4; { int8_t o = (int8_t)m6809f_fetch(c); ea = m6809f_rd16(c, (u16)(*R + (u16)(int16_t)o)); } break; /* [n8,R]  */
        case 0x19: c->cycles += 7; { u16 o = m6809f_fetch16(c); ea = m6809f_rd16(c, (u16)(*R + o)); } break;                        /* [n16,R] */
        case 0x1b: c->cycles += 7; ea = m6809f_rd16(c, (u16)(*R + m6809f_gd(c))); break;                /* [D,R]  */
        case 0x1c: c->cycles += 4; { int8_t o = (int8_t)m6809f_fetch(c); ea = m6809f_rd16(c, (u16)(c->pc + (u16)(int16_t)o)); } break; /* [n8,PC]  */
        case 0x1d: c->cycles += 8; { u16 o = m6809f_fetch16(c); ea = m6809f_rd16(c, (u16)(c->pc + o)); } break;                        /* [n16,PC] */
        case 0x1f: if (reg == 0) { c->cycles += 5; u16 t = m6809f_fetch16(c); ea = m6809f_rd16(c, t); }  /* [n16] extended indirect */
                   else { c->stop = 3; ea = 0; } break;
        default:   c->stop = 3; ea = 0; break;    /* illegal postbyte (0x07,0a,0e,0f,0x10,12,17,1a,1e) */
    }
    return ea;
}

/* ---- step: fetch opcode, dispatch ---- */
void m6809f_step(M6809F *c)
{
    if (c->cwai || c->sync) { c->cycles += 4; return; }
    u8 op = m6809f_fetch(c);
    m6809f_p1[op](c);
}

/* ---- reset: matches mc6809_reset ---- */
void m6809f_reset(M6809F *c)
{
    static int tables_ready = 0;
    if (!tables_ready) {
        for (int i = 0; i < 256; i++) { m6809f_p1[i] = op_illegal; m6809f_p2[i] = op_illegal; m6809f_p3[i] = op_illegal; }
        m6809f_p1[0x10] = op_page2;
        m6809f_p1[0x11] = op_page3;
        m6809f_install_alu8(); m6809f_install_alu16(); m6809f_install_load();
        m6809f_install_rmw();  m6809f_install_branch(); m6809f_install_stack();
        m6809f_install_misc(); m6809f_install_page2(); m6809f_install_page3();
        tables_ready = 1;
    }
    c->dp = 0;
    c->cc = CC_F | CC_I;                 /* FIRQ + IRQ masked at reset */
    c->nmi_armed = c->nmi = c->firq = c->irq = c->cwai = c->sync = c->stop = 0;
    c->pc = m6809f_rd16(c, 0xfffe);      /* reset vector */
}

/* ---- IRQ: take a pending unmasked IRQ (push full state, vector via FFF8) ---- */
void m6809f_irq(M6809F *c)
{
    if (!c->irq) return;
    /* SYNC resumes on an asserted interrupt even when that interrupt is
     * masked; exception entry only happens when the mask permits it. */
    if (c->sync) {
        c->sync = 0;
        if (c->cc & CC_I) { c->irq = 0; return; }
    }
    if (c->cc & CC_I) return;
    if (c->cwai) {
        c->cwai = 0;                                 /* CWAI already stacked the state -> do NOT re-push */
    } else {
        c->cc |= CC_E;                               /* entire state pushed */
        m6809f_wr(c, --c->s, (u8)c->pc); m6809f_wr(c, --c->s, (u8)(c->pc >> 8));
        m6809f_wr(c, --c->s, (u8)c->u);  m6809f_wr(c, --c->s, (u8)(c->u >> 8));
        m6809f_wr(c, --c->s, (u8)c->y);  m6809f_wr(c, --c->s, (u8)(c->y >> 8));
        m6809f_wr(c, --c->s, (u8)c->x);  m6809f_wr(c, --c->s, (u8)(c->x >> 8));
        m6809f_wr(c, --c->s, c->dp);
        m6809f_wr(c, --c->s, c->b); m6809f_wr(c, --c->s, c->a);
        m6809f_wr(c, --c->s, c->cc);
    }
    c->cc |= CC_I;                                   /* mask further IRQ */
    c->pc = m6809f_rd16(c, 0xfff8);
    c->cycles += 19;
    c->irq = 0;     /* edge: consume the line (vblank re-asserts each frame; handler-ack also clears) */
}
