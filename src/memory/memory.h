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
#define GBA_SAVE_SIZE  0x20000   /* largest cartridge backup: 128 KB EEPROM */

struct HW;

/* Cartridge backup chips.  The backup lives in the 0x0A000000-0x0D00FFFF
 * window, but each kind answers differently: SRAM is plain memory, Flash
 * is a command-driven chip and EEPROM is a bit-serial device. */
enum {
    SAVE_NONE = 0,
    SAVE_SRAM,     /* 8/32/64 KB of plain memory */
    SAVE_EEPROM,   /* 512 B - 128 KB, bit-serial at 0x0D000000 */
    SAVE_FLASH     /* 64/128 KB, command driven */
};

/* Bit-serial phase of an EEPROM access. */
enum {
    EEP_ADDR = 0,  /* shifting in an address, terminated by a 1 bit */
    EEP_DATA       /* shifting the payload in or out */
};

typedef struct CartSave {
    uint32_t kind;
    uint32_t size;      /* bytes of `data` in use */
    uint32_t bank;      /* selected flash bank */
    uint32_t flash_cmd; /* pending flash command, 0 when none */
    uint32_t flash_cmd_addr;
    uint32_t step;       /* which part of an access is in progress */
    uint32_t addr;       /* address bits collected so far */
    uint32_t steps_left; /* bits still expected in the current step */
    uint32_t addr_bits;  /* address width: the chip knows this, it is fixed */
    uint32_t data_bits;  /* payload width: 2 for the narrow chips, else 8 */
    uint32_t bit_index;  /* next payload bit to hand out on a read */
    uint32_t pad;        /* trailing bits of a byte that belong to no access */
    uint8_t  data[GBA_SAVE_SIZE];
} CartSave;

typedef struct Memory {
    uint8_t  bios[GBA_BIOS_SIZE];
    uint8_t  ewram[GBA_EWRAM_SIZE];
    uint8_t  iwram[GBA_IWRAM_SIZE];
    uint8_t  mmio[GBA_MMIO_SIZE];
    uint8_t  pal[GBA_PAL_SIZE];
    uint8_t  vram[GBA_VRAM_SIZE];
    uint8_t  oam[GBA_OAM_SIZE];
    CartSave save;
    uint8_t *rom;
    uint32_t rom_size;
    struct HW *hw;   /* set via memory_set_io to route the MMIO region */
} Memory;

void memory_init(Memory *mem);
void memory_free(Memory *mem);

int   memory_load_rom(Memory *mem, const char *path);
int   memory_load_rom_data(Memory *mem, const uint8_t *data, uint32_t size);

/* Map a 16 KB BIOS image.  Without one the emulator high-level emulates the
 * BIOS services, which works for almost every game. */
int   memory_load_bios(Memory *mem, const char *path);

/* Cartridge backup (SRAM/flash).  Loading a missing file is not an error:
 * the save is simply empty. */
int   memory_load_save(Memory *mem, const char *path);
int   memory_store_save(const Memory *mem, const char *path);

/* Route 04000000h-040003FFh accesses to the hardware subsystem. Passing a
 * NULL hw keeps the plain register array behaviour. */
void memory_set_io(Memory *mem, struct HW *hw);

uint8_t  memory_read8 (const Memory *mem, uint32_t address);
uint16_t memory_read16(const Memory *mem, uint32_t address);
uint32_t memory_read32(const Memory *mem, uint32_t address);

void memory_write8 (Memory *mem, uint32_t address, uint8_t value);
void memory_write16(Memory *mem, uint32_t address, uint16_t value);
void memory_write32(Memory *mem, uint32_t address, uint32_t value);

#endif