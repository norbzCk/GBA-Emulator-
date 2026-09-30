#include "bios.h"

#include <math.h>
#include <string.h>

/* The GBA BIOS is copyrighted and cannot be shipped, so the services games
 * rely on are re-implemented here. Anything not listed falls through to
 * "do nothing and return", which is what an unhandled SWI amounts to on
 * hardware whose BIOS service is a no-op. */

#define BIOS_CHECKSUM 0xBAAE187Fu
#define BIOS_SIZE     0x4000u

enum {
    SWI_SOFT_RESET         = 0x00,
    SWI_REGISTER_RAM_RESET = 0x01,
    SWI_HALT               = 0x02,
    SWI_STOP               = 0x03,
    SWI_INTR_WAIT          = 0x04,
    SWI_VBLANK_INTR_WAIT   = 0x05,
    SWI_DIV                = 0x06,
    SWI_DIV_ARM            = 0x07,
    SWI_SQRT               = 0x08,
    SWI_ARCTAN             = 0x09,
    SWI_ARCTAN2            = 0x0A,
    SWI_CPU_SET            = 0x0B,
    SWI_CPU_FAST_SET       = 0x0C,
    SWI_BIOS_CHECKSUM      = 0x0D,
    SWI_BG_AFFINE_SET      = 0x0E,
    SWI_OBJ_AFFINE_SET     = 0x0F,
    SWI_BIT_UNPACK         = 0x10,
    SWI_LZ77_WRAM          = 0x11,
    SWI_LZ77_VRAM          = 0x12,
    SWI_HUFFMAN            = 0x13,
    SWI_RL_WRAM            = 0x14,
    SWI_RL_VRAM            = 0x15,
    SWI_DIFF8_WRAM         = 0x16,
    SWI_DIFF8_VRAM         = 0x17,
    SWI_DIFF16             = 0x18,
    SWI_SOUND_BIAS         = 0x19,
    SWI_MIDI_KEY2FREQ      = 0x1F,
    SWI_INTERNAL_STALL     = 0xF0
};

/* ---- output writer ------------------------------------------------------ */

/* The VRAM flavours of the decompressors may not write single bytes, so
 * their output is assembled into halfwords and flushed as they complete. */
typedef struct Out {
    Memory *mem;
    uint32_t dest;
    int width;         /* 1 = byte, 2 = halfword */
    uint16_t pending;
    int has_pending;
} Out;

static void out_put(Out *o, uint32_t value) {
    if (o->width == 1) {
        memory_write8(o->mem, o->dest, (uint8_t)value);
        o->dest += 1;
        return;
    }

    /* Halfword mode: bytes are paired up and written as one 16 bit store. */
    if (o->has_pending) {
        o->pending |= (uint16_t)((value & 0xFF) << 8);
        memory_write16(o->mem, o->dest, o->pending);
        o->dest += 2;
        o->has_pending = 0;
    } else {
        o->pending = (uint16_t)(value & 0xFF);
        o->has_pending = 1;
    }
}

static void out_flush(Out *o) {
    if (o->width == 2 && o->has_pending) {
        memory_write16(o->mem, o->dest, o->pending);
        o->has_pending = 0;
    }
}

/* ---- arithmetic --------------------------------------------------------- */

static void bios_div(CPU *cpu, int32_t num, int32_t denom) {
    if (denom == 0) {
        /* The hardware spins here forever; clamp rather than hang. */
        cpu->reg[0] = (num < 0) ? 0xFFFFFFFFu : 1u;
        cpu->reg[1] = (uint32_t)num;
        cpu->reg[3] = 1;
        return;
    }
    if (denom == -1 && num == (int32_t)0x80000000) {
        cpu->reg[0] = 0x80000000u;
        cpu->reg[1] = 0;
        cpu->reg[3] = 0x80000000u;
        return;
    }

    int32_t quot = num / denom;
    cpu->reg[0] = (uint32_t)quot;
    cpu->reg[1] = (uint32_t)(num % denom);
    cpu->reg[3] = (quot < 0) ? (uint32_t)(-quot) : (uint32_t)quot;
}

static uint32_t isqrt32(uint32_t x) {
    uint32_t res = 0;
    uint32_t bit = 1u << 30;

    while (bit > x) {
        bit >>= 2;
    }
    while (bit != 0) {
        if (x >= res + bit) {
            x -= res + bit;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    return res;
}

/* 1.14 fixed point angle. This is the BIOS' own polynomial so that the
 * rounding of the result matches. */
/* Internally the trig runs in 1.14 radians, which is also the scale the
 * hardware takes its arguments in.  Results come back in the BIOS' own
 * angle scale: 10000h is a full turn, so 4000h is PI/2 (GBATEK SWI 09h/0Ah). */
#define RAD_PI_2 25736
#define ARC_PI_2 0x4000
#define ARC_PI   0x8000

static int32_t rad_to_arc(int32_t rad) {
    int64_t n = (int64_t)rad * ARC_PI_2;
    /* round half away from zero so the result is symmetric */
    if (n < 0) {
        return -(int32_t)((-n + RAD_PI_2 / 2) / RAD_PI_2);
    }
    return (int32_t)((n + RAD_PI_2 / 2) / RAD_PI_2);
}

/* atan(t) for |t| <= 0.5 in 1.14, from the alternating series. */
static int32_t atan_small(int32_t t) {
    int32_t t2 = (t * t) >> 14;
    int32_t acc = -1092;               /* -1/15 */
    acc = ((acc * t2) >> 14) + 1261;   /* +1/13 */
    acc = ((acc * t2) >> 14) - 1490;   /* -1/11 */
    acc = ((acc * t2) >> 14) + 1820;   /* +1/9 */
    acc = ((acc * t2) >> 14) - 2340;   /* -1/7 */
    acc = ((acc * t2) >> 14) + 3277;   /* +1/5 */
    acc = ((acc * t2) >> 14) - 5461;   /* -1/3 */
    acc = ((acc * t2) >> 14) + 16384;  /* +1 */
    return (t * acc) >> 14;
}

/* atan(x) for 0 <= x <= 1.0 in 1.14. */
static int32_t atan_unit(int32_t x) {
    if (x <= 0x2000) {
        return atan_small(x);
    }
    /* atan(x) = pi/4 + atan((x-1)/(x+1)); the argument is small again. */
    return RAD_PI_2 / 2 + atan_small(((x - 0x4000) << 14) / (x + 0x4000));
}

/* atan(x) for x in 1.14 within [-2,2]. */
static int32_t atan_q14(int32_t x) {
    int32_t sign = 1;
    int32_t a;

    if (x < 0) {
        sign = -1;
        x = -x;
    }
    if (x > 0x7FFF) {
        x = 0x7FFF; /* past +/-2 the result is pi/2 to within a LSB */
    }

    if (x <= 0x4000) {
        a = atan_unit(x);
    } else {
        /* atan(x) = pi/2 - atan(1/x), and 1/x is back in range */
        a = RAD_PI_2 - atan_unit((0x4000 << 14) / x);
    }
    return sign * a;
}

/* SWI 09h ArcTan.  r1 and r3 come back as the BIOS leaves them; games only
 * ever use the angle in r0. */
static int16_t arctan1(int32_t i, int32_t *r1, int32_t *r3) {
    if (r1) *r1 = 0;
    if (r3) *r3 = 0x8000;
    return (int16_t)rad_to_arc(atan_q14(i));
}

/* SWI 0Ah ArcTan2(y, x) in 1.14, in the range -pi..pi. */
static int16_t arctan2_1(int32_t x, int32_t y, int32_t *r1) {
    int32_t ratio;
    int32_t angle;

    if (r1) *r1 = 0;

    if (!x && !y) {
        return 0;
    }
    if (!x) {
        return (int16_t)(y > 0 ? ARC_PI_2 : -ARC_PI_2);
    }
    if (!y) {
        return (int16_t)(x > 0 ? 0 : ARC_PI);
    }

    /* The ratio in 1.14 saturates, which is fine: |atan| is then pi/2. */
    ratio = (int32_t)(((int64_t)y << 14) / x);
    if (ratio > 0x7FFF) ratio = 0x7FFF;
    if (ratio < -0x7FFF) ratio = -0x7FFF;
    angle = rad_to_arc(atan_q14(ratio));

    if (x < 0) {
        angle += (y > 0) ? ARC_PI : -ARC_PI;
    }
    return (int16_t)(uint16_t)angle; /* hardware wraps negatives to 8000h+ */
}

/* ---- block copies ------------------------------------------------------- */

/* SWI 0Bh CpuSet / 0Ch CpuFastSet: 21 bit count, bit26 selects 32 bit units,
 * bit24 keeps the source address fixed. */
void bios_cpu_set(Memory *mem, CPU *cpu, int fast) {
    uint32_t src = cpu->reg[0];
    uint32_t dst = cpu->reg[1];
    uint32_t control = cpu->reg[2];
    uint32_t count = control & 0x1FFFFF;
    int wide = fast || (((control >> 26) & 1) != 0);
    int fixed = ((control >> 24) & 1) != 0;

    if (fast && (count & 7)) {
        count = (count + 7) & ~7u; /* CpuFastSet moves whole blocks of 8 */
    }

    for (uint32_t i = 0; i < count; i++) {
        if (wide) {
            memory_write32(mem, dst, memory_read32(mem, src));
        } else {
            memory_write16(mem, dst, memory_read16(mem, src));
        }
        dst += wide ? 4u : 2u;
        if (!fixed) {
            src += wide ? 4u : 2u;
        }
    }
}

/* ---- decompression ------------------------------------------------------ */

/* SWI 11h/12h LZ77UnCompWram / LZ77UnCompVram. `width` is 1 for the byte
 * flavour and 2 for the halfword (VRAM) one. */
void bios_lz77(Memory *mem, uint32_t src, uint32_t dst, int width) {
    Out o = { mem, dst, width, 0, 0 };
    uint32_t remaining = (memory_read32(mem, src) & 0xFFFFFF00u) >> 8;
    uint8_t flags = 0;
    int flagcount = 0;

    src += 4;

    while (remaining > 0) {
        if (flagcount == 0) {
            flags = memory_read8(mem, src++);
            flagcount = 8;
        }

        if (flags & 0x80) {
            /* back reference: 12 bit distance, 4 bit length */
            uint16_t block = (uint16_t)((memory_read8(mem, src) << 8) |
                                        memory_read8(mem, src + 1));
            uint32_t disp = o.dest - (block & 0x0FFF) - 1;
            int bytes = ((block >> 12) & 0xF) + 3;

            src += 2;
            while (bytes-- > 0 && remaining > 0) {
                out_put(&o, memory_read8(mem, disp++));
                remaining--;
            }
        } else {
            out_put(&o, memory_read8(mem, src++));
            remaining--;
        }

        flags = (uint8_t)(flags << 1);
        flagcount--;
    }

    out_flush(&o);
}

/* SWI 14h/15h RLUnCompWram / RLUnCompVram. */
void bios_rl(Memory *mem, uint32_t src, uint32_t dst, int width) {
    Out o = { mem, dst, width, 0, 0 };
    uint32_t size = (memory_read32(mem, src) & 0xFFFFFF00u) >> 8;
    uint32_t remaining = size;
    int padding = (int)((4u - (size & 3u)) & 3u);

    src += 4;

    while (remaining > 0) {
        uint8_t header = memory_read8(mem, src++);

        if (header & 0x80) {
            uint8_t value = memory_read8(mem, src++);
            int count = (header & 0x7F) + 3;
            while (count-- > 0 && remaining > 0) {
                out_put(&o, value);
                remaining--;
            }
        } else {
            int count = header + 1;
            while (count-- > 0 && remaining > 0) {
                out_put(&o, memory_read8(mem, src++));
                remaining--;
            }
        }
    }
    out_flush(&o);

    /* Output is padded out to the next word boundary. */
    if (width == 2) {
        for (; padding > 0; padding -= 2) {
            memory_write16(mem, o.dest, 0);
            o.dest += 2;
        }
    } else {
        while (padding-- > 0) {
            out_put(&o, 0);
        }
    }
}

/* SWI 13h HuffUnComp: canonical Huffman trees, walked most significant bit
 * first, producing 32 bit data blocks. */
void bios_huffman(Memory *mem, uint32_t src, uint32_t dst) {
    uint32_t header = memory_read32(mem, src & ~3u);
    uint32_t remaining = header >> 8;
    unsigned bits = header & 0xF;
    unsigned treesize;
    uint32_t treebase, npointer, block = 0;
    int bitsseen = 0, bitsremaining;
    uint8_t node;

    if (bits == 0 || (32 % bits) || bits == 1) {
        return; /* malformed stream */
    }

    src &= ~3u;
    treesize = (unsigned)(memory_read8(mem, src + 4) << 1) + 1;
    treebase = src + 5;
    src = treebase + treesize;
    npointer = treebase;
    node = memory_read8(mem, npointer);

    while (remaining > 0) {
        uint32_t bitstream = memory_read32(mem, src);
        src += 4;

        for (bitsremaining = 32; bitsremaining > 0 && remaining > 0;
             --bitsremaining, bitstream <<= 1) {
            uint32_t next = (npointer & ~1u) + ((node & 0x3F) * 2) + 2;
            int readbits;

            if (bitstream & 0x80000000u) {
                /* right child */
                if (node & 0x40) {
                    readbits = memory_read8(mem, next + 1);
                } else {
                    npointer = next + 1;
                    node = memory_read8(mem, npointer);
                    continue;
                }
            } else {
                /* left child */
                if (node & 0x80) {
                    readbits = memory_read8(mem, next);
                } else {
                    npointer = next;
                    node = memory_read8(mem, npointer);
                    continue;
                }
            }

            block |= (uint32_t)(readbits & ((1u << bits) - 1u)) << bitsseen;
            bitsseen += bits;
            npointer = treebase;
            node = memory_read8(mem, npointer);

            if (bitsseen == 32) {
                bitsseen = 0;
                memory_write32(mem, dst, block);
                dst += 4;
                remaining = (remaining >= 4) ? remaining - 4 : 0;
                block = 0;
            }
        }
    }
}

/* SWI 10h BitUnPack: expand `srcwidth` bit values into `destwidth` bit ones,
 * adding a bias to non-zero entries. */
void bios_bit_unpack(Memory *mem, uint32_t src, uint32_t dst, uint32_t info) {
    uint32_t srclen = memory_read16(mem, info);
    unsigned srcwidth = memory_read8(mem, info + 2);
    unsigned destwidth = memory_read8(mem, info + 3);
    uint32_t bias = memory_read32(mem, info + 4);
    uint8_t in = 0;
    uint32_t out = 0;
    int bitsremaining = 0, bitseaten = 0;

    if (srcwidth != 1 && srcwidth != 2 && srcwidth != 4 && srcwidth != 8) {
        return;
    }
    if (destwidth != 1 && destwidth != 2 && destwidth != 4 &&
        destwidth != 8 && destwidth != 16 && destwidth != 32) {
        return;
    }

    while (srclen > 0 || bitsremaining) {
        if (!bitsremaining) {
            in = memory_read8(mem, src);
            src++;
            srclen--;
            bitsremaining = 8;
        }

        uint32_t scaled = in & ((1u << srcwidth) - 1u);
        in = (uint8_t)(in >> srcwidth);
        if (scaled || (bias & 0x80000000u)) {
            scaled += bias & 0x7FFFFFFFu;
        }

        bitsremaining -= (int)srcwidth;
        out |= scaled << bitseaten;
        bitseaten += (int)destwidth;

        if (bitseaten == 32) {
            memory_write32(mem, dst, out);
            bitseaten = 0;
            out = 0;
            dst += 4;
        }
    }
}

/* SWI 16h/17h/18h Diff8bitUnFilterWram / ...Vram / Diff16bitUnFilter. */
void bios_filter(Memory *mem, uint32_t src, uint32_t dst, int inwidth) {
    uint32_t size = memory_read32(mem, src & ~3u) >> 8; /* output bytes */
    int32_t  old = 0;

    src = (src & ~3u) + 4;

    if (inwidth == 1) {
        /* Two 8 bit signed deltas make up one 16 bit sample. */
        while (size >= 2) {
            int32_t a = old + (int8_t)memory_read8(mem, src++);
            int32_t b = a + (int8_t)memory_read8(mem, src++);
            memory_write16(mem, dst, (uint16_t)((b << 8) | (a & 0xFF)));
            dst += 2;
            size -= 2;
            old = b;
        }
        return;
    }

    while (size >= 2) {
        int32_t next = old + (int16_t)memory_read16(mem, src);
        memory_write16(mem, dst, (uint16_t)next);
        dst += 2;
        src += 2;
        size -= 2;
        old = next;
    }
}

/* ---- affine helpers ----------------------------------------------------- */

/* SWI 0Eh BgAffineSet: 20 byte source records (origin, screen centre, scale,
 * 8.8 rotation) turned into 16 byte PA..PD + X/Y parameter blocks. */
static void bg_affine_set(Memory *mem, CPU *cpu) {
    uint32_t src = cpu->reg[0];
    uint32_t dst = cpu->reg[1];
    int count = (int)cpu->reg[2];
    const float pi = 3.14159265358979f;

    for (int i = 0; i < count; i++) {
        float ox = (float)(int32_t)memory_read32(mem, src) / 256.0f;
        float oy = (float)(int32_t)memory_read32(mem, src + 4) / 256.0f;
        float cx = (float)(int16_t)memory_read16(mem, src + 8);
        float cy = (float)(int16_t)memory_read16(mem, src + 10);
        float sx = (float)(int16_t)memory_read16(mem, src + 12) / 256.0f;
        float sy = (float)(int16_t)memory_read16(mem, src + 14) / 256.0f;
        float theta = (float)(memory_read16(mem, src + 16) >> 8) / 128.0f * pi;
        float a = cosf(theta) * sx;
        float b = -sinf(theta) * sx;
        float c = sinf(theta) * sy;
        float d = cosf(theta) * sy;
        float rx = ox - (a * cx + b * cy);
        float ry = oy - (c * cx + d * cy);

        src += 20;

        memory_write16(mem, dst,      (uint16_t)(int16_t)(a * 256.0f));
        memory_write16(mem, dst + 2,  (uint16_t)(int16_t)(b * 256.0f));
        memory_write16(mem, dst + 4,  (uint16_t)(int16_t)(c * 256.0f));
        memory_write16(mem, dst + 6,  (uint16_t)(int16_t)(d * 256.0f));
        memory_write32(mem, dst + 8,  (uint32_t)(int32_t)(rx * 256.0f));
        memory_write32(mem, dst + 12, (uint32_t)(int32_t)(ry * 256.0f));
        dst += 16;
    }
}

/* SWI 0Fh ObjAffineSet: 8 byte source records written `step` bytes apart
 * (the OAM affine parameter groups). */
static void obj_affine_set(Memory *mem, CPU *cpu) {
    uint32_t src = cpu->reg[0];
    uint32_t dst = cpu->reg[1];
    int count = (int)cpu->reg[2];
    uint32_t step = cpu->reg[3];
    const float pi = 3.14159265358979f;

    for (int i = 0; i < count; i++) {
        float sx = (float)(int16_t)memory_read16(mem, src) / 256.0f;
        float sy = (float)(int16_t)memory_read16(mem, src + 2) / 256.0f;
        float theta = (float)(memory_read16(mem, src + 4) >> 8) / 128.0f * pi;

        src += 8;

        memory_write16(mem, dst,            (uint16_t)(int16_t)(cosf(theta) * sx * 256.0f));
        memory_write16(mem, dst + step,     (uint16_t)(int16_t)(-sinf(theta) * sx * 256.0f));
        memory_write16(mem, dst + step * 2, (uint16_t)(int16_t)(sinf(theta) * sy * 256.0f));
        memory_write16(mem, dst + step * 3, (uint16_t)(int16_t)(cosf(theta) * sy * 256.0f));
        dst += step * 4;
    }
}

/* ---- system ------------------------------------------------------------- */

/* SWI 01h RegisterRamReset: r0 is a set of flags saying what to clear. */
static void register_ram_reset(Memory *mem, CPU *cpu, HW *hw) {
    uint32_t what = cpu->reg[0];
    uint16_t dispcnt = hw_io_read(hw, 0x000, 16);

    if (what & 0x1C) {
        /* blank the screen while the video memory is being wiped */
        hw_io_write(hw, 0x000, (uint16_t)(dispcnt | 0x0080), 16);
    }

    if (what & 0x01) memset(mem->ewram, 0, GBA_EWRAM_SIZE);
    if (what & 0x02) memset(mem->iwram, 0, GBA_IWRAM_SIZE - 0x200);
    if (what & 0x04) memset(mem->pal, 0, GBA_PAL_SIZE);
    if (what & 0x08) memset(mem->vram, 0, GBA_VRAM_SIZE);
    if (what & 0x10) memset(mem->oam, 0, GBA_OAM_SIZE);

    if (what & 0x20) {
        hw_io_write(hw, 0x128, 0, 16); /* SIO */
        hw_io_write(hw, 0x134, 0, 16); /* RCNT */
    }
    if (what & 0x40) {
        hw_io_write(hw, 0x088, 0x200, 16); /* SOUNDBIAS */
    }
    if (what & 0x80) {
        for (int a = 0x004; a <= 0x054; a += 2) {
            hw_io_write(hw, a, 0, 16);
        }
        /* affine BG parameters reset to "no rotation" */
        hw_io_write(hw, 0x020, 0x0100, 16); /* BG2PA */
        hw_io_write(hw, 0x030, 0x0100, 16); /* BG3PA */
        for (int i = 0; i < 4; i++) {
            hw_io_write(hw, 0x100 + i * 4, 0, 16); /* TMnCNT_L */
            hw_io_write(hw, 0x102 + i * 4, 0, 16); /* TMnCNT_H */
        }
        for (int i = 0; i < 4; i++) {
            for (int j = 0; j < 6; j++) {
                hw_io_write(hw, 0xB0 + i * 12 + j * 2, 0, 16);
            }
        }
        hw_io_write(hw, 0x200, 0, 16);       /* IE */
        hw_io_write(hw, 0x202, 0xFFFF, 16);  /* IF */
        hw_io_write(hw, 0x204, 0, 16);       /* WAITCNT */
        hw_io_write(hw, 0x208, 0, 16);       /* IME */
    }

    if (what & 0x1C) {
        hw_io_write(hw, 0x000, dispcnt, 16);
    }
}

/* SWI 04h IntrWait / 05h VBlankIntrWait. The CPU is stopped until the
 * requested interrupt flags show up; if they are already pending the call
 * returns straight away. */
static void intr_wait(CPU *cpu, HW *hw, uint32_t number) {
    uint16_t mask = (number == SWI_VBLANK_INTR_WAIT)
                        ? (uint16_t)HW_IRQ_VBLANK
                        : (uint16_t)(cpu->reg[1] & 0xFFFF);

    /* r0 = 0 leaves interrupts masked: the BIOS only needs the flag to be
     * raised, not the handler to run. */
    hw->ime = (number == SWI_VBLANK_INTR_WAIT || cpu->reg[0] != 0) ? 1 : 0;

    if ((hw->if_ & mask) != mask) {
        hw_intr_wait_begin(hw, mask);
        cpu_set_wait(cpu, CPU_HALT);
    }

    cpu->reg[1] = hw->if_ & mask;
}

/* ---- dispatch ----------------------------------------------------------- */

void bios_swi(Memory *mem, CPU *cpu, HW *hw, uint32_t number) {
    switch (number) {
        case SWI_SOFT_RESET:
            cpu_reset(cpu);
            cpu_write_reg(cpu, 13, 0x03007F00u);
            /* cpu_set_pc, not a direct write: the step loop overwrites R15
             * with the next sequential address unless the PC was flagged. */
            cpu_set_pc(cpu, 0x08000000u);
            break;

        case SWI_REGISTER_RAM_RESET:
            register_ram_reset(mem, cpu, hw);
            break;

        case SWI_HALT:
            cpu_set_wait(cpu, CPU_HALT);
            break;

        case SWI_STOP:
            cpu_set_wait(cpu, CPU_STOP);
            break;

        case SWI_INTR_WAIT:
        case SWI_VBLANK_INTR_WAIT:
            intr_wait(cpu, hw, number);
            break;

        case SWI_DIV:
            bios_div(cpu, (int32_t)cpu->reg[0], (int32_t)cpu->reg[1]);
            break;

        case SWI_DIV_ARM:
            bios_div(cpu, (int32_t)cpu->reg[1], (int32_t)cpu->reg[0]);
            break;

        case SWI_SQRT:
            cpu->reg[0] = isqrt32(cpu->reg[0]);
            break;

        case SWI_ARCTAN: {
            int32_t r1 = 0, r3 = 0;
            cpu->reg[0] = (uint32_t)(int32_t)arctan1((int32_t)cpu->reg[0], &r1, &r3);
            cpu->reg[1] = (uint32_t)r1;
            cpu->reg[3] = (uint32_t)r3;
            break;
        }

        case SWI_ARCTAN2: {
            int32_t r1 = 0;
            cpu->reg[0] = (uint32_t)(int32_t)arctan2_1((int32_t)cpu->reg[0],
                                                       (int32_t)cpu->reg[1], &r1);
            cpu->reg[1] = (uint32_t)r1;
            cpu->reg[3] = 0x170;
            break;
        }

        case SWI_CPU_SET:
            bios_cpu_set(mem, cpu, 0);
            break;

        case SWI_CPU_FAST_SET:
            bios_cpu_set(mem, cpu, 1);
            break;

        case SWI_BIOS_CHECKSUM:
            cpu->reg[0] = BIOS_CHECKSUM;
            cpu->reg[1] = 1;
            cpu->reg[3] = BIOS_SIZE;
            break;

        case SWI_BG_AFFINE_SET:
            bg_affine_set(mem, cpu);
            break;

        case SWI_OBJ_AFFINE_SET:
            obj_affine_set(mem, cpu);
            break;

        case SWI_BIT_UNPACK:
            bios_bit_unpack(mem, cpu->reg[0], cpu->reg[1], cpu->reg[2]);
            break;

        case SWI_LZ77_WRAM:
        case SWI_LZ77_VRAM:
            bios_lz77(mem, cpu->reg[0], cpu->reg[1],
                      number == SWI_LZ77_WRAM ? 1 : 2);
            break;

        case SWI_RL_WRAM:
        case SWI_RL_VRAM:
            bios_rl(mem, cpu->reg[0], cpu->reg[1],
                    number == SWI_RL_WRAM ? 1 : 2);
            break;

        case SWI_HUFFMAN:
            bios_huffman(mem, cpu->reg[0], cpu->reg[1]);
            break;

        case SWI_DIFF8_WRAM:
            bios_filter(mem, cpu->reg[0], cpu->reg[1], 1);
            break;

        case SWI_DIFF8_VRAM:
            bios_filter(mem, cpu->reg[0], cpu->reg[1], 1);
            break;

        case SWI_DIFF16:
            bios_filter(mem, cpu->reg[0], cpu->reg[1], 2);
            break;

        case SWI_SOUND_BIAS:
            /* Only meaningful with a sound engine running. */
            break;

        case SWI_MIDI_KEY2FREQ: {
            /* r0 points at a 32 bit key number, r1/r2 the fine tuning. */
            uint32_t key = memory_read32(mem, cpu->reg[0] + 4);
            double tune = (180.0 - (double)cpu->reg[1] -
                           (double)cpu->reg[2] / 256.0) / 12.0;
            cpu->reg[0] = (uint32_t)(key / exp2(tune));
            break;
        }

        case SWI_INTERNAL_STALL:
            cpu->reg[11] = 0;
            break;

        default:
            /* Sound driver calls and anything unknown: no-op. */
            break;
    }
}
