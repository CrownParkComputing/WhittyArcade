/* galaga88_machine.c -- Namco System-1 machine for Galaga '88 (set `galaga88`).
 * 3x MC6809E (main/sub/audio @ 1.536 MHz) + HD63701V0 MCU (@ 6.144 MHz) on the
 * CUS117 MMU. Verified against MAME namco/namcos1.cpp + namcos1_m.cpp.
 *
 * The 64 KB 6809 spaces go through CUS117: reads always remap via an 8x 8 KB
 * bank table into a 23-bit virtual space; writes < 0xE000 remap, writes
 * 0xE000-0xFFFF hit the CUS117 register zone (banking / subres / IRQ+FIRQ ack /
 * watchdog / sub-bank7). The virtual space dispatches to the C123 tilemap VRAM,
 * CUS48 spriteram, C116 palette, CUS30 PSG, triram, keycus, work RAM and the
 * 4 MB mainrom. The audio 6809 has its own sound_map (banked ROM + YM2151 stub
 * + CUS30 + triram + sound RAM); the HD63701 MCU has the mcu_map (DIP LS157,
 * P1/P2, banked voice ROM, triram+mcu_patch, DAC, voice bankselect, IRQ ack).
 *
 * YM2151 + DACs are stubbed (status = ready, no IRQ; DAC writes stored) so the
 * audio CPU boots; real sound is layered on afterwards.
 */
#include "namcos1_machine.h"
#include "mc6809.h"
#include "m6801.h"
#include <string.h>
#ifdef HOST_DIAG
#include <stdio.h>
#endif

extern void g88_ym2151_reset(void);
extern void g88_ym2151_write_addr(uint8_t v);
extern void g88_ym2151_write_data(uint8_t v);
extern uint8_t g88_ym2151_read_status(void);
extern int g88_ym2151_irq_active(void);

#define CYCLES_PER_FRAME 25344   /* 1.536 MHz / 60.606 Hz */
#define SLICE 40                 /* 38400 Hz quantum       */
#define MCU_MULT 4               /* 6.144 MHz / 1.536 MHz  */

/* =============== regions (assembled by g88_load) =============== */
static uint8_t mainrom[0x400000];   /* 4 MB program ROM image        */
static uint8_t audiocpu[0x20000];   /* 128 KB sound CPU ROM          */
static uint8_t mcu_introm[0x1000];  /* HD63701 internal ROM (cus64)  */
static uint8_t voice[0xc0000];      /* 768 KB voice ROM (MCU DAC)    */
static uint8_t chrrom[0x100000];    /* 1 MB C123 tile gfx            */
static uint8_t maskrom[0x20000];    /* 128 KB C123 tile mask         */
static uint8_t sprrom[0x100000];    /* 1 MB sprite gfx               */

/* =============== shared / working RAM =============== */
static uint8_t videoram[0x8000];    /* C123 tilemap VRAM (32 KB)     */
static uint8_t spriteram[0x800];    /* CUS48 sprite RAM (2 KB)       */
static uint8_t control[0x20];       /* C123 control regs             */
static uint8_t paletteram[0x8000];  /* C116 palette window (32 KB)   */
static uint8_t cus30[0x400];        /* CUS30 PSG shared RAM          */
static uint8_t triram[0x800];       /* tri-port shared RAM (2 KB)    */
static uint8_t scratchpad[0x800];   /* 2 KB scratchpad               */
static uint8_t workram[0x8000];     /* 32 KB work RAM                */
static uint8_t soundram[0x2000];    /* 8 KB sound RAM                */
static uint8_t nvram[0x800];        /* 2 KB EEPROM                   */
static uint8_t mcu_iram[0x100];     /* HD63701 internal RAM          */

/* =============== CUS117 MMU =============== */
static uint32_t offs[2][8];         /* bank -> virtual base (low 13 bits 0) */
static int      subres;             /* 1 = sub/audio/mcu RUN, 0 = reset held  */
static int      prev_subres;        /* detect 0->1 release transition          */
static int      wdog;               /* watchdog kick bitmask                  */

/* =============== sound / MCU banking =============== */
static int soundbank;               /* 0..7  -> 16 KB of 128 KB audiocpu    */
static int mcubank;                 /* 0..23 -> 32 KB of 768 KB voice       */

/* =============== keycus type-2 (CUS153, id 0x31) =============== */
static uint8_t key_id = 0x12;
static uint8_t key[8];
static uint32_t key_quotient, key_reminder, key_numhi;

/* =============== mcu_patch (triram[0] must stick at 0xA6) =============== */
static uint8_t mcu_patch_data;

/* =============== CPUs =============== */
static mc6809__t main, sub, audio;
static m6801_t   mcu;
static int fault_code, flip_screen, copy_armed;

/* =============== inputs (active-low) =============== */
static uint8_t p1_port = 0xff, p2_port = 0xff, coin_port = 0xff, dipsw = 0xff;

static int fault_cpu_id;
static void on_fault(mc6809__t *c, mc6809fault__t f)
{
    fault_cpu_id = (c == &main) ? 0 : (c == &sub) ? 1 : (c == &audio) ? 2 : 9;
    fault_code = (int)f ? (int)f : 1;
    /* stdio not linked on Amiga; keep diagnostics minimal. */
    (void)fault_cpu_id;
    longjmp(c->err, (int)f ? (int)f : 1);
}

/* =============== keycus type-2 =============== */
static uint8_t keycus_r(uint32_t off)
{
    key_numhi = 0;                       /* any read clears the high word */
    off &= 0x1fff;
    if (off == 0) return (key_reminder >> 8) & 0xff;
    if (off == 1) return  key_reminder & 0xff;
    if (off == 2) return (key_quotient >> 8) & 0xff;
    if (off == 3) return  key_quotient & 0xff;
    if (off == 4) return  key_id;
    return 0;
}
static void keycus_w(uint32_t off, uint8_t data)
{
    off &= 0x1fff;
    if (off > 4) return;
    key[off] = data;
    if (off == 3) {
        uint32_t d = ((uint32_t)key[0] << 8) | key[1];
        uint32_t n = (key_numhi << 16) | ((uint32_t)key[2] << 8) | key[3];
        if (d) { key_quotient = n / d; key_reminder = n % d; }
        else   { key_quotient = 0xffff; key_reminder = 0; }
        key_numhi = ((uint32_t)key[2] << 8) | key[3];
    }
}

/* =============== C116 palette window + clip registers =============== */
static uint16_t c116_regs[8];       /* C116 internal 16-bit regs (left/right/top/bottom/...) */
static uint8_t palette_r(uint32_t off) {
    off &= 0x7fff;
    if ((off & 0x1800) == 0x1800) {            /* register mirror */
        int reg = (off & 0xf) >> 1;
        return (off & 1) ? (c116_regs[reg] & 0xff) : (c116_regs[reg] >> 8);
    }
    return paletteram[off];
}
static void palette_w(uint32_t off, uint8_t v) {
    off &= 0x7fff;
    if ((off & 0x1800) == 0x1800) {            /* C116 register write (even=MSB, odd=LSB) */
        int reg = (off & 0xf) >> 1;
        if (off & 1) c116_regs[reg] = (c116_regs[reg] & 0xff00) | v;
        else         c116_regs[reg] = (c116_regs[reg] & 0x00ff) | (v << 8);
        return;
    }
    paletteram[off] = v;
}

/* =============== virtual space dispatch =============== */
static uint8_t vread(uint32_t va)
{
    if (va >= 0x400000 && va <= 0x7fffff) return mainrom[va - 0x400000];
    if (va >= 0x300000 && va <= 0x307fff) return workram[va - 0x300000];
    if (va >= 0x2ff000 && va <= 0x2fffff) return triram[(va - 0x2ff000) & 0x7ff];
    if (va >= 0x2fe000 && va <= 0x2fefff) return cus30[(va - 0x2fe000) & 0x3ff];
    if (va >= 0x2fd000 && va <= 0x2fdfff) return control[(va - 0x2fd000) & 0x1f];
    if (va >= 0x2fc800 && va <= 0x2fcfff) return spriteram[(va - 0x2fc800) & 0x7ff];
    if (va >= 0x2fc000 && va <= 0x2fc7ff) return scratchpad[va - 0x2fc000];
    if (va >= 0x2f8000 && va <= 0x2f9fff) return keycus_r(va - 0x2f8000);
    if (va >= 0x2f0000 && va <= 0x2f7fff) return videoram[va - 0x2f0000];
    if (va >= 0x2e0000 && va <= 0x2e7fff) return palette_r(va - 0x2e0000);
    return 0xff;
}
static void vwrite(uint32_t va, uint8_t v)
{
    if (va >= 0x400000 && va <= 0x7fffff) return;                 /* ROM */
    if (va >= 0x300000 && va <= 0x307fff) { workram[va - 0x300000] = v; return; }
    if (va >= 0x2ff000 && va <= 0x2fffff) { triram[(va - 0x2ff000) & 0x7ff] = v; return; }
    if (va >= 0x2fe000 && va <= 0x2fefff) { cus30[(va - 0x2fe000) & 0x3ff] = v; return; }
    if (va >= 0x2fd000 && va <= 0x2fdfff) { control[(va - 0x2fd000) & 0x1f] = v; return; }
    if (va >= 0x2fc800 && va <= 0x2fcfff) {
        uint32_t o = (va - 0x2fc800) & 0x7ff;
        spriteram[o] = v;
        if (o == 0x7f2) copy_armed = 1;
        if (o == 0x7f6) flip_screen = v & 1;
        return;
    }
    if (va >= 0x2fc000 && va <= 0x2fc7ff) { scratchpad[va - 0x2fc000] = v; return; }
    if (va >= 0x2f8000 && va <= 0x2f9fff) { keycus_w(va - 0x2f8000, v); return; }
    if (va >= 0x2f0000 && va <= 0x2f7fff) { videoram[va - 0x2f0000] = v; return; }
    if (va >= 0x2e0000 && va <= 0x2e7fff) { palette_w(va - 0x2e0000, v); return; }
    if (va >= 0x2c0000 && va <= 0x2c1fff) return;                 /* 3dcs */
}

/* =============== CUS117 register zone + banking =============== */
static void bankswitch(int cpu, int bank, int a0, uint8_t data)
{
    if (a0 == 0)
        offs[cpu][bank] = (offs[cpu][bank] & 0x1fe000) | (uint32_t)((data & 3) * 0x200000);
    else
        offs[cpu][bank] = (offs[cpu][bank] & 0x600000) | (uint32_t)(data * 0x2000);
}
static void register_w(int cpu, uint16_t a, uint8_t v)
{
    int reg = (a >> 9) & 0xf;
    if (reg <= 7) { bankswitch(cpu, reg, a & 1, v); return; }
    switch (reg) {
        case 8:  if (cpu == 0) { subres = v & 1;
                 if (subres && !prev_subres) {   /* 0->1 release: pulse RESET on the held
                     * CPUs so they re-fetch their reset vectors from the now-configured
                     * banks (main sets sub bank7 via reg 14 before releasing) -- mirrors
                     * MAME subres_cb(CLEAR_LINE) pulsing INPUT_LINE_RESET. */
                     mc6809_reset(&sub); mc6809_reset(&audio); m6801_reset(&mcu);
                     sub.cycles = main.cycles; audio.cycles = main.cycles; mcu.cycles = main.cycles;
                 }
                 prev_subres = subres; } break;          /* SUBRES (main only) */
        case 9:  wdog |= (1 << cpu); break;                    /* watchdog kick */
        case 11: if (cpu == 0) main.irq = false; else if (cpu == 1) sub.irq = false;
                 else audio.irq = false; break;                /* IRQ ack */
        case 12: if (cpu == 0) main.firq = false; else if (cpu == 1) sub.firq = false;
                 else audio.firq = false; break;               /* FIRQ ack */
        case 13: if (cpu == 0) sub.firq = true; break;         /* assert sub FIRQ */
        case 14: if (cpu == 0) offs[1][7] = 0x600000 | (uint32_t)(v * 0x2000); break;
        default: break;
    }
}

static uint32_t remap(int cpu, uint16_t a) { return offs[cpu][a >> 13] | (a & 0x1fff); }

/* =============== main / sub 6809 maps =============== */
static mc6809byte__t main_rd(mc6809__t *c, mc6809addr__t a, bool ifetch)
{ (void)c; (void)ifetch; return vread(remap(0, a)); }
static void main_wr(mc6809__t *c, mc6809addr__t a, mc6809byte__t v)
{ (void)c; if (a < 0xE000) vwrite(remap(0, a), v); else register_w(0, a, v); }
static mc6809byte__t sub_rd(mc6809__t *c, mc6809addr__t a, bool ifetch)
{ (void)c; (void)ifetch; return vread(remap(1, a)); }
static void sub_wr(mc6809__t *c, mc6809addr__t a, mc6809byte__t v)
{ (void)c; if (a < 0xE000) vwrite(remap(1, a), v); else register_w(1, a, v); }

/* =============== audio 6809 map =============== */
static mc6809byte__t snd_rd(mc6809__t *c, mc6809addr__t a, bool ifetch)
{
    (void)c; (void)ifetch;
    if (a < 0x4000)  return audiocpu[soundbank * 0x4000 + a];
    if (a == 0x4000 || a == 0x4001) return g88_ym2151_read_status();
    if (a >= 0x5000 && a <= 0x5fff) return cus30[(a - 0x5000) & 0x3ff];
    if (a >= 0x7000 && a <= 0x7fff) return triram[(a - 0x7000) & 0x7ff];
    if (a >= 0x8000 && a <= 0x9fff) return soundram[a - 0x8000];
    if (a >= 0xc000)               return audiocpu[a - 0xc000];  /* fixed top 16 KB */
    return 0xff;
}
static void snd_wr(mc6809__t *c, mc6809addr__t a, mc6809byte__t v)
{
    (void)c;
    if (a < 0x4000) return;
    if (a == 0x4000) { g88_ym2151_write_addr(v); return; }
    if (a == 0x4001) { g88_ym2151_write_data(v); return; }
    if (a >= 0x5000 && a <= 0x5fff) { cus30[(a - 0x5000) & 0x3ff] = v; return; }
    if (a >= 0x7000 && a <= 0x7fff) { triram[(a - 0x7000) & 0x7ff] = v; return; }
    if (a >= 0x8000 && a <= 0x9fff) { soundram[a - 0x8000] = v; return; }
    if (a == 0xc000 || a == 0xc001) { soundbank = (v & 0x70) >> 4; return; }
    if (a == 0xd001) { wdog |= 4; return; }            /* sound watchdog -> kick audio */
    if (a == 0xe000) { audio.irq = false; return; }    /* audio IRQ ack */
}

/* =============== HD63701 MCU map =============== */
static uint8_t dsw_r(uint32_t o)
{
    int sel = (o >> 1) & 1;
    int nib = sel ? ((dipsw >> 4) & 0xf) : (dipsw & 0xf);
    return 0xf0 | nib;
}
static void mcu_patch_w(uint8_t v)
{
    if (mcu_patch_data == 0xa6) return;     /* once 0xA6 sticks, ignore further writes */
    mcu_patch_data = v;
    triram[0] = v;
}
static void mcu_bankswitch(uint8_t v)
{
    int b = 0;
    switch (v & 0xfc) {
        case 0xf8: b = 0;  v ^= 2; break;   /* ROM0, A16 inverted */
        case 0xf4: b = 4;  break;
        case 0xec: b = 8;  break;
        case 0xdc: b = 12; break;
        case 0xbc: b = 16; break;
        case 0x7c: b = 20; break;
        default:   b = 0;  break;
    }
    mcubank = b + (v & 3);
}
static uint8_t mrd(m6801_t *c, uint16_t a)
{
    (void)c;
    if (a < 0x100) {
        if (a == 0x02) return coin_port;       /* P1 = COIN input  */
        if (a == 0x03) return 0xff;            /* P2 = output      */
        return mcu_iram[a];
    }
    if (a >= 0x1000 && a <= 0x1003) return dsw_r(a - 0x1000);
    if (a == 0x1400) return p1_port;
    if (a == 0x1401) return p2_port;
    if (a >= 0x4000 && a <= 0xbfff) return voice[mcubank * 0x8000 + (a - 0x4000)];
    if (a >= 0xc000 && a <= 0xc7ff) return triram[a - 0xc000];
    if (a >= 0xc800 && a <= 0xcfff) return nvram[a - 0xc800];
    if (a >= 0xf000)               return mcu_introm[a - 0xf000];
    return 0xff;
}
static void mwr(m6801_t *c, uint16_t a, uint8_t v)
{
    (void)c;
    if (a < 0x100) {
        mcu_iram[a] = v;
        /* P1 output -> coin counters, P2 output -> DAC gain: ignored on host */
        return;
    }
    if (a >= 0x1000 && a <= 0x1003) return;
    if (a >= 0x4000 && a <= 0xbfff) return;
    if (a >= 0xc000 && a <= 0xc7ff) {
        if (a == 0xc000) mcu_patch_w(v);
        else             triram[a - 0xc000] = v;
        return;
    }
    if (a >= 0xc800 && a <= 0xcfff) { nvram[a - 0xc800] = v; return; }
    if (a == 0xd000 || a == 0xd400) return;   /* DAC data (host: ignored) */
    if (a == 0xd800) { mcu_bankswitch(v); return; }
    if (a == 0xf000) { mcu.irq1 = false; return; }   /* MCU IRQ ack */
}

/* =============== vblank =============== */
static void copy_sprites(void)
{
    if (!copy_armed) return;
    for (int i = 0; i < 127; i++) {
        uint8_t *s = spriteram + i * 0x10;
        for (int j = 4; j <= 9; j++) s[j + 6] = s[j];   /* 4..9 -> 10..15 */
    }
    copy_armed = 0;
}
static void vblank_rising(void)
{
    copy_sprites();
    main.irq = true;
    sub.irq  = true;
}
static void vblank_falling(void)
{
    audio.irq = true;
    mcu.irq1  = true;
}

/* =============== public API =============== */
static void load64(uint8_t *dst, int base, const uint8_t *src)
{
    memcpy(dst + base, src, 0x10000);
}
static void reload512(uint8_t *region, int base, const uint8_t *src)
{
    for (int i = 0; i < 8; i++) memcpy(region + base + i * 0x10000, src, 0x10000);
}
static void reload128(uint8_t *region, int base, const uint8_t *src)
{
    memcpy(region + base, src, 0x10000);
    memcpy(region + base + 0x10000, src, 0x10000);   /* ROM_LOAD_HS: reload once -> 128 KB */
}
void g88_load(const uint8_t *p0,const uint8_t *p1,const uint8_t *p5,
              const uint8_t *p6,const uint8_t *p7,
              const uint8_t *s0,const uint8_t *s1,const uint8_t *mcu,
              const uint8_t *v0,const uint8_t *v1,const uint8_t *v2,
              const uint8_t *v3,const uint8_t *v4,const uint8_t *v5,
              const uint8_t *chr0,const uint8_t *chr1,const uint8_t *chr2,
              const uint8_t *chr3,const uint8_t *mask,
              const uint8_t *obj0,const uint8_t *obj1,const uint8_t *obj2,
              const uint8_t *obj3,const uint8_t *obj4,const uint8_t *obj5)
{
    memset(mainrom, 0, sizeof mainrom);
    reload512(mainrom, 0x000000, p0);
    reload512(mainrom, 0x080000, p1);
    reload512(mainrom, 0x280000, p5);
    reload512(mainrom, 0x300000, p6);
    reload512(mainrom, 0x380000, p7);
    /* driver_init PRG7 bit-16 inversion on the 0x380000 slot (no-op on
     * replicated ROM_LOAD_512 data, but faithful to MAME): swap the two 64 KB
     * halves within each 128 KB sub-block where !(i & 0x10000). */
    for (uint32_t i = 0; i < 0x80000; i++)
        if (!(i & 0x010000)) {
            uint8_t t = mainrom[0x380000 + i];
            mainrom[0x380000 + i] = mainrom[0x380000 + i + 0x010000];
            mainrom[0x380000 + i + 0x010000] = t;
        }

    memset(audiocpu, 0, sizeof audiocpu);
    load64(audiocpu, 0x00000, s0);
    load64(audiocpu, 0x10000, s1);

    memcpy(mcu_introm, mcu, sizeof mcu_introm);

    memset(voice, 0, sizeof voice);
    reload128(voice, 0x00000, v0);
    reload128(voice, 0x20000, v1);
    reload128(voice, 0x40000, v2);
    reload128(voice, 0x60000, v3);
    reload128(voice, 0x80000, v4);
    reload128(voice, 0xa0000, v5);

    memset(chrrom, 0, sizeof chrrom);
    memcpy(chrrom + 0x00000, chr0, 0x20000);
    memcpy(chrrom + 0x20000, chr1, 0x20000);
    memcpy(chrrom + 0x40000, chr2, 0x20000);
    memcpy(chrrom + 0x60000, chr3, 0x20000);
    memcpy(maskrom, mask, sizeof maskrom);
    memset(sprrom, 0, sizeof sprrom);
    memcpy(sprrom + 0x00000, obj0, 0x20000);
    memcpy(sprrom + 0x20000, obj1, 0x20000);
    memcpy(sprrom + 0x40000, obj2, 0x20000);
    memcpy(sprrom + 0x60000, obj3, 0x20000);
    memcpy(sprrom + 0x80000, obj4, 0x20000);
    memcpy(sprrom + 0xa0000, obj5, 0x20000);
    (void)key;
}

void g88_load_pacmania(const uint8_t *p6,const uint8_t *p7,
                       const uint8_t *s0,const uint8_t *s1,
                       const uint8_t *mcu,const uint8_t *v0,
                       const uint8_t *chr0,const uint8_t *chr1,
                       const uint8_t *chr2,const uint8_t *chr3,
                       const uint8_t *mask,
                       const uint8_t *obj0,const uint8_t *obj1)
{
    memset(mainrom, 0, sizeof mainrom);
    for (int i = 0; i < 4; ++i)
        memcpy(mainrom + 0x300000 + i * 0x20000, p6, 0x20000);
    reload512(mainrom, 0x380000, p7);
    for (uint32_t i = 0; i < 0x80000; i++)
        if (!(i & 0x010000)) {
            uint8_t t = mainrom[0x380000 + i];
            mainrom[0x380000 + i] = mainrom[0x380000 + i + 0x010000];
            mainrom[0x380000 + i + 0x010000] = t;
        }

    memcpy(audiocpu + 0x00000, s0, 0x10000);
    memcpy(audiocpu + 0x10000, s1, 0x10000);
    memcpy(mcu_introm, mcu, sizeof mcu_introm);
    memset(voice, 0, sizeof voice);
    reload128(voice, 0, v0);
    memset(chrrom, 0, sizeof chrrom);
    memcpy(chrrom + 0x00000, chr0, 0x20000);
    memcpy(chrrom + 0x20000, chr1, 0x20000);
    memcpy(chrrom + 0x40000, chr2, 0x20000);
    memcpy(chrrom + 0x60000, chr3, 0x20000);
    memset(maskrom, 0, sizeof maskrom);
    memcpy(maskrom, mask, 0x10000);
    memset(sprrom, 0, sizeof sprrom);
    memcpy(sprrom + 0x00000, obj0, 0x20000);
    memcpy(sprrom + 0x20000, obj1, 0x20000);
    key_id = 0x12;
}

void g88_reset(void)
{
    memset(videoram, 0, sizeof videoram);
    memset(spriteram, 0, sizeof spriteram);
    memset(control, 0, sizeof control);
    memset(paletteram, 0, sizeof paletteram);
    memset(cus30, 0, sizeof cus30);
    memset(triram, 0, sizeof triram);
    memset(scratchpad, 0, sizeof scratchpad);
    memset(workram, 0, sizeof workram);
    memset(soundram, 0, sizeof soundram);
    memset(nvram, 0, sizeof nvram);
    memset(mcu_iram, 0, sizeof mcu_iram);
    memset(offs, 0, sizeof offs);
    /* CUS117 device_reset defaults (c117.cpp): main bank0+bank1=RAM(0x300000),
     * main bank7=PRG7(0x7FE000); sub bank0=RAM, sub bank7=PRG7 (sub bank1 left 0).
     * main and sub share the reset vector in PRG7 and differentiate only by
     * their bank tables (no CPU-ID register). */
    offs[0][0] = 0x300000; offs[0][1] = 0x300000; offs[0][7] = 0x7FE000;
    offs[1][0] = 0x300000;                       offs[1][7] = 0x7FE000;
    subres = 0; prev_subres = 0; wdog = 0; soundbank = 0; mcubank = 0;
    key_quotient = key_reminder = key_numhi = 0;
    memset(key, 0, sizeof key);
    mcu_patch_data = 0; fault_code = 0; flip_screen = 0; copy_armed = 0;
    g88_ym2151_reset();
    memset(c116_regs, 0, sizeof c116_regs);

    main.read = main_rd; main.write = main_wr; main.fault = on_fault; main.user = 0;
    sub.read  = sub_rd;  sub.write  = sub_wr;  sub.fault  = on_fault; sub.user  = 0;
    audio.read= snd_rd;  audio.write= snd_wr;  audio.fault= on_fault; audio.user= 0;
    mc6809_reset(&main); mc6809_reset(&sub); mc6809_reset(&audio);
    mcu.read = mrd; mcu.write = mwr;
    m6801_reset(&mcu);
}

void g88_run_frame(void)
{
    unsigned long target = main.cycles + CYCLES_PER_FRAME;
    vblank_rising();
    while (main.cycles < target && !fault_code) {
        unsigned long ms = main.cycles + SLICE;
        while (main.cycles < ms && !fault_code) { if (mc6809_step(&main)) break; }
        if (subres) {
            while (sub.cycles  < ms && !fault_code) { if (mc6809_step(&sub))  break; }
            while (audio.cycles < ms && !fault_code){
                if (g88_ym2151_irq_active()) audio.firq = true;
                if (mc6809_step(&audio))break;
            }
            if (g88_ym2151_irq_active()) audio.firq = true;
            unsigned long es = mcu.cycles + MCU_MULT * SLICE;
            while (mcu.cycles < es && !mcu.trap_op) m6801_step(&mcu);
            if (mcu.trap_op) break;
        }
    }
    if (subres) vblank_falling();
}

/* ---- video state accessors ---- */
uint8_t *g88_videoram(void)  { return videoram; }
uint8_t *g88_spriteram(void) { return spriteram; }
uint8_t *g88_control(void)   { return control; }
uint8_t *g88_paletteram(void){ return paletteram; }
uint8_t *g88_cus30_base(void){ return cus30; }
uint8_t  g88_cus30(unsigned i) { return cus30[i & 0x3ff]; }
uint8_t *g88_triram(void)      { return triram; }
unsigned g88_c116_reg(unsigned n) { return c116_regs[n & 7]; }   /* 16-bit C116 clip reg */
int      g88_mcu_patch_done(void) { return mcu_patch_data == 0xa6; }
void     g88_dump_banks(int cpu, unsigned *out8) { for (int i=0;i<8;i++) out8[i] = offs[cpu&1][i]; }

/* ---- inputs ---- */
void g88_set_p1(uint8_t v) { p1_port = v; }
void g88_set_p2(uint8_t v) { p2_port = v; }
void g88_set_coin(uint8_t v){ coin_port = v; }
void g88_set_dipsw(uint8_t v){ dipsw = v; }

/* ---- introspection ---- */
unsigned g88_pc(int cpu) {
    if (cpu == 0) return main.pc.w;
    if (cpu == 1) return sub.pc.w;
    return audio.pc.w;
}
unsigned g88_instpc(int cpu) {
    if (cpu == 0) return main.instpc;
    if (cpu == 1) return sub.instpc;
    return audio.instpc;
}
unsigned      g88_mcu_pc(void)    { return mcu.pc; }
int           g88_fault(void)     { return fault_code; }
int           g88_mcu_trap(void)  { return mcu.trap_op; }
unsigned long g88_cycles(int cpu) { return cpu==0?main.cycles : cpu==1?sub.cycles : audio.cycles; }
uint8_t       g88_peek_main(unsigned a){ return vread(remap(0, (mc6809addr__t)a)); }

/* gfx ROM accessors for the renderer (defined here, next to the regions) */
const uint8_t *g88_chrrom(void)  { return chrrom; }
const uint8_t *g88_maskrom(void) { return maskrom; }
const uint8_t *g88_sprrom(void)  { return sprrom; }
int            g88_flip(void)    { return flip_screen; }
