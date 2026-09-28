#include "hw.h"

#include "../ppu/ppu.h"

#include <string.h>

static const uint64_t timer_prescaler[4] = { 1, 64, 256, 1024 };

/* ---- timer helpers ------------------------------------------------------ */

static uint64_t timer_ticks(const HW *hw, int i) {
    const HWTimer *t = &hw->timer[i];
    if (!(t->cnt & 0x80)) {
        return 0;
    }
    if (t->cnt & 0x04) {
        /* cascade: count-up timing, clocked by previous timer overflows */
        if (i == 0) return 0;
        const HWTimer *p = &hw->timer[i - 1];
        if (!(p->cnt & 0x80)) return 0;
        return ((hw->cycles - p->start_cycle) / timer_prescaler[p->cnt & 3]) >> 16;
    }
    return (hw->cycles - t->start_cycle) / timer_prescaler[t->cnt & 3];
}

uint16_t hw_timer_read(HW *hw, int index) {
    const HWTimer *t = &hw->timer[index];
    return (uint16_t)(t->reload + (timer_ticks(hw, index) & 0xFFFF));
}

/* ---- DMA helpers -------------------------------------------------------- */

void hw_dma_run(HW *hw, int index) {
    DmaChannel *d = &hw->dma[index];
    uint32_t src, dst, count;
    int width32 = (d->cnt >> 10) & 1;
    int sc = (d->cnt >> 7) & 3;   /* source address control */
    int dc = (d->cnt >> 5) & 3;   /* dest address control */
    uint32_t stride;

    if (!(d->cnt & 0x8000)) return;
    if (sc == 3) return;          /* prohibited */

    /* Only the low 28 bits of a DMA address are used. */
    src = d->source & 0x0FFFFFFFu;
    dst = d->dest & 0x0FFFFFFFu;
    count = d->count;

    if (count == 0) {
        count = width32 ? 0x4000u : 0x10000u;
    } else {
        count += 1; /* CNT_L holds transfer count minus one */
    }

    stride = width32 ? 4 : 2;

    for (uint32_t u = 0; u < count; u++) {
        if (width32) {
            memory_write32(hw->mem, dst, memory_read32(hw->mem, src));
        } else {
            memory_write16(hw->mem, dst, memory_read16(hw->mem, src));
        }
        switch (sc) { case 0: src += stride; break; case 1: src -= stride; break; default: break; }
        switch (dc) { case 0: dst += stride; break; case 1: dst -= stride; break; default: break; }
    }

    if (d->cnt & (1u << 14)) {
        hw_raise_irq(hw, (uint16_t)(HW_IRQ_DMA0 << index));
    }
    if (!(d->cnt & (1u << 9))) { /* repeat */
        d->cnt &= ~0x8000u;
    }
}

void hw_dma_on_event(HW *hw, int condition) {
    for (int i = 0; i < 4; i++) {
        if ((hw->dma[i].cnt & 0x8000) && (((hw->dma[i].cnt >> 12) & 3) == condition)) {
            hw_dma_run(hw, i);
        }
    }
}

/* ---- IRQ ------------------------------------------------------------------ */

void hw_raise_irq(HW *hw, uint16_t bits) {
    hw->if_ |= bits;
}

void hw_clear_irq(HW *hw, uint16_t bits) {
    hw->if_ &= (uint16_t)~bits;
}

int hw_irq_pending(const HW *hw) {
    return (hw->ime && (hw->ie & hw->if_)) ? 1 : 0;
}

/* ---- interrupt waiting / halt wake-ups ---------------------------------- */

/* HALT wakes on any enabled interrupt. STOP is much greedier on real
 * hardware: without a sound engine running it only wakes for a keypad or
 * cartridge interrupt. */
int hw_should_wake(const HW *hw) {
    uint16_t pending = hw->ie & hw->if_;

    if (hw->intr_wait_active) {
        /* Check the requested flags before dispatching the interrupt, so a
         * game that clears IF inside its handler still makes progress. */
        return (hw->if_ & hw->intr_wait_mask) == hw->intr_wait_mask;
    }

    switch (hw->cpu->wait_mode) {
        case CPU_HALT:
            return pending != 0;
        case CPU_STOP:
            return (pending & (HW_IRQ_KEYPAD | HW_IRQ_GAMEPAK)) != 0;
        default:
            return 0;
    }
}

void hw_intr_wait_begin(HW *hw, uint16_t mask) {
    hw->intr_wait_mask = mask;
    hw->intr_wait_active = 1;
    /* The real BIOS records what IntrWait is waiting for at 0x03007FF8, and
     * the interrupt dispatcher later acknowledges exactly those flags. */
    hw->bios_irq_flags |= mask;
}

void hw_intr_wait_end(HW *hw) {
    hw->bios_irq_flags &= (uint16_t)~hw->intr_wait_mask;
    hw->intr_wait_active = 0;
    hw->intr_wait_mask = 0;
}

/* ---- keypad ---------------------------------------------------------------- */

void hw_set_keys(HW *hw, uint16_t mask, uint16_t value) {
    hw->keyinput = (uint16_t)((hw->keyinput & ~mask) | (value & mask));

    if (hw->keycnt & 0x4000) { /* interrupt enable */
        uint16_t sel = hw->keycnt & 0x3FF;
        uint16_t act = (uint16_t)(hw->keyinput & sel);
        int andmode = (hw->keycnt >> 15) & 1;
        int match = andmode ? (act == 0) : (act != sel);
        if (match) {
            hw_raise_irq(hw, HW_IRQ_KEYPAD);
        }
    }
}

/* ---- tick ----------------------------------------------------------------- */

uint16_t hw_tick(HW *hw, uint32_t cycles) {
    uint16_t raised = 0;
    hw->cycles += cycles;

    for (int i = 0; i < 4; i++) {
        HWTimer *t = &hw->timer[i];
        if (!(t->cnt & 0x80)) {
            t->overflow = 0;
            continue;
        }
        uint64_t ticks = timer_ticks(hw, i);
        uint64_t total = ((uint64_t)t->reload + ticks) >> 16;
        if (total > t->overflow) {
            if (t->cnt & 0x40) {
                raised |= (uint16_t)(HW_IRQ_TIMER0 << i);
            }
        }
        t->overflow = (uint32_t)total;
    }

    if (raised) {
        hw->if_ |= raised;
    }
    return raised;
}

/* ---- MMIO ---------------------------------------------------------------- */

uint32_t hw_io_read(HW *hw, uint32_t address, int width) {
    if (width == 8) {
        uint16_t v = (uint16_t)hw_io_read(hw, address & ~1u, 16);
        return (address & 1) ? (v >> 8) : v & 0xFF;
    }
    if (width == 32) {
        return (uint32_t)hw_io_read(hw, address, 16)
             | ((uint32_t)hw_io_read(hw, address + 2, 16) << 16);
    }

    address &= 0x3FF;

    switch (address) {
        case HW_REG_KEYINPUT: return hw->keyinput;
        case HW_REG_KEYCNT:   return hw->keycnt;
        case HW_REG_IE:       return hw->ie;
        case HW_REG_IF:       return hw->if_;
        case HW_REG_WAITCNT:  return hw->waitcnt;
        case HW_REG_IME:      return hw->ime;
        case 0x300:           return hw->haltcnt;
        default: break;
    }

    if (address >= 0x100 && address <= 0x10E) {
        int index = (int)((address - 0x100) >> 2);
        return (address & 2) ? hw->timer[index].cnt : hw_timer_read(hw, index);
    }

    if (address >= 0xB0 && address <= 0xDE) {
        int index = (int)((address - 0xB0) / 12);
        int rem = (int)((address - 0xB0) % 12);
        const DmaChannel *d = &hw->dma[index];
        switch (rem) {
            case 0:  return (uint16_t)d->source;
            case 2:  return (uint16_t)(d->source >> 16);
            case 4:  return (uint16_t)d->dest;
            case 6:  return (uint16_t)(d->dest >> 16);
            case 8:  return d->count;
            case 10: return d->cnt;
            default: return 0;
        }
    }

    if (address < 0x60) {
        return ppu_io_read16(hw->ppu, address);
    }
    return 0;
}

void hw_io_write(HW *hw, uint32_t address, uint32_t value, int width) {
    if (width == 8) {
        uint16_t v = (uint16_t)hw_io_read(hw, address & ~1u, 16);
        if (address & 1) {
            v = (uint16_t)((v & 0x00FF) | ((uint16_t)(value & 0xFF) << 8));
        } else {
            v = (uint16_t)((v & 0xFF00) | (value & 0xFF));
        }
        hw_io_write(hw, address & ~1u, v, 16);
        return;
    }
    if (width == 32) {
        hw_io_write(hw, address, value & 0xFFFF, 16);
        hw_io_write(hw, address + 2, value >> 16, 16);
        return;
    }

    address &= 0x3FF;

    switch (address) {
        case HW_REG_KEYCNT:
            hw->keycnt = (uint16_t)value;
            break;
        case HW_REG_IE:
            hw->ie = (uint16_t)value;
            break;
        case HW_REG_IF:
            hw_clear_irq(hw, (uint16_t)value); /* write-1-to-clear */
            break;
        case HW_REG_WAITCNT:
            hw->waitcnt = (uint16_t)value;
            break;
        case HW_REG_IME:
            hw->ime = (uint16_t)(value & 1);
            break;
        case 0x300:
            /* HALTCNT (byte write only) */
            hw->haltcnt = (uint8_t)value;
            cpu_set_wait(hw->cpu, ((value & 3) == 0)   ? CPU_RUN :
                                    ((value & 3) == 1) ? CPU_HALT : CPU_STOP);
            break;
        default:
            break;
    }

    if (address >= 0x100 && address <= 0x10E) {
        int index = (int)((address - 0x100) >> 2);
        HWTimer *t = &hw->timer[index];
        if (!(address & 2)) {
            t->reload = (uint16_t)value;
            if (t->cnt & 0x80) { /* running: reload the counter */
                t->start_cycle = hw->cycles;
                t->overflow = 0;
            }
        } else {
            t->cnt = (uint16_t)(value & 0x00C7);
            if (t->cnt & 0x80) {
                t->start_cycle = hw->cycles;
                t->overflow = 0;
            }
        }
        return;
    }

    if (address >= 0xB0 && address <= 0xDE) {
        int index = (int)((address - 0xB0) / 12);
        int rem = (int)((address - 0xB0) % 12);
        DmaChannel *d = &hw->dma[index];
        switch (rem) {
            case 0:  d->source = (uint32_t)((d->source & 0xFFFF0000u) | (uint16_t)value); break;
            case 2:  d->source = (d->source & 0x0000FFFFu) | ((uint32_t)value << 16); break;
            case 4:  d->dest = (uint32_t)((d->dest & 0xFFFF0000u) | (uint16_t)value); break;
            case 6:  d->dest = (d->dest & 0x0000FFFFu) | ((uint32_t)value << 16); break;
            case 8:  d->count = (uint16_t)value; break;
            case 10:
                d->cnt = (uint16_t)value;
                if (value & 0x8000) {
                    if (((value >> 12) & 3) == HW_DMA_NOW) {
                        hw_dma_run(hw, index);
                    }
                }
                break;
            default: break;
        }
        return;
    }

    if (address < 0x60) {
        ppu_io_write16(hw->ppu, address, (uint16_t)value);
    }
}

/* ---- lifecycle ------------------------------------------------------------ */

void hw_init(HW *hw, Memory *mem, CPU *cpu, PPU *ppu) {
    memset(hw, 0, sizeof(*hw));
    hw->mem = mem;
    hw->cpu = cpu;
    hw->ppu = ppu;
    hw_reset(hw);
}

void hw_reset(HW *hw) {
    hw->cycles = 0;
    hw->ie = 0;
    hw->if_ = 0;
    hw->ime = 0;
    hw->waitcnt = 0;
    hw->haltcnt = 0;
    hw->keycnt = 0;
    hw->keyinput = 0xFFFF; /* all keys released, active low */
    hw->intr_wait_mask = 0;
    hw->intr_wait_active = 0;
    hw->bios_irq_flags = 0;
    memset(hw->timer, 0, sizeof(hw->timer));
    memset(hw->dma, 0, sizeof(hw->dma));
}