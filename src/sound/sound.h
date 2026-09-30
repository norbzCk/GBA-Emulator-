#ifndef SOUND_H
#define SOUND_H

#include <stdint.h>

/* The PSG runs at 262.144kHz internally and the final output is resampled to
 * 32768Hz, which is also the rate the DirectSound FIFOs are clocked at. */
#define SOUND_SAMPLE_RATE 32768u
#define SOUND_CPU_HZ      16777216.0

/* Real hardware holds 32 words per FIFO. */
#define SOUND_FIFO_LEN 32u

/* Host facing ring buffer. */
#define SOUND_RING_LEN 8192u

typedef struct SoundChannel {
    uint16_t cnt_l, cnt_h, cnt_x; /* register shadows */

    int      on;           /* the channel is producing output */
    uint32_t freq;         /* 11 bit SOUNDxCNT_X value */
    uint64_t next_step;    /* cycle of the next duty/wave advance */
    int      phase;        /* 0..7 duty position, or the wave RAM index */
    int      duty;         /* 0..3 */
    int      raw_sample;   /* wave channel: the 4 bit sample being played */

    int      volume;       /* 0..15 tone/noise, 0..3 wave */
    int      force_75;     /* wave channel: ignore the volume field */
    int      env_period;   /* 0..7 */
    int      env_dir;
    uint64_t next_env;

    uint32_t length;       /* remaining length, in LENGTH_DIV units */
    int      length_enable;
    uint64_t next_length;

    /* Sweep, channel 1 only. */
    int      sweep_period;
    int      sweep_shift;
    int      sweep_dir;
    int      sweep_dead;
    uint64_t next_sweep;

    /* Noise, channel 4 only. */
    uint32_t lfsr;
    uint64_t next_noise;
} SoundChannel;

typedef struct Sound {
    uint64_t last_cycles; /* absolute CPU cycles already processed */

    uint16_t reg[0x30];  /* 0x4000060..0x400008F, halfword indexed */
    uint8_t  wave[0x20]; /* 0x4000090, two banks of 32 nibbles */
    SoundChannel ch[4];

    /* FIFO A (0x40000A0) and FIFO B (0x40000A4). */
    int8_t   fifo[2][SOUND_FIFO_LEN];
    uint32_t fifo_head[2];
    uint32_t fifo_count[2];
    int      fifo_sample[2]; /* last word written, centred on 0x80 */
    uint64_t fifo_next[2];
    uint16_t fifo_timer[2];  /* reload of the timer dma_timer[i] selects */

    int enable;             /* SOUND_CNT_X bit 7, the PSG/FIFO master */
    int psg_vol_right;      /* SOUNDCNT_L bits 0-2, 0..7 */
    int psg_vol_left;       /* SOUNDCNT_L bits 4-6, 0..7 */
    int ch_right[4];        /* SOUNDCNT_L bits 8-11 */
    int ch_left[4];         /* SOUNDCNT_L bits 12-15 */
    int psg_ratio;          /* SOUNDCNT_H bits 0-1: 0=25% 1=50% 2=100% */

    int dma_vol[2];         /* SOUNDCNT_H bits 2-3, 0=50% 1=100% */
    int dma_right[2];       /* SOUNDCNT_H bits 8 and 12 */
    int dma_left[2];        /* SOUNDCNT_H bits 9 and 13 */
    int dma_timer[2];       /* SOUNDCNT_H bits 10 and 14: 0 = TM0, 1 = TM1 */

    int bias;               /* SOUNDBIAS bits 1-9, default 0x200 */
    int amp_bits;           /* SOUNDBIAS bits 14-15 */

    /* Fractional sample clock.  `frac` counts 1/65536 of a sample and
     * `frac_step` is how much one CPU cycle adds. */
    uint32_t frac, frac_step;

    int16_t  ring[SOUND_RING_LEN * 2];
    uint32_t ring_head, ring_count;
} Sound;

void sound_init(Sound *s);
void sound_reset(Sound *s);

/* Advance the engine to an absolute CPU cycle count, emitting samples for
 * every 512 cycles that went by. */
void sound_tick(Sound *s, uint64_t cpu_cycles);

/* Register file access, in full 0x04000000 based addresses.  Both return 1
 * when the address belongs to the sound controller. */
int sound_read16(Sound *s, uint32_t address, uint16_t *out);
int sound_write16(Sound *s, uint32_t address, uint16_t value);

/* A 32 bit FIFO write is how the hardware is actually fed: the word at
 * 0x040000A0 supplies four consecutive samples, least significant byte
 * first.  Passing the word through two 16 bit writes would drop half of
 * them, so the MMIO layer calls this directly. */
int sound_write32(Sound *s, uint32_t address, uint32_t value);

/* Drain interleaved stereo 16 bit sample frames.  Returns how many of the
 * requested frames were available. */
uint32_t sound_read(Sound *s, int16_t *out, uint32_t frames);

#endif
