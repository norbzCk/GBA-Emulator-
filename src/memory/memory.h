#ifndef GBA_MEMORY_H
#define GBA_MEMORY_H

#include <stdint.h>
#include <stdlib.h>

#define GBA_BIOS_SIZE  0x4000
#define GBA_EWRAM_SIZE 0x40000
#define GBA_IWRAM_SIZE 0x8000
#define GBA_MMIO_SIZE  0x400
#define GBA_PAL_SIZE   0x400
#define GBA_VRAM_SIZE  0x18000
#define GBA_OAM_SIZE   0x400
#define GBA_ROM_SIZE   0x2000000
#define GBA_SRAM_SIZE  0x10000

typedef struct Memory {
    uint8_t  bios[GBA_BIOS_SIZE];
    uint8_t  ewram[GBA_EWRAM_SIZE];
    uint8_t  iwram[GBA_IWRAM_SIZE];
    uint8_t  mmio[GBA_MMIO_SIZE];
    uint8_t  pal[GBA_PAL_SIZE];
    uint8_t  vram[GBA_VRAM_SIZE];
    uint8_t  oam[GBA_OAM_SIZE];
    uint8_t  sram[GBA_SRAM_SIZE];
    uint8_t *rom;
    uint32_t rom_size;
} Memory;

void memory_init(Memory *mem);
void memory_free(Memory *mem);

int   memory_load_rom(Memory *mem, const char *path);

uint8_t  memory_read8 (const Memory *mem, uint32_t address);
uint16_t memory_read16(const Memory *mem, uint32_t address);
uint32_t memory_read32(const Memory *mem, uint32_t address);

void memory_write8 (Memory *mem, uint32_t address, uint8_t value);
void memory_write16(Memory *mem, uint32_t address, uint16_t value);
void memory_write32(Memory *mem, uint32_t address, uint32_t value);

#endif