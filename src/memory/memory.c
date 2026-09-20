#include "memory.h"

#include <stdio.h>
#include <string.h>

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

uint8_t memory_read8(const Memory *mem, uint32_t address) {
    const uint8_t *p = region_ptr(mem, &address);
    return p ? *p : 0;
}

uint16_t memory_read16(const Memory *mem, uint32_t address) {
    return (uint16_t)memory_read8(mem, address)
        | (uint16_t)((uint16_t)memory_read8(mem, address + 1) << 8);
}

uint32_t memory_read32(const Memory *mem, uint32_t address) {
    return (uint32_t)memory_read16(mem, address)
        | ((uint32_t)memory_read16(mem, address + 2) << 16);
}

void memory_write8(Memory *mem, uint32_t address, uint8_t value) {
    uint8_t *p = (uint8_t *)region_ptr(mem, &address);
    if (p) {
        *p = value;
    }
}

void memory_write16(Memory *mem, uint32_t address, uint16_t value) {
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

    memory_free(mem);
    mem->rom = buf;
    mem->rom_size = (uint32_t)len;
    return 1;
}