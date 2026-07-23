/* galaga88_machine.h -- portable Namco System-1 machine model for Galaga '88.
 * 3x MC6809E (main/sub/audio @ 1.536 MHz) + HD63701V0 MCU (@ 6.144 MHz),
 * sharing the CUS117 MMU banked 4 MB ROM space, CUS116 palette, C123 tilemap
 * VRAM, CUS48 spriteram, CUS30 PSG, triram, keycus type-2 protection (CUS153,
 * id 0x31) + the triram[0]=0xA6 mcu_patch boot kludge. Verified against MAME
 * namco/namcos1.cpp (set `galaga88`, init_galaga88). No Amiga deps.
 *
 * See galaga88_machine.c for the full address maps + CUS117 banking.
 */
#ifndef GALAGA88_MACHINE_H
#define GALAGA88_MACHINE_H
#include <stdint.h>

/* Load the raw ROM blobs and assemble the regions (mirrors MAME ROM_START +
 * driver_init: ROM_LOAD_512 reloads, ROM_LOAD_HS, PRG7 bit-16 inversion). */
void g88_load(const uint8_t *p0,const uint8_t *p1,const uint8_t *p5,
              const uint8_t *p6,const uint8_t *p7,            /* mainrom 5x64K  */
              const uint8_t *s0,const uint8_t *s1,            /* audiocpu 2x64K*/
              const uint8_t *mcu,                             /* HD63701 4K    */
              const uint8_t *v0,const uint8_t *v1,const uint8_t *v2,
              const uint8_t *v3,const uint8_t *v4,const uint8_t *v5, /* voice 6x64K */
              const uint8_t *chr0,const uint8_t *chr1,
              const uint8_t *chr2,const uint8_t *chr3,        /* c123tmap 4x128K*/
              const uint8_t *mask,                            /* c123tmap mask 128K */
              const uint8_t *obj0,const uint8_t *obj1,const uint8_t *obj2,
              const uint8_t *obj3,const uint8_t *obj4,const uint8_t *obj5); /* sprite 6x128K */

void g88_load_pacmania(const uint8_t *p6,const uint8_t *p7,
                       const uint8_t *s0,const uint8_t *s1,
                       const uint8_t *mcu,const uint8_t *v0,
                       const uint8_t *chr0,const uint8_t *chr1,
                       const uint8_t *chr2,const uint8_t *chr3,
                       const uint8_t *mask,
                       const uint8_t *obj0,const uint8_t *obj1);

void g88_reset(void);
void g88_run_frame(void);     /* one ~60.61 Hz frame: vblank IRQs + 4-CPU interleave */

/* ---- video state for the renderer ---- */
uint8_t *g88_videoram(void);   /* C123 tilemap VRAM, 0x8000 (32 KB)            */
uint8_t *g88_spriteram(void);  /* CUS48 sprite RAM, 0x800 (2 KB)               */
uint8_t *g88_control(void);    /* C123 control regs, 0x20 bytes                */
uint8_t *g88_paletteram(void); /* C116 palette window, 0x8000 (32 KB)          */
uint8_t  g88_cus30(unsigned i);/* shared CUS30 PSG RAM byte i (0..0x3ff) */
uint8_t *g88_cus30_base(void);
uint8_t *g88_triram(void);     /* tri-port shared RAM (2 KB)                  */
unsigned g88_c116_reg(unsigned n); /* 16-bit C116 clip reg (0=left,1=right,2=top,3=bottom) */
int      g88_mcu_patch_done(void); /* 1 once triram[0] has stuck at 0xA6       */
void     g88_dump_banks(int cpu, unsigned *out8); /* fill 8 bank-table entries   */

/* gfx ROM accessors for the renderer */
const uint8_t *g88_chrrom(void);   /* 1 MB C123 tile gfx            */
const uint8_t *g88_maskrom(void);  /* 128 KB C123 tile mask (1 bpp) */
const uint8_t *g88_sprrom(void);   /* 1 MB sprite gfx (32x32 4bpp)  */
int g88_flip(void);                /* global flip-screen state      */

/* ---- inputs (active-low; 0xff = nothing) ---- */
/* P1/P2: bit0=R bit1=L bit2=D bit3=U bit4=BTN1 bit5=BTN2 bit6=BTN3 bit7=START */
void g88_set_p1(uint8_t v);
void g88_set_p2(uint8_t v);
/* COIN: bit4=COIN1 bit3=COIN2 bit6=SERVICE1 bit5=SERVICE2 (active-low) */
void g88_set_coin(uint8_t v);
void g88_set_dipsw(uint8_t v);  /* 8 DIP switches (active-low), LS157 nibble-muxed */

/* ---- introspection / debug ---- */
unsigned g88_pc(int cpu);       /* cpu 0=main 1=sub 2=audio */
unsigned g88_instpc(int cpu);   /* pc of the faulting instruction (main/sub/audio) */
unsigned g88_mcu_pc(void);
int      g88_fault(void);
int      g88_mcu_trap(void);
unsigned long g88_cycles(int cpu);
uint8_t  g88_peek_main(unsigned addr);

#endif
