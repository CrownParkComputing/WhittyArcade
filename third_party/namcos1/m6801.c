/* src/cores/m6801.c -- HD6301/HD63701 (6801-family) CPU core.
 * Clean implementation; MAME m6801.cpp / 6800ops.hxx used only as a semantic
 * reference. Unimplemented opcodes set cpu->trap_op (so the real MCU ROM tells
 * us which opcodes it actually uses). Cycle counts are approximate (the MCU is
 * driven by vblank IRQ + frame budget, not cycle-exact timing). */
#include "m6801.h"

/* TCSR bits */
#define TCSR_ETOI 0x04
#define TCSR_EOCI 0x08
#define TCSR_TOF  0x20
#define TCSR_OCF  0x40

/* on-chip timer registers 0x08-0x0c are handled internally, not via callback */
static uint8_t tmr_rd(m6801_t *c, uint16_t a){
    switch(a){
    case 0x08: c->pending_tcsr = 0; return c->tcsr;
    case 0x09: if (!(c->pending_tcsr & TCSR_TOF)) c->tcsr &= ~TCSR_TOF; return c->frc>>8;
    case 0x0a: return c->frc & 0xff;
    case 0x0b: return c->ocr>>8;
    case 0x0c: return c->ocr&0xff;
    }
    return 0xff;
}
static void tmr_wr(m6801_t *c, uint16_t a, uint8_t v){
    switch(a){
    case 0x08:
        c->tcsr = (c->tcsr & 0xe0) | (v & 0x1f);
        c->pending_tcsr &= c->tcsr;
        break;
    case 0x09:
        c->counter_high_latch = v;
        c->frc = 0xfff8;
        break;
    case 0x0a:
        c->frc = ((uint16_t)c->counter_high_latch << 8) | v;
        break;
    case 0x0b:
        if (!(c->pending_tcsr & TCSR_OCF)) c->tcsr &= ~TCSR_OCF;
        c->ocr=(c->ocr&0x00ff)|(v<<8);
        break;
    case 0x0c:
        if (!(c->pending_tcsr & TCSR_OCF)) c->tcsr &= ~TCSR_OCF;
        c->ocr=(c->ocr&0xff00)|v;
        break;
    }
}
static inline uint8_t m_read(m6801_t *c, uint16_t a){
    const uint8_t *p = c->rdpage[a >> 8];
    if (p) return p[a & 0xff];                                  /* direct RAM/ROM */
    return (a>=0x08&&a<=0x0c)?tmr_rd(c,a):(*c->read)(c,a);
}
static inline void    m_write(m6801_t *c, uint16_t a, uint8_t v){ if(a>=0x08&&a<=0x0c) tmr_wr(c,a,v); else (*c->write)(c,a,v); }
#define RD(a)    m_read(c,(uint16_t)(a))
#define WR(a,v)  m_write(c,(uint16_t)(a),(uint8_t)(v))

static inline uint16_t rd16(m6801_t *c, uint16_t a){ return (RD(a)<<8)|RD(a+1); }
static inline void     wr16(m6801_t *c, uint16_t a, uint16_t v){ WR(a,v>>8); WR(a+1,v); }

static inline void push8 (m6801_t *c, uint8_t v){ WR(c->sp, v); c->sp--; }
static inline uint8_t pull8(m6801_t *c){ c->sp++; return RD(c->sp); }
static inline void push16(m6801_t *c, uint16_t v){ push8(c,v&0xff); push8(c,v>>8); }
static inline uint16_t pull16(m6801_t *c){ uint16_t hi=pull8(c); uint16_t lo=pull8(c); return (hi<<8)|lo; }

#define SET(f)   (c->cc |= (f))
#define CLR(f)   (c->cc &= ~(f))
#define UPD(f,x) do{ if(x) SET(f); else CLR(f); }while(0)
#define NZ8(r)   do{ UPD(M6801_N,(r)&0x80); UPD(M6801_Z,((r)&0xff)==0); }while(0)
#define NZ16(r)  do{ UPD(M6801_N,(r)&0x8000); UPD(M6801_Z,((r)&0xffff)==0); }while(0)

/* addressing-mode effective address (advances PC) */
static inline uint16_t ea_dir(m6801_t *c){ return RD(c->pc++); }
static inline uint16_t ea_idx(m6801_t *c){ return (uint16_t)(c->x + RD(c->pc++)); }
static inline uint16_t ea_ext(m6801_t *c){ uint16_t a=rd16(c,c->pc); c->pc+=2; return a; }
static inline uint8_t  imm8(m6801_t *c){ return RD(c->pc++); }
static inline uint16_t imm16(m6801_t *c){ uint16_t v=rd16(c,c->pc); c->pc+=2; return v; }

/* 8-bit ALU primitives (set flags like the 6800) */
static uint8_t op_sub(m6801_t *c, uint8_t d, uint8_t s){ unsigned r=d-s;
    UPD(M6801_C,r&0x100); UPD(M6801_V,((d^s)&(d^r))&0x80); NZ8(r); return r&0xff; }
static uint8_t op_sbc(m6801_t *c, uint8_t d, uint8_t s){ unsigned cf=c->cc&M6801_C?1:0; unsigned r=d-s-cf;
    UPD(M6801_C,r&0x100); UPD(M6801_V,((d^s)&(d^r))&0x80); NZ8(r); return r&0xff; }
static uint8_t op_add(m6801_t *c, uint8_t d, uint8_t s){ unsigned r=d+s;
    UPD(M6801_C,r&0x100); UPD(M6801_H,((d^s^r)&0x10)); UPD(M6801_V,(~(d^s)&(d^r))&0x80); NZ8(r); return r&0xff; }
static uint8_t op_adc(m6801_t *c, uint8_t d, uint8_t s){ unsigned cf=c->cc&M6801_C?1:0; unsigned r=d+s+cf;
    UPD(M6801_C,r&0x100); UPD(M6801_H,((d^s^r)&0x10)); UPD(M6801_V,(~(d^s)&(d^r))&0x80); NZ8(r); return r&0xff; }
static void    op_cmp(m6801_t *c, uint8_t d, uint8_t s){ (void)op_sub(c,d,s); }
static uint8_t op_and(m6801_t *c, uint8_t d, uint8_t s){ uint8_t r=d&s; CLR(M6801_V); NZ8(r); return r; }
static uint8_t op_ora(m6801_t *c, uint8_t d, uint8_t s){ uint8_t r=d|s; CLR(M6801_V); NZ8(r); return r; }
static uint8_t op_eor(m6801_t *c, uint8_t d, uint8_t s){ uint8_t r=d^s; CLR(M6801_V); NZ8(r); return r; }

/* read-modify-write primitives */
static uint8_t rmw_inc(m6801_t *c,uint8_t v){ uint8_t r=v+1; UPD(M6801_V,v==0x7f); NZ8(r); return r; }
static uint8_t rmw_dec(m6801_t *c,uint8_t v){ uint8_t r=v-1; UPD(M6801_V,v==0x80); NZ8(r); return r; }
static uint8_t rmw_clr(m6801_t *c){ CLR(M6801_N|M6801_V|M6801_C); SET(M6801_Z); return 0; }
static uint8_t rmw_com(m6801_t *c,uint8_t v){ uint8_t r=~v; CLR(M6801_V); SET(M6801_C); NZ8(r); return r; }
static uint8_t rmw_neg(m6801_t *c,uint8_t v){ uint8_t r=-v; UPD(M6801_C,r!=0); UPD(M6801_V,r==0x80); NZ8(r); return r; }
static uint8_t rmw_asl(m6801_t *c,uint8_t v){ uint8_t r=v<<1; UPD(M6801_C,v&0x80); NZ8(r); UPD(M6801_V,(c->cc&M6801_N)^((c->cc&M6801_C)?M6801_N:0)); return r; }
static uint8_t rmw_lsr(m6801_t *c,uint8_t v){ uint8_t r=v>>1; UPD(M6801_C,v&1); CLR(M6801_N); UPD(M6801_Z,r==0); UPD(M6801_V,v&1); return r; }
static uint8_t rmw_asr(m6801_t *c,uint8_t v){ uint8_t r=(v>>1)|(v&0x80); UPD(M6801_C,v&1); NZ8(r); UPD(M6801_V,((c->cc&M6801_N)?1:0)^(v&1)); return r; }
static uint8_t rmw_rol(m6801_t *c,uint8_t v){ uint8_t cf=c->cc&M6801_C?1:0; uint8_t r=(v<<1)|cf; UPD(M6801_C,v&0x80); NZ8(r); UPD(M6801_V,((c->cc&M6801_N)?1:0)^((c->cc&M6801_C)?1:0)); return r; }
static uint8_t rmw_ror(m6801_t *c,uint8_t v){ uint8_t cf=c->cc&M6801_C?1:0; uint8_t r=(v>>1)|(cf<<7); UPD(M6801_C,v&1); NZ8(r); UPD(M6801_V,((c->cc&M6801_N)?1:0)^(v&1)); return r; }
static void    op_tst(m6801_t *c,uint8_t v){ CLR(M6801_V|M6801_C); NZ8(v); }

static void enter_irq(m6801_t *c, uint16_t vec){
    push16(c,c->pc); push16(c,c->x); push8(c,c->a); push8(c,c->b); push8(c,c->cc);
    SET(M6801_I); c->pc = rd16(c,vec); c->wai=false;
}

void m6801_reset(m6801_t *c){
    c->inhibit_interrupts=false;
    c->cycles=0; c->cc = M6801_I | 0xC0; c->irq1=false; c->wai=false; c->halted=false;
    c->trap_op=0; c->trap_pc=0; c->a=c->b=0; c->x=0; c->sp=0;
    c->frc=0; c->ocr=0xffff; c->tcsr=0; c->pending_tcsr=0;
    c->counter_high_latch=0;
    c->pc = rd16(c, M6801_VEC_RESET);
}

/* branch helper */
#define BRANCH(cond) do{ int8_t off=(int8_t)RD(c->pc++); if(cond) c->pc += off; }while(0)

/* Per-opcode cycle counts for the HD6301/HD63701 (6301-family) opcode map.
 * Transcribed verbatim from MAME 0.288 m6801_cpu_device::cycles_63701[256]
 * (src/devices/cpu/m6800/m6801.cpp:157-176) -- this is the cycle table the
 * HD63701V0 device is actually built with (m6801.cpp:494). MAME's "XX"
 * (illegal opcode, unknown cycle count) is #defined to 4 (m6801.cpp:134) and
 * is expanded to 4 here. 6301 branches are a fixed 3 cycles regardless of
 * taken/not-taken, so no conditional extra is modelled. */
static const uint8_t hd63701_cyc[256] = {
    /*       0   1   2   3   4   5   6   7   8   9   A   B   C   D   E   F */
    /*0*/    4,  1,  4,  4,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    /*1*/    1,  1,  4,  4,  4,  4,  1,  1,  2,  2,  4,  1,  4,  4,  4,  4,
    /*2*/    3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,
    /*3*/    1,  1,  3,  3,  1,  1,  4,  4,  4,  5,  1, 10,  5,  7,  9, 12,
    /*4*/    1,  4,  4,  1,  1,  4,  1,  1,  1,  1,  1,  4,  1,  1,  4,  1,
    /*5*/    1,  4,  4,  1,  1,  4,  1,  1,  1,  1,  1,  4,  1,  1,  4,  1,
    /*6*/    6,  7,  7,  6,  6,  7,  6,  6,  6,  6,  6,  5,  6,  4,  3,  5,
    /*7*/    6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  6,  4,  6,  4,  3,  5,
    /*8*/    2,  2,  2,  3,  2,  2,  2,  4,  2,  2,  2,  2,  3,  5,  3,  4,
    /*9*/    3,  3,  3,  4,  3,  3,  3,  3,  3,  3,  3,  3,  4,  5,  4,  4,
    /*A*/    4,  4,  4,  5,  4,  4,  4,  4,  4,  4,  4,  4,  5,  5,  5,  5,
    /*B*/    4,  4,  4,  5,  4,  4,  4,  4,  4,  4,  4,  4,  5,  6,  5,  5,
    /*C*/    2,  2,  2,  3,  2,  2,  2,  4,  2,  2,  2,  2,  3,  4,  3,  4,
    /*D*/    3,  3,  3,  4,  3,  3,  3,  3,  3,  3,  3,  3,  4,  4,  4,  4,
    /*E*/    4,  4,  4,  5,  4,  4,  4,  4,  4,  4,  4,  4,  5,  5,  5,  5,
    /*F*/    4,  4,  4,  5,  4,  4,  4,  4,  4,  4,  4,  4,  5,  5,  5,  5,
};

int m6801_step(m6801_t *c)
{
    if (c->halted) return c->trap_op;

    /* interrupts taken at the instruction boundary: timer OCF, TOF, then IRQ1 */
    if (!c->inhibit_interrupts && !(c->cc & M6801_I)) {
        if ((c->tcsr & TCSR_OCF) && (c->tcsr & TCSR_EOCI)) { enter_irq(c, M6801_VEC_OCF); c->cycles+=12; return 0; }
        if ((c->tcsr & TCSR_TOF) && (c->tcsr & TCSR_ETOI)) { enter_irq(c, M6801_VEC_TOF); c->cycles+=12; return 0; }
        if (c->irq1) { c->irq1=false; enter_irq(c, M6801_VEC_IRQ1); c->cycles+=12; return 0; }
    }

    if (c->wai) {                        /* asleep until an interrupt; keep the timer ticking */
        unsigned d = 4;
        const uint16_t compare_distance = (uint16_t)(c->ocr - c->frc);
        if (compare_distance != 0 && compare_distance <= d) {
            c->tcsr |= TCSR_OCF;
            c->pending_tcsr |= TCSR_OCF;
        }
        if ((unsigned)c->frc + d > 0xffff) {
            c->tcsr |= TCSR_TOF;
            c->pending_tcsr |= TCSR_TOF;
        }
        c->frc += d; c->cycles += d;
        return 0;
    }

    unsigned long c0 = c->cycles;
    uint16_t op_pc = c->pc;
    uint8_t op = RD(c->pc++);
    uint16_t ea; uint8_t m; uint16_t d16;
    c->cycles += hd63701_cyc[op];   /* accurate per-opcode timing (MAME cycles_63701) */

    switch (op) {
    /* ---- inherent / misc ---- */
    case 0x01: break;                                            /* NOP */
    case 0x04: d16=((c->a<<8)|c->b)>>1; UPD(M6801_C,((c->a<<8)|c->b)&1); c->a=d16>>8; c->b=d16; NZ16(d16); UPD(M6801_V,(c->cc&M6801_N)?(!(c->cc&M6801_C)):(c->cc&M6801_C)); break; /* LSRD */
    case 0x05: d16=((c->a<<8)|c->b)<<1; UPD(M6801_C,((c->a<<8)|c->b)&0x8000); c->a=d16>>8; c->b=d16; NZ16(d16); break; /* ASLD */
    case 0x06: c->cc = c->a | 0xC0; break;                        /* TAP */
    case 0x07: c->a = c->cc; break;                               /* TPA */
    case 0x08: c->x++; UPD(M6801_Z,c->x==0); break;               /* INX */
    case 0x09: c->x--; UPD(M6801_Z,c->x==0); break;               /* DEX */
    case 0x0A: CLR(M6801_V); break;                               /* CLV */
    case 0x0B: SET(M6801_V); break;                               /* SEV */
    case 0x0C: CLR(M6801_C); break;                               /* CLC */
    case 0x0D: SET(M6801_C); break;                               /* SEC */
    case 0x0E: CLR(M6801_I); break;                               /* CLI */
    case 0x0F: SET(M6801_I); break;                               /* SEI */
    case 0x10: c->a=op_sub(c,c->a,c->b); break;                   /* SBA */
    case 0x11: op_cmp(c,c->a,c->b); break;                        /* CBA */
    case 0x12: c->x += RD(c->sp + 1); break;                      /* undoc1 (63701): X += [SP+1] */
    case 0x13: c->x += RD(c->sp + 1); break;                      /* undoc2 (63701): X += [SP+1] */
    case 0x16: c->b=c->a; NZ8(c->b); CLR(M6801_V); break;         /* TAB */
    case 0x17: c->a=c->b; NZ8(c->a); CLR(M6801_V); break;         /* TBA */
    case 0x18: d16=c->x; c->x=(c->a<<8)|c->b; c->a=d16>>8; c->b=d16; break; /* XGDX (6301) */
    case 0x19: { uint8_t hi=c->a&0xf0, lo=c->a&0x0f; uint8_t add=0; /* DAA */
        if(lo>9||(c->cc&M6801_H)) add|=0x06; if(hi>0x90||(c->cc&M6801_C)||(hi>0x80&&lo>9)) {add|=0x60;SET(M6801_C);} c->a=op_add(c,c->a,add); break; }
    case 0x1A: c->wai=true; break;                                /* SLP (6301) -- sleep until IRQ */
    case 0x1B: c->a=op_add(c,c->a,c->b); break;                   /* ABA */
    /* ---- branches (relative) ---- */
    case 0x20: BRANCH(1); break;                                  /* BRA */
    case 0x21: BRANCH(0); break;                                  /* BRN */
    case 0x22: BRANCH(!((c->cc&M6801_C)||(c->cc&M6801_Z))); break;/* BHI */
    case 0x23: BRANCH(((c->cc&M6801_C)||(c->cc&M6801_Z))); break; /* BLS */
    case 0x24: BRANCH(!(c->cc&M6801_C)); break;                   /* BCC/BHS */
    case 0x25: BRANCH(c->cc&M6801_C); break;                      /* BCS/BLO */
    case 0x26: BRANCH(!(c->cc&M6801_Z)); break;                   /* BNE */
    case 0x27: BRANCH(c->cc&M6801_Z); break;                      /* BEQ */
    case 0x28: BRANCH(!(c->cc&M6801_V)); break;                   /* BVC */
    case 0x29: BRANCH(c->cc&M6801_V); break;                      /* BVS */
    case 0x2A: BRANCH(!(c->cc&M6801_N)); break;                   /* BPL */
    case 0x2B: BRANCH(c->cc&M6801_N); break;                      /* BMI */
    case 0x2C: BRANCH(!(((c->cc&M6801_N)?1:0)^((c->cc&M6801_V)?1:0))); break; /* BGE */
    case 0x2D: BRANCH((((c->cc&M6801_N)?1:0)^((c->cc&M6801_V)?1:0))); break;  /* BLT */
    case 0x2E: BRANCH(!((c->cc&M6801_Z)||(((c->cc&M6801_N)?1:0)^((c->cc&M6801_V)?1:0)))); break; /* BGT */
    case 0x2F: BRANCH(((c->cc&M6801_Z)||(((c->cc&M6801_N)?1:0)^((c->cc&M6801_V)?1:0)))); break;  /* BLE */
    /* ---- stack / inherent ---- */
    case 0x30: c->x=c->sp+1; break;                               /* TSX */
    case 0x31: c->sp++; break;                                    /* INS */
    case 0x32: c->a=pull8(c); break;                              /* PULA */
    case 0x33: c->b=pull8(c); break;                              /* PULB */
    case 0x34: c->sp--; break;                                    /* DES */
    case 0x35: c->sp=c->x-1; break;                               /* TXS */
    case 0x36: push8(c,c->a); break;                              /* PSHA */
    case 0x37: push8(c,c->b); break;                              /* PSHB */
    case 0x38: c->x=pull16(c); break;                             /* PULX */
    case 0x39: c->pc=pull16(c); break;                            /* RTS */
    case 0x3A: c->x=(uint16_t)(c->x+c->b); break;                 /* ABX */
    case 0x3B: c->cc=pull8(c); c->b=pull8(c); c->a=pull8(c); c->x=pull16(c); c->pc=pull16(c); break; /* RTI */
    case 0x3C: push16(c,c->x); break;                             /* PSHX */
    case 0x3D: d16=c->a*c->b; c->a=d16>>8; c->b=d16; UPD(M6801_C,d16&0x80); UPD(M6801_Z,d16==0); break; /* MUL */
    case 0x3E: c->wai=true; break;                                /* WAI */
    case 0x3F: push16(c,c->pc); push16(c,c->x); push8(c,c->a); push8(c,c->b); push8(c,c->cc); SET(M6801_I); c->pc=rd16(c,M6801_VEC_SWI); break; /* SWI */
    /* ---- ACCA inherent 0x40-0x4F ---- */
    case 0x40: c->a=rmw_neg(c,c->a); break;
    case 0x43: c->a=rmw_com(c,c->a); break;
    case 0x44: c->a=rmw_lsr(c,c->a); break;
    case 0x46: c->a=rmw_ror(c,c->a); break;
    case 0x47: c->a=rmw_asr(c,c->a); break;
    case 0x48: c->a=rmw_asl(c,c->a); break;
    case 0x49: c->a=rmw_rol(c,c->a); break;
    case 0x4A: c->a=rmw_dec(c,c->a); break;
    case 0x4C: c->a=rmw_inc(c,c->a); break;
    case 0x4D: op_tst(c,c->a); break;
    case 0x4F: c->a=rmw_clr(c); break;
    /* ---- ACCB inherent 0x50-0x5F ---- */
    case 0x50: c->b=rmw_neg(c,c->b); break;
    case 0x53: c->b=rmw_com(c,c->b); break;
    case 0x54: c->b=rmw_lsr(c,c->b); break;
    case 0x56: c->b=rmw_ror(c,c->b); break;
    case 0x57: c->b=rmw_asr(c,c->b); break;
    case 0x58: c->b=rmw_asl(c,c->b); break;
    case 0x59: c->b=rmw_rol(c,c->b); break;
    case 0x5A: c->b=rmw_dec(c,c->b); break;
    case 0x5C: c->b=rmw_inc(c,c->b); break;
    case 0x5D: op_tst(c,c->b); break;
    case 0x5F: c->b=rmw_clr(c); break;
    /* ---- indexed RMW 0x60-0x6F (incl. 6301 AIM/OIM/EIM/TIM) ---- */
    case 0x60: ea=ea_idx(c); WR(ea,rmw_neg(c,RD(ea))); break;
    case 0x61: { m=imm8(c); ea=ea_idx(c); uint8_t r=RD(ea)&m; WR(ea,r); CLR(M6801_V); NZ8(r);} break; /* AIM idx */
    case 0x62: { m=imm8(c); ea=ea_idx(c); uint8_t r=RD(ea)|m; WR(ea,r); CLR(M6801_V); NZ8(r);} break; /* OIM idx */
    case 0x63: ea=ea_idx(c); WR(ea,rmw_com(c,RD(ea))); break;
    case 0x64: ea=ea_idx(c); WR(ea,rmw_lsr(c,RD(ea))); break;
    case 0x65: { m=imm8(c); ea=ea_idx(c); uint8_t r=RD(ea)^m; WR(ea,r); CLR(M6801_V); NZ8(r);} break; /* EIM idx */
    case 0x66: ea=ea_idx(c); WR(ea,rmw_ror(c,RD(ea))); break;
    case 0x67: ea=ea_idx(c); WR(ea,rmw_asr(c,RD(ea))); break;
    case 0x68: ea=ea_idx(c); WR(ea,rmw_asl(c,RD(ea))); break;
    case 0x69: ea=ea_idx(c); WR(ea,rmw_rol(c,RD(ea))); break;
    case 0x6A: ea=ea_idx(c); WR(ea,rmw_dec(c,RD(ea))); break;
    case 0x6B: { m=imm8(c); ea=ea_idx(c); uint8_t r=RD(ea)&m; CLR(M6801_V); NZ8(r);} break; /* TIM idx */
    case 0x6C: ea=ea_idx(c); WR(ea,rmw_inc(c,RD(ea))); break;
    case 0x6D: ea=ea_idx(c); op_tst(c,RD(ea)); break;
    case 0x6E: c->pc=ea_idx(c); break;                            /* JMP idx */
    case 0x6F: ea=ea_idx(c); WR(ea,rmw_clr(c)); break;
    /* ---- extended RMW 0x70-0x7F (incl. 6301 AIM/OIM/EIM/TIM direct) ---- */
    case 0x70: ea=ea_ext(c); WR(ea,rmw_neg(c,RD(ea))); break;
    case 0x71: { m=imm8(c); ea=ea_dir(c); uint8_t r=RD(ea)&m; WR(ea,r); CLR(M6801_V); NZ8(r);} break; /* AIM dir */
    case 0x72: { m=imm8(c); ea=ea_dir(c); uint8_t r=RD(ea)|m; WR(ea,r); CLR(M6801_V); NZ8(r);} break; /* OIM dir */
    case 0x73: ea=ea_ext(c); WR(ea,rmw_com(c,RD(ea))); break;
    case 0x74: ea=ea_ext(c); WR(ea,rmw_lsr(c,RD(ea))); break;
    case 0x75: { m=imm8(c); ea=ea_dir(c); uint8_t r=RD(ea)^m; WR(ea,r); CLR(M6801_V); NZ8(r);} break; /* EIM dir */
    case 0x76: ea=ea_ext(c); WR(ea,rmw_ror(c,RD(ea))); break;
    case 0x77: ea=ea_ext(c); WR(ea,rmw_asr(c,RD(ea))); break;
    case 0x78: ea=ea_ext(c); WR(ea,rmw_asl(c,RD(ea))); break;
    case 0x79: ea=ea_ext(c); WR(ea,rmw_rol(c,RD(ea))); break;
    case 0x7A: ea=ea_ext(c); WR(ea,rmw_dec(c,RD(ea))); break;
    case 0x7B: { m=imm8(c); ea=ea_dir(c); uint8_t r=RD(ea)&m; CLR(M6801_V); NZ8(r);} break; /* TIM dir */
    case 0x7C: ea=ea_ext(c); WR(ea,rmw_inc(c,RD(ea))); break;
    case 0x7D: ea=ea_ext(c); op_tst(c,RD(ea)); break;
    case 0x7E: c->pc=ea_ext(c); break;                            /* JMP ext */
    case 0x7F: ea=ea_ext(c); WR(ea,rmw_clr(c)); break;

#define ACCA_OPS(BASE, GETM) \
    case BASE+0x00: c->a=op_sub(c,c->a,GETM); break;                 /* SUBA */ \
    case BASE+0x01: op_cmp(c,c->a,GETM); break;                      /* CMPA */ \
    case BASE+0x02: c->a=op_sbc(c,c->a,GETM); break;                 /* SBCA */ \
    case BASE+0x04: c->a=op_and(c,c->a,GETM); break;                 /* ANDA */ \
    case BASE+0x05: op_and(c,c->a,GETM); break;                      /* BITA */ \
    case BASE+0x08: c->a=op_eor(c,c->a,GETM); break;                 /* EORA */ \
    case BASE+0x09: c->a=op_adc(c,c->a,GETM); break;                 /* ADCA */ \
    case BASE+0x0A: c->a=op_ora(c,c->a,GETM); break;                 /* ORAA */ \
    case BASE+0x0B: c->a=op_add(c,c->a,GETM); break;                 /* ADDA */
#define ACCB_OPS(BASE, GETM) \
    case BASE+0x00: c->b=op_sub(c,c->b,GETM); break;                 /* SUBB */ \
    case BASE+0x01: op_cmp(c,c->b,GETM); break;                      /* CMPB */ \
    case BASE+0x02: c->b=op_sbc(c,c->b,GETM); break;                 /* SBCB */ \
    case BASE+0x04: c->b=op_and(c,c->b,GETM); break;                 /* ANDB */ \
    case BASE+0x05: op_and(c,c->b,GETM); break;                      /* BITB */ \
    case BASE+0x08: c->b=op_eor(c,c->b,GETM); break;                 /* EORB */ \
    case BASE+0x09: c->b=op_adc(c,c->b,GETM); break;                 /* ADCB */ \
    case BASE+0x0A: c->b=op_ora(c,c->b,GETM); break;                 /* ORAB */ \
    case BASE+0x0B: c->b=op_add(c,c->b,GETM); break;                 /* ADDB */

    /* ACCA immediate 0x80.. / direct 0x90.. / indexed 0xA0.. / extended 0xB0.. */
    ACCA_OPS(0x80, imm8(c))
    ACCA_OPS(0x90, RD(ea_dir(c)))
    ACCA_OPS(0xA0, RD(ea_idx(c)))
    ACCA_OPS(0xB0, RD(ea_ext(c)))
    ACCB_OPS(0xC0, imm8(c))
    ACCB_OPS(0xD0, RD(ea_dir(c)))
    ACCB_OPS(0xE0, RD(ea_idx(c)))
    ACCB_OPS(0xF0, RD(ea_ext(c)))

    /* STAA / STAB */
    case 0x97: WR(ea_dir(c),c->a); NZ8(c->a); CLR(M6801_V); break;
    case 0xA7: WR(ea_idx(c),c->a); NZ8(c->a); CLR(M6801_V); break;
    case 0xB7: WR(ea_ext(c),c->a); NZ8(c->a); CLR(M6801_V); break;
    case 0xD7: WR(ea_dir(c),c->b); NZ8(c->b); CLR(M6801_V); break;
    case 0xE7: WR(ea_idx(c),c->b); NZ8(c->b); CLR(M6801_V); break;
    case 0xF7: WR(ea_ext(c),c->b); NZ8(c->b); CLR(M6801_V); break;
    /* LDAA / LDAB */
    case 0x86: c->a=imm8(c); NZ8(c->a); CLR(M6801_V); break;
    case 0x96: c->a=RD(ea_dir(c)); NZ8(c->a); CLR(M6801_V); break;
    case 0xA6: c->a=RD(ea_idx(c)); NZ8(c->a); CLR(M6801_V); break;
    case 0xB6: c->a=RD(ea_ext(c)); NZ8(c->a); CLR(M6801_V); break;
    case 0xC6: c->b=imm8(c); NZ8(c->b); CLR(M6801_V); break;
    case 0xD6: c->b=RD(ea_dir(c)); NZ8(c->b); CLR(M6801_V); break;
    case 0xE6: c->b=RD(ea_idx(c)); NZ8(c->b); CLR(M6801_V); break;
    case 0xF6: c->b=RD(ea_ext(c)); NZ8(c->b); CLR(M6801_V); break;

    /* ---- 16-bit ops: SUBD/ADDD/CPX/LDS/STS/LDX/STX/LDD/STD/JSR ---- */
    case 0x83: d16=imm16(c); goto subd;
    case 0x93: d16=rd16(c,ea_dir(c)); goto subd;
    case 0xA3: d16=rd16(c,ea_idx(c)); goto subd;
    case 0xB3: d16=rd16(c,ea_ext(c)); goto subd;
    subd: { uint16_t D=(c->a<<8)|c->b; unsigned r=D-d16; UPD(M6801_C,r&0x10000);
            UPD(M6801_V,((D^d16)&(D^r))&0x8000); c->a=r>>8; c->b=r; NZ16(r); } break;
    case 0xC3: d16=imm16(c); goto addd;
    case 0xD3: d16=rd16(c,ea_dir(c)); goto addd;
    case 0xE3: d16=rd16(c,ea_idx(c)); goto addd;
    case 0xF3: d16=rd16(c,ea_ext(c)); goto addd;
    addd: { uint16_t D=(c->a<<8)|c->b; unsigned r=D+d16; UPD(M6801_C,r&0x10000);
            UPD(M6801_V,(~(D^d16)&(D^r))&0x8000); c->a=r>>8; c->b=r; NZ16(r); } break;
    case 0x8C: d16=imm16(c); goto cpx;
    case 0x9C: d16=rd16(c,ea_dir(c)); goto cpx;
    case 0xAC: d16=rd16(c,ea_idx(c)); goto cpx;
    case 0xBC: d16=rd16(c,ea_ext(c)); goto cpx;
    cpx: { unsigned r=c->x-d16; UPD(M6801_V,((c->x^d16)&(c->x^r))&0x8000); NZ16(r); } break;
    case 0xCC: c->a=(d16=imm16(c))>>8; c->b=d16; NZ16(d16); CLR(M6801_V); break;   /* LDD imm */
    case 0xDC: d16=rd16(c,ea_dir(c)); c->a=d16>>8; c->b=d16; NZ16(d16); CLR(M6801_V); break;
    case 0xEC: d16=rd16(c,ea_idx(c)); c->a=d16>>8; c->b=d16; NZ16(d16); CLR(M6801_V); break;
    case 0xFC: d16=rd16(c,ea_ext(c)); c->a=d16>>8; c->b=d16; NZ16(d16); CLR(M6801_V); break;
    case 0xDD: wr16(c,ea_dir(c),(c->a<<8)|c->b); d16=(c->a<<8)|c->b; NZ16(d16); CLR(M6801_V); break; /* STD */
    case 0xED: wr16(c,ea_idx(c),(c->a<<8)|c->b); d16=(c->a<<8)|c->b; NZ16(d16); CLR(M6801_V); break;
    case 0xFD: wr16(c,ea_ext(c),(c->a<<8)|c->b); d16=(c->a<<8)|c->b; NZ16(d16); CLR(M6801_V); break;
    case 0xCE: c->x=imm16(c); NZ16(c->x); CLR(M6801_V); break;      /* LDX imm */
    case 0xDE: c->x=rd16(c,ea_dir(c)); NZ16(c->x); CLR(M6801_V); break;
    case 0xEE: c->x=rd16(c,ea_idx(c)); NZ16(c->x); CLR(M6801_V); break;
    case 0xFE: c->x=rd16(c,ea_ext(c)); NZ16(c->x); CLR(M6801_V); break;
    case 0xDF: wr16(c,ea_dir(c),c->x); NZ16(c->x); CLR(M6801_V); break; /* STX */
    case 0xEF: wr16(c,ea_idx(c),c->x); NZ16(c->x); CLR(M6801_V); break;
    case 0xFF: wr16(c,ea_ext(c),c->x); NZ16(c->x); CLR(M6801_V); break;
    case 0x8E: c->sp=imm16(c); NZ16(c->sp); CLR(M6801_V); break;    /* LDS imm */
    case 0x9E: c->sp=rd16(c,ea_dir(c)); NZ16(c->sp); CLR(M6801_V); break;
    case 0xAE: c->sp=rd16(c,ea_idx(c)); NZ16(c->sp); CLR(M6801_V); break;
    case 0xBE: c->sp=rd16(c,ea_ext(c)); NZ16(c->sp); CLR(M6801_V); break;
    case 0x9F: wr16(c,ea_dir(c),c->sp); NZ16(c->sp); CLR(M6801_V); break; /* STS */
    case 0xAF: wr16(c,ea_idx(c),c->sp); NZ16(c->sp); CLR(M6801_V); break;
    case 0xBF: wr16(c,ea_ext(c),c->sp); NZ16(c->sp); CLR(M6801_V); break;
    /* JSR / BSR */
    case 0x8D: { int8_t off=(int8_t)RD(c->pc++); push16(c,c->pc); c->pc+=off; } break; /* BSR */
    case 0x9D: ea=ea_dir(c); push16(c,c->pc); c->pc=ea; break;     /* JSR dir */
    case 0xAD: ea=ea_idx(c); push16(c,c->pc); c->pc=ea; break;     /* JSR idx */
    case 0xBD: ea=ea_ext(c); push16(c,c->pc); c->pc=ea; break;     /* JSR ext */

    default:
        c->trap_op = op ? op : 0x100;   /* record unimplemented opcode */
        c->trap_pc = op_pc;
        c->halted  = true;
        c->pc = op_pc;                  /* rewind so the trap PC is the bad op */
        break;
    }

    /* advance the free-running timer by the cycles this instruction took */
    { unsigned d = (unsigned)(c->cycles - c0);
      const uint16_t compare_distance = (uint16_t)(c->ocr - c->frc);
      if (compare_distance != 0 && compare_distance <= d) {
          c->tcsr |= TCSR_OCF;
          c->pending_tcsr |= TCSR_OCF;
      }
      if ((unsigned)c->frc + d > 0xffff) {
          c->tcsr |= TCSR_TOF;
          c->pending_tcsr |= TCSR_TOF;
      }
      c->frc = (uint16_t)(c->frc + d); }
    return c->trap_op;
}
