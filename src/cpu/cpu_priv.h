#ifndef GBA_CPU_PRIV_H
#define GBA_CPU_PRIV_H

#include "cpu.h"

/* Internal helpers shared between the ARM and Thumb cores.
 * Declared here rather than in cpu.h to keep the public API tidy. */

uint32_t cpu_read_reg(const CPU *cpu, uint32_t r);
void     cpu_write_reg(CPU *cpu, uint32_t r, uint32_t value);

int      cpu_flag_n(const CPU *cpu);
int      cpu_flag_z(const CPU *cpu);
int      cpu_flag_c(const CPU *cpu);
int      cpu_flag_v(const CPU *cpu);
void     cpu_set_flags(CPU *cpu, uint32_t n, uint32_t z, uint32_t c, uint32_t v);

/* a + b + carry_in; reports unsigned carry-out and signed overflow. */
uint32_t cpu_add_carry(uint32_t a, uint32_t b, uint32_t carry_in,
                       uint32_t *carry_out, uint32_t *overflow_out);

/* Barrel shifter. shift types follow the ARM convention. */
enum {
    CPU_SHIFT_LSL = 0,
    CPU_SHIFT_LSR = 1,
    CPU_SHIFT_ASR = 2,
    CPU_SHIFT_ROR = 3
};

uint32_t cpu_shift_c(CPU *cpu, uint32_t type, uint32_t value, uint32_t amount,
                     uint32_t carry_in, uint32_t *carry_out);

/* Thumb core: executes one 16-bit instruction at cpu->reg[15] - 4 (the
 * instruction address), returning an approximate cycle count. */
uint32_t thumb_execute(CPU *cpu, Memory *mem, uint16_t insn);

#endif