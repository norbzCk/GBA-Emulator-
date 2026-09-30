/* GBA sound controller: four Game Boy style PSG channels plus the two
 * DirectSound FIFOs, mixed down to a 32768Hz stereo stream.
 *
 * The register layouts and the output levels follow GBATEK's "GBA Sound
 * Controller" chapter: each PSG channel spans +/-0x80 of the 10 bit output
 * range and each FIFO spans +/-0x200, SOUNDBIAS is added to that signed sum,
 * and the result is clipped to 0..0x3FF. */

#include <string.h>

#include "sound.h"

/* Timings in CPU cycles. */
#define STEP_DIV        16.0   /* one of the 8 duty steps of a square wave */
#define WAVE_STEP_DIV    8.0   /* one 4 bit wave RAM sample */
#define LENGTH_DIV  (65536.0)   /* 1/256 s */
#define ENVELOPE_DIV (262144.0) /* 1/64 s */
#define SWEEP_DIV   (131072.0)  /* 1/128 s */

static const uint32_t noise_divisors[8] = { 8, 16, 32, 64, 96, 128, 160, 192 };

static int idx_of(uint32_t addr) {
    return (int)((addr - 0x4000060u) / 2u);
}

/* ---- channel timing ------------------------------------------------------- */

static void channel_stop(SoundChannel *c) {
    c->on = 0;
    c->length = 0;
}

static uint32_t noise_shift(const SoundChannel *c) {
    return 13u - ((c->cnt_l >> 4) & 0xF) / 2u;
}

static void channel_trigger(Sound *s, int i) {
    SoundChannel *c = &s->ch[i];

    c->on = 1;
    c->phase = 0;
    c->raw_sample = 0;

    /* SOUNDxCNT_X bit 14 is the length flag; with it clear the channel plays
     * on until it is retriggered. */
    c->length_enable = (c->cnt_x & 0x4000u) != 0;

    if (i == 0 || i == 1) {
        /* SOUND1CNT_H / SOUND2CNT_L: length 0-5, duty 6-7, envelope 8-15. */
        c->duty = (c->cnt_h >> 6) & 3;
        c->volume = (int)((c->cnt_h >> 12) & 0xF);
        c->env_period = (int)((c->cnt_h >> 8) & 7);
        c->env_dir = (c->cnt_h >> 11) & 1;
        c->length = 64u - (uint32_t)(c->cnt_h & 0x3Fu);
    } else if (i == 2) {
        /* SOUND3CNT_H: length 0-7, volume 13-14, force 75% at bit 15. */
        c->volume = (int)((c->cnt_h >> 13) & 3);
        c->force_75 = (c->cnt_h & 0x8000u) != 0;
        c->length = 256u - (uint32_t)(c->cnt_h & 0xFFu);
    } else {
        /* SOUND4CNT_L: length 0-7, envelope 8-15. */
        c->volume = (int)((c->cnt_l >> 12) & 0xF);
        c->env_period = (int)((c->cnt_l >> 8) & 7);
        c->env_dir = (c->cnt_l >> 11) & 1;
        c->length = 64u - (uint32_t)(c->cnt_l & 0x3Fu);
        c->lfsr = 0x7FFFu;
    }

    /* The whole 11 bit frequency register is SOUNDxCNT_X bits 0-10. */
    c->freq = (uint32_t)(c->cnt_x & 0x7FFu);

    if (i == 0) {
        c->sweep_period = (c->cnt_l >> 4) & 7;
        c->sweep_shift = c->cnt_l & 7;
        c->sweep_dir = (c->cnt_l >> 3) & 1;
        c->sweep_dead = 0;
    }

    if (i == 2) {
        c->next_step = s->last_cycles +
                       (uint64_t)(WAVE_STEP_DIV * (2048.0 - (double)c->freq));
    } else if (i == 3) {
        c->next_noise = s->last_cycles +
                        (uint64_t)(noise_divisors[(c->cnt_l >> 4) & 7]
                                   << noise_shift(c));
    } else {
        c->next_step = s->last_cycles +
                       (uint64_t)(STEP_DIV * (2048.0 - (double)c->freq));
    }

    c->next_env = s->last_cycles + (uint64_t)((c->env_period + 1) * ENVELOPE_DIV);
    c->next_length = s->last_cycles + (uint64_t)LENGTH_DIV;
    if (i == 0) {
        c->next_sweep = s->last_cycles + (uint64_t)((c->sweep_period + 1) * SWEEP_DIV);
    }
}

static void channel_envelope(SoundChannel *c) {
    if (c->env_period == 0) {
        return; /* a step time of zero means no envelope at all */
    }
    if (c->env_dir) {
        if (c->volume < 15) c->volume++;
    } else if (c->volume > 0) {
        c->volume--;
    }
}

static void channel_sweep(SoundChannel *c) {
    uint32_t delta;

    if (c->sweep_period == 0) {
        return; /* sweep is disabled by a time of zero */
    }

    delta = c->freq >> c->sweep_shift;
    if (c->sweep_dir) {
        if (c->freq <= delta) {
            c->sweep_dead = 1;
            channel_stop(c);
            return;
        }
        c->freq -= delta;
    } else {
        c->freq += delta;
    }
    if (c->freq > 0x7FFu) {
        c->sweep_dead = 1;
        channel_stop(c);
    }
}

static void channel_step(Sound *s, int i) {
    SoundChannel *c = &s->ch[i];
    double period;

    if (i == 2) {
        /* SOUND3CNT_L bit 5 picks 32 or 64 digits of wave RAM. */
        uint32_t two_banks = (s->reg[idx_of(0x4000070u)] >> 5) & 1u;
        uint32_t n = two_banks ? 64u : 32u;
        c->phase = (c->phase + 1) % (int)n;
        c->raw_sample = (s->wave[c->phase >> 1] >> ((c->phase & 1) ? 0 : 4)) & 0xF;
        period = WAVE_STEP_DIV * (2048.0 - (double)c->freq);
    } else {
        c->phase = (c->phase + 1) & 7;
        period = STEP_DIV * (2048.0 - (double)c->freq);
    }
    if (period < 1.0) period = 1.0;
    c->next_step += (uint64_t)period;
}

/* Signed output of one PSG channel.  A channel at full volume spans +/-0x80
 * of the 10 bit range, so a level of 15 is about +/-120. */
static int32_t channel_level(const SoundChannel *c, int i) {
    static const uint8_t duty_high[4][8] = {
        { 0, 0, 0, 0, 0, 0, 0, 1 },  /* 12.5% */
        { 0, 0, 0, 0, 0, 0, 1, 1 },  /* 25%   */
        { 0, 0, 0, 0, 1, 1, 1, 1 },  /* 50%   */
        { 0, 1, 1, 1, 1, 1, 1, 1 },  /* 75%   */
    };
    int32_t amp = (int32_t)c->volume * 8;

    if (!c->on || amp == 0) {
        return 0;
    }

    if (i == 2) {
        /* The wave channel plays 4 bit samples centred on zero, scaled by the
         * volume field: 0 = mute, 1 = 100%, 2 = 50%, 3 = 25%. */
        int32_t v = ((int32_t)c->raw_sample * 2 - 15) * 8;
        int num = c->force_75 ? 3 : (c->volume == 0 ? 0 : 4 >> c->volume);
        return v * num / 4;
    }
    if (i == 3) {
        return (c->lfsr & 1u) ? amp : -amp;
    }
    return duty_high[c->duty][c->phase & 7] ? amp : -amp;
}

/* ---- mixer ---------------------------------------------------------------- */

static void emit_sample(Sound *s) {
    int32_t acc[2] = { 0, 0 };
    int16_t out[2] = { 0, 0 };

    /* SOUND_CNT_X bit 7 gates the whole controller.  With it clear the mixer
     * still keeps its timing, but nothing reaches the output. */
    if (s->enable) {
        /* SOUNDCNT_H bits 0-1 pick 25%, 50% or 100% for the PSG. */
        int32_t ratio = (int32_t)(s->psg_ratio + 1) * 256 / 4;
        /* SOUNDCNT_L bits 0-2 and 4-6 are the PSG master volume. */
        int32_t vol_l = (int32_t)(s->psg_vol_left + 1) * 256 / 8;
        int32_t vol_r = (int32_t)(s->psg_vol_right + 1) * 256 / 8;

        for (int i = 0; i < 4; i++) {
            int32_t lvl = channel_level(&s->ch[i], i);
            if (s->ch_left[i]) acc[0] += lvl;
            if (s->ch_right[i]) acc[1] += lvl;
        }
        acc[0] = acc[0] * ratio * vol_l / (256 * 256);
        acc[1] = acc[1] * ratio * vol_r / (256 * 256);

        for (int i = 0; i < 2; i++) {
            /* An 8 bit FIFO sample spans +/-0x200 of the output range. */
            int32_t v = ((int32_t)s->fifo_sample[i] - 0x80) * 4;
            /* SOUNDCNT_H bits 2-3 select 50% or 100%. */
            v = v * ((s->dma_vol[i] + 1) * 256 / 2) / 256;
            if (s->dma_left[i]) acc[0] += v;
            if (s->dma_right[i]) acc[1] += v;
        }

        for (int m = 0; m < 2; m++) {
            int32_t u = acc[m] + s->bias;
            if (u < 0) u = 0;
            if (u > 0x3FF) u = 0x3FF;
            /* The PWM output is unsigned; centre it for signed 16 bit. */
            out[m] = (int16_t)((u - 0x200) * 32);
        }
    }

    s->ring[s->ring_head * 2 + 0] = out[0];
    s->ring[s->ring_head * 2 + 1] = out[1];
    s->ring_head = (s->ring_head + 1) % SOUND_RING_LEN;
    if (s->ring_count < SOUND_RING_LEN) {
        s->ring_count++;
    }
}

static void fifo_push(Sound *s, int which, int8_t value) {
    uint32_t tail = (s->fifo_head[which] + s->fifo_count[which]) % SOUND_FIFO_LEN;

    if (s->fifo_count[which] == SOUND_FIFO_LEN) {
        /* Full: the oldest sample is dropped. */
        s->fifo_head[which] = (s->fifo_head[which] + 1) % SOUND_FIFO_LEN;
    } else {
        s->fifo_count[which]++;
    }
    s->fifo[which][tail] = value;
}

static void fifo_step(Sound *s, int which) {
    uint32_t period = s->fifo_timer[which];

    if (period == 0) {
        /* The timer driving this FIFO is stopped, so it never advances. */
        return;
    }

    if (s->fifo_count[which]) {
        s->fifo_sample[which] = s->fifo[which][s->fifo_head[which]];
        s->fifo_head[which] = (s->fifo_head[which] + 1) % SOUND_FIFO_LEN;
        s->fifo_count[which]--;
    }
    /* Otherwise the last sample is held, which is what the hardware does. */
    s->fifo_next[which] += period;
}

/* Fire every event whose time has arrived, rescheduling it for later. */
static void sound_fire_due(Sound *s) {
    uint64_t now = s->last_cycles;

    for (int i = 0; i < 4; i++) {
        SoundChannel *c = &s->ch[i];
        if (!c->on) continue;

        if (i != 3) while (c->next_step <= now) channel_step(s, i);
        while (c->next_env <= now) {
            channel_envelope(c);
            c->next_env += (uint64_t)((c->env_period + 1) * ENVELOPE_DIV);
        }
        if (i == 0) {
            while (c->next_sweep <= now) {
                if (!c->sweep_dead) channel_sweep(c);
                c->next_sweep += (uint64_t)((c->sweep_period + 1) * SWEEP_DIV);
            }
        }
        if (i == 3) {
            while (c->next_noise <= now) {
                /* A 15 bit LFSR clocked in from the low two bits, which is
                 * what gives the noise its character. */
                uint32_t x = c->lfsr & 1u;
                c->lfsr >>= 1;
                x ^= (c->lfsr & 1u);
                c->lfsr ^= (x << 14);
                if (c->cnt_l & 0x0008u) {
                    /* The 7 bit variant also feeds bit 6. */
                    c->lfsr = (c->lfsr & ~0x0040u) | (x << 6);
                }
                c->next_noise +=
                    (uint64_t)(noise_divisors[(c->cnt_l >> 4) & 7]
                               << noise_shift(c));
            }
        }
        if (c->length_enable) {
            while (c->next_length <= now) {
                if (c->length > 0 && --c->length == 0) {
                    channel_stop(c);
                    break;
                }
                c->next_length += (uint64_t)LENGTH_DIV;
            }
        } else {
            c->next_length = now + (uint64_t)LENGTH_DIV;
        }
    }

    for (int i = 0; i < 2; i++) {
        /* The FIFO is clocked by whichever timer SOUNDCNT_H selected. */
        if (!s->dma_left[i] && !s->dma_right[i]) continue;
        if (s->fifo_timer[i] == 0) continue;
        while (s->fifo_next[i] <= now) {
            fifo_step(s, i);
        }
    }
}

void sound_tick(Sound *s, uint64_t cpu_cycles) {
    if (cpu_cycles <= s->last_cycles) {
        return;
    }

    while (s->last_cycles < cpu_cycles) {
        uint64_t next = cpu_cycles;
        uint64_t dt;

        sound_fire_due(s);

        for (int i = 0; i < 4; i++) {
            SoundChannel *c = &s->ch[i];
            if (!c->on) continue;
            if (i != 3 && c->next_step > s->last_cycles &&
                c->next_step < next) {
                next = c->next_step;
            }
            if (c->next_env > s->last_cycles && c->next_env < next) {
                next = c->next_env;
            }
            if (c->length_enable && c->next_length > s->last_cycles &&
                c->next_length < next) {
                next = c->next_length;
            }
            if (i == 0 && c->next_sweep > s->last_cycles &&
                c->next_sweep < next) {
                next = c->next_sweep;
            }
            if (i == 3 && c->next_noise > s->last_cycles &&
                c->next_noise < next) {
                next = c->next_noise;
            }
        }
        for (int i = 0; i < 2; i++) {
            if ((s->dma_left[i] || s->dma_right[i]) && s->fifo_timer[i] &&
                s->fifo_next[i] > s->last_cycles && s->fifo_next[i] < next) {
                next = s->fifo_next[i];
            }
        }

        dt = next - s->last_cycles;
        s->last_cycles = next;

        /* One output sample per 512 CPU cycles. */
        s->frac += (uint32_t)dt * s->frac_step;
        while (s->frac >= 65536u) {
            s->frac -= 65536u;
            emit_sample(s);
        }
    }

    sound_fire_due(s);
}

/* ---- registers ------------------------------------------------------------ */

int sound_read16(Sound *s, uint32_t address, uint16_t *out) {
    if (address >= 0x4000090u && address < 0x40000A0u) {
        *out = s->wave[(address - 0x4000090u) >> 1];
        return 1;
    }
    if (address >= 0x40000A0u && address < 0x40000A4u) {
        /* Only DMA writes these, so a read returns the last sample played. */
        *out = (uint16_t)(s->fifo_sample[0] & 0xFF);
        return 1;
    }
    if (address >= 0x40000A4u && address < 0x40000A8u) {
        *out = (uint16_t)(s->fifo_sample[1] & 0xFF);
        return 1;
    }
    if (address < 0x4000060u || address >= 0x4000060u + sizeof s->reg) {
        return 0;
    }
    if (address == 0x4000084u) {
        /* Bits 0-3 report whether each channel is running. */
        uint16_t v = (uint16_t)(s->reg[idx_of(address)] & 0xFFF0u);
        for (int i = 0; i < 4; i++) {
            if (s->ch[i].on) v |= (uint16_t)(1u << i);
        }
        *out = v;
        return 1;
    }
    *out = s->reg[idx_of(address)];
    return 1;
}

/* SOUND_CNT_X bit 7 off resets every PSG register in 0x4000060..0x4000081. */
static void reset_psg_registers(Sound *s) {
    for (int i = 0; i <= idx_of(0x4000081u); i++) {
        s->reg[i] = 0;
    }
    for (int i = 0; i < 4; i++) {
        memset(&s->ch[i], 0, sizeof s->ch[i]);
    }
    s->psg_vol_right = 0;
    s->psg_vol_left = 0;
    for (int i = 0; i < 4; i++) {
        s->ch_right[i] = 0;
        s->ch_left[i] = 0;
    }
}

int sound_write16(Sound *s, uint32_t address, uint16_t value) {
    if (address >= 0x4000090u && address < 0x40000A0u) {
        s->wave[(address - 0x4000090u) >> 1] = (uint8_t)value;
        return 1;
    }
    if (address >= 0x40000A0u && address < 0x40000A4u) {
        fifo_push(s, 0, (int8_t)(uint8_t)value);
        fifo_push(s, 0, (int8_t)(uint8_t)(value >> 8));
        return 1;
    }
    if (address >= 0x40000A4u && address < 0x40000A8u) {
        fifo_push(s, 1, (int8_t)(uint8_t)value);
        fifo_push(s, 1, (int8_t)(uint8_t)(value >> 8));
        return 1;
    }
    if (address < 0x4000060u || address >= 0x4000060u + sizeof s->reg) {
        return 0;
    }

    s->reg[idx_of(address)] = value;

    switch (address) {
    case 0x4000060u: s->ch[0].cnt_l = value; break;
    case 0x4000062u: s->ch[0].cnt_h = value; break;
    case 0x4000064u:
        s->ch[0].cnt_x = value;
        if (value & 0x8000u) channel_trigger(s, 0);
        break;
    case 0x4000068u: s->ch[1].cnt_l = value; break;
    case 0x400006Au: s->ch[1].cnt_h = value; break;
    case 0x400006Cu:
        s->ch[1].cnt_x = value;
        if (value & 0x8000u) channel_trigger(s, 1);
        break;
    case 0x4000070u:
        /* Bit 7 stops or starts channel 3 playback. */
        if (!(value & 0x0080u)) s->ch[2].on = 0;
        break;
    case 0x4000072u: s->ch[2].cnt_h = value; break;
    case 0x4000074u:
        s->ch[2].cnt_x = value;
        if (value & 0x8000u) channel_trigger(s, 2);
        break;
    case 0x4000078u: s->ch[3].cnt_l = value; break;
    case 0x400007Au: s->ch[3].cnt_h = value; break;
    case 0x400007Cu:
        s->ch[3].cnt_x = value;
        if (value & 0x8000u) channel_trigger(s, 3);
        break;
    case 0x4000080u: /* SOUNDCNT_L */
        s->psg_vol_right = value & 7;
        s->psg_vol_left = (value >> 4) & 7;
        for (int i = 0; i < 4; i++) {
            s->ch_right[i] = (value & (0x0100u << i)) != 0;  /* bits 8-11 */
            s->ch_left[i] = (value & (0x1000u << i)) != 0;   /* bits 12-15 */
        }
        break;
    case 0x4000082u: /* SOUNDCNT_H */
        s->psg_ratio = value & 3;
        s->dma_vol[0] = (value >> 2) & 1;
        s->dma_vol[1] = (value >> 3) & 1;
        s->dma_right[0] = (value & 0x0100u) != 0;  /* bit 8 */
        s->dma_left[0] = (value & 0x0200u) != 0;   /* bit 9 */
        s->dma_timer[0] = (value & 0x0400u) ? 1 : 0; /* bit 10 */
        s->dma_right[1] = (value & 0x1000u) != 0;  /* bit 12 */
        s->dma_left[1] = (value & 0x2000u) != 0;   /* bit 13 */
        s->dma_timer[1] = (value & 0x4000u) ? 1 : 0; /* bit 14 */
        if (value & 0x0800u) {  /* DMA A reset FIFO, bit 11 */
            s->fifo_head[0] = s->fifo_count[0] = 0;
            s->fifo_sample[0] = 0x80;
        }
        if (value & 0x8000u) {  /* DMA B reset FIFO, bit 15 */
            s->fifo_head[1] = s->fifo_count[1] = 0;
            s->fifo_sample[1] = 0x80;
        }
        if (!(s->dma_left[0] || s->dma_right[0])) s->fifo_next[0] = s->last_cycles;
        if (!(s->dma_left[1] || s->dma_right[1])) s->fifo_next[1] = s->last_cycles;
        break;
    case 0x4000084u: /* SOUND_CNT_X */
        s->enable = (value & 0x0080u) != 0;
        if (!s->enable) reset_psg_registers(s);
        break;
    case 0x4000088u: /* SOUNDBIAS */
        s->bias = (value >> 1) & 0x1FF;
        s->amp_bits = (value >> 14) & 3;
        break;
    default:
        break;
    }
    return 1;
}

int sound_write32(Sound *s, uint32_t address, uint32_t value) {
    int which;

    if (address >= 0x40000A0u && address < 0x40000A8u) {
        which = 0;
    } else if (address >= 0x40000B0u && address < 0x40000B8u) {
        which = 1;
    } else {
        return 0;
    }

    /* Four consecutive samples, least significant byte first. */
    for (int i = 0; i < 4; i++) {
        fifo_push(s, which, (int8_t)(uint8_t)(value >> (i * 8)));
    }
    return 1;
}

uint32_t sound_read(Sound *s, int16_t *out, uint32_t frames) {
    uint32_t n = s->ring_count < frames ? s->ring_count : frames;
    uint32_t start = (s->ring_head + SOUND_RING_LEN - s->ring_count) % SOUND_RING_LEN;

    for (uint32_t i = 0; i < n; i++) {
        uint32_t idx = (start + i) % SOUND_RING_LEN;
        out[i * 2 + 0] = s->ring[idx * 2 + 0];
        out[i * 2 + 1] = s->ring[idx * 2 + 1];
    }
    s->ring_head = (start + n) % SOUND_RING_LEN;
    s->ring_count -= n;
    return n;
}

void sound_reset(Sound *s) {
    for (int i = 0; i < 4; i++) {
        memset(&s->ch[i], 0, sizeof s->ch[i]);
    }
    for (int i = 0; i < 2; i++) {
        memset(s->fifo[i], 0, sizeof s->fifo[i]);
        s->fifo_head[i] = 0;
        s->fifo_count[i] = 0;
        s->fifo_sample[i] = 0x80;
        s->fifo_next[i] = 0;
        s->fifo_timer[i] = 0;
        s->dma_right[i] = 0;
        s->dma_left[i] = 0;
        s->dma_timer[i] = i;
    }
    s->enable = 0;
    s->psg_vol_right = 0;
    s->psg_vol_left = 0;
    s->psg_ratio = 0;
    s->dma_vol[0] = 0;
    s->dma_vol[1] = 0;
    /* SOUNDBIAS powers up at 0x200, which is what puts silence at zero. */
    s->bias = 0x200;
    s->amp_bits = 0;
    s->last_cycles = 0;
    s->frac = 0;
    s->ring_head = s->ring_count = 0;
}

void sound_init(Sound *s) {
    memset(s, 0, sizeof *s);
    /* 65536 accumulator units per sample, one sample every 512 CPU cycles. */
    s->frac_step = (uint32_t)(65536.0 / (SOUND_CPU_HZ / SOUND_SAMPLE_RATE));
    sound_reset(s);
}
