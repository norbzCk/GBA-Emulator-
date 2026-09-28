#ifndef GBA_BIOS_H
#define GBA_BIOS_H

#include <stdint.h>

#include "cpu.h"
#include "../memory/memory.h"
#include "../hw/hw.h"

/* Execute BIOS function `number` (the SWI immediate) on behalf of the game.
 * The CPU's reg[15] already holds the address to resume at, so returning
 * from this function returns to the caller of the SWI. */
void bios_swi(Memory *mem, CPU *cpu, HW *hw, uint32_t number);

/* Individual services, exposed for testing. */
void bios_cpu_set(Memory *mem, CPU *cpu, int fast);
void bios_lz77(Memory *mem, uint32_t src, uint32_t dst, int width);
void bios_rl(Memory *mem, uint32_t src, uint32_t dst, int width);
void bios_huffman(Memory *mem, uint32_t src, uint32_t dst);
void bios_bit_unpack(Memory *mem, uint32_t src, uint32_t dst, uint32_t info);
void bios_filter(Memory *mem, uint32_t src, uint32_t dst, int inwidth);

#endif
