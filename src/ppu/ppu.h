#ifndef GBA_PPU_H
#define GBA_PPU_H

#include <stdint.h>

#include "../memory/memory.h"

#define PPU_W 240
#define PPU_H 160

struct CPU;
struct HW;

typedef struct PPU {
    Memory *mem;

    /* Display control */
    uint16_t dispcnt;    /* 0x04000000 */
    uint16_t dispstat;   /* 0x04000004 */
    uint16_t vcount;     /* 0x04000006 */

    /* Background control */
    uint16_t bgcnt[4];   /* 0x04000008 + 2n */
    uint16_t bghofs[4];  /* 0x04000010 + 4n */
    uint16_t bgvofs[4];  /* 0x04000012 + 4n */

    /* Affine parameters (BG2 at 0x20, BG3 at 0x30) */
    int16_t  pa[4], pb[4], pc[4], pd[4];
    int32_t  bgx[4];     /* rotation/fitting coordinate X */
    int32_t  bgy[4];

    /* Windows */
    uint16_t winh[2];     /* 0x04000040/42, X1 = high byte, X2 = low byte */
    uint16_t winv[2];     /* 0x04000044/46, Y1 = high byte, Y2 = low byte */
    uint16_t winin;       /* 0x04000048, low byte: window 0, high byte: window 1 */
    uint16_t winout;      /* 0x0400004A, low byte: outside, high byte: object window */

    /* Color special effects */
    uint16_t mosaic;      /* 0x0400004C (not implemented) */
    uint16_t bldcnt;      /* 0x04000050 */
    uint16_t bldalpha;    /* 0x04000052 */
    uint16_t bldy;        /* 0x04000054 */

    /* Scanline accumulator for alpha blending. */
    uint32_t frame[PPU_W * PPU_H]; /* 0x00RRGGBB */
} PPU;

void     ppu_init(PPU *ppu, Memory *mem);
void     ppu_reset(PPU *ppu);

uint16_t ppu_io_read16(PPU *ppu, uint32_t address);
void     ppu_io_write16(PPU *ppu, uint32_t address, uint16_t value);

/* Update VCOUNT and the vcounter-match flag in DISPSTAT. */
void ppu_set_vcount(PPU *ppu, uint16_t v);

/* Enable (1) or disable (0) the VBlank (bit0) / HBlank (bit1) status flag. */
void ppu_set_vblank(PPU *ppu, int on);
void ppu_set_hblank(PPU *ppu, int on);

/* Render scanline `y` (0-159) into ppu->frame. */
void ppu_render_scanline(PPU *ppu, int y);

/* Interrupt flags currently requested by DISPSTAT (masked 3 bits). */
uint16_t ppu_irq_flags(const PPU *ppu);

#endif