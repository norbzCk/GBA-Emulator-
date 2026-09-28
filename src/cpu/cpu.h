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

/* Reason the CPU is not executing instructions. */
enum {
    CPU_RUN   = 0,  /* running normally */
    CPU_HALT  = 1,  /* SWI 2 / HALTCNT=1: wake on any enabled interrupt */
    CPU_STOP  = 2   /* SWI 3 / HALTCNT=2: wake only on keypad or gamepak */
};

typedef struct CPU {
    uint32_t reg[16];
    uint32_t r8_12_fiq[5];
    uint32_t r13[BANK_COUNT];
    uint32_t r14[BANK_COUNT];
    uint32_t spsr[BANK_COUNT];
    uint32_t cpsr;
    bool     halted;
    int      wait_mode;   /* CPU_RUN / CPU_HALT / CPU_STOP */
    /* Set whenever an instruction writes reg[15] itself. cpu_step() uses this
     * instead of comparing reg[15] against the pipeline value, because a
     * branch target may legitimately equal addr + 4 (Thumb) or addr + 8
     * (ARM) - e.g. "bx pc" inside a two-byte-aligned function. */
    bool     pc_written;

    /* Software interrupt hook. When set it is called instead of vectoring
     * into the BIOS, which lets the emulator run the BIOS services
     * (SWI handlers) in C. reg[15] already points at the return address. */
    void (*swi_hook)(void *ctx, uint32_t number);
    void  *swi_ctx;
} CPU;

void     cpu_init(CPU *cpu);
void     cpu_reset(CPU *cpu);

/* Write a branch target into reg[15] and record that the instruction branched. */
void     cpu_set_pc(CPU *cpu, uint32_t value);

/* Put the CPU into one of the CPU_* wait states. */
void     cpu_set_wait(CPU *cpu, int mode);

/* Execute one instruction (ARM or Thumb depending on the T flag).
 * Returns an approximate cycle count. */
uint32_t cpu_step(CPU *cpu, Memory *mem);

/* Enter the IRQ exception: saves CPSR/SPSR, switches to IRQ mode and
 * vectors to 0x18. */
void cpu_irq(CPU *cpu);

/* Read/write a register through the current mode's bank set. */
uint32_t cpu_read_reg(const CPU *cpu, uint32_t r);
void     cpu_write_reg(CPU *cpu, uint32_t r, uint32_t value);

#endif