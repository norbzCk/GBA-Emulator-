#include "testrom.h"

#include <string.h>

/* A minimal ARM assembler: just the handful of instructions the test program
 * needs, plus a literal pool.  Keeping it here means the end-to-end test has
 * no external dependencies. */

enum {
    MAX_POOL = 16,
    MAX_FIXUPS = 16
};

enum { L_GRAY, L_ROW, L_PIXEL, L_HALT, L_COUNT };

typedef struct Asm {
    uint32_t *code;
    uint32_t n;
    uint32_t label[L_COUNT];
    uint32_t pool[MAX_POOL];
    uint32_t pool_n;
    uint32_t ldr_insn[MAX_FIXUPS];
    uint32_t ldr_slot[MAX_FIXUPS];
    uint32_t ldr_n;
    uint32_t br_insn[MAX_FIXUPS];
    uint32_t br_cond[MAX_FIXUPS];
    uint32_t br_label[MAX_FIXUPS];
    uint32_t br_n;
} Asm;

static uint32_t emit(Asm *a, uint32_t insn) {
    a->code[a->n++] = insn;
    return a->n;
}

static void mark(Asm *a, int id) {
    a->label[id] = a->n;
}

/* ldr rd, =value */
static void ldr_const(Asm *a, int rd, uint32_t value) {
    uint32_t at = emit(a, 0xE59F0000u | ((uint32_t)rd << 12));
    a->ldr_insn[a->ldr_n] = at;
    a->ldr_slot[a->ldr_n] = a->pool_n;
    a->ldr_n++;
    a->pool[a->pool_n++] = value;
}

/* b/bl to a label, patched once every label is known */
static void branch(Asm *a, uint32_t cond, int label) {
    uint32_t at = emit(a, 0);
    a->br_insn[a->br_n] = at;
    a->br_cond[a->br_n] = cond;
    a->br_label[a->br_n] = (uint32_t)label;
    a->br_n++;
}

#define A_MOV(rd, rm)          (0xE1A00000u | ((uint32_t)(rd) << 12) | (uint32_t)(rm))
#define A_MOVI(rd, imm)        (0xE3A00000u | ((uint32_t)(imm) & 0xFFu) | ((uint32_t)(rd) << 12))
#define A_STR(rd, rn)          (0xE5800000u | ((uint32_t)(rn) << 16) | ((uint32_t)(rd) << 12))
#define A_STRH(rd, rn)         (0xE1C000B0u | ((uint32_t)(rn) << 16) | ((uint32_t)(rd) << 12))
#define A_STRB(rd, rn)         (0xE5C00000u | ((uint32_t)(rn) << 16) | ((uint32_t)(rd) << 12))
/* ADD rd, rn, rm, LSL #shift */
#define A_ADD(rd, rn, rm, shift) (0xE0800000u | ((uint32_t)(rn) << 16) \
                                 | ((uint32_t)(rd) << 12) | ((uint32_t)(shift) << 7) \
                                 | (uint32_t)(rm))
#define A_ADDI(rd, rn, imm)    (0xE2800000u | ((uint32_t)(imm) & 0xFFu) \
                                | ((uint32_t)(rn) << 16) | ((uint32_t)(rd) << 12))
#define A_CMP(rn, imm)         (0xE3500000u | ((uint32_t)(imm) & 0xFFu) | ((uint32_t)(rn) << 16))
#define A_CMPR(rn, rm)        (0xE0500000u | ((uint32_t)(rn) << 16) | (uint32_t)(rm))
#define A_SWI(n)               (0xEF000000u | (uint32_t)(n))

#define A_LT 0xBu
#define A_AL 0xEu

/* ARM immediates are 8 bits rotated right by an even amount, so values like
 * 256 need the rotate field; anything else becomes a literal pool entry. */
static void mov_imm(Asm *a, int rd, uint32_t value) {
    if (value <= 0xFFu) {
        emit(a, A_MOVI(rd, value));
        return;
    }
    for (uint32_t rot = 1; rot < 16; rot++) {
        /* the encoder holds v ROR (rot*2), so v is value rotated the other way */
        uint32_t v = (value << (rot * 2)) | (value >> (32 - rot * 2));
        if (v <= 0xFFu) {
            emit(a, 0xE3A00000u | v | (rot << 8) | ((uint32_t)rd << 12));
            return;
        }
    }
    ldr_const(a, rd, value);
}

static void finish(Asm *a) {
    /* The pool follows the code, so entry N lives at word a->n + N.  PC reads
     * as the instruction address plus 8, which is two words further on; the
     * LDR offset field counts bytes. */
    for (uint32_t i = 0; i < a->ldr_n; i++) {
        uint32_t at = a->ldr_insn[i];
        uint32_t off = (a->n + a->ldr_slot[i] - at - 1) << 2;
        a->code[at - 1] = 0xE59F0000u | (a->code[at - 1] & 0xF000u)
                        | (off & 0xFFFu);
    }
    for (uint32_t i = 0; i < a->br_n; i++) {
        uint32_t at = a->br_insn[i];
        int32_t delta = (int32_t)a->label[a->br_label[i]] - (int32_t)at - 1;
        uint32_t op = (a->br_cond[i] << 28) | 0x0A000000u;
        a->code[at - 1] = op | ((uint32_t)delta & 0x00FFFFFFu);
    }
}

void test_rom_build(uint8_t *rom, uint32_t size) {
    uint32_t words[TEST_ROM_SIZE / 4];
    Asm a;

    memset(rom, 0, size);
    memset(&a, 0, sizeof(a));
    a.code = words;

    /* DISPCNT = mode 4 (8bpp paletted), BG2 on. */
    ldr_const(&a, 0, 0x04000000u);
    ldr_const(&a, 1, 0x0404u);
    emit(&a, A_STRH(1, 0));

    /* Palette entry k = k, so the whole ramp is easy to predict. */
    ldr_const(&a, 2, 0x05000000u);
    emit(&a, A_MOVI(5, 0));           /* k */
    mov_imm(&a, 4, 256);              /* limit */
    mark(&a, L_GRAY);
    emit(&a, A_ADD(6, 2, 5, 1));      /* palette + 2k */
    emit(&a, A_STRH(5, 6));
    emit(&a, A_ADDI(5, 5, 1));
    emit(&a, A_CMPR(5, 4));
    branch(&a, A_LT, L_GRAY);

    /* Fill the frame buffer: pixel (x,y) = (x + y) & 0xFF. */
    ldr_const(&a, 1, 0x06000000u);    /* cursor */
    emit(&a, A_MOVI(6, 0));           /* y */
    mark(&a, L_ROW);
    emit(&a, A_MOV(2, 1));            /* row cursor */
    emit(&a, A_MOVI(4, 0));           /* x */
    emit(&a, A_MOVI(3, 240));         /* row width */
    mark(&a, L_PIXEL);
    emit(&a, A_ADD(5, 4, 6, 0));      /* x + y */
    emit(&a, A_STRB(5, 2));
    emit(&a, A_ADDI(2, 2, 1));
    emit(&a, A_ADDI(4, 4, 1));
    emit(&a, A_CMPR(4, 3));
    branch(&a, A_LT, L_PIXEL);
    emit(&a, A_MOV(1, 2));
    emit(&a, A_ADDI(6, 6, 1));
    emit(&a, A_CMP(6, 160));
    branch(&a, A_LT, L_ROW);

    /* Enable the VBlank interrupt. */
    emit(&a, A_MOVI(1, 1));
    ldr_const(&a, 0, 0x04000200u);    /* IE */
    emit(&a, A_STRH(1, 0));
    ldr_const(&a, 0, 0x04000208u);    /* IME */
    emit(&a, A_STRH(1, 0));

    /* Two VBlankIntrWait calls, so the halt/resume path runs more than once. */
    emit(&a, A_MOVI(0, 1));
    emit(&a, A_SWI(0x05));
    emit(&a, A_MOV(0, 1));
    emit(&a, A_SWI(0x05));

    /* Marker last: the test looks for this in internal WRAM, so it only shows
     * up once the waits above have come back. */
    ldr_const(&a, 0, 0x03000000u);
    ldr_const(&a, 1, 0xC0DEC0DEu);
    emit(&a, A_STR(1, 0));

    mark(&a, L_HALT);
    branch(&a, A_AL, L_HALT);         /* b . : spin forever once done */

    finish(&a);

    /* Append the literal pool so control never falls into it, and store the
     * image little endian. */
    for (uint32_t i = 0; i < a.pool_n; i++) {
        words[a.n++] = a.pool[i];
    }
    for (uint32_t i = 0; i < a.n && (i * 4 + 3) < size; i++) {
        rom[i * 4 + 0] = (uint8_t)(words[i] & 0xFF);
        rom[i * 4 + 1] = (uint8_t)((words[i] >> 8) & 0xFF);
        rom[i * 4 + 2] = (uint8_t)((words[i] >> 16) & 0xFF);
        rom[i * 4 + 3] = (uint8_t)((words[i] >> 24) & 0xFF);
    }
}

uint32_t test_rom_expected_color(uint32_t value) {
    uint32_t r = ((value & 0x1Fu) * 255u) / 31u;
    uint32_t g = (((value >> 5) & 0x1Fu) * 255u) / 31u;
    uint32_t b = (((value >> 10) & 0x1Fu) * 255u) / 31u;
    return (r << 16) | (g << 8) | b;
}
