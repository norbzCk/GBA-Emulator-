#ifndef GBA_CPU_H
#define GBA_CPU_H

#include <stdbool.h>
#include <stdint.h>

#include "../memory/memory.h"

#define MODE_USR 0x10
#define MODE_FIQ 0x11
#define MODE_IRQ 0x12
#define MODE_SVC 0x13
#define MODE_ABT 0x17
#define MODE_UND 0x1B
#define MODE_SYS 0x1F

#define FLAG_N 0x80000000u
#define FLAG_Z 0x40000000u
#define FLAG_C 0x20000000u
#define FLAG_V 0x10000000u
#define FLAG_I 0x00000080u
#define FLAG_F 0x00000040u
#define FLAG_T 0x00000020u

enum {
    BANK_USR = 0,
    BANK_FIQ = 1,
    BANK_SVC = 2,
    BANK_ABT = 3,
    BANK_IRQ = 4,
    BANK_UND = 5,
    BANK_COUNT = 6
};

typedef struct CPU {
    uint32_t reg[16];
    uint32_t r8_12_fiq[5];
    uint32_t r13[BANK_COUNT];
    uint32_t r14[BANK_COUNT];
    uint32_t spsr[BANK_COUNT];
    uint32_t cpsr;
    bool     halted;
} CPU;

void cpu_init(CPU *cpu);
void cpu_reset(CPU *cpu);
void cpu_step(CPU *cpu, Memory *mem);

#endif