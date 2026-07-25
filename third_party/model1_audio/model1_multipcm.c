// license:BSD-3-Clause
// Framework-free adaptation of MAME's MultiPCM/GEW implementation.
/*
 * Geometrizer - Sega Model 1 Emulator
 * audio/multipcm.c
 *
 * Yamaha YMW258-F / Sega 315-5560 "MultiPCM" sound chip.
 * Ported from MAME gew.cpp + multipcm.cpp (BSD-3-Clause, M.A. Horna).
 *
 * 28 voices, 8-bit signed PCM, 4-stage EG, pitch+amplitude LFO, pan.
 * Native rate: 10 MHz / 224 = 44642 Hz.
 */

#include "model1_multipcm.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef BIT
#define BIT(value, bit) (((value) >> (bit)) & 1u)
#endif

/* -------------------------------------------------------------------------
 * Fixed-point shifts (match MAME gew.h constants)
 * ------------------------------------------------------------------------- */

#define TL_SHIFT    12u         /* total-level accumulator fractional bits  */
#define EG_SHIFT    16u         /* envelope generator fractional bits       */
#define LFO_SHIFT    8u         /* LFO phase fractional bits                */

#define NUM_VOICES   28
#define TL_TABLE_SIZE 0x800     /* 0x80 levels ÃÂ 0x10 pan positions         */
#define EG_TABLE_SIZE 0x400     /* linearÃ¢ÂÂexp lookup                        */

/* -------------------------------------------------------------------------
 * Internal data structures
 * ------------------------------------------------------------------------- */

typedef enum {
    EG_ATTACK  = 0,
    EG_DECAY1  = 1,
    EG_DECAY2  = 2,
    EG_RELEASE = 3
} eg_state_t;

typedef struct {
    u32 start;          /* sample start address (22-bit ROM address space) */
    u32 loop;           /* loop start offset in samples                    */
    u32 end;            /* sample end offset in samples                    */
    u8  attack_reg;
    u8  decay1_reg;
    u8  decay2_reg;
    u8  decay_level;
    u8  release_reg;
    u8  key_rate_scale;
    u8  lfo_vibrato_reg;
    u8  lfo_amplitude_reg;
    u8  format;         /* bit2 = 12-bit mode (not used by MultiPCM basic) */
} sample_desc_t;

typedef struct {
    s32        volume;          /* current EG volume (fixed-point, EG_SHIFT) */
    eg_state_t state;
    s32        attack_rate;
    s32        decay1_rate;
    s32        decay2_rate;
    s32        release_rate;
    s32        decay_level;     /* 0xf - raw decay_level field               */
} eg_t;

typedef struct {
    u32        phase;           /* current LFO phase (fixed-point, LFO_SHIFT) */
    u32        phase_step;      /* phase increment per sample                  */
    const s32 *table;           /* pointer into pitch_table or amplitude_table */
    const s32 *scale;           /* pointer into scale table for current depth  */
} lfo_t;

typedef struct {
    u8          regs[8];        /* raw register bytes written by CPU           */
    bool        playing;
    sample_desc_t sample;
    u32         offset;         /* read position in sample (fixed TL_SHIFT)   */
    u8          octave;         /* 4-bit signed octave (raw from reg 3 hi)     */
    u16         pitch;          /* 10-bit pitch fnum                           */
    u32         step;           /* pitch step per output sample (TL_SHIFT)     */
    bool        reverse;
    u32         pan;            /* 4-bit pan code                              */
    u32         total_level;    /* current TL interpolation (TL_SHIFT)         */
    u32         dest_total_level; /* target TL from register write             */
    s32         total_level_step;
    s32         prev_sample;
    eg_t        eg;
    u8          lfo_frequency;
    lfo_t       pitch_lfo;
    u8          vibrato;        /* pitch LFO depth index                       */
    lfo_t       amplitude_lfo;
    u8          tremolo;        /* amplitude LFO depth index                   */
} slot_t;

struct multipcm {
    /* ROM */
    const u8   *rom;
    u32         rom_size;
    u32         bank_offset;    /* upper 1 MB bank window base                 */

    /* Voices */
    slot_t      slots[NUM_VOICES];
    s32         cur_slot;       /* currently addressed voice (-1 = invalid)    */
    u32         address;        /* currently addressed register (0..7)         */

    /* Lookup tables (allocated once in multipcm_create) */
    u32        *attack_step;    /* [64]  */
    u32        *decay_step;     /* [64]  */
    u32        *freq_step;      /* [0x400] pitch fnum Ã¢ÂÂ step                   */
    s32        *left_pan;       /* [TL_TABLE_SIZE]                             */
    s32        *right_pan;      /* [TL_TABLE_SIZE]                             */
    s32        *lin_to_exp;     /* [EG_TABLE_SIZE] linearÃ¢ÂÂexponential          */
    s32         tl_step_dn;     /* TL interpolation step (decrease)            */
    s32         tl_step_up;     /* TL interpolation step (increase)            */

    /* LFO tables */
    s32        *pitch_table;            /* [256] raw LFO waveform              */
    s32        *amplitude_table;        /* [256]                               */
    s32        *pitch_scale[8];         /* [8][256] PM depth Ã¢ÂÂ fixed mult      */
    s32        *amplitude_scale[8];     /* [8][256] AM depth Ã¢ÂÂ fixed mult      */

    /* Tick-from-M68K-fiber ring + Q32 fractional sample accumulator.
     * MULTIPCM_NATIVE_RATE / 10 MHz is approximately 0.00446 samples per
     * M68K cycle, so the
     * fractional part matters across slices. Size is power-of-two for cheap
     * masking. */
    s32        *tick_ring;              /* [TICK_RING_FRAMES * 2] interleaved */
    u32         tick_ring_w;
    u32         tick_ring_r;
    u64         tick_cycle_frac;
};

#define TICK_RING_FRAMES      4096u
#define TICK_RING_MASK        (TICK_RING_FRAMES - 1u)
static const u64 TICK_STEP_PER_M68K_CYCLE =
    ((u64)MULTIPCM_NATIVE_RATE << 32) / 10000000u;

/* -------------------------------------------------------------------------
 * Voice-slot mapping (MAME multipcm.cpp, slots 7/15/23/31 are invalid)
 * ------------------------------------------------------------------------- */

static const s32 VALUE_TO_CHANNEL[32] = {
     0,  1,  2,  3,  4,  5,  6, -1,
     7,  8,  9, 10, 11, 12, 13, -1,
    14, 15, 16, 17, 18, 19, 20, -1,
    21, 22, 23, 24, 25, 26, 27, -1,
};

/* -------------------------------------------------------------------------
 * Envelope rate base times (milliseconds at 44100 Hz, from MAME gew.cpp)
 * ------------------------------------------------------------------------- */

static const double BASE_TIMES[64] = {
    0,          0,          0,          0,
    6222.95,    4978.37,    4148.66,    3556.01,
    3111.47,    2489.21,    2074.33,    1778.00,
    1555.74,    1244.63,    1037.19,     889.02,
     777.87,     622.31,     518.59,     444.54,
     388.93,     311.16,     259.32,     222.27,
     194.47,     155.60,     129.66,     111.16,
      97.23,      77.82,      64.85,      55.60,
      48.62,      38.91,      32.43,      27.80,
      24.31,      19.46,      16.24,      13.92,
      12.15,       9.75,       8.12,       6.98,
       6.08,       4.90,       4.08,       3.49,
       3.04,       2.49,       2.13,       1.90,
       1.72,       1.41,       1.18,       1.04,
       0.91,       0.73,       0.59,       0.50,
       0.45,       0.45,       0.45,       0.45
};

/* Attack/decay rate ratio used by MultiPCM (MAME: 14.32833) */
#define ATTACK_DECAY_RATIO 14.32833

/* LFO frequency table (Hz) */
static const float LFO_FREQ[8] = {
    0.168f, 2.019f, 3.196f, 4.206f, 5.215f, 5.888f, 6.224f, 7.066f
};

/* Pitch LFO depth table (cents) */
static const float PHASE_SCALE_LIMIT[8] = {
    0.0f, 3.378f, 5.065f, 6.750f, 10.114f, 20.170f, 40.180f, 79.307f
};

/* Amplitude LFO depth table (dB) */
static const float AMPLITUDE_SCALE_LIMIT[8] = {
    0.0f, 0.4f, 0.8f, 1.5f, 3.0f, 6.0f, 12.0f, 24.0f
};

/* -------------------------------------------------------------------------
 * Helper: floatÃ¢ÂÂfixed conversion
 * ------------------------------------------------------------------------- */

static inline u32 value_to_fixed(u32 bits, float value)
{
    return (u32)((float)(1u << bits) * value);
}

/* -------------------------------------------------------------------------
 * ROM access: matches MAME segam1audio mpcm{1,2}_map address decode.
 *   0x000000-0x0FFFFF : direct ROM read (lower 1 MB, fixed)
 *   0x100000-0x1FFFFF : banked 1 MB window, bank_offset selects which
 *                       1 MB page of the sample ROM is exposed there.
 * The chip's 22-bit address bus exists but segam1audio only routes the
 * lower 2 MB to it Ã¢ÂÂ addresses Ã¢ÂÂ¥ 0x200000 should never be issued.
 * ------------------------------------------------------------------------- */

static inline u8 rom_read(const multipcm_t *m, u32 addr22)
{
    u32 phys;
    if (addr22 & 0x100000u) {
        phys = m->bank_offset + (addr22 & 0x0FFFFFu);
    } else {
        phys = addr22 & 0x0FFFFFu;
    }
    if (phys >= m->rom_size) {
        return 0;
    }
    return m->rom[phys];
}

/* -------------------------------------------------------------------------
 * Sample header decode (12 bytes per entry at ROM offset 0)
 * Matches MAME multipcm_device::init_sample().
 * ------------------------------------------------------------------------- */

static void init_sample(const multipcm_t *m, sample_desc_t *s, u32 index)
{
    u32 addr = index * 12u;
    u32 raw_start = ((u32)rom_read(m, addr) << 16)
                  | ((u32)rom_read(m, addr + 1) << 8)
                  |  (u32)rom_read(m, addr + 2);

    s->format  = (u8)((raw_start >> 20) & 0xFEu);
    s->start   = raw_start & 0x3FFFFFu;
    s->loop    = ((u32)rom_read(m, addr + 3) << 8) | rom_read(m, addr + 4);
    /* end stored as 2's-complement negation of sample count */
    s->end     = 0x10000u - (((u32)rom_read(m, addr + 5) << 8)
                              | rom_read(m, addr + 6));
    /* byte 7 = LFO freq + vibrato depth (written back to reg 6) */
    s->lfo_vibrato_reg  = rom_read(m, addr + 7);
    /* byte 8 = attack[7:4] + decay1[3:0] */
    u8 b8 = rom_read(m, addr + 8);
    s->attack_reg  = (b8 >> 4) & 0xFu;
    s->decay1_reg  = b8 & 0xFu;
    /* byte 9 = sustain_level[7:4] + decay2[3:0] */
    u8 b9 = rom_read(m, addr + 9);
    s->decay_level = (b9 >> 4) & 0xFu;
    s->decay2_reg  = b9 & 0xFu;
    /* byte 10 = key_rate_scale[7:4] + release[3:0] */
    u8 b10 = rom_read(m, addr + 10);
    s->key_rate_scale = (b10 >> 4) & 0xFu;
    s->release_reg    = b10 & 0xFu;
    /* byte 11 = amplitude LFO depth (written back to reg 7) */
    s->lfo_amplitude_reg = rom_read(m, addr + 11) & 0xFu;
}

/* -------------------------------------------------------------------------
 * Envelope generator
 * ------------------------------------------------------------------------- */

static u32 eg_get_rate(const u32 *steps, s32 rate, u32 val)
{
    if (val == 0)    return steps[0];
    if (val == 0xF)  return steps[0x3F];
    s32 r = 4 * (s32)val + rate;
    if (r < 0)    r = 0;
    if (r > 0x3F) r = 0x3F;
    return steps[r];
}

static void eg_calc(multipcm_t *m, slot_t *sl)
{
    s32 octave = sl->octave;
    if (octave & 8) octave -= 16;   /* sign-extend 4-bit */

    s32 rate;
    if (sl->sample.key_rate_scale != 0xF) {
        rate = (octave + (s32)sl->sample.key_rate_scale) * 2
             + (s32)BIT(sl->pitch, 9);
    } else {
        rate = 0;
    }

    sl->eg.attack_rate   = (s32)eg_get_rate(m->attack_step, rate, sl->sample.attack_reg);
    sl->eg.decay1_rate   = (s32)eg_get_rate(m->decay_step,  rate, sl->sample.decay1_reg);
    sl->eg.decay2_rate   = (s32)eg_get_rate(m->decay_step,  rate, sl->sample.decay2_reg);
    sl->eg.release_rate  = (s32)eg_get_rate(m->decay_step,  rate, sl->sample.release_reg);
    sl->eg.decay_level   = (s32)(0xF - sl->sample.decay_level);
}

/*
 * Returns the EG volume multiplier (fixed TL_SHIFT) for the current step.
 * Sets slot->playing = false when release phase decays to zero.
 */
static s32 eg_update(multipcm_t *m, slot_t *sl)
{
    switch (sl->eg.state) {
    case EG_ATTACK:
        sl->eg.volume += sl->eg.attack_rate;
        if (sl->eg.volume >= (s32)(0x3FF << EG_SHIFT)) {
            sl->eg.state   = EG_DECAY1;
            if (sl->eg.decay1_rate >= (s32)(0x400 << EG_SHIFT)) {
                sl->eg.state = EG_DECAY2;    /* skip decay1 */
            }
            sl->eg.volume = (s32)(0x3FF << EG_SHIFT);
        }
        break;

    case EG_DECAY1:
        sl->eg.volume -= sl->eg.decay1_rate;
        if (sl->eg.volume <= 0) sl->eg.volume = 0;
        if ((sl->eg.volume >> (EG_SHIFT + 6)) <= sl->eg.decay_level) {
            sl->eg.state = EG_DECAY2;
        }
        break;

    case EG_DECAY2:
        sl->eg.volume -= sl->eg.decay2_rate;
        if (sl->eg.volume <= 0) sl->eg.volume = 0;
        break;

    case EG_RELEASE:
        sl->eg.volume -= sl->eg.release_rate;
        if (sl->eg.volume <= 0) {
            sl->eg.volume  = 0;
            sl->playing    = false;
        }
        break;
    }

    return m->lin_to_exp[sl->eg.volume >> EG_SHIFT];
}

/* -------------------------------------------------------------------------
 * Pitch step update
 * ------------------------------------------------------------------------- */

static void update_step(multipcm_t *m, slot_t *sl)
{
    /*
     * freq_step[pitch] = value_to_fixed(TL_SHIFT, rate * (1024+pitch)/1024)
     *                  = (1<<TL_SHIFT) * rate * (1024+pitch)/1024
     * Dividing by rate (as MAME does) gives the per-sample position advance
     * in TL_SHIFT fixed-point units.
     */
    u8   oct   = (sl->octave - 1u) & 0xFu;
    u32  pitch = m->freq_step[sl->pitch];
    if (oct & 0x8u) {
        pitch >>= (16u - oct);
    } else {
        pitch <<= oct;
    }
    sl->step = (u32)((float)pitch / (float)MULTIPCM_NATIVE_RATE);
}

/* -------------------------------------------------------------------------
 * Retrigger a voice
 * ------------------------------------------------------------------------- */

static void retrigger(multipcm_t *m, slot_t *sl)
{
    sl->offset      = 0;
    sl->prev_sample = 0;
    sl->total_level = sl->dest_total_level << TL_SHIFT;
    sl->reverse     = false;

    eg_calc(m, sl);
    sl->eg.state  = EG_ATTACK;
    sl->eg.volume = 0;
}

/* -------------------------------------------------------------------------
 * LFO
 * ------------------------------------------------------------------------- */

static void lfo_compute_step(multipcm_t *m, lfo_t *lfo,
                             u32 freq_idx, u32 scale_idx, bool is_amplitude)
{
    float step     = LFO_FREQ[freq_idx] * 256.0f / MULTIPCM_NATIVE_RATE;
    lfo->phase_step = value_to_fixed(LFO_SHIFT, step);
    if (is_amplitude) {
        lfo->table = m->amplitude_table;
        lfo->scale = m->amplitude_scale[scale_idx];
    } else {
        lfo->table = m->pitch_table;
        lfo->scale = m->pitch_scale[scale_idx];
    }
}

/*
 * Advance LFO phase and return fixed-point multiplier shifted to TL_SHIFT.
 * Both pitch and amplitude LFO use the same formula: the waveform table value
 * is used directly as the index into the scale table (matches MAME's
 * `p = lfo.m_scale[p]`).  For pitch, the scale table was built with
 * [i+128] so pitch_table values [1..255] map to the correct entries.
 */
static s32 lfo_step(lfo_t *lfo)
{
    lfo->phase += lfo->phase_step;
    s32 p = lfo->table[(lfo->phase >> LFO_SHIFT) & 0xFFu];
    p = lfo->scale[p];
    return p << (TL_SHIFT - LFO_SHIFT);
}

/* Amplitude LFO step Ã¢ÂÂ same formula as pitch (MAME uses identical code). */
static s32 amplitude_lfo_step(lfo_t *lfo)
{
    lfo->phase += lfo->phase_step;
    s32 p = lfo->table[(lfo->phase >> LFO_SHIFT) & 0xFFu];
    p = lfo->scale[p];
    return p << (TL_SHIFT - LFO_SHIFT);
}

/* -------------------------------------------------------------------------
 * Write a single slot register (matches MAME multipcm_device::write_slot)
 * ------------------------------------------------------------------------- */

static void write_slot(multipcm_t *m, slot_t *sl, s32 reg, u8 data)
{
    const u8 previous = sl->regs[reg];
    sl->regs[reg] = data;

    switch (reg) {
    case 0: /* Pan */
        sl->pan = (data >> 4) & 0xFu;
        break;

    case 1: /* Sample index (low 8 bits) */
    {
        /* 9-bit index: reg1 = bits[7:0], reg2 bit0 = bit8 */
        u32 idx = (u32)sl->regs[1] | ((u32)(sl->regs[2] & 1u) << 8);
        init_sample(m, &sl->sample, idx);
        /* Load LFO defaults from sample header, then update step */
        write_slot(m, sl, 6, sl->sample.lfo_vibrato_reg);
        write_slot(m, sl, 7, sl->sample.lfo_amplitude_reg);
        if (sl->playing) retrigger(m, sl);
        break;
    }

    case 2: /* Pitch LSB */
    case 3: /* Pitch MSB: [7:4]=octave [3:0]=fnum_hi */
        sl->octave = sl->regs[3] >> 4;
        sl->pitch  = (u16)(((u32)(sl->regs[3] & 0xFu) << 6)
                           | ((u32)sl->regs[2] >> 2));
        update_step(m, sl);
        break;

    case 4: /* KeyOn / KeyOff */
        if (data & 0x80u) {
            /* Key On */
            sl->playing = true;
            retrigger(m, sl);
            if (getenv("MODEL1_AUDIO_KEY_TRACE")) {
                fprintf(stderr,
                    "MultiPCM key-on slot=%ld sample=%u start=%06x "
                    "loop=%04x end=%04x pan=%x tl=%02x atk=%x d1=%x "
                    "d2=%x rel=%x bank=%06x\n",
                    (long)(sl - m->slots),
                    (unsigned)(sl->regs[1] | ((sl->regs[2] & 1u) << 8)),
                    sl->sample.start, sl->sample.loop, sl->sample.end,
                    sl->pan, sl->dest_total_level, sl->sample.attack_reg,
                    sl->sample.decay1_reg, sl->sample.decay2_reg,
                    sl->sample.release_reg, m->bank_offset);
            }
        } else {
            /* Key Off */
            if (sl->playing) {
                if (sl->sample.release_reg != 0xFu) {
                    sl->eg.state = EG_RELEASE;
                } else {
                    sl->playing = false;
                }
            }
        }
        break;

    case 5: /* Total Level + interpolation flag */
        sl->dest_total_level = (data >> 1) & 0x7Fu;
        if (!(data & 1u)) {
            /* interpolate */
            if ((sl->total_level >> TL_SHIFT) > sl->dest_total_level) {
                sl->total_level_step = m->tl_step_dn;
            } else {
                sl->total_level_step = m->tl_step_up;
            }
        } else {
            /* direct set */
            sl->total_level = sl->dest_total_level << TL_SHIFT;
        }
        if (getenv("MODEL1_AUDIO_REG_TRACE") && data != previous) {
            fprintf(stderr,
                    "MultiPCM TL slot=%ld data=%02x current=%02x target=%02x %s\n",
                    (long)(sl - m->slots), data,
                    (unsigned)(sl->total_level >> TL_SHIFT),
                    (unsigned)sl->dest_total_level,
                    (data & 1u) ? "direct" : "ramp");
        }
        break;

    case 6: /* LFO frequency + pitch LFO depth */
    case 7: /* Amplitude LFO depth */
        sl->lfo_frequency = (sl->regs[6] >> 3) & 7u;
        sl->vibrato        = sl->regs[6] & 7u;
        sl->tremolo        = sl->regs[7] & 7u;
        if (data) {
            lfo_compute_step(m, &sl->pitch_lfo,
                             sl->lfo_frequency, sl->vibrato, false);
            lfo_compute_step(m, &sl->amplitude_lfo,
                             sl->lfo_frequency, sl->tremolo, true);
        }
        break;
    }
}

/* -------------------------------------------------------------------------
 * Table initialisation helpers
 * ------------------------------------------------------------------------- */

static void init_tables(multipcm_t *m)
{
    float rate = (float)MULTIPCM_NATIVE_RATE;

    /* --- Envelope step tables --- */
    for (s32 i = 4; i < 0x40; ++i) {
        m->attack_step[i] = (u32)((float)(0x400u << EG_SHIFT)
                          / (float)(BASE_TIMES[i] * 44100.0 / 1000.0));
        m->decay_step[i]  = (u32)((float)(0x400u << EG_SHIFT)
                          / (float)(BASE_TIMES[i] * ATTACK_DECAY_RATIO
                                    * 44100.0 / 1000.0));
    }
    m->attack_step[0] = m->attack_step[1] = m->attack_step[2] = m->attack_step[3] = 0;
    m->attack_step[0x3F] = 0x400u << EG_SHIFT;
    m->decay_step[0] = m->decay_step[1] = m->decay_step[2] = m->decay_step[3] = 0;

    /* --- Pitch step table --- */
    for (s32 i = 0; i < 0x400; ++i) {
        float fcent = rate * (1024.0f + (float)i) / 1024.0f;
        m->freq_step[i] = value_to_fixed(TL_SHIFT, fcent);
    }

    /* --- Pan + volume table --- */
    for (s32 level = 0; level < 0x80; ++level) {
        float vol_db     = (float)level * (-24.0f) / 64.0f;
        float total_lvl  = powf(10.0f, vol_db / 20.0f) / 4.0f;

        for (s32 pan = 0; pan < 0x10; ++pan) {
            float pan_left, pan_right;
            if (pan == 0x8) {
                pan_left = pan_right = 0.0f;
            } else if (pan == 0x0) {
                pan_left = pan_right = 1.0f;
            } else if (pan & 0x8) {
                /* left attenuated */
                pan_left = 1.0f;
                s32 inv  = 0x10 - pan;
                float db = (float)inv * (-12.0f) / 4.0f;
                pan_right = powf(10.0f, db / 20.0f);
                if ((inv & 7) == 7) pan_right = 0.0f;
            } else {
                /* right attenuated */
                pan_right = 1.0f;
                float db  = (float)pan * (-12.0f) / 4.0f;
                pan_left  = powf(10.0f, db / 20.0f);
                if ((pan & 7) == 7) pan_left = 0.0f;
            }

            s32 idx = (pan << 7) | level;
            m->left_pan[idx]  = (s32)value_to_fixed(TL_SHIFT, pan_left  * total_lvl);
            m->right_pan[idx] = (s32)value_to_fixed(TL_SHIFT, pan_right * total_lvl);
        }
    }

    /* --- LinearÃ¢ÂÂexponential volume table --- */
    for (s32 i = 0; i < EG_TABLE_SIZE; ++i) {
        float db       = -(96.0f - 96.0f * (float)i / (float)EG_TABLE_SIZE);
        float exp_vol  = powf(10.0f, db / 20.0f);
        m->lin_to_exp[i] = (s32)value_to_fixed(TL_SHIFT, exp_vol);
    }

    /* --- TL interpolation steps --- */
    m->tl_step_dn = -(s32)((float)(0x80u << TL_SHIFT) / (78.2f * 44100.0f / 1000.0f));
    m->tl_step_up =  (s32)((float)(0x80u << TL_SHIFT) / (78.2f * 2.0f * 44100.0f / 1000.0f));

    /* --- LFO waveform tables --- */
    /*
     * Pitch LFO triangle wave: phase [0..255] Ã¢ÂÂ value [1..255] with 128 as
     * center (no pitch shift).  Values are used directly as an index into the
     * pitch_scale table (which is built with entry[i+128] for i=-128..127, so
     * entries [0..255] Ã¢ÂÂ exactly the range produced here).
     */
    for (s32 i = 0; i < 256; ++i) {
        s32 pv;
        if (i < 64)       pv = i * 2 + 128;        /* 128..254 (ramp up)  */
        else if (i < 128) pv = 383 - i * 2;         /* 255..129 (ramp dn)  */
        else if (i < 192) pv = 384 - i * 2;         /* 128..2   (ramp dn)  */
        else              pv = i * 2 - 383;          /* 1..127   (ramp up)  */
        m->pitch_table[i] = pv;
    }
    /* Amplitude LFO: full-wave rectified triangle, unsigned [0,255] */
    for (s32 i = 0; i < 256; ++i) {
        if (i < 128) m->amplitude_table[i] = 255 - i * 2;
        else         m->amplitude_table[i] = i * 2 - 256;
    }

    /* --- LFO scale tables --- */
    for (s32 tbl = 0; tbl < 8; ++tbl) {
        /*
         * Pitch scale: maps signed input i Ã¢ÂÂ [-128,127] to a fixed-point
         * pitch multiplier.  Stored at index [i+128] so that the pitch_table
         * values (1..255 Ã¢ÂÂ¡ unsigned distance from center 128) map correctly:
         * pitch_table[phase] is already the [i+128] index.
         */
        float plimit = PHASE_SCALE_LIMIT[tbl];
        for (s32 i = -128; i < 128; ++i) {
            float val = plimit * (float)i / 128.0f;
            float cvt = powf(2.0f, val / 1200.0f);
            m->pitch_scale[tbl][i + 128] = (s32)value_to_fixed(LFO_SHIFT, cvt);
        }

        /* Amplitude scale: input unsigned [0,255] Ã¢ÂÂ attenuation multiplier */
        float alimit = -AMPLITUDE_SCALE_LIMIT[tbl];
        for (s32 i = 0; i < 256; ++i) {
            float val = alimit * (float)i / 256.0f;
            float cvt = powf(10.0f, val / 20.0f);
            m->amplitude_scale[tbl][i] = (s32)value_to_fixed(LFO_SHIFT, cvt);
        }
    }
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

multipcm_t *multipcm_create(const u8 *rom, u32 rom_size)
{
    multipcm_t *m = (multipcm_t *)calloc(1, sizeof(multipcm_t));
    if (!m) return NULL;

    m->rom      = rom;
    m->rom_size = rom_size;
    m->cur_slot = 0;
    m->address  = 0;

    /* Allocate tables */
    m->tick_ring    = (s32 *)calloc(TICK_RING_FRAMES * 2u, sizeof(s32));
    m->attack_step  = (u32 *)calloc(0x40, sizeof(u32));
    m->decay_step   = (u32 *)calloc(0x40, sizeof(u32));
    m->freq_step    = (u32 *)calloc(0x400, sizeof(u32));
    m->left_pan     = (s32 *)calloc(TL_TABLE_SIZE, sizeof(s32));
    m->right_pan    = (s32 *)calloc(TL_TABLE_SIZE, sizeof(s32));
    m->lin_to_exp   = (s32 *)calloc(EG_TABLE_SIZE, sizeof(s32));
    m->pitch_table     = (s32 *)calloc(256, sizeof(s32));
    m->amplitude_table = (s32 *)calloc(256, sizeof(s32));
    for (s32 i = 0; i < 8; ++i) {
        m->pitch_scale[i]     = (s32 *)calloc(256, sizeof(s32));
        m->amplitude_scale[i] = (s32 *)calloc(256, sizeof(s32));
    }

    if (!m->tick_ring
        || !m->attack_step || !m->decay_step || !m->freq_step
        || !m->left_pan || !m->right_pan || !m->lin_to_exp
        || !m->pitch_table || !m->amplitude_table) {
        multipcm_destroy(m);
        return NULL;
    }
    for (s32 i = 0; i < 8; ++i) {
        if (!m->pitch_scale[i] || !m->amplitude_scale[i]) {
            multipcm_destroy(m);
            return NULL;
        }
    }

    init_tables(m);
    multipcm_reset(m);
    return m;
}

void multipcm_destroy(multipcm_t *m)
{
    if (!m) return;
    free(m->tick_ring);
    free(m->attack_step);
    free(m->decay_step);
    free(m->freq_step);
    free(m->left_pan);
    free(m->right_pan);
    free(m->lin_to_exp);
    free(m->pitch_table);
    free(m->amplitude_table);
    for (s32 i = 0; i < 8; ++i) {
        free(m->pitch_scale[i]);
        free(m->amplitude_scale[i]);
    }
    free(m);
}

void multipcm_reset(multipcm_t *m)
{
    if (!m) return;
    for (s32 i = 0; i < NUM_VOICES; ++i) {
        m->slots[i].playing = false;
    }
    m->cur_slot   = 0;
    m->address    = 0;
    m->bank_offset = 0;
    m->tick_ring_w    = 0;
    m->tick_ring_r    = 0;
    m->tick_cycle_frac = 0;
}

static u32 s_multipcm_writes = 0;

u32 multipcm_debug_writes(void) { return s_multipcm_writes; }

u32 multipcm_active_voices(const multipcm_t *m)
{
    u32 active = 0;
    if (!m) return 0;
    for (s32 i = 0; i < NUM_VOICES; ++i)
        if (m->slots[i].playing) ++active;
    return active;
}

void multipcm_write(multipcm_t *m, u8 offset, u8 data)
{
    if (!m) return;
    s_multipcm_writes++;
    switch (offset) {
    case 0: /* data Ã¢ÂÂ current slot's current register */
        if (m->cur_slot >= 0 && m->cur_slot < NUM_VOICES) {
            write_slot(m, &m->slots[m->cur_slot], (s32)m->address, data);
        }
        break;
    case 1: /* select voice slot */
        m->cur_slot = VALUE_TO_CHANNEL[data & 0x1Fu];
        break;
    case 2: /* select register */
        m->address = (data > 7u) ? 7u : (u32)data;
        break;
    default:
        break;
    }
}

u8 multipcm_read(const multipcm_t *m)
{
    (void)m;
    return 0;
}

void multipcm_set_bank(multipcm_t *m, u8 bank)
{
    if (!m) return;
    /* Each bank step is 1 MB (0x100000 bytes); the upper half of 22-bit
     * space starts at 0x200000.  bank bits[1:0] select the 1 MB page. */
    /* MAME: m_mpcmbank->configure_entries(0, 4, region->base(), 0x100000)
     * — four 1 MB entries starting at ROM offset 0. Bank N exposes
     * ROM[N*1MB .. (N+1)*1MB - 1] in the 0x100000-0x1FFFFF window. */
    m->bank_offset = (u32)(bank & 3u) * 0x100000u;
}

/* -------------------------------------------------------------------------
 * Audio render callback
 * ------------------------------------------------------------------------- */

/*
 * Generate exactly one stereo sample by ticking every active voice forward
 * by one chip-rate step. Extracted from the original render-loop body so it
 * can be invoked both eagerly (from the M68K-fiber tick path) and lazily
 * (from multipcm_render's underrun fallback).
 */
static void multipcm_step_one(multipcm_t *m, s32 *out_l, s32 *out_r)
{
    s32 smpl = 0, smpr = 0;

    for (s32 sl_idx = 0; sl_idx < NUM_VOICES; ++sl_idx) {
        slot_t *sl = &m->slots[sl_idx];
        if (!sl->playing) continue;

        u32 vol  = (sl->total_level >> TL_SHIFT) | (sl->pan << 7);
        u32 spos = sl->offset >> TL_SHIFT;
        u32 step = sl->step;
        u32 fpart = sl->offset & ((1u << TL_SHIFT) - 1u);

        if (sl->reverse)
            spos = sl->sample.end - spos - 1;

        s32 csample;
        if (sl->sample.format & 4u) {
            /* Packed signed 12-bit linear: two samples in three bytes.
             * This format is used by higher-quality engine/SFX voices. */
            const u32 address = sl->sample.start + (spos >> 1) * 3u;
            if ((spos & 1u) == 0) {
                const u16 packed = (u16)((u16)rom_read(m, address) << 8) |
                    (u16)((rom_read(m, address + 1) & 0x0fu) << 4);
                csample = (s32)(s16)packed;
            } else {
                const u16 packed =
                    (u16)((u16)rom_read(m, address + 2) << 8) |
                    (u16)(rom_read(m, address + 1) & 0xf0u);
                csample = (s32)(s16)packed;
            }
        } else {
            /* Signed 8-bit sample stored in the high byte. */
            csample = (s32)(s16)((u16)rom_read(
                m, sl->sample.start + spos) << 8);
        }

        /* Linear interpolation between previous and current sample */
        s32 sample = ((csample * (s32)fpart)
                    + (sl->prev_sample * (s32)((1u << TL_SHIFT) - fpart)))
                   >> TL_SHIFT;

        /* Pitch LFO (vibrato) */
        if (sl->vibrato) {
            s32 lfo_val = lfo_step(&sl->pitch_lfo);
            step = (u32)(((s64)step * lfo_val) >> TL_SHIFT);
        }

        /* Advance position */
        sl->offset += step;

        /* Update prev_sample when we cross to a new integer position */
        if (spos != (sl->offset >> TL_SHIFT)) {
            sl->prev_sample = csample;
        }

        /* Loop handling */
        if (sl->offset >= (sl->sample.end << TL_SHIFT)) {
            sl->offset -= (sl->sample.end - sl->sample.loop) << TL_SHIFT;
            sl->reverse = false;
        }

        /* Total level interpolation */
        if ((sl->total_level >> TL_SHIFT) != sl->dest_total_level) {
            sl->total_level = (u32)((s32)sl->total_level + sl->total_level_step);
        }

        /* Amplitude LFO (tremolo) */
        if (sl->tremolo) {
            s32 am_val = amplitude_lfo_step(&sl->amplitude_lfo);
            sample = (s32)(((s64)sample * am_val) >> TL_SHIFT);
        }

        /* Envelope */
        s32 eg_vol = eg_update(m, sl);
        sample = (sample * eg_vol) >> 10;

        /* Pan + volume mix */
        smpl += (m->left_pan[vol]  * sample) >> TL_SHIFT;
        smpr += (m->right_pan[vol] * sample) >> TL_SHIFT;
    }

    *out_l = smpl;
    *out_r = smpr;
}

void multipcm_tick_m68k_cycles(multipcm_t *m, u32 m68k_cycles)
{
    if (!m || m68k_cycles == 0) return;

    m->tick_cycle_frac += TICK_STEP_PER_M68K_CYCLE * (u64)m68k_cycles;
    u32 due = (u32)(m->tick_cycle_frac >> 32);
    if (due == 0) return;
    m->tick_cycle_frac &= 0xFFFFFFFFu;

    /* Cap to ring free so a stalled mixer doesn't let producers spin. */
    u32 used = (m->tick_ring_w - m->tick_ring_r) & TICK_RING_MASK;
    u32 cap  = TICK_RING_MASK - used;   /* keep one slot empty */
    if (due > cap) due = cap;

    while (due--) {
        s32 l, r;
        multipcm_step_one(m, &l, &r);
        u32 w = m->tick_ring_w;
        m->tick_ring[(w & TICK_RING_MASK) * 2 + 0] = l;
        m->tick_ring[(w & TICK_RING_MASK) * 2 + 1] = r;
        m->tick_ring_w = (w + 1) & TICK_RING_MASK;
    }
}

void multipcm_render(void *userdata, s32 *stereo_out, u32 frames)
{
    multipcm_t *m = (multipcm_t *)userdata;
    if (!m || frames == 0) return;

    /* Drain whatever the M68K-fiber tick already produced. */
    u32 used = (m->tick_ring_w - m->tick_ring_r) & TICK_RING_MASK;
    u32 take = used < frames ? used : frames;
    for (u32 i = 0; i < take; ++i) {
        u32 r = m->tick_ring_r;
        stereo_out[i * 2 + 0] = m->tick_ring[(r & TICK_RING_MASK) * 2 + 0];
        stereo_out[i * 2 + 1] = m->tick_ring[(r & TICK_RING_MASK) * 2 + 1];
        m->tick_ring_r = (r + 1) & TICK_RING_MASK;
    }

    /* Fallback: tick directly for whatever's still owed (typical on the
     * very first frame, before the M68K fiber has run). */
    for (u32 i = take; i < frames; ++i) {
        s32 l, r;
        multipcm_step_one(m, &l, &r);
        stereo_out[i * 2 + 0] = l;
        stereo_out[i * 2 + 1] = r;
    }
}
