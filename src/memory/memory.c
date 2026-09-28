#include "memory.h"

#include <stdio.h>
#include <string.h>

#include "../hw/hw.h"

static int io_region(uint32_t address) {
    return (address & 0xFF000000) == 0x04000000;
}

/*
 * Resolve an address to a pointer in one of the GBA's memory regions.
 *
 * Each region lives in a power-of-two bank slot and mirrors every
 * power-of-two size, so the mirroring is just a mask. Addresses outside
 * the real data (e.g. 0x00004000+ in the BIOS slot, or past the end of a
 * loaded ROM) are open bus: reads return zero, writes are dropped.
 *
 * On success, *address is replaced by the index within the region.
 */
static const uint8_t *region_ptr(const Memory *mem, uint32_t *address) {
    uint32_t index;
    const uint8_t *base;

    /* BIOS: only the first 16 KB of the 0x00xxxxxx slot is real. */
    if ((*address & 0xFF000000) == 0x00000000) {
        index = *address & (GBA_BIOS_SIZE - 1);
        if (index < GBA_BIOS_SIZE) {
            *address = index;
            return mem->bios + index;
        }
        return NULL;
    }

    /* External WRAM. */
    if ((*address & 0xFF000000) == 0x02000000) {
        index = *address & (GBA_EWRAM_SIZE - 1);
        base  = mem->ewram;
    }
    /* Internal WRAM. */
    else if ((*address & 0xFF000000) == 0x03000000) {
        index = *address & (GBA_IWRAM_SIZE - 1);
        base  = mem->iwram;
    }
    /* MMIO registers. */
    else if ((*address & 0xFF000000) == 0x04000000) {
        index = *address & (GBA_MMIO_SIZE - 1);
        base  = mem->mmio;
    }
    /* Palette RAM. */
    else if ((*address & 0xFF000000) == 0x05000000) {
        index = *address & (GBA_PAL_SIZE - 1);
        base  = mem->pal;
    }
    /* VRAM: 96 KB used; the last 32 KB of the 128 KB window mirrors low. */
    else if ((*address & 0xFF000000) == 0x06000000) {
        index = *address & 0x1FFFF;
        if (index >= GBA_VRAM_SIZE) {
            index &= 0x7FFF;
        }
        base = mem->vram;
    }
    /* OAM. */
    else if ((*address & 0xFF000000) == 0x07000000) {
        index = *address & (GBA_OAM_SIZE - 1);
        base  = mem->oam;
    }
    /* GamePak ROM. */
    else if ((*address & 0xFE000000) == 0x08000000) {
        index = *address & 0x01FFFFFF;
        if (index < mem->rom_size) {
            return mem->rom + index;
        }
        return NULL; /* open bus */
    }
    /* Cart SRAM (0x0A, 0x0E and 0x10 slots all map here). */
    else if ((*address & 0xFE000000) == 0x0A000000 ||
             (*address & 0xFE000000) == 0x0E000000 ||
             (*address & 0xF0000000) == 0x10000000) {
        index = *address & (GBA_SRAM_SIZE - 1);
        base  = mem->sram;
    }
    else {
        return NULL; /* unmapped */
    }

    *address = index;
    return base + index;
}

/* The BIOS records what IntrWait is waiting for at the top of IWRAM
 * (0x03007FF8, mirrored throughout 0x03xxxxxx). The hardware block shadows
 * that word so the emulated interrupt dispatcher can acknowledge the same
 * flags the BIOS would, so reads and writes have to go through it. Only the
 * two IntrWaitFlags bytes are affected: 0x03007FFA holds the BIOS IRQ check
 * bits and 0x03007FFC the handler address, both plain IWRAM. */
static int bios_irq_flags_at(uint32_t address) {
    uint32_t index = address & (GBA_IWRAM_SIZE - 1);
    return index == 0x7FF8u || index == 0x7FF9u;
}

uint8_t memory_read8(const Memory *mem, uint32_t address) {
    if (io_region(address) && mem->hw) {
        return (uint8_t)hw_io_read(mem->hw, address & 0x3FF, 8);
    }
    if (mem->hw && bios_irq_flags_at(address)) {
        return (uint8_t)(mem->hw->bios_irq_flags >> ((address & 1) * 8));
    }
    const uint8_t *p = region_ptr(mem, &address);
    return p ? *p : 0;
}

uint16_t memory_read16(const Memory *mem, uint32_t address) {
    if (io_region(address) && mem->hw) {
        return (uint16_t)hw_io_read(mem->hw, address & 0x3FF, 16);
    }
    return (uint16_t)memory_read8(mem, address)
        | (uint16_t)((uint16_t)memory_read8(mem, address + 1) << 8);
}

uint32_t memory_read32(const Memory *mem, uint32_t address) {
    return (uint32_t)memory_read16(mem, address)
        | ((uint32_t)memory_read16(mem, address + 2) << 16);
}

void memory_write8(Memory *mem, uint32_t address, uint8_t value) {
    if (io_region(address) && mem->hw) {
        hw_io_write(mem->hw, address & 0x3FF, value, 8);
        return;
    }
    if (mem->hw && bios_irq_flags_at(address)) {
        if (address & 1) {
            mem->hw->bios_irq_flags =
                (uint16_t)((mem->hw->bios_irq_flags & 0x00FFu) | ((uint16_t)value << 8));
        } else {
            mem->hw->bios_irq_flags =
                (uint16_t)((mem->hw->bios_irq_flags & 0xFF00u) | value);
        }
        return;
    }
    uint8_t *p = (uint8_t *)region_ptr(mem, &address);
    if (p) {
        *p = value;
    }
}

void memory_write16(Memory *mem, uint32_t address, uint16_t value) {
    if (io_region(address) && mem->hw) {
        hw_io_write(mem->hw, address & 0x3FF, value, 16);
        return;
    }
    uint8_t *p = (uint8_t *)region_ptr(mem, &address);
    if (p && (address & 1) == 0) {
        p[0] = value & 0xFF;
        p[1] = (value >> 8) & 0xFF;
    }
}

void memory_write32(Memory *mem, uint32_t address, uint32_t value) {
    memory_write16(mem, address, (uint16_t)value);
    memory_write16(mem, address + 2, (uint16_t)(value >> 16));
}

void memory_init(Memory *mem) {
    memset(mem, 0, sizeof(*mem));
}

void memory_set_io(Memory *mem, struct HW *hw) {
    mem->hw = hw;
}

void memory_free(Memory *mem) {
    free(mem->rom);
    mem->rom = NULL;
    mem->rom_size = 0;
}

int memory_load_rom(Memory *mem, const char *path) {
    FILE *f = fopen(path, "rb");
    long len;
    uint8_t *buf;

    if (!f) return 0;

    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > (long)GBA_ROM_SIZE) {
        fclose(f);
        return 0;
    }

    buf = malloc((size_t)len);
    if (!buf) {
        fclose(f);
        return 0;
    }
    if (fread(buf, 1, (size_t)len, f) != (size_t)len) {
        free(buf);
        fclose(f);
        return 0;
    }
    fclose(f);

    if (!memory_load_rom_data(mem, buf, (uint32_t)len)) {
        free(buf);
        return 0;
    }
    return 1;
}

int memory_load_rom_data(Memory *mem, const uint8_t *data, uint32_t size) {
    uint8_t *buf;

    if (!data || size == 0 || size > GBA_ROM_SIZE) {
        return 0;
    }
    buf = malloc(size);
    if (!buf) {
        return 0;
    }
    memcpy(buf, data, size);

    memory_free(mem);
    mem->rom = buf;
    mem->rom_size = size;
    return 1;
}

int memory_load_bios(Memory *mem, const char *path) {
    FILE *f = fopen(path, "rb");
    size_t got;

    if (!f) return 0;

    memset(mem->bios, 0, sizeof(mem->bios));
    got = fread(mem->bios, 1, sizeof(mem->bios), f);
    fclose(f);
    return got == sizeof(mem->bios);
}

int memory_load_save(Memory *mem, const char *path) {
    FILE *f = fopen(path, "rb");
    size_t got;

    memset(mem->sram, 0, sizeof(mem->sram));
    if (!f) return 0;

    got = fread(mem->sram, 1, sizeof(mem->sram), f);
    fclose(f);
    return got > 0;
}

int memory_store_save(const Memory *mem, const char *path) {
    FILE *f = fopen(path, "wb");
    size_t put;

    if (!f) return 0;

    put = fwrite(mem->sram, 1, sizeof(mem->sram), f);
    fclose(f);
    return put == sizeof(mem->sram);
}