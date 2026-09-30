#ifndef GBA_HW_H
#define GBA_HW_H

#include <stdint.h>

#include "../cpu/cpu.h"
#include "../memory/memory.h"
#include "../sound/sound.h"

/* Forward declarations. */
typedef struct PPU PPU;

/* Interrupt flags (IF/IE bits). */
#define HW_IRQ_VBLANK  0x0001
#define HW_IRQ_HBLANK  0x0002
#define HW_IRQ_VCOUNT  0x0004
#define HW_IRQ_TIMER0  0x0008
#define HW_IRQ_TIMER1  0x0010
#define HW_IRQ_TIMER2  0x0020
#define HW_IRQ_TIMER3  0x0040
#define HW_IRQ_SERIAL  0x0080
#define HW_IRQ_DMA0    0x0100
#define HW_IRQ_DMA1    0x0200
#define HW_IRQ_DMA2    0x0400
#define HW_IRQ_DMA3    0x0800
#define HW_IRQ_KEYPAD  0x1000
#define HW_IRQ_GAMEPAK 0x2000

/* MMIO offsets that are not part of the contiguous register files. */
#define HW_REG_KEYINPUT  0x130
#define HW_REG_KEYCNT    0x132
#define HW_REG_IE        0x200
#define HW_REG_IF        0x202
#define HW_REG_WAITCNT   0x204
#define HW_REG_IME       0x208

/* Keypad bits (KEYINPUT / KEYCNT). Active low. */
#define HW_KEY_A      0x0001
#define HW_KEY_B      0x0002
#define HW_KEY_SELECT 0x0004
#define HW_KEY_START  0x0008
#define HW_KEY_RIGHT  0x0010
#define HW_KEY_LEFT   0x0020
#define HW_KEY_UP     0x0040
#define HW_KEY_DOWN   0x0080
#define HW_KEY_R      0x0100
#define HW_KEY_L      0x0200

/* DMA start conditions (CNT_H bits 12:13). */
#define HW_DMA_NOW     0
#define HW_DMA_VBLANK  1
#define HW_DMA_HBLANK  2
#define HW_DMA_SPECIAL 3

typedef struct HWTimer {
    uint16_t reload;      /* TMnCNT_L: reload/count value */
    uint16_t cnt;         /* TMnCNT_H */
    uint64_t start_cycle; /* transport cycle count when last enabled */
    uint32_t overflow;    /* number of overflows already signalled */
} HWTimer;

typedef struct DmaChannel {
    uint32_t source;      /* DMAnSAD */
    uint32_t dest;        /* DMAnDAD */
    uint16_t count;       /* DMAnCNT_L */
    uint16_t cnt;         /* DMAnCNT_H */
} DmaChannel;

typedef struct HW {
    Memory *mem;
    CPU    *cpu;
    PPU    *ppu;

    uint64_t cycles;      /* total machine cycles executed */

    /* Interrupt controller */
    uint16_t ie;          /* 0x04000200 */
    uint16_t if_;         /* 0x04000202 */
    uint16_t ime;         /* 0x04000208 */
    uint16_t waitcnt;     /* 0x04000204 */
    uint8_t  haltcnt;     /* 0x04000301, byte write only */

    /* Keypad */
    uint16_t keyinput;    /* 0x04000130, active low */
    uint16_t keycnt;      /* 0x04000132 */

    /* BIOS SWI 04h/05h wait state: the interrupt flags the BIOS call is
     * blocked on. The CPU stays stopped until all of them show up in IF. */
    uint16_t intr_wait_mask;
    int      intr_wait_active;

    /* Shadow copy of the BIOS "IntrWaitFlags" word (0x03007FF8), used by
     * SWI 04h/05h to wait for *new* interrupts. */
    uint16_t bios_irq_flags;

    HWTimer   timer[4];
    DmaChannel dma[4];

    /* Sound: the PSG, the DirectSound FIFOs and the mixer. */
    Sound     sound;
} HW;

void     hw_init(HW *hw, Memory *mem, CPU *cpu, PPU *ppu);
void     hw_reset(HW *hw);

uint32_t hw_io_read(HW *hw, uint32_t address, int width);
void     hw_io_write(HW *hw, uint32_t address, uint32_t value, int width);

/* Advance the hardware by `cycles` machine cycles, updating timers.
 * Returns the interrupt flags newly raised by this tick. */
uint16_t hw_tick(HW *hw, uint32_t cycles);

/* Raise/clear interrupt flags on IF. */
void hw_raise_irq(HW *hw, uint16_t bits);
void hw_clear_irq(HW *hw, uint16_t bits);

/* Fire DMA transfers that are waiting on the given start condition. */
void hw_dma_on_event(HW *hw, int condition);

/* Set the keypad state. `mask` selects which keys to update and `value` holds
 * their new KEYINPUT bits - the register is active low, so a cleared bit
 * means the key is pressed (e.g. hw_set_keys(hw, HW_KEY_A, 0) presses A). */
void hw_set_keys(HW *hw, uint16_t mask, uint16_t value);

/* True if a maskable interrupt is currently pending. */
int hw_irq_pending(const HW *hw);

/* The CPU must be resumed from its wait state when this returns non-zero.
 * `intr_wait` is the BIOS IntrWait mask the CPU is blocked on, if any. */
int hw_should_wake(const HW *hw);

/* Arm a BIOS SWI 04h/05h style wait for the given interrupt flags. */
void hw_intr_wait_begin(HW *hw, uint16_t mask);
void hw_intr_wait_end(HW *hw);

uint16_t hw_timer_read(HW *hw, int index);
void     hw_dma_run(HW *hw, int index);

#endif