#include "memory.h"

#include <stdio.h>
#include <string.h>

#include "../hw/hw.h"

/* Everything in the 0x04000000 page belongs to the I/O controller, which
 * owns the decoding; nothing may fall through to a backing array. */
static int io_region(uint32_t address) {
    return (address & 0xFF000000u) == 0x04000000u;
}

/* The register window itself is only 1 KB.  The rest of the page is open
 * bus, and letting it wrap would let a DMA with an incrementing destination
 * scribble over unrelated registers. */
static int io_window(uint32_t address) {
    return (address & 0xFFFFFC00u) == 0x04000000u;
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
    /* The cartridge backup window (0x0A/0x0E/0x10, plus 0x0D for EEPROM) is
     * not plain memory: SRAM, Flash and EEPROM all answer differently, so
     * those addresses are serviced by save_read8/save_write8 instead. */
    else {
        return NULL; /* unmapped */
    }

    *address = index;
    return base + index;
}

/* ---- cartridge backup ---------------------------------------------------- */

/* Flash command state, in the order a game walks through it.  The three
 * step unlock sequence reuses 0x5555 as its first and last address, so the
 * states have to be distinguishable to tell the two apart. */
enum {
    FLASH_IDLE = 0,
    FLASH_UNLOCK_A,   /* 0xAA written to 0x5555 */
    FLASH_UNLOCK_B,   /* 0x55 written to 0x2AAA */
    FLASH_UNLOCK_C,   /* 0xAA written to 0x5555 again: the chip is unlocked */
    FLASH_ERASE_A,    /* 0x80 written, awaiting the type byte */
    FLASH_ERASE,      /* awaiting the 0x10 or 0x30 to confirm */
    FLASH_PROGRAM,    /* 0xA0 written, the data byte follows */
    FLASH_ID,         /* 0x90: identification mode */
    FLASH_BANK        /* 0xB0: bank select on 128 KB chips */
};

/* Which chip answers at this address. */
static int save_region(uint32_t address) {
    return ((address & 0xFF000000) == 0x0D000000u) ||
           ((address & 0xFE000000) == 0x0A000000u) ||
           ((address & 0xFE000000) == 0x0E000000u) ||
           ((address & 0xF0000000) == 0x10000000u);
}

/* The chip is always addressable over the full array; a smaller header size
 * just masks the addresses so the save file stays the right length. */
static uint32_t save_mask(const CartSave *s) {
    uint32_t n = s->size ? s->size : GBA_SAVE_SIZE;
    return n - 1;
}

/* ---- EEPROM -------------------------------------------------------------- */

/* EEPROM is bit-serial: the game shifts a fixed width address in, then the
 * payload, one bit at a time. The width comes from the cartridge header
 * because the chip is a fixed part -- it is not something the game can vary,
 * and it is not something that can be guessed from the bits, since any
 * address containing a 1 would look like an early terminator.
 *
 * Bits arrive a whole byte at a time, most significant bit first, so an access
 * is padded out to a byte boundary. Those padding bits belong to nothing, so
 * they are counted off rather than fed into the next address. */


/* The chip reads the stream in fixed size steps: `addr_bits` of address, one
 * terminator bit, then the payload. Running a counter across all three keeps
 * them from bleeding into each other, which is what a pair of independent
 * flags got wrong at widths that are not a whole number of bytes. */
enum { EEP_STEP_ADDR = 0, EEP_STEP_TERM, EEP_STEP_DATA };

static void eeprom_reset(CartSave *s) {
    s->step       = EEP_STEP_ADDR;
    s->addr       = 0;
    s->steps_left = s->addr_bits;
    s->bit_index  = 0;
}

/* Called once a whole byte has been handed over, padding aside. */
static void eeprom_push_bit(CartSave *s, uint8_t bit, int left_in_byte) {
    switch (s->step) {
        case EEP_STEP_ADDR:
            s->addr = (s->addr << 1) | bit;
            if (--s->steps_left == 0) {
                s->steps_left = 1;
                s->step       = EEP_STEP_TERM;
            }
            break;

        case EEP_STEP_TERM:
            /* The terminator carries no data; it only marks the end of the
             * address, so it is consumed and dropped. Whatever is left in
             * this byte is padding, and the payload starts in the next one. */
            s->steps_left = s->data_bits;
            s->bit_index  = 0;
            s->step       = EEP_STEP_DATA;
            s->pad        = (uint32_t)left_in_byte;
            if (s->steps_left == 0) {
                eeprom_reset(s);
            }
            break;

        default:
            /* A cell erases to all ones and programming can only ever drive
             * bits towards zero, so the payload is ANDed in. Writing can
             * never put a bit back, which is what tells this apart from a
             * plain memory write. */
            if (!bit) {
                s->data[s->addr & save_mask(s)] &=
                    (uint8_t)~(uint8_t)(0x80u >> s->bit_index);
            }
            if (++s->bit_index >= s->data_bits) {
                eeprom_reset(s);
                s->pad = (uint32_t)left_in_byte;
            }
            break;
    }
}

static void eeprom_push_byte(CartSave *s, uint8_t value) {
    for (int i = 7; i >= 0; i--) {
        if (s->pad) {
            s->pad--;
            continue;
        }
        eeprom_push_bit(s, (uint8_t)((value >> i) & 1u), i);
    }
}

/* A read is the same stream as a write, but the payload comes back out of the
 * chip on the DO line. The game clocks one bit out per read, so the line
 * floats high once the payload is exhausted. */
static uint8_t eeprom_pull(CartSave *s) {
    uint8_t bit;

    if (s->pad) {
        s->pad--;
        return 1;
    }
    if (s->step != EEP_STEP_DATA || s->steps_left == 0) {
        return 1; /* no payload pending: the line floats high */
    }
    bit   = (uint8_t)((s->data[s->addr & save_mask(s)] >> (7u - s->bit_index)) & 1u);
    s->bit_index++;
    if (--s->steps_left == 0) {
        eeprom_reset(s);
    }
    return bit;
}

/* ---- Flash --------------------------------------------------------------- */

#define FLASH_BANK_SIZE  0x2000
#define FLASH_ID_VENDOR  0x32
#define FLASH_ID_DEVICE  0x62
#define FLASH_ID_SIZE    0x13

static uint32_t flash_offset(const CartSave *s, uint32_t address) {
    return FLASH_BANK_SIZE * s->bank + (address & (FLASH_BANK_SIZE - 1));
}

static uint8_t flash_read8(const CartSave *s, uint32_t address) {
    switch (s->flash_cmd) {
        case FLASH_ID:
            switch (address & 0xFFF) {
                case 0x000: return FLASH_ID_VENDOR;
                case 0x001: return FLASH_ID_DEVICE;
                default:     return FLASH_ID_SIZE;
            }
        case FLASH_BANK:
            return (uint8_t)s->bank;
        default:
            return s->data[flash_offset(s, address) & save_mask(s)];
    }
}

/* A command is three bytes long: 0xAA to 0x5555, 0x55 to 0x2AAA, then 0xAA
 * to 0x5555 again. The chip is then unlocked and the next byte written is the
 * command itself. */
static void flash_command(CartSave *s, uint32_t address, uint8_t value) {
    uint32_t a = address & 0x7FFF;

    /* The third step repeats the first, so it has to be recognised before the
     * first step swallows it and the sequence never completes. */
    if (a == 0x5555 && value == 0xAA) {
        s->flash_cmd = (s->flash_cmd == FLASH_UNLOCK_B) ? FLASH_UNLOCK_C
                                                        : FLASH_UNLOCK_A;
        return;
    }
    if (a == 0x2AAA && value == 0x55 && s->flash_cmd == FLASH_UNLOCK_A) {
        s->flash_cmd = FLASH_UNLOCK_B;
        return;
    }
    /* Identification, bank select, erase and reset are all single bytes to
     * 0x5555 and work on a chip that was not armed first, so they are honoured
     * either way. Only program and erase, which destroy data, want the full
     * three byte unlock. */
    if (s->flash_cmd != FLASH_UNLOCK_C && value != 0xA0 && value != 0x80) {
        switch (value) {
            case 0x90: s->flash_cmd = FLASH_ID; return;
            case 0xB0: s->flash_cmd = FLASH_BANK; return;
            case 0xF0:
            case 0xFF: s->flash_cmd = FLASH_IDLE; return;
            default: s->flash_cmd = FLASH_IDLE; return;
        }
    }
    switch (value) {
        case 0xA0: /* program: the data byte follows at this address */
            s->flash_cmd = FLASH_PROGRAM;
            s->flash_cmd_addr = address;
            break;
        case 0x80: /* erase setup, awaiting the type and the confirmation */
            s->flash_cmd = FLASH_ERASE_A;
            s->flash_cmd_addr = address;
            break;
        case 0x90: s->flash_cmd = FLASH_ID;   break;
        case 0xB0: s->flash_cmd = FLASH_BANK; break;
        case 0xF0: /* reset: back to reading the array */
        case 0xFF: s->flash_cmd = FLASH_IDLE; break;
        default:   s->flash_cmd = FLASH_IDLE; break;
    }
}

static void flash_write8(CartSave *s, uint32_t address, uint8_t value) {
    uint32_t a = address & 0x7FFF;

    /* The erase type and its confirmation are only meaningful inside the
     * sequence that armed them, and both land on 0x5555 or 0x2AAA. */
    if (s->flash_cmd == FLASH_ERASE_A) {
        if (a == 0x2AAA && value == 0xAA) {
            s->flash_cmd = FLASH_ERASE;
        } else {
            s->flash_cmd = FLASH_IDLE;
        }
        return;
    }
    if (s->flash_cmd == FLASH_ERASE) {
        if (a == 0x5555 && value == 0x10) {
            memset(s->data, 0xFF, s->size ? s->size : GBA_SAVE_SIZE);
            s->flash_cmd = FLASH_IDLE;
        } else if (a == 0x5555 && value == 0x30) {
            /* A 4 KB block erase works in 8 KB blocks, of which only the first
             * half is in the window. */
            uint32_t block = (s->flash_cmd_addr & 0xE000u) >> 1;
            memset(s->data + block, 0xFF, FLASH_BANK_SIZE);
            s->flash_cmd = FLASH_IDLE;
        } else {
            s->flash_cmd = FLASH_IDLE;
        }
        return;
    }
    if (s->flash_cmd == FLASH_PROGRAM) {
        /* The data byte goes to the address the 0xA0 was written to, which is
         * not necessarily where the byte itself lands. */
        s->data[flash_offset(s, s->flash_cmd_addr) & save_mask(s)] &= value;
        s->flash_cmd = FLASH_IDLE;
        return;
    }
    if (s->flash_cmd == FLASH_BANK) {
        s->bank = value & 1u;
        s->flash_cmd = FLASH_IDLE;
        return;
    }
    flash_command(s, address, value);
}

/* ---- dispatch ------------------------------------------------------------ */

static uint8_t save_read8(CartSave *s, uint32_t address) {
    switch (s->kind) {
        case SAVE_EEPROM: return eeprom_pull(s);
        case SAVE_FLASH:  return flash_read8(s, address);
        default:          return s->data[address & save_mask(s)];
    }
}

static void save_write8(CartSave *s, uint32_t address, uint8_t value) {
    switch (s->kind) {
        case SAVE_EEPROM:
            /* Only the bit stream matters here; the address the game uses to
             * pick which of the command bytes it is writing is not a
             * register we need to model. */
            eeprom_push_byte(s, value);
            break;
        case SAVE_FLASH:
            flash_write8(s, address, value);
            break;
        default:
            s->data[address & save_mask(s)] = value;
            break;
    }
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
        return io_window(address) ? (uint8_t)hw_io_read(mem->hw, address & 0x3FF, 8) : 0;
    }
    if (mem->hw && bios_irq_flags_at(address)) {
        return (uint8_t)(mem->hw->bios_irq_flags >> ((address & 1) * 8));
    }
    if (save_region(address)) {
        return save_read8((CartSave *)&mem->save, address);
    }
    const uint8_t *p = region_ptr(mem, &address);
    return p ? *p : 0;
}

uint16_t memory_read16(const Memory *mem, uint32_t address) {
    if (io_region(address) && mem->hw) {
        return io_window(address) ? (uint16_t)hw_io_read(mem->hw, address & 0x3FF, 16) : 0;
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
        if (io_window(address)) {
            hw_io_write(mem->hw, address & 0x3FF, value, 8);
        }
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
    if (save_region(address)) {
        save_write8(&mem->save, address, value);
        return;
    }
    uint8_t *p = (uint8_t *)region_ptr(mem, &address);
    if (p) {
        *p = value;
    }
}

void memory_write16(Memory *mem, uint32_t address, uint16_t value) {
    if (io_region(address) && mem->hw) {
        if (io_window(address)) {
            hw_io_write(mem->hw, address & 0x3FF, value, 16);
        }
        return;
    }
    if (save_region(address) && (address & 1) == 0) {
        /* The backup chips take their bit stream a byte at a time, so a wide
         * store to the window has to be split up by hand. */
        memory_write8(mem, address, (uint8_t)(value & 0xFF));
        memory_write8(mem, address + 1, (uint8_t)(value >> 8));
        return;
    }
    uint8_t *p = (uint8_t *)region_ptr(mem, &address);
    if (p && (address & 1) == 0) {
        p[0] = value & 0xFF;
        p[1] = (uint8_t)(value >> 8);
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

/* The backup chip is named by a single byte in the cartridge header, but the
 * codes in circulation disagree about which size each one means, and plenty
 * of dumps carry the wrong value.  So the byte only picks the kind and a
 * generous size: a chip smaller than the array is masked down on access, and
 * a wrong size costs a few wasted kilobytes in the save file rather than a
 * corrupt save. */
static void save_detect(CartSave *s, const uint8_t *rom, uint32_t size) {
    /* Address width and payload width per EEPROM size, in header order. */
    static const struct { uint32_t size, addr_bits, data_bits; } eeprom[7] = {
        { 0,      0, 8 },  /* 00: unused */
        { 0x0200, 6, 2 },  /* 01: 512 B  */
        { 0x0400, 7, 2 },  /* 02: 2 KB   */
        { 0x0800, 8, 2 },  /* 03: 4 KB   */
        { 0x2000, 8, 8 },  /* 04: 8 KB   */
        { 0x4000, 14, 8 }, /* 05: 16 KB  */
        { 0x20000, 16, 8 } /* 06: 64 KB  */
    };
    static const uint32_t sram_size[12] = {
        0, 0, 0, 0, 0, 0, 0, 0, 0x8000, 0x10000, 0x10000, 0x20000
    };
    uint8_t type = (size > 0xB3) ? rom[0xB2] : 0x00;

    memset(s, 0, sizeof(*s));
    if (type >= 0x01 && type <= 0x06) {
        s->kind      = SAVE_EEPROM;
        s->size      = eeprom[type].size;
        s->addr_bits = eeprom[type].addr_bits;
        s->data_bits = eeprom[type].data_bits;
    } else if (type >= 0x08 && type <= 0x0B) {
        s->kind = SAVE_SRAM;
        s->size = sram_size[type];
    } else if (type == 0x0C || type == 0x0D || type == 0x14 || type == 0x15) {
        s->kind = SAVE_FLASH;
        s->size = (type & 1) ? 0x20000 : 0x10000;
    } else {
        /* Unknown header. Assume the common 8 KB EEPROM so a wrong backup
         * byte still gives a working save, and the window stays mapped. */
        s->kind      = SAVE_EEPROM;
        s->size      = 0x2000;
        s->addr_bits = 8;
        s->data_bits = 8;
    }
    if (s->size > GBA_SAVE_SIZE) {
        s->size = GBA_SAVE_SIZE;
    }
    /* A freshly inserted cartridge has an erased chip. SRAM is the odd one
     * out and powers up zeroed; every other kind erases to all ones. */
    memset(s->data, (s->kind == SAVE_SRAM) ? 0x00 : 0xFF, GBA_SAVE_SIZE);
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
    save_detect(&mem->save, mem->rom, size);
    eeprom_reset(&mem->save);
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
    FILE *f;
    size_t got;
    size_t size = mem->save.size ? mem->save.size : GBA_SAVE_SIZE;

    /* A fresh chip is erased, which for every kind means all ones.  SRAM
     * ships zeroed instead, so the kind decides. */
    if (mem->save.kind == SAVE_SRAM) {
        memset(mem->save.data, 0x00, size);
    } else {
        memset(mem->save.data, 0xFF, size);
    }
    eeprom_reset(&mem->save);
    mem->save.bank      = 0;
    mem->save.flash_cmd = 0;
    mem->save.pad       = 0;

    f = fopen(path, "rb");
    if (!f) return 0;

    got = fread(mem->save.data, 1, size, f);
    fclose(f);
    return got > 0;
}

int memory_store_save(const Memory *mem, const char *path) {
    FILE *f = fopen(path, "wb");
    size_t put;
    size_t size = mem->save.size ? mem->save.size : GBA_SAVE_SIZE;

    if (!f) return 0;
    if (mem->save.kind == SAVE_NONE) {
        fclose(f);
        return 0; /* nothing worth writing */
    }

    put = fwrite(mem->save.data, 1, size, f);
    fclose(f);
    return put == size;
}
