#include "emulator.h"
#include "../cpu/bios.h"

#include <string.h>

/* One GBA line is 1232 cycles: 960 'drawing' cycles followed by a 272 cycle
 * HBlank window (approximation of the real ~1006 cycle mark). */
#define DRAW_CYCLES   960u
#define HBLANK_CYCLES 272u

/* How many cycles a stopped CPU burns before we look for a wake-up. Keeping
 * this small makes timer driven wake-ups reasonably precise. */
#define IDLE_CHUNK 16u

/* An interrupt is only taken when interrupts are enabled and the CPU is not
 * running in user mode. */
static int irq_takeable(const Emulator *emu) {
    uint32_t mode = emu->cpu.cpsr & 0x1F;
    return !(emu->cpu.cpsr & FLAG_I) && mode != MODE_USR;
}

/* Without a BIOS image nothing is mapped at 0x00000018, so the BIOS interrupt
 * dispatcher is emulated: the caller-saved registers are saved, the handler the
 * game chained in at 0x03007FFC is called, the flags IntrWait was waiting on
 * are acknowledged, and the interrupt is returned from. A handler that ends in
 * `bx lr` comes back to HLE_IRQ_RETURN, which the step loop watches for. */
#define HLE_IRQ_HANDLER 0x03007FFCu
#define HLE_IRQ_RETURN  0x0000010Cu

static const int HLE_IRQ_SAVED[] = {0, 1, 2, 3, 12, 14};
#define HLE_IRQ_SAVED_N (sizeof(HLE_IRQ_SAVED) / sizeof(HLE_IRQ_SAVED[0]))

/* The BIOS acknowledges exactly the flags recorded at 0x03007FF8. */
static void hle_irq_ack(Emulator *emu) {
    uint16_t ack = (uint16_t)(emu->hw.bios_irq_flags & emu->hw.ie);
    if (ack) {
        hw_clear_irq(&emu->hw, ack);
    }
}

static void hle_irq_return(Emulator *emu) {
    CPU *cpu = &emu->cpu;
    uint32_t sp = cpu_read_reg(cpu, 13);
    uint32_t regs[HLE_IRQ_SAVED_N];

    for (unsigned i = 0; i < HLE_IRQ_SAVED_N; i++) {
        regs[i] = memory_read32(&emu->mem, sp + 4 * i);
    }
    cpu_write_reg(cpu, 13, sp + 4 * HLE_IRQ_SAVED_N);

    hle_irq_ack(emu);

    for (unsigned i = 0; i < HLE_IRQ_SAVED_N; i++) {
        cpu_write_reg(cpu, HLE_IRQ_SAVED[i], regs[i]);
    }
    cpu->cpsr = cpu->spsr[BANK_IRQ];
    cpu->reg[15] = cpu->r14[BANK_IRQ] - 4;
}

static void hle_irq_dispatch(Emulator *emu) {
    CPU *cpu = &emu->cpu;
    uint32_t handler = memory_read32(&emu->mem, HLE_IRQ_HANDLER);
    uint32_t sp = cpu_read_reg(cpu, 13);

    if (!handler) {
        /* Nothing chained in: acknowledge and return immediately. */
        hle_irq_ack(emu);
        cpu->cpsr = cpu->spsr[BANK_IRQ];
        cpu->reg[15] = cpu->r14[BANK_IRQ] - 4;
        return;
    }

    cpu_write_reg(cpu, 13, sp - 4 * HLE_IRQ_SAVED_N);
    for (unsigned i = 0; i < HLE_IRQ_SAVED_N; i++) {
        memory_write32(&emu->mem, sp - 4 * HLE_IRQ_SAVED_N + 4 * i,
                       cpu_read_reg(cpu, HLE_IRQ_SAVED[i]));
    }

    /* The BIOS loads the handler out of [0x03FFFFFC], with r0 pointing at the
     * IO block on the way in. */
    cpu->reg[0] = 0x04000000u;
    cpu_write_reg(cpu, 14, HLE_IRQ_RETURN);
    cpu->reg[15] = handler & ~1u;
    if (handler & 1u) {
        cpu->cpsr |= FLAG_T;
    } else {
        cpu->cpsr &= ~FLAG_T;
    }
}

/* Run the CPU for approximately `cycles` machine cycles, then advance the
 * timers and service interrupts. Steps past the target, never under-runs. */
static void run_cpu(Emulator *emu, uint32_t cycles) {
    uint32_t acc = 0;

    while (acc < cycles) {
        if (emu->cpu.halted) {
            /* A stopped CPU still consumes cycles, and wakes up when the
             * condition it is waiting for becomes true. */
            uint32_t chunk = cycles - acc;
            if (chunk > IDLE_CHUNK) {
                chunk = IDLE_CHUNK;
            }
            if (hw_should_wake(&emu->hw)) {
                hw_intr_wait_end(&emu->hw);
                cpu_set_wait(&emu->cpu, CPU_RUN);
            }
            acc += chunk;
            hw_tick(&emu->hw, chunk);
            continue;
        }

        acc += cpu_step(&emu->cpu, &emu->mem);

        if (!emu->has_bios && emu->cpu.reg[15] == HLE_IRQ_RETURN) {
            hle_irq_return(emu);
        }

        if (hw_irq_pending(&emu->hw) && irq_takeable(emu)) {
            cpu_irq(&emu->cpu);
            if (!emu->has_bios) {
                hle_irq_dispatch(emu);
            }
        }
    }

    if (acc > cycles) {
        hw_tick(&emu->hw, acc - cycles);
    }

    if (hw_irq_pending(&emu->hw) && irq_takeable(emu)) {
        cpu_irq(&emu->cpu);
        if (!emu->has_bios) {
            hle_irq_dispatch(emu);
        }
    }
}

/* Detect rising edges of the enabled DISPSTAT interrupt conditions and map
 * them onto IF bits plus the corresponding DMA start conditions. */
static void dispstat_sync(Emulator *emu) {
    uint16_t now = ppu_irq_flags(&emu->ppu);
    uint16_t rise = now & (uint16_t)~emu->prev_ppu_irq;
    emu->prev_ppu_irq = now;

    if (rise & 1u) {
        hw_raise_irq(&emu->hw, HW_IRQ_VBLANK);
        hw_dma_on_event(&emu->hw, HW_DMA_VBLANK);
    }
    if (rise & 2u) {
        hw_raise_irq(&emu->hw, HW_IRQ_HBLANK);
        hw_dma_on_event(&emu->hw, HW_DMA_HBLANK);
    }
    if (rise & 4u) {
        hw_raise_irq(&emu->hw, HW_IRQ_VCOUNT);
    }
}

void emulator_frame(Emulator *emu) {
    for (uint16_t y = 0; y < 228; y++) {
        ppu_set_vcount(&emu->ppu, y);

        if (y == 0) {
            ppu_set_vblank(&emu->ppu, 0);
        } else if (y == 160) {
            ppu_set_vblank(&emu->ppu, 1);
        } else if (y == 227) {
            ppu_set_vblank(&emu->ppu, 0);
        }

        dispstat_sync(emu);

        if (y < 160) {
            ppu_render_scanline(&emu->ppu, y);
        }

        run_cpu(emu, DRAW_CYCLES);

        ppu_set_hblank(&emu->ppu, 1);
        dispstat_sync(emu);

        run_cpu(emu, HBLANK_CYCLES);

        ppu_set_hblank(&emu->ppu, 0);
        dispstat_sync(emu);
    }

    emu->frames++;
}

/* ---- software interrupts -------------------------------------------------- */

/* Called by the CPU core for every SWI, with reg[15] already pointing at the
 * instruction after the SWI. With a BIOS image mapped the real BIOS runs,
 * otherwise the services are emulated in C (see bios.c). */
void emulator_swi(void *ctx, uint32_t number) {
    Emulator *emu = ctx;
    CPU *cpu = &emu->cpu;
    int thumb = (cpu->cpsr & FLAG_T) != 0;

    if (emu->has_bios) {
        cpu->r14[BANK_SVC] = cpu->reg[15] - (thumb ? 2u : 4u);
        cpu->spsr[BANK_SVC] = cpu->cpsr;
        cpu->cpsr = (cpu->cpsr & ~(0x1Fu | FLAG_T)) | MODE_SVC | FLAG_I;
        cpu->reg[15] = 0x00000008;
        return;
    }

    bios_swi(&emu->mem, cpu, &emu->hw, number);
}

/* ---- lifecycle ------------------------------------------------------------ */

void emulator_init(Emulator *emu) {
    memory_init(&emu->mem);
    cpu_init(&emu->cpu);
    ppu_init(&emu->ppu, &emu->mem);
    hw_init(&emu->hw, &emu->mem, &emu->cpu, &emu->ppu);
    memory_set_io(&emu->mem, &emu->hw);
    emu->has_bios = 0;
    emulator_reset(emu);
}

void emulator_free(Emulator *emu) {
    memory_free(&emu->mem);
}

int emulator_load_rom(Emulator *emu, const char *path) {
    if (!memory_load_rom(&emu->mem, path)) {
        return 0;
    }
    emulator_reset(emu);
    return 1;
}

int emulator_load_bios(Emulator *emu, const char *path) {
    if (!memory_load_bios(&emu->mem, path)) {
        emu->has_bios = 0;
        return 0;
    }
    emu->has_bios = 1;
    emulator_reset(emu);
    return 1;
}

int emulator_load_save(Emulator *emu, const char *path) {
    return memory_load_save(&emu->mem, path);
}

int emulator_store_save(const Emulator *emu, const char *path) {
    return memory_store_save(&emu->mem, path);
}

void emulator_reset(Emulator *emu) {
    cpu_reset(&emu->cpu);
    ppu_reset(&emu->ppu);
    hw_reset(&emu->hw);
    emu->frames = 0;
    emu->prev_ppu_irq = 0;

    /* CPU is in SVC mode after reset: write the banked stack pointer. */
    cpu_write_reg(&emu->cpu, 13, GBA_STACK_INIT);
    /* With a BIOS mapped, start at the BIOS reset vector so it can do its own
     * hardware init; otherwise jump straight to the cartridge entry, which is
     * where the BIOS jump lands anyway. */
    emu->cpu.reg[15] = emu->has_bios ? 0x00000000u : GBA_ROM_ENTRY;
    emu->cpu.swi_hook = emulator_swi;
    emu->cpu.swi_ctx = emu;
}

/* A halted CPU is waiting for an interrupt, which is a normal state, so the
 * machine is only "not running" while it is being set up or torn down. */
int emulator_running(const Emulator *emu) {
    return emu != NULL;
}