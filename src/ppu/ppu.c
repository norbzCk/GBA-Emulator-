#include "ppu.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

/* ---- layer identifiers -------------------------------------------------- */

enum {
    LYR_BG0 = 0,
    LYR_BG1,
    LYR_BG2,
    LYR_BG3,
    LYR_OBJ,
    LYR_BACK      /* backdrop / no layer at all */
};

/* Blend modes (low two bits of the effect fields). */
enum {
    BLEND_OFF = 0,
    BLEND_ALPHA = 1,
    BLEND_BRIGHT = 2,
    BLEND_DARK = 3
};

/* One pixel of a scanline, with enough information to apply windows and
 * blending after all layers have been drawn. */
typedef struct Px {
    uint32_t color;  /* 0x00RRGGBB */
    uint8_t  layer;
    uint8_t  semi;   /* semi transparent object */
} Px;

/* Draw one pixel over whatever is there, keeping the previous content as
 * the "under" pixel for blending. */
static void put(Px *out, Px *under, int x, uint32_t color, int layer, int semi) {
    under[x] = out[x];
    out[x].color = color;
    out[x].layer = (uint8_t)layer;
    out[x].semi = (uint8_t)semi;
}

/* ---- palette / vram / oam helpers -------------------------------------- */

/* VRAM access with the correct 96 KB mapping: offsets past 0x18000
 * mirror to the low 32 KB (like region_ptr in memory.c). */
static inline uint8_t vr8(PPU *p, uint32_t off) {
    uint32_t index = off & 0x1FFFF;
    if (index >= GBA_VRAM_SIZE) index &= 0x7FFF;
    return p->mem->vram[index];
}

static inline uint16_t vr16(PPU *p, uint32_t off) {
    uint32_t index = off & 0x1FFFE;
    if (index >= GBA_VRAM_SIZE) index &= 0x7FFE;
    return (uint16_t)(p->mem->vram[index] | ((uint16_t)p->mem->vram[index + 1] << 8));
}

static inline uint16_t pal16(PPU *p, uint32_t idx) {
    return (uint16_t)(p->mem->pal[idx * 2] | ((uint16_t)p->mem->pal[idx * 2 + 1] << 8));
}

static inline uint16_t oam16(PPU *p, uint32_t slot) {
    return (uint16_t)(p->mem->oam[slot * 2] | ((uint16_t)p->mem->oam[slot * 2 + 1] << 8));
}

/* 15-bit 0x0BBBBBGGGGRRRRR -> 0x00RRGGBB. */
static uint32_t c15(uint16_t c) {
    uint8_t r = (uint8_t)(((c & 0x1F) * 255) / 31);
    uint8_t g = (uint8_t)((((c >> 5) & 0x1F) * 255) / 31);
    uint8_t b = (uint8_t)((((c >> 10) & 0x1F) * 255) / 31);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* Affine (rotation/scaling) parameter block. Index 0-3 selects PA..PD. */
static int16_t bg_aff(PPU *p, int b, int which) {
    switch (which) {
        case 0: return p->pa[b];
        case 1: return p->pb[b];
        case 2: return p->pc[b];
        default: return p->pd[b];
    }
}

/* OBJ affine parameters, from the gaps between OAM attributes.
 * Group n occupies PA at 0x06+n*0x20, then PB/PC/PD 8 bytes apart. */
static int16_t oam_aff(PPU *p, int n, int which) {
    uint32_t off = 0x06u + (uint32_t)n * 0x20u + (uint32_t)which * 0x08u;
    return (int16_t)(p->mem->oam[off] | (p->mem->oam[off + 1] << 8));
}

/* ---- objects ------------------------------------------------------------- */

typedef struct ObjShape {
    int w, h;      /* size in pixels */
    int tpr;       /* tiles per row, for 1D mapping */
} ObjShape;

/* Indexed by [shape][size]; shape 3 is prohibited. */
static const ObjShape obj_shapes[3][4] = {
    /* square */
    { { 8, 8, 1 }, { 16, 16, 2 }, { 32, 32, 4 }, { 64, 64, 8 } },
    /* horizontal (w wider than h) */
    { { 16, 8, 2 }, { 32, 8, 4 }, { 32, 16, 4 }, { 64, 32, 8 } },
    /* vertical */
    { { 8, 16, 1 }, { 8, 32, 1 }, { 16, 32, 2 }, { 32, 64, 4 } }
};

/* VRAM slot of the tile holding shape pixel (lx, ly). 1D mapping walks a
 * plain strip of tiles, w/8 per row; 2D mapping uses a 32 tile wide matrix
 * where the tile number's low 5 bits are the offset inside its page. */
static int obj_slot(int tpid, int oned, int tpr, int lx, int ly) {
    if (oned) return tpid + (ly >> 3) * tpr + (lx >> 3);
    return (tpid & 0x3E0) + (tpid & 0x1F) + (ly >> 3) * 32 + (lx >> 3);
}

/* One object pixel; returns 0 for transparent. An 8bpp tile is 64 bytes,
 * a 4bpp tile 32.  Only the 4bpp form has a transparent index: in 8bpp every
 * one of the 256 entries is a real colour, index 0 included, so the caller
 * has to decide transparency from the bit depth rather than from the value. */
static int obj_pixel(PPU *ppu, int slot, int bits, int pbk, int lx, int ly) {
    if (bits == 8) {
        uint32_t base = 0x06010000u + (uint32_t)slot * 64u;
        return vr8(ppu, base + (uint32_t)ly * 8u + (uint32_t)lx);
    }
    uint32_t base = 0x06010000u + (uint32_t)slot * 32u;
    uint8_t byte = vr8(ppu, base + (uint32_t)ly * 4u + ((uint32_t)lx >> 1));
    int idx = (lx & 1) ? (byte >> 4) : (byte & 0xF);
    return idx ? idx + pbk * 16 : 0;
}

/* Whether a sampled index covers its pixel. */
static int obj_solid(int bits, int idx) {
    return bits == 8 || idx != 0;
}

static void draw_obj(PPU *ppu, int y, Px *out, Px *under, uint8_t *objwin, int i) {
    uint16_t a0 = oam16(ppu, i * 4 + 0);
    uint16_t a1 = oam16(ppu, i * 4 + 1);
    uint16_t a2 = oam16(ppu, i * 4 + 2);

    int objmode = (a0 >> 10) & 3;     /* 0 normal, 1 semi transparent, 2 window */
    if (objmode == 3) return;
    int affine = (a0 >> 8) & 1;
    int dbl    = (a0 >> 9) & 1;       /* double size only for affine objects */
    int shape  = (a0 >> 14) & 3;
    int size   = (a1 >> 14) & 3;
    if (shape == 3) return;
    if (!affine && dbl) return;       /* bit 9 disables a non-affine object */

    const ObjShape *os = &obj_shapes[shape][size];
    int w = os->w, h = os->h, tpr = os->tpr;
    int bits = (a0 >> 13) & 1 ? 8 : 4;    /* 256 colour (8bpp) or 16 colour */
    /* DISPCNT bit 6: set = one dimensional tile mapping, clear = two. */
    int oned = (ppu->dispcnt & 0x0040) ? 1 : 0;
    int tpid = a2 & 0x3FF;
    int pbk  = (a2 >> 12) & 0xF;
    int sx   = a1 & 0x1FF;
    int sy   = (a0 & 0x00FF) - 8;   /* the OAM Y coordinate is screen Y + 8 */

    if (!affine) {
        int fy = y - sy;
        if (fy < 0 || fy >= h) return;
        int ly = fy;
        if ((a1 >> 13) & 1) ly = h - 1 - ly;
        int row = ly & 7;

        for (int px = 0; px < w; px++) {
            int dx = sx + px;
            if (dx < 0 || dx >= PPU_W) continue;
            int lx = px & 7;
            if ((a1 >> 12) & 1) lx = 7 - lx;
            /* `px` picks the tile column, `lx` the dot inside that tile. */
            int slot = obj_slot(tpid, oned, tpr, px, ly);
            int idx = obj_pixel(ppu, slot, bits, pbk, lx, row);
            if (obj_solid(bits, idx)) {
                if (objmode == 2) {
                    /* Object window mode: the object is not drawn, it only
                     * marks the object window region. */
                    objwin[dx] = 1;
                } else {
                    put(out, under, dx, c15(pal16(ppu, 256u + (uint32_t)idx)),
                        LYR_OBJ, objmode == 1);
                }
            }
        }
        return;
    }

    /* Affine object: the sample point is the centre of the shape, and the
     * display area is the (optionally doubled) bounding box. */
    int afford = (a1 >> 9) & 0x1F;
    double pa = oam_aff(ppu, afford, 0) / 256.0;
    double pb = oam_aff(ppu, afford, 1) / 256.0;
    double pc = oam_aff(ppu, afford, 2) / 256.0;
    double pd = oam_aff(ppu, afford, 3) / 256.0;
    double det = pa * pd - pb * pc;
    if (det > -1e-9 && det < 1e-9) return;

    int x0 = dbl ? sx - w : sx;
    int y0 = dbl ? sy - h : sy;
    int rw = dbl ? w * 2 : w;
    int rh = dbl ? h * 2 : h;
    if (y < y0 || y >= y0 + rh) return;

    double cx = sx + w * 0.5;
    double cy = sy + h * 0.5;
    double dy = y - cy;

    for (int px = 0; px < rw; px++) {
        int dx = x0 + px;
        if (dx < 0 || dx >= PPU_W) continue;
        double ddx = dx - cx;
        double u = w * 0.5 + (pd * ddx - pb * dy) / det;
        double v = h * 0.5 + (-pc * ddx + pa * dy) / det;
        int tu = (int)floor(u);
        int tv = (int)floor(v);
        if (tu < 0 || tu >= w || tv < 0 || tv >= h) continue;

        int slot = obj_slot(tpid, oned, tpr, tu, tv);
        int idx = obj_pixel(ppu, slot, bits, pbk, tu & 7, tv & 7);
        if (obj_solid(bits, idx)) {
            if (objmode == 2) {
                objwin[dx] = 1;
            } else {
                put(out, under, dx, c15(pal16(ppu, 256u + (uint32_t)idx)),
                    LYR_OBJ, objmode == 1);
            }
        }
    }
}

static void render_objs(PPU *ppu, int y, Px *out, Px *under, int prio,
                        uint8_t *objwin) {
    /* A lower OAM index has priority, so draw from the end and let earlier
     * objects overwrite what later ones drew. */
    for (int i = 127; i >= 0; i--) {
        if (((oam16(ppu, i * 4 + 2) >> 10) & 3) != prio) continue;
        draw_obj(ppu, y, out, under, objwin, i);
    }
}

/* ---- windows ------------------------------------------------------------ */

/* Windows are regions, not per-layer masks: the pixel belongs to the first
 * region that covers it (window 0, window 1, object window, outside), and
 * that region's control byte decides which layers and color special effects
 * apply.  Window 0 has the highest priority, the object window the lowest. */

static int win_hit(const PPU *ppu, int idx, int x, int y) {
    int x1 = ppu->winh[idx] >> 8, x2 = ppu->winh[idx] & 0xFF;
    int y1 = ppu->winv[idx] >> 8, y2 = ppu->winv[idx] & 0xFF;
    int xs = (x1 > x2) ? (x >= x1 || x < x2) : (x >= x1 && x < x2);
    int ys = (y1 > y2) ? (y >= y1 || y < y2) : (y >= y1 && y < y2);
    return xs && ys;
}

/* Control byte (BG0-3, OBJ, color special effects) for this pixel. */
static uint8_t region_control(const PPU *ppu, int x, int y, int objwin) {
    if ((ppu->dispcnt & 0x2000) && win_hit(ppu, 0, x, y)) {
        return (uint8_t)(ppu->winin & 0x3F);
    }
    if ((ppu->dispcnt & 0x4000) && win_hit(ppu, 1, x, y)) {
        return (uint8_t)((ppu->winin >> 8) & 0x3F);
    }
    if ((ppu->dispcnt & 0x8000) && objwin) {
        return (uint8_t)((ppu->winout >> 8) & 0x3F);
    }
    return (uint8_t)(ppu->winout & 0x3F);
}

/* Is `layer` enabled in this region?  The backdrop is always shown. */
static int layer_shown(uint8_t control, int layer) {
    if (layer == LYR_BACK) return 1;
    return (control >> layer) & 1;
}

/* ---- blending ----------------------------------------------------------- */

static uint32_t blend_pixel(int mode, uint32_t top, uint32_t under,
                            int eva, int evb, int evy) {
    int tr = (int)((top >> 16) & 0xFF), tg = (int)((top >> 8) & 0xFF), tb = (int)(top & 0xFF);
    int br = (int)((under >> 16) & 0xFF), bg = (int)((under >> 8) & 0xFF), bb = (int)(under & 0xFF);
    int r, g, b;

    switch (mode) {
        case BLEND_ALPHA:
            r = (tr * eva + br * evb) >> 4;
            g = (tg * eva + bg * evb) >> 4;
            b = (tb * eva + bb * evb) >> 4;
            break;
        case BLEND_BRIGHT:
            r = tr + (((255 - tr) * evy) >> 4);
            g = tg + (((255 - tg) * evy) >> 4);
            b = tb + (((255 - tb) * evy) >> 4);
            break;
        case BLEND_DARK:
            r = (tr * evy) >> 4;
            g = (tg * evy) >> 4;
            b = (tb * evy) >> 4;
            break;
        default:
            return top;
    }

    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* ---- layer drawing ------------------------------------------------------ */

/* ---- text (tile/map) background ----------------------------------------- */

static void render_bg_text(PPU *ppu, int b, int y, Px *out, Px *under) {
    uint16_t cnt = ppu->bgcnt[b];
    int eight = (cnt >> 7) & 1;
    int size  = cnt >> 14;
    int w = (size == 1 || size == 3) ? 512 : 256;
    int h = (size >= 2) ? 512 : 256;
    int cbb  = (cnt >> 2) & 3;
    int sbb  = (cnt >> 8) & 31;
    int mapcols = w >> 8;   /* 256-pixel wide columns */
    int rowoff = (ppu->bgvofs[b] + (uint32_t)y) & (h - 1);
    int ty = rowoff >> 3;
    int row = rowoff & 7;

    for (int x = 0; x < PPU_W; x++) {
        int xoff = (ppu->bghofs[b] + (uint32_t)x) & (w - 1);
        int col = xoff >> 8;
        int tx = (xoff >> 3) & 31;
        int px = xoff & 7;

        uint32_t start = 0x06000000u
                       + (uint32_t)(sbb + col + (rowoff >= 256 ? mapcols : 0)) * 0x800u
                       + (uint32_t)(ty & 31) * 64u + (uint32_t)tx * 2u;
        uint16_t entry = vr16(ppu, start);
        int tid = entry & 0x3FF;
        int flipx = (entry >> 10) & 1;
        int flipy = (entry >> 11) & 1;
        int pbk = (entry >> 12) & 0xF;

        int lx = px, ly = row;
        if (flipx) lx = 7 - lx;
        if (flipy) ly = 7 - ly;

        uint32_t taddr = (uint32_t)cbb * 0x4000u;
        int idx;
        if (eight) {
            /* 8bpp: all 256 entries are colours, so index 0 still covers its
             * pixel and the tile index in the map cannot be zero-skipped. */
            idx = vr8(ppu, taddr + (uint32_t)tid * 64u + (uint32_t)ly * 8u + (uint32_t)lx);
            put(out, under, x, c15(pal16(ppu, (uint32_t)idx)), b, 0);
            continue;
        } else {
            uint8_t byte = vr8(ppu, taddr + (uint32_t)tid * 32u + (uint32_t)ly * 4u + ((uint32_t)lx >> 1));
            idx = (lx & 1) ? (byte >> 4) : (byte & 0xF);
            if (idx) idx += pbk * 16;
        }
        if (idx) {
            put(out, under, x, c15(pal16(ppu, (uint32_t)idx)), b, 0);
        }
    }
}

/* ---- affine (rotation/scaling) background ------------------------------- */

static void render_bg_affine(PPU *ppu, int b, int y, Px *out, Px *under) {
    uint16_t cnt = ppu->bgcnt[b];
    int sbb = (cnt >> 8) & 31;
    int size = cnt >> 14;
    int dim = 128 << size;   /* 128/256/512/1024 */
    int wrap = (cnt >> 13) & 1;
    int cbb = (cnt >> 2) & 3;
    int mapw = dim >> 3;
    uint32_t mapbase = 0x06000000u + (uint32_t)sbb * 0x800u;

    int64_t pa = bg_aff(ppu, b, 0), pb = bg_aff(ppu, b, 1);
    int64_t pc = bg_aff(ppu, b, 2), pd = bg_aff(ppu, b, 3);
    int64_t curx = (int64_t)ppu->bgx[b] + pb * y * 256;
    int64_t cury = (int64_t)ppu->bgy[b] + pd * y * 256;

    for (int x = 0; x < PPU_W; x++) {
        int32_t mx = (int32_t)(curx >> 16);
        int32_t my = (int32_t)(cury >> 16);
        if (wrap) {
            mx &= dim - 1;
            my &= dim - 1;
        } else if (mx < 0 || my < 0 || mx >= dim || my >= dim) {
            curx += pa * 256;
            cury += pc * 256;
            continue;
        }

        uint8_t tid = vr8(ppu, mapbase + (uint32_t)(my >> 3) * (uint32_t)mapw + (uint32_t)(mx >> 3));
        if (tid) {
            uint32_t taddr = (uint32_t)cbb * 0x4000u;
            /* Affine backgrounds are always 8bpp, so the index is a colour
             * and never a transparent hole. */
            uint8_t index = vr8(ppu, taddr + (uint32_t)tid * 64u
                            + (uint32_t)(my & 7) * 8u + (uint32_t)(mx & 7));
            put(out, under, x, c15(pal16(ppu, index)), b, 0);
        }

        curx += pa * 256;
        cury += pc * 256;
    }
}

/* ---- bitmap backgrounds (modes 3, 4, 5) ---------------------------------- */

static void render_bg_bitmap(PPU *ppu, int y, Px *out, Px *under) {
    int mode = ppu->dispcnt & 7;
    int page = (ppu->dispcnt >> 4) & 1;

    if (mode == 3) {
        for (int x = 0; x < PPU_W; x++) {
            put(out, under, x, c15(vr16(ppu, (uint32_t)(y * PPU_W + x) * 2u)), LYR_BG2, 0);
        }
    } else if (mode == 4) {
        /* Mode 4 is an 8bpp indexed bitmap: no entry is transparent, so
         * index 0 shows palette colour 0 like every other one. */
        for (int x = 0; x < PPU_W; x++) {
            uint8_t idx = vr8(ppu, (uint32_t)page * 0xA000u + (uint32_t)(y * PPU_W + x));
            put(out, under, x, c15(pal16(ppu, idx)), LYR_BG2, 0);
        }
    } else { /* mode 5 */
        if (y >= 128) return;
        for (int x = 0; x < 160; x++) {
            put(out, under, x,
                c15(vr16(ppu, (uint32_t)page * 0xA000u + (uint32_t)(y * 160 + x) * 2u)),
                LYR_BG2, 0);
        }
    }
}


void ppu_render_scanline(PPU *ppu, int y) {
    Px out[PPU_W], under[PPU_W];
    uint8_t objwin[PPU_W];
    uint16_t cnt = ppu->dispcnt;
    int mode = cnt & 7;
    int forced = (cnt >> 7) & 1;
    uint32_t backdrop = c15(pal16(ppu, 0));

    for (int x = 0; x < PPU_W; x++) {
        out[x].color = backdrop;
        out[x].layer = LYR_BACK;
        out[x].semi = 0;
        under[x] = out[x];
        objwin[x] = 0;
    }

    if (forced) {
        for (int x = 0; x < PPU_W; x++) {
            ppu->frame[y * PPU_W + x] = 0xFFFFFFu;
        }
        return;
    }

    /* Draw in priority order: 3 (lowest) first so priority 0 ends up on
     * top, and within a priority the higher BG number wins. */
    for (int p = 3; p >= 0; p--) {
        for (int b = 0; b < 4; b++) {
            if (!(cnt & (1u << (8 + b)))) continue;
            int usable;
            switch (mode) {
                case 0: usable = 1; break;
                case 1: usable = (b < 3); break;
                case 2: usable = (b >= 2); break;
                default: usable = (b == 2); break;
            }
            if (!usable) continue;
            if ((ppu->bgcnt[b] & 3) != p) continue;

            if (mode >= 3) {
                render_bg_bitmap(ppu, y, out, under);
            } else if (mode == 2 || (mode == 1 && b == 2)) {
                render_bg_affine(ppu, b, y, out, under);
            } else {
                render_bg_text(ppu, b, y, out, under);
            }
        }
        if (cnt & (1u << 12)) {
            render_objs(ppu, y, out, under, p, objwin);
        }
    }

    /* Resolve the window region of each pixel, then apply color special
     * effects.  With no window enabled at all the hardware shows every
     * enabled layer, regardless of the (reset value) WININ/WINOUT. */
    {
        int windows_on = (cnt & 0xE000) != 0;
        int obj_mode = ppu->bldcnt & 3;         /* semi transparent OBJ mode */
        int effect = (ppu->bldcnt >> 6) & 3;    /* general color effect */
        int eva = ppu->bldalpha & 0x1F;
        int evb = (ppu->bldalpha >> 8) & 0x1F;
        int evy = ppu->bldy & 0x1F;
        if (eva > 16) eva = 16;
        if (evb > 16) evb = 16;
        if (evy > 16) evy = 16;

        for (int x = 0; x < PPU_W; x++) {
            uint8_t control = windows_on ? region_control(ppu, x, y, objwin[x]) : 0x3F;
            Px top = out[x];
            Px back = under[x];

            /* Skip layers that the region does not enable. */
            if (!layer_shown(control, top.layer)) {
                top = back;
            }
            back.color = backdrop;
            back.layer = LYR_BACK;
            back.semi = 0;
            if (!layer_shown(control, top.layer)) {
                top.color = backdrop;
                top.layer = LYR_BACK;
                top.semi = 0;
            }

            int blend = BLEND_OFF;
            int first = (ppu->bldcnt >> top.layer) & 1;
            int second = (ppu->bldcnt >> (8 + back.layer)) & 1;
            int win_blend = (control >> 5) & 1;

            if (top.semi) {
                /* Semi transparent objects always blend with what is below
                 * them, using the mode from BLDCNT bits 0-1. */
                blend = obj_mode ? obj_mode : BLEND_ALPHA;
            } else if (effect != BLEND_OFF && win_blend && first && second) {
                blend = effect;
            }

            ppu->frame[y * PPU_W + x] =
                blend == BLEND_OFF ? top.color
                                   : blend_pixel(blend, top.color, back.color, eva, evb, evy);
        }
    }
}

/* ---- register access ------------------------------------------------------------- */

uint16_t ppu_io_read16(PPU *ppu, uint32_t address) {
    switch (address & 0x7E) {
        case 0x00: return ppu->dispcnt;
        case 0x04: return ppu->dispstat;
        case 0x06: return ppu->vcount;
        case 0x08: return ppu->bgcnt[0];
        case 0x0A: return ppu->bgcnt[1];
        case 0x0C: return ppu->bgcnt[2];
        case 0x0E: return ppu->bgcnt[3];
        case 0x40: return ppu->winh[0];
        case 0x42: return ppu->winh[1];
        case 0x44: return ppu->winv[0];
        case 0x46: return ppu->winv[1];
        case 0x48: return ppu->winin;
        case 0x4A: return ppu->winout;
        case 0x4C: return ppu->mosaic;
        case 0x50: return ppu->bldcnt;
        case 0x52: return ppu->bldalpha;
        case 0x54: return ppu->bldy;
        default:   return 0;
    }
}

void ppu_io_write16(PPU *ppu, uint32_t address, uint16_t value) {
    switch (address & 0x7E) {
        case 0x00: ppu->dispcnt = value; break;
        case 0x04:
            ppu->dispstat = (uint16_t)((ppu->dispstat & 0x00C7) | (value & 0x0038) | (value & 0xFF00));
            break;
        case 0x08: ppu->bgcnt[0] = value; break;
        case 0x0A: ppu->bgcnt[1] = value; break;
        case 0x0C: ppu->bgcnt[2] = value; break;
        case 0x0E: ppu->bgcnt[3] = value; break;
        case 0x10: ppu->bghofs[0] = value; break;
        case 0x12: ppu->bgvofs[0] = value; break;
        case 0x14: ppu->bghofs[1] = value; break;
        case 0x16: ppu->bgvofs[1] = value; break;
        case 0x18: ppu->bghofs[2] = value; break;
        case 0x1A: ppu->bgvofs[2] = value; break;
        case 0x1C: ppu->bghofs[3] = value; break;
        case 0x1E: ppu->bgvofs[3] = value; break;
        case 0x20: ppu->pa[2] = (int16_t)value; break;
        case 0x22: ppu->pb[2] = (int16_t)value; break;
        case 0x24: ppu->pc[2] = (int16_t)value; break;
        case 0x26: ppu->pd[2] = (int16_t)value; break;
        case 0x28: ppu->bgx[2] = (int32_t)((uint32_t)ppu->bgx[2] & 0xFFFF0000u) | value; break;
        case 0x2A: ppu->bgx[2] = (int32_t)((uint32_t)ppu->bgx[2] & 0x0000FFFFu) | ((uint32_t)value << 16); break;
        case 0x2C: ppu->bgy[2] = (int32_t)((uint32_t)ppu->bgy[2] & 0xFFFF0000u) | value; break;
        case 0x2E: ppu->bgy[2] = (int32_t)((uint32_t)ppu->bgy[2] & 0x0000FFFFu) | ((uint32_t)value << 16); break;
        case 0x30: ppu->pa[3] = (int16_t)value; break;
        case 0x32: ppu->pb[3] = (int16_t)value; break;
        case 0x34: ppu->pc[3] = (int16_t)value; break;
        case 0x36: ppu->pd[3] = (int16_t)value; break;
        case 0x38: ppu->bgx[3] = (int32_t)((uint32_t)ppu->bgx[3] & 0xFFFF0000u) | value; break;
        case 0x3A: ppu->bgx[3] = (int32_t)((uint32_t)ppu->bgx[3] & 0x0000FFFFu) | ((uint32_t)value << 16); break;
        case 0x3C: ppu->bgy[3] = (int32_t)((uint32_t)ppu->bgy[3] & 0xFFFF0000u) | value; break;
        case 0x3E: ppu->bgy[3] = (int32_t)((uint32_t)ppu->bgy[3] & 0x0000FFFFu) | ((uint32_t)value << 16); break;
        case 0x40: ppu->winh[0] = value; break;
        case 0x42: ppu->winh[1] = value; break;
        case 0x44: ppu->winv[0] = value; break;
        case 0x46: ppu->winv[1] = value; break;
        case 0x48: ppu->winin = value; break;
        case 0x4A: ppu->winout = value; break;
        case 0x4C: ppu->mosaic = value; break;
        case 0x50: ppu->bldcnt = value; break;
        case 0x52: ppu->bldalpha = value; break;
        case 0x54: ppu->bldy = value; break;
        default:   break;
    }
}

void ppu_set_vcount(PPU *ppu, uint16_t v) {
    ppu->vcount = v;
    uint16_t setting = (ppu->dispstat >> 8) & 0xFF;
    ppu->dispstat = (uint16_t)((ppu->dispstat & ~0x0004u)
                            | ((((v & 0xFF) == setting) ? 0x0004u : 0u)));
}

void ppu_set_vblank(PPU *ppu, int on) {
    ppu->dispstat = (uint16_t)(on ? (ppu->dispstat | 1u) : (ppu->dispstat & ~1u));
}

void ppu_set_hblank(PPU *ppu, int on) {
    ppu->dispstat = (uint16_t)(on ? (ppu->dispstat | 2u) : (ppu->dispstat & ~2u));
}

uint16_t ppu_irq_flags(const PPU *ppu) {
    uint16_t flags = 0;
    if ((ppu->dispstat & 0x0001u) && (ppu->dispstat & 0x0008u)) flags |= 1u;
    if ((ppu->dispstat & 0x0002u) && (ppu->dispstat & 0x0010u)) flags |= 2u;
    if ((ppu->dispstat & 0x0004u) && (ppu->dispstat & 0x0020u)) flags |= 4u;
    return flags;
}

void ppu_init(PPU *ppu, Memory *mem) {
    ppu->mem = mem;
    ppu_reset(ppu);
}

void ppu_reset(PPU *ppu) {
    Memory *mem = ppu->mem;
    memset(ppu, 0, sizeof(*ppu));
    ppu->mem = mem;
    /* DISPSTAT comes up with the VBlank IRQ enable set, so a game that never
     * touches it still gets the vblank interrupt. */
    ppu->dispstat = 0x0008u;
}
