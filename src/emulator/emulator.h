#ifndef GBA_EMULATOR_H
#define GBA_EMULATOR_H

#include <stdint.h>

#include "../cpu/cpu.h"
#include "../memory/memory.h"
#include "../ppu/ppu.h"
#include "../hw/hw.h"

/* Cartridge entry point: real GBA hardware boots the BIOS first. Without a
 * BIOS image we start execution directly at the ROM entry vector, which is
 * where the BIOS jump lands anyway, and run the BIOS services in C. */
#define GBA_ROM_ENTRY 0x08000000u

/* The GBA initialises the (SVC mode) stack to the top of internal WRAM. */
#define GBA_STACK_INIT 0x03007F00u

typedef struct Emulator {
    Memory mem;
    CPU    cpu;
    PPU    ppu;
    HW     hw;

    int      has_bios;        /* a real BIOS image is mapped at 0x00000000 */
    uint64_t frames;          /* completed frames */
    uint16_t prev_ppu_irq;     /* last DISPSTAT interrupt conditions */
    uint16_t prev_ppu_period;  /* last DISPSTAT VBlank/HBlank status bits */
    uint32_t cycle_carry;     /* CPU cycles overrun from the previous slice */
} Emulator;

void     emulator_init(Emulator *emu);
void     emulator_free(Emulator *emu);
int      emulator_load_rom(Emulator *emu, const char *path);

/* Map a real BIOS image.  Without one the BIOS services are emulated, which
 * keeps the emulator usable on games whose BIOS calls are unusual. */
int      emulator_load_bios(Emulator *emu, const char *path);

/* Cartridge backup handling; `path` is usually the ROM name with a .sav
 * extension.  A missing save file is fine, it just starts out empty. */
int      emulator_load_save(Emulator *emu, const char *path);
int      emulator_store_save(const Emulator *emu, const char *path);

void     emulator_reset(Emulator *emu);

/* The machine is always alive: a halted CPU is waiting for an interrupt. */
int      emulator_running(const Emulator *emu);

/* Run exactly one frame (228 scanlines x 1232 cycles, 280896 cycles).
 * Renders into emu->ppu.frame. */
void emulator_frame(Emulator *emu);

/* Software interrupt hook, installed into the CPU by emulator_reset(). */
void emulator_swi(void *ctx, uint32_t number);

/* ---- audio ---------------------------------------------------------------- */

/* The mixer runs at a fixed rate; drain interleaved stereo 16 bit frames from
 * the sound engine's ring buffer. Returns how many frames were available. */
#define EMULATOR_AUDIO_RATE 32768u
uint32_t emulator_audio_read(Emulator *emu, int16_t *out, uint32_t frames);

#endif