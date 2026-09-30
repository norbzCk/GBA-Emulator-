#include <stdio.h>
#include <string.h>

#include "cpu/cpu.h"
#include "cpu/bios.h"
#include "emulator/emulator.h"
#include "memory/memory.h"
#include "hw/hw.h"
#include "ppu/ppu.h"
#include "testrom.h"

static int failures    = 0;
static int total_tests = 0;

static void t_check(const char *what, int ok) {
    total_tests++;
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}

/* Expand a BGR555 colour the way the PPU writes it into the framebuffer. */
static uint32_t rgb5(unsigned v) {
    uint8_t r = (uint8_t)(((v & 0x1F) * 255) / 31);
    uint8_t g = (uint8_t)((((v >> 5) & 0x1F) * 255) / 31);
    uint8_t b = (uint8_t)((((v >> 10) & 0x1F) * 255) / 31);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

/* Colour of an object pixel holding palette index `v`. The object palette is
 * filled with v * 0x21, so the framebuffer says exactly what index a pixel
 * sampled. */
static uint32_t objpx(int v) {
    return rgb5(((unsigned)v * 0x21) & 0x7FFF);
}

/* Palette index the object tile data below stores for tile `t` of `count`,
 * row `r`, column `c`. Values stay unique per (tile, row) so the framebuffer
 * reveals which tile a pixel came from, and the tile row within it. */
static int objidx(int t, int r, int c, int by_col) {
    if (by_col) return 1 + c;
    return t < 31 ? 1 + t * 8 + r : 200 + (t - 31) * 8 + r;
}

/* Attribute 0 with the 8bpp colour mode set, the shape square, and the Y
 * coordinate 8, which the PPU reads as screen Y 0. */
#define OBJ8 0x2008

/* Fill `count` 8x8 8bpp object tiles with the indices objidx() returns. */
static void obj_fill(Memory *mem, int count, int by_col) {
    for (int t = 0; t < count; t++) {
        for (int r = 0; r < 8; r++) {
            for (int c = 0; c < 8; c++) {
                memory_write8(mem, 0x06010000 + 64 * t + r * 8 + c,
                              (uint8_t)objidx(t, r, c, by_col));
            }
        }
    }
}

/* Write one instruction at `addr`, set PC there, execute a single step. */
static void run_one(CPU *cpu, Memory *mem, uint32_t addr, uint32_t insn) {
    memory_write32(mem, addr, insn);
    cpu->reg[15] = addr;
    cpu_step(cpu, mem);
}

/* Same as run_one but for a 16-bit Thumb instruction. */
static void run_thumb(CPU *cpu, Memory *mem, uint32_t addr, uint16_t insn) {
    memory_write16(mem, addr, insn);
    cpu->cpsr |= FLAG_T;
    cpu->reg[15] = addr;
    cpu_step(cpu, mem);
}

/* ---- BIOS services ------------------------------------------------------- */

/* Stands in for emulator_swi: with no BIOS image mapped an SWI is serviced
 * in C. Reaching that handler only needs the memory and the hardware, both
 * of which the hardware block already holds on to. */
static void test_swi_hook(void *ctx, uint32_t number) {
    HW *hw = ctx;
    bios_swi(hw->mem, hw->cpu, hw, number);
}

static void test_bios_services(void) {
    Memory memory;
    CPU    cpu;
    PPU    ppu;
    HW     hw;

    memory_init(&memory);
    cpu_init(&cpu);
    ppu_init(&ppu, &memory);
    hw_init(&hw, &memory, &cpu, &ppu);
    memory_set_io(&memory, &hw);
    cpu.reg[15] = 0x08000040u; /* return address, checked below */

    printf("=== BIOS (HLE) tests ===\n");

    /* --- Div: r0 = quotient, r1 = remainder, r3 = |quotient| (r2 is
     *     left alone, the GBATEK example is -1234/10 -> -123, -4, 123) --- */
    cpu.reg[0] = 6;
    cpu.reg[1] = 7;
    bios_swi(&memory, &cpu, &hw, 0x06);
    t_check("Div 6/7", cpu.reg[0] == 0 && cpu.reg[1] == 6
                        && (int32_t)cpu.reg[3] == 0);

    cpu.reg[0] = (uint32_t)-1234;
    cpu.reg[1] = 10;
    bios_swi(&memory, &cpu, &hw, 0x06);
    t_check("Div -1234/10 truncates toward zero",
            (int32_t)cpu.reg[0] == -123 && (int32_t)cpu.reg[1] == -4
                                       && (int32_t)cpu.reg[3] == 123);

    cpu.reg[0] = 1000;
    bios_swi(&memory, &cpu, &hw, 0x08);
    t_check("Sqrt 1000", cpu.reg[0] == 31);

    /* --- ArcTan/ArcTan2: 10000h is a full turn, so 4000h is pi/2 --- */
    cpu.reg[0] = 0x4000;                       /* tan = 1.0 */
    bios_swi(&memory, &cpu, &hw, 0x09);
    t_check("ArcTan 1.0 = pi/4", (uint16_t)cpu.reg[0] == 0x2000);

    cpu.reg[0] = 0x0000;
    bios_swi(&memory, &cpu, &hw, 0x09);
    t_check("ArcTan 0.0", cpu.reg[0] == 0);

    cpu.reg[0] = (uint32_t)-0x4000;
    bios_swi(&memory, &cpu, &hw, 0x09);
    t_check("ArcTan -1.0", (uint16_t)cpu.reg[0] == 0xE000);

    cpu.reg[0] = 0x4000;                       /* x */
    cpu.reg[1] = 0x4000;                       /* y */
    bios_swi(&memory, &cpu, &hw, 0x0A);
    t_check("ArcTan2(1,1) = pi/4", (uint16_t)cpu.reg[0] == 0x2000);

    cpu.reg[0] = 0x0000;
    cpu.reg[1] = 0x4000;
    bios_swi(&memory, &cpu, &hw, 0x0A);
    t_check("ArcTan2(0,1) = pi/2", (uint16_t)cpu.reg[0] == 0x4000);

    cpu.reg[0] = 0x4000;
    cpu.reg[1] = 0x0000;
    bios_swi(&memory, &cpu, &hw, 0x0A);
    t_check("ArcTan2(1,0) = 0", cpu.reg[0] == 0);

    cpu.reg[0] = 0x4000;                       /* x */
    cpu.reg[1] = (uint32_t)-0x4000;            /* y */
    bios_swi(&memory, &cpu, &hw, 0x0A);
    t_check("ArcTan2(-1,1) wraps to 1.75 turns", (uint16_t)cpu.reg[0] == 0xE000);

    /* --- CpuSet, 16 bit, count is in halfwords --- */
    memory_write32(&memory, 0x03000000u, 0xAAAAAAAAu);
    memory_write32(&memory, 0x03000004u, 0xBBBBBBBBu);
    memory_write32(&memory, 0x03000008u, 0xCCCCCCCCu);
    memory_write32(&memory, 0x0300000Cu, 0xDDDDDDDDu);
    cpu.reg[0] = 0x03000000u;
    cpu.reg[1] = 0x03000010u;
    cpu.reg[2] = 8;                            /* 8 halfwords = 4 words */
    bios_swi(&memory, &cpu, &hw, 0x0B);
    t_check("CpuSet 16 bit copy",
            memory_read32(&memory, 0x03000010u) == 0xAAAAAAAAu
         && memory_read32(&memory, 0x0300001Cu) == 0xDDDDDDDDu);

    /* --- CpuSet, 32 bit with a fixed source --- */
    memory_write32(&memory, 0x03000000u, 0x5A5A5A5Au);
    cpu.reg[0] = 0x03000000u;
    cpu.reg[1] = 0x03000040u;
    cpu.reg[2] = 4 | (1u << 24) | (1u << 26);  /* 4 words, fixed, 32 bit */
    bios_swi(&memory, &cpu, &hw, 0x0B);
    t_check("CpuSet 32 bit fixed source",
            memory_read32(&memory, 0x03000040u) == 0x5A5A5A5Au
         && memory_read32(&memory, 0x0300004Cu) == 0x5A5A5A5Au);

    /* --- CpuFastSet always moves whole blocks of 8 words --- */
    for (uint32_t i = 0; i < 8; i++) {
        memory_write32(&memory, 0x03000000u + i * 4u, 0x12345600u + i);
    }
    cpu.reg[0] = 0x03000000u;
    cpu.reg[1] = 0x03000080u;
    cpu.reg[2] = 3;                            /* rounded up to 8 words */
    bios_swi(&memory, &cpu, &hw, 0x0C);
    t_check("CpuFastSet 32 bit copy",
            memory_read32(&memory, 0x03000080u) == 0x12345600u
         && memory_read32(&memory, 0x0300009Cu) == 0x12345607u);

    /* --- LZ77: size in bits 8-23, then flag bytes and blocks --- */
    memory_write32(&memory, 0x03000100u, 3u << 8);  /* 3 bytes out */
    memory_write8(&memory, 0x03000104u, 0x40);       /* literal, then backref */
    memory_write8(&memory, 0x03000105u, 0xAA);       /* the literal */
    memory_write8(&memory, 0x03000106u, 0x00);       /* dist 1, length 3 */
    memory_write8(&memory, 0x03000107u, 0x00);
    bios_lz77(&memory, 0x03000100u, 0x03000200u, 1);
    t_check("LZ77 decompress",
            memory_read8(&memory, 0x03000200u) == 0xAA
         && memory_read8(&memory, 0x03000201u) == 0xAA
         && memory_read8(&memory, 0x03000202u) == 0xAA
         && memory_read8(&memory, 0x03000203u) == 0x00);

    /* --- run length: a run block and three single literals --- */
    memory_write32(&memory, 0x03000200u, 6u << 8);
    memory_write8(&memory, 0x03000204u, 0x83);       /* 3 + 3 repeats */
    memory_write8(&memory, 0x03000205u, 0x55);
    bios_rl(&memory, 0x03000200u, 0x03000300u, 1);
    t_check("RL run block",
            memory_read8(&memory, 0x03000300u) == 0x55
         && memory_read8(&memory, 0x03000305u) == 0x55
         && memory_read8(&memory, 0x03000306u) == 0x00);

    memory_write32(&memory, 0x03000200u, 3u << 8);
    memory_write8(&memory, 0x03000204u, 0x00);
    memory_write8(&memory, 0x03000205u, 0x11);
    memory_write8(&memory, 0x03000206u, 0x00);
    memory_write8(&memory, 0x03000207u, 0x22);
    memory_write8(&memory, 0x03000208u, 0x00);
    memory_write8(&memory, 0x03000209u, 0x33);
    bios_rl(&memory, 0x03000200u, 0x03000310u, 1);
    t_check("RL literal blocks",
            memory_read8(&memory, 0x03000310u) == 0x11
         && memory_read8(&memory, 0x03000311u) == 0x22
         && memory_read8(&memory, 0x03000312u) == 0x33);

    /* --- difference filter: two 8 bit deltas make one 16 bit sample --- */
    memory_write32(&memory, 0x03000400u, 4u << 8);    /* 4 bytes out */
    memory_write8(&memory, 0x03000404u, 1);
    memory_write8(&memory, 0x03000405u, 2);
    memory_write8(&memory, 0x03000406u, 3);
    memory_write8(&memory, 0x03000407u, 4);
    bios_filter(&memory, 0x03000400u, 0x03000500u, 1);
    t_check("Difference filter 8->16",
            memory_read16(&memory, 0x03000500u) == 0x0301
         && memory_read16(&memory, 0x03000502u) == 0x0A06);

    /* --- difference filter, 16 bit deltas (including a negative one) --- */
    memory_write32(&memory, 0x03000410u, 6u << 8);    /* 6 bytes out */
    memory_write16(&memory, 0x03000414u, 0x0010);
    memory_write16(&memory, 0x03000416u, 0x0005);
    memory_write16(&memory, 0x03000418u, 0xFFF0);
    bios_filter(&memory, 0x03000410u, 0x03000510u, 2);
    t_check("Difference filter 16->16",
            memory_read16(&memory, 0x03000510u) == 0x0010
         && memory_read16(&memory, 0x03000512u) == 0x0015
         && memory_read16(&memory, 0x03000514u) == 0x0005);

    /* --- Halt and Stop put the CPU into a wait state --- */
    cpu.wait_mode = CPU_RUN;
    bios_swi(&memory, &cpu, &hw, 0x02);
    t_check("Halt sets wait mode", cpu.wait_mode == CPU_HALT && cpu.halted);
    bios_swi(&memory, &cpu, &hw, 0x03);
    t_check("Stop sets wait mode", cpu.wait_mode == CPU_STOP && cpu.halted);
    cpu_set_wait(&cpu, CPU_RUN);

    /* IntrWait with the flag already pending must return immediately. */
    hw.ie = HW_IRQ_VBLANK;
    hw.if_ = HW_IRQ_VBLANK;
    hw.ime = 0;
    cpu.reg[0] = 0;
    cpu.reg[1] = HW_IRQ_VBLANK;
    bios_swi(&memory, &cpu, &hw, 0x04);
    t_check("IntrWait returns when the flag is set",
            !cpu.halted && cpu.wait_mode == CPU_RUN && cpu.reg[1] == HW_IRQ_VBLANK);

    /* With nothing pending it has to stop until the flag shows up. */
    hw.if_ = 0;
    bios_swi(&memory, &cpu, &hw, 0x04);
    t_check("IntrWait halts when the flag is missing", cpu.halted);
    t_check("Halted CPU wakes on the requested interrupt",
            (hw_raise_irq(&hw, HW_IRQ_VBLANK), hw_should_wake(&hw)));
    hw_intr_wait_end(&hw);
    cpu_set_wait(&cpu, CPU_RUN);

    t_check("SWI returns to the caller", cpu.reg[15] == 0x08000040u);

    /* --- SoftReset ---
     * SWI 0x00 must land in the cartridge entry point. The step loop
     * overwrites R15 with the next sequential address unless the PC was
     * flagged as written, so a plain register store here is undone the
     * moment the SWI returns and the reset jumps nowhere. */
    {
        static uint8_t rom[0x200];
        CPU    soft;
        Memory smem;
        memory_init(&smem);
        memory_load_rom_data(&smem, rom, sizeof(rom));
        cpu_init(&soft);
        /* Route the SWI to the same HLE path the emulator installs. */
        soft.swi_hook = test_swi_hook;
        soft.swi_ctx  = &hw;
        memory_set_io(&smem, &hw);
        /* The hook works off hw->cpu, so point it at the CPU under test. */
        hw.cpu = &soft;
        hw.mem = &smem;
        soft.cpsr = MODE_USR;
        soft.reg[15] = 0x08000040u;
        memory_write32(&smem, 0x08000040u, 0xEF000000u); /* SWI 0 */
        cpu_step(&soft, &smem);
        t_check("SoftReset reaches the cartridge entry point",
                soft.reg[15] == 0x08000000u);
        t_check("SoftReset leaves ARM state", (soft.cpsr & FLAG_T) == 0);
        t_check("SoftReset points the stack at the top of IWRAM",
                cpu_read_reg(&soft, 13) == 0x03007F00u);
        /* A sequential step after the reset must not disturb the PC. */
        memory_write32(&smem, 0x08000000u, 0xE1A00000u); /* MOV r0, r0 */
        cpu_step(&soft, &smem);
        t_check("SoftReset PC survives the next step",
                soft.reg[15] == 0x08000004u);
        memory_set_io(&smem, NULL);
        memory_free(&smem);
    }

    /* --- SWI into a real BIOS image ---
     * With an image mapped the SWI has to vector to 0x08 and let the BIOS
     * code run, rather than jumping to the next instruction. */
    {
        Emulator emu;
        emulator_init(&emu);
        static uint8_t rom[0x200];
        memory_load_rom_data(&emu.mem, rom, sizeof(rom));
        emu.has_bios = 1;
        cpu_init(&emu.cpu);
        emu.cpu.cpsr = MODE_USR;
        emu.cpu.reg[15] = 0x08000040u;
        /* The BIOS entry at 0x08 is left as zeros, so the CPU falls
         * straight through it; what matters is the vector. */
        emulator_swi(&emu, 0x00);
        t_check("A mapped BIOS vectors SWI to 0x08",
                emu.cpu.reg[15] == 0x00000008u);
        t_check("A mapped BIOS switches to SVC", (emu.cpu.cpsr & 0x1F) == MODE_SVC);
        t_check("A mapped BIOS stores the return address in SVC LR",
                emu.cpu.r14[BANK_SVC] == 0x0800003Cu);
        emulator_free(&emu);
    }

    memory_set_io(&memory, NULL);
    memory_free(&memory);
}

/* ---- end to end ---------------------------------------------------------- */

/* ---- long multiply (UMULL/SMULL/UMLAL/SMLAL) ----------------------------- */

/* Run a 64-bit multiply and return the result as {high, low}. In the ARMv4T
 * long-multiply encoding RdLo is in bits 15-12, RdHi in bits 19-16, Rm in
 * bits 11-8 and Rs in bits 3-0 -- the source operands sit in the opposite
 * fields from the 32-bit form. */
static void run_long_mult(CPU *cpu, Memory *mem, uint32_t insn,
                          uint32_t rdlo, uint32_t rdhi,
                          uint32_t rm, uint32_t rs,
                          uint32_t *hi, uint32_t *lo) {
    unsigned lo_idx = (insn >> 12) & 0xF;
    unsigned hi_idx = (insn >> 16) & 0xF;

    cpu_init(cpu);
    memory_write32(mem, 0x03000000, insn);
    memory_write32(mem, 0x03000004, 0xEAFFFFFE);   /* b . */
    cpu_write_reg(cpu, lo_idx, rdlo);
    cpu_write_reg(cpu, hi_idx, rdhi);
    cpu_write_reg(cpu, (insn >> 8) & 0xF, rm);
    cpu_write_reg(cpu, insn & 0xF, rs);
    cpu->cpsr |= FLAG_I | FLAG_F;
    cpu->cpsr &= ~FLAG_T;
    cpu->reg[15] = 0x03000000;
    cpu_step(cpu, mem);

    *lo = cpu_read_reg(cpu, lo_idx);
    *hi = cpu_read_reg(cpu, hi_idx);
}

static void test_long_multiply(Memory *memory) {
    CPU cpu;
    uint32_t hi, lo;

    printf("=== 64-bit multiply ===\n");

    /* UMULL r0, r1, r2, r3 with r2=3, r3=7 -> 21. */
    run_long_mult(&cpu, memory, 0xE0810392, 0, 0, 3, 7, &hi, &lo);
    t_check("UMULL produces the full 64-bit product", lo == 21 && hi == 0);
    t_check("UMULL writes the low word to RdLo", cpu_read_reg(&cpu, 0) == 21);
    t_check("UMULL writes the high word to RdHi", cpu_read_reg(&cpu, 1) == 0);

    /* The high word must not be forced to zero. */
    run_long_mult(&cpu, memory, 0xE0810392, 0, 0, 0x10000, 0x10000, &hi, &lo);
    t_check("UMULL fills the high word when it overflows 32 bits",
            lo == 0 && hi == 1);

    /* SMULL r0, r1, r2, r3 with r2=3, r3=-7 -> -21. */
    run_long_mult(&cpu, memory, 0xE0C10392, 0, 0, 3, 0xFFFFFFF9u, &hi, &lo);
    t_check("SMULL sign extends both operands",
            lo == 0xFFFFFFEB && hi == 0xFFFFFFFFu);

    /* UMLAL accumulates into the full RdHi:RdLo pair, not just RdLo. */
    run_long_mult(&cpu, memory, 0xE0A10392, 5, 0, 3, 7, &hi, &lo);
    t_check("UMLAL adds the product to the low accumulator", lo == 26 && hi == 0);
    run_long_mult(&cpu, memory, 0xE0A10392, 0, 7, 3, 7, &hi, &lo);
    t_check("UMLAL adds the product to the high accumulator too",
            lo == 21 && hi == 7);

    /* SMLAL accumulates a negative product. */
    run_long_mult(&cpu, memory, 0xE0E10392, 5, 0xFFFFFFFFu, 3, 0xFFFFFFF9u,
                  &hi, &lo);
    t_check("SMLAL accumulates into a negative 64-bit value",
            lo == 0xFFFFFFF0 && hi == 0xFFFFFFFEu);

    /* The S variants set flags from the 64-bit result. */
    run_long_mult(&cpu, memory, 0xE0910392, 0, 0, 0, 0, &hi, &lo);
    t_check("UMULLS sets Z when the 64-bit result is zero",
            (cpu.cpsr & FLAG_Z) != 0);
    t_check("UMULLS leaves N clear for a zero result",
            (cpu.cpsr & FLAG_N) == 0);

    run_long_mult(&cpu, memory, 0xE0D10392, 0, 0, 0, 0, &hi, &lo);
    t_check("SMULLS sets Z when the 64-bit result is zero",
            (cpu.cpsr & FLAG_Z) != 0);
}

static void test_boot_rom(void) {
    static uint8_t rom[TEST_ROM_SIZE];
    Emulator emu;

    printf("=== Boot ROM (end to end) ===\n");

    test_rom_build(rom, sizeof(rom));

    emulator_init(&emu);
    if (!memory_load_rom_data(&emu.mem, rom, sizeof(rom))) {
        t_check("load synthetic ROM", 0);
        emulator_free(&emu);
        return;
    }
    t_check("load synthetic ROM", emu.mem.rom_size == sizeof(rom));

    for (int i = 0; i < 30; i++) {
        emulator_frame(&emu);
        if (memory_read32(&emu.mem, 0x03000000u) == 0xC0DEC0DEu) {
            break;
        }
    }

    t_check("program reached its end marker",
            memory_read32(&emu.mem, 0x03000000u) == 0xC0DEC0DEu);

    /* The marker shows up part way through a frame, so run a couple more to
     * render the frame buffer the program has finished filling. */
    for (int i = 0; i < 2; i++) {
        emulator_frame(&emu);
    }
    t_check("VBlankIntrWait returned", !emu.cpu.halted
                                   && emu.hw.intr_wait_active == 0);
    t_check("paletted bitmap enabled", (emu.ppu.dispcnt & 0x0407u) == 0x0404u);

    /* Pixel (x,y) holds palette entry (x+y) & 0xFF. */
    t_check("bitmap pixel (0,0)", emu.ppu.frame[0] == 0x000000u);
    t_check("bitmap pixel (1,0)", emu.ppu.frame[1] == test_rom_expected_color(1));
    t_check("bitmap pixel (16,0)", emu.ppu.frame[16] == test_rom_expected_color(16));
    t_check("bitmap pixel (0,8)", emu.ppu.frame[8 * 240] == test_rom_expected_color(8));
    t_check("bitmap pixel (239,159)",
            emu.ppu.frame[159 * 240 + 239]
                == test_rom_expected_color((239u + 159u) & 0xFFu));

    emulator_free(&emu);
}

/* ---- sound ---------------------------------------------------------------- */

/* Drive the sound engine directly so the tests do not depend on a game.
 * The ring buffer is finite, so the engine is advanced in small steps and
 * drained as it goes. */
/* Advance `cycles` of CPU time in 512 cycle sample steps, draining the ring
 * between steps.  Returns the number of sample frames produced. */
static uint32_t snd_run(Sound *s, uint64_t cycles, int16_t *peak) {
    int16_t buf[4096];
    uint32_t total = 0;
    int16_t running = 0;

    for (uint64_t c = 512u; c <= cycles; c += 512u) {
        uint32_t n;
        sound_tick(s, c);
        while ((n = sound_read(s, buf, 2048)) > 0) {
            for (uint32_t i = 0; i < n * 2; i++) {
                int16_t v = buf[i] < 0 ? (int16_t)-buf[i] : buf[i];
                if (v > running) running = v;
            }
            total += n;
        }
    }
    *peak = running;
    return total;
}

static int16_t snd_peak(Sound *s, uint64_t cycles) {
    int16_t peak;
    (void)snd_run(s, cycles, &peak);
    return peak;
}

/* Peak level of the left and right outputs separately. */
static void snd_peaks(Sound *s, uint64_t cycles, int16_t *left, int16_t *right) {
    int16_t buf[4096];
    uint16_t l = 0, r = 0;

    for (uint64_t c = 512u; c <= cycles; c += 512u) {
        uint32_t n;
        sound_tick(s, c);
        while ((n = sound_read(s, buf, 2048)) > 0) {
            for (uint32_t i = 0; i < n; i++) {
                int16_t vl = buf[i * 2] < 0 ? (int16_t)-buf[i * 2] : buf[i * 2];
                int16_t vr = buf[i * 2 + 1] < 0 ? (int16_t)-buf[i * 2 + 1] : buf[i * 2 + 1];
                if (vl > l) l = vl;
                if (vr > r) r = vr;
            }
        }
    }
    *left = (int16_t)l;
    *right = (int16_t)r;
}

static void test_sound(void) {
    Sound  s;
    int16_t peak = 0;

    printf("=== Sound (PSG + DirectSound) ===\n");

    /* A 50% duty square wave on channel 1 at full volume, frequency 0x600.
     * GBATEK: SOUND1CNT_H bits 0-5 are the length, 6-7 the duty, 12-15 the
     * initial volume; SOUND1CNT_X bits 0-10 the frequency, 14 the length
     * flag and 15 the restart trigger. */
#define SND_SQUARE(s_, x_) do {                                       \
        sound_write16(&(s_), 0x4000060u, 0x0000u); /* no sweep */      \
        sound_write16(&(s_), 0x4000062u, 0xF080u); /* vol 15, 50% */   \
        sound_write16(&(s_), 0x4000064u, (x_));    /* freq + restart */\
    } while (0)
#define SND_ENABLE_ALL(s_) do {                                       \
        sound_write16(&(s_), 0x4000080u, 0x7777u); /* PSG vol 7+7, all */\
        sound_write16(&(s_), 0x4000082u, 0x0002u); /* PSG at 100% */   \
        sound_write16(&(s_), 0x4000084u, 0x0080u); /* master enable */ \
    } while (0)

    /* The mixer samples once every 512 CPU cycles. */
    sound_init(&s);
    t_check("the mixer samples once every 512 CPU cycles",
            s.frac_step == 128u && EMULATOR_AUDIO_RATE == 32768u);

    /* One second of CPU time is 16777216 cycles, which is 32768 samples. */
    sound_init(&s);
    t_check("one second of CPU time yields 32768 sample frames",
            snd_run(&s, 16777216u, &peak) == 32768u);

    /* SOUNDBIAS powers up at 0x200, so an idle mixer is exactly zero. */
    sound_init(&s);
    t_check("SOUNDBIAS defaults to 0x200 so silence is zero",
            s.bias == 0x200 && snd_peak(&s, 16777216u) == 0);

    /* SOUND_CNT_X bit 7 gates everything the mixer emits. */
    sound_init(&s);
    sound_write16(&s, 0x4000080u, 0x7777u);
    sound_write16(&s, 0x4000082u, 0x0002u);
    SND_SQUARE(s, 0x8600u);
    sound_write16(&s, 0x4000084u, 0x0000u);
    t_check("SOUND_CNT_X bit 7 gates the output", snd_peak(&s, 16777216u) == 0);

    sound_init(&s);
    sound_write16(&s, 0x4000080u, 0x7777u);
    sound_write16(&s, 0x4000082u, 0x0002u);
    SND_SQUARE(s, 0x8600u);
    sound_write16(&s, 0x4000084u, 0x0080u);
    t_check("an enabled square wave is audible", snd_peak(&s, 16777216u) > 0);

    /* Clearing the master enable also zeroes the PSG registers. */
    sound_init(&s);
    sound_write16(&s, 0x4000080u, 0x7777u);
    SND_SQUARE(s, 0x8600u);
    sound_write16(&s, 0x4000084u, 0x0080u);
    sound_write16(&s, 0x4000084u, 0x0000u);
    t_check("clearing SOUND_CNT_X bit 7 resets the PSG registers",
            s.reg[0x00] == 0 && s.reg[0x02] == 0 && s.reg[0x10] == 0
                && !s.ch[0].on);

    /* SOUNDCNT_L bits 8-15 gate each channel on the right and the left. */
    {
        int16_t l, r;

        sound_init(&s);
        sound_write16(&s, 0x4000080u, 0x0701u); /* right ch1 only */
        sound_write16(&s, 0x4000082u, 0x0002u);
        SND_SQUARE(s, 0x8600u);
        sound_write16(&s, 0x4000084u, 0x0080u);
        snd_peaks(&s, 262144u, &l, &r);
        t_check("SOUNDCNT_L bits 8-15 route each channel to one side",
                l == 0 && r > 0);

        sound_init(&s);
        sound_write16(&s, 0x4000080u, 0x7777u); /* every channel, both sides */
        sound_write16(&s, 0x4000082u, 0x0002u);
        SND_SQUARE(s, 0x8600u);
        sound_write16(&s, 0x4000084u, 0x0080u);
        snd_peaks(&s, 262144u, &l, &r);
        t_check("enabling a channel on both sides feeds both outputs", l > 0 && r > 0);
    }

    /* SOUNDCNT_L bits 0-2 and 4-6 are the PSG master volume. */
    {
        int16_t l1, r1, l2, r2;

        sound_init(&s);
        sound_write16(&s, 0x4000080u, 0x7707u); /* both sides, right volume 7 */
        sound_write16(&s, 0x4000082u, 0x0002u);
        SND_SQUARE(s, 0x8600u);
        sound_write16(&s, 0x4000084u, 0x0080u);
        snd_peaks(&s, 262144u, &l1, &r1);

        sound_init(&s);
        sound_write16(&s, 0x4000080u, 0x7700u); /* right volume 0 */
        sound_write16(&s, 0x4000082u, 0x0002u);
        SND_SQUARE(s, 0x8600u);
        sound_write16(&s, 0x4000084u, 0x0080u);
        snd_peaks(&s, 262144u, &l2, &r2);

        t_check("SOUNDCNT_L bits 0-2 scale the right PSG volume",
                r1 > r2 && r2 > 0);
        t_check("SOUNDCNT_L bits 4-6 leave the left side alone",
                l1 == l2 && l1 > 0);
    }

    /* SOUNDCNT_H bits 0-1 pick 25%, 50% or 100% for the whole PSG. */
    {
        int16_t full, quarter;

        sound_init(&s);
        sound_write16(&s, 0x4000080u, 0x7777u);
        sound_write16(&s, 0x4000082u, 0x0002u); /* 100% */
        SND_SQUARE(s, 0x8600u);
        sound_write16(&s, 0x4000084u, 0x0080u);
        full = snd_peak(&s, 16777216u);

        sound_init(&s);
        sound_write16(&s, 0x4000080u, 0x7777u);
        sound_write16(&s, 0x4000082u, 0x0000u); /* 25% */
        SND_SQUARE(s, 0x8600u);
        sound_write16(&s, 0x4000084u, 0x0080u);
        quarter = snd_peak(&s, 16777216u);

        t_check("SOUNDCNT_H bits 0-1 scale the PSG (100% louder than 25%)",
                full > quarter && quarter > 0);
    }

    /* The length counter only runs when SOUNDxCNT_X bit 14 is set. */
    sound_init(&s);
    sound_write16(&s, 0x4000080u, 0x7777u);
    sound_write16(&s, 0x4000082u, 0x0002u);
    SND_SQUARE(s, 0x8600u);                  /* bit 14 clear */
    sound_write16(&s, 0x4000084u, 0x0080u);
    (void)snd_peak(&s, 16777216u);
    t_check("a channel with the length flag off keeps playing",
            s.ch[0].on && !s.ch[0].length_enable);

    sound_init(&s);
    sound_write16(&s, 0x4000080u, 0x7777u);
    sound_write16(&s, 0x4000082u, 0x0002u);
    sound_write16(&s, 0x4000060u, 0x0000u);  /* no sweep */
    sound_write16(&s, 0x4000062u, 0xF080u);  /* length 0, vol 15, 50% duty */
    sound_write16(&s, 0x4000064u, 0xC600u);  /* length flag + restart */
    sound_write16(&s, 0x4000084u, 0x0080u);
    (void)snd_peak(&s, 16777216u);
    t_check("the length counter stops a length-enabled channel", !s.ch[0].on);

    /* The envelope counts down the initial volume. */
    sound_init(&s);
    sound_write16(&s, 0x4000080u, 0x7777u);
    sound_write16(&s, 0x4000082u, 0x0002u);
    sound_write16(&s, 0x4000060u, 0x0000u);
    sound_write16(&s, 0x4000062u, 0xF108u);  /* step 1, decreasing, vol 15 */
    sound_write16(&s, 0x4000064u, 0x8600u);
    sound_write16(&s, 0x4000084u, 0x0080u);
    t_check("the initial volume is 15", s.ch[0].volume == 15);
    (void)snd_peak(&s, 16777216u);
    t_check("a decreasing envelope runs the volume to zero",
            s.ch[0].volume == 0 && snd_peak(&s, 65536u) == 0);

    /* The wave channel plays the nibbles in wave RAM, two banks selectable. */
    sound_init(&s);
    for (int i = 0; i < 16; i++) {
        s.wave[i] = (uint8_t)(i * 2);
        s.wave[i + 16] = (uint8_t)(15 - i);
    }
    sound_write16(&s, 0x4000080u, 0x7777u);
    sound_write16(&s, 0x4000082u, 0x0002u);
    sound_write16(&s, 0x4000070u, 0x0080u);  /* channel 3 playback on */
    sound_write16(&s, 0x4000072u, 0x2000u);  /* 100% volume */
    sound_write16(&s, 0x4000074u, 0x8000u);  /* restart, sample rate 0 */
    sound_write16(&s, 0x4000084u, 0x0080u);
    t_check("wave channel 3 plays wave RAM", snd_peak(&s, 16777216u) > 0);

    /* SOUND3CNT_L bit 7 stops channel 3. */
    sound_write16(&s, 0x4000070u, 0x0000u);
    t_check("SOUND3CNT_L bit 7 stops the wave channel", !s.ch[2].on);

    /* The noise channel's LFSR shifts as its timer fires. */
    sound_init(&s);
    sound_write16(&s, 0x4000080u, 0x7777u);
    sound_write16(&s, 0x4000082u, 0x0002u);
    sound_write16(&s, 0x4000078u, 0xF000u);  /* vol 15 */
    sound_write16(&s, 0x400007Cu, 0x8000u);  /* restart */
    sound_write16(&s, 0x4000084u, 0x0080u);
    t_check("the noise LFSR powers up as all ones", s.ch[3].lfsr == 0x7FFFu);
    (void)snd_peak(&s, 16777216u);
    t_check("the noise channel is audible and its LFSR moves",
            s.ch[3].lfsr != 0x7FFFu);

    /* DirectSound: an 8 bit stream in FIFO A reaches the mixer. */
    sound_init(&s);
    for (int i = 0; i < 32; i++) {
        s.fifo[0][i] = (int8_t)(i < 16 ? 0xC0 : 0x40);
    }
    s.fifo_count[0] = 32;
    s.fifo_next[0] = 0;
    s.fifo_timer[0] = 1024; /* TM0 clocking the FIFO */
    sound_write16(&s, 0x4000080u, 0x7000u);
    sound_write16(&s, 0x4000082u, 0x0002u | 0x0200u); /* PSG 100%, FIFO A left */
    sound_write16(&s, 0x4000084u, 0x0080u);
    t_check("DirectSound FIFO A reaches the mixer", snd_peak(&s, 16777216u) > 0);
    t_check("the FIFO is consumed as the timer fires", s.fifo_count[0] < 32u);

    /* A FIFO that is routed nowhere stays silent. */
    sound_init(&s);
    for (int i = 0; i < 32; i++) {
        s.fifo[0][i] = (int8_t)0xC0;
    }
    s.fifo_count[0] = 32;
    s.fifo_next[0] = 0;
    s.fifo_timer[0] = 1024;
    sound_write16(&s, 0x4000080u, 0x7000u);
    sound_write16(&s, 0x4000082u, 0x0002u);  /* no FIFO routing at all */
    sound_write16(&s, 0x4000084u, 0x0080u);
    t_check("an unrouted FIFO is silent", snd_peak(&s, 16777216u) == 0);

    /* SOUNDCNT_H bits 8-15 route FIFO A, and bit 11 resets it. */
    sound_init(&s);
    for (int i = 0; i < 8; i++) {
        sound_write16(&s, 0x40000A0u, 0xC000u);
    }
    sound_write16(&s, 0x4000082u, 0x0100u);  /* FIFO A right */
    t_check("SOUNDCNT_H bit 8 routes FIFO A to the right", s.dma_right[0]);
    sound_write16(&s, 0x4000082u, 0x0800u);  /* reset FIFO A */
    t_check("SOUNDCNT_H bit 11 resets FIFO A",
            s.fifo_count[0] == 0 && s.fifo_head[0] == 0);
    sound_write16(&s, 0x4000082u, 0x4000u);  /* FIFO B uses timer 1 */
    t_check("SOUNDCNT_H bit 14 selects timer 1 for FIFO B", s.dma_timer[1] == 1);

    /* Word writes into the FIFO feed it, and the FIFO is 32 words deep. */
    sound_init(&s);
    for (int i = 0; i < 40; i++) {
        sound_write16(&s, 0x40000A0u, (uint16_t)(i < 16 ? 0xC000u : 0x4000u));
    }
    t_check("the FIFO holds at most 32 words", s.fifo_count[0] == 32);

    /* A 32 bit FIFO word is four samples, least significant byte first. */
    sound_init(&s);
    sound_write32(&s, 0x40000A0u, 0x44332211u);
    t_check("a 32 bit FIFO A write pushes four samples, LSB first",
            s.fifo_count[0] == 4u
                && s.fifo[0][0] == 0x11 && s.fifo[0][1] == 0x22
                && s.fifo[0][2] == 0x33 && s.fifo[0][3] == 0x44);
    sound_init(&s);
    sound_write32(&s, 0x40000A4u, 0x00000080u);
    t_check("0x040000A4 feeds the same FIFO A",
            s.fifo_count[0] == 4u && s.fifo[0][0] == (int8_t)0x80);
    sound_init(&s);
    sound_write32(&s, 0x40000B0u, 0x01020304u);
    t_check("0x040000B0 feeds FIFO B", s.fifo_count[1] == 4u && s.fifo_count[0] == 0u);

    /* A stopped timer leaves the FIFO frozen. */
    sound_init(&s);
    sound_write32(&s, 0x40000A0u, 0x80808080u);
    sound_write16(&s, 0x4000080u, 0x7000u);
    sound_write16(&s, 0x4000082u, 0x0002u | 0x0200u);
    sound_write16(&s, 0x4000084u, 0x0080u);
    s.fifo_timer[0] = 0; /* TM0 not running */
    (void)snd_peak(&s, 16777216u);
    t_check("a stopped timer never drains the FIFO", s.fifo_count[0] == 4u);

    /* Registers read back what the program wrote. */
    sound_init(&s);
    sound_write16(&s, 0x4000060u, 0x1234u);
    sound_write16(&s, 0x4000062u, 0x5678u);
    sound_write16(&s, 0x4000064u, 0x9ABCu);
    sound_write16(&s, 0x4000068u, 0x0FFFu);
    sound_write16(&s, 0x4000080u, 0xF0AAu);
    sound_write16(&s, 0x4000082u, 0xBB55u);
    sound_write16(&s, 0x4000084u, 0x0080u);
    sound_write16(&s, 0x4000088u, 0x8C40u);
    t_check("SOUND1CNT_L/H/X read back",
            s.reg[0x00] == 0x1234u && s.reg[0x01] == 0x5678u
                && s.reg[0x02] == 0x9ABCu && s.reg[0x04] == 0x0FFFu);
    t_check("SOUNDCNT_L/H read back",
            s.reg[0x10] == 0xF0AAu && s.reg[0x11] == 0xBB55u);
    t_check("SOUNDBIAS 0x8C40 decodes to bias 0x020 and 7 bit output",
            s.bias == 0x020u && s.amp_bits == 2u);

    /* SOUND_CNT_X bits 0-3 report which channels are running. */
    sound_init(&s);
    sound_write16(&s, 0x4000084u, 0x0080u);
    sound_write16(&s, 0x4000064u, 0x8600u);
    {
        uint16_t v = 0;
        t_check("SOUND_CNT_X bit 0 reports channel 1 running",
                sound_read16(&s, 0x4000084u, &v) && (v & 0x0001u) != 0);
        sound_write16(&s, 0x4000064u, 0x0000u);
    }
    sound_write16(&s, 0x4000064u, 0xC600u);  /* length flag + restart */
    {
        uint16_t v = 0;
        (void)snd_peak(&s, 16777216u);
        t_check("SOUND_CNT_X bit 0 clears when the length expires",
                sound_read16(&s, 0x4000084u, &v) && (v & 0x0001u) == 0);
    }

#undef SND_SQUARE
#undef SND_ENABLE_ALL
}

/* ---- cartridge backup ---------------------------------------------------- */

/* EEPROM is bit-serial but the game still writes whole bytes, and the chip
 * consumes the eight bits of each one from the top down. An access is the
 * address at the chip's own fixed width followed by the payload, so this sends
 * `addr` right aligned in `addr_bits` bits and then the data byte. */
static void eeprom_write_byte(Memory *mem, uint8_t byte) {
    memory_write8(mem, 0x0D000000u, byte);
}

/* Send the address at the chip's own width followed by the terminating 1 bit,
 * left aligned in whole bytes. The chip counts the trailing bits of the last
 * byte off as padding, so nothing else is needed here. */
static void eeprom_send_address(Memory *mem, uint32_t addr) {
    int bits  = mem->save.addr_bits + 1;   /* address plus its terminator */
    int bytes = (bits + 7) / 8;
    /* The stream starts at the top of the first byte, so a width that is not
     * a whole number of bytes leaves the terminator part way down one. */
    uint32_t stream = ((addr << 1) | 1u) << (bytes * 8 - bits);

    for (int i = 0; i < bytes; i++) {
        eeprom_write_byte(mem, (uint8_t)(stream >> ((bytes - 1 - i) * 8)));
    }
}

static void eeprom_write_8(Memory *mem, uint32_t addr, uint8_t data) {
    eeprom_send_address(mem, addr);
    eeprom_write_byte(mem, data);
}

/* Open an access for reading. The address goes out, then the chip shifts the
 * payload back a bit at a time. */
static void eeprom_select(Memory *mem, uint32_t addr) {
    eeprom_send_address(mem, addr);
}

/* Read `count` payload bits, one per read of the same address. */
static uint32_t eeprom_read_bits(Memory *mem, int count) {
    uint32_t v = 0;
    for (int i = 0; i < count; i++) {
        v = (v << 1) | memory_read8(mem, 0x0D000000u);
    }
    return v;
}

static void test_cartridge_backup(void) {
    Memory mem;
    CPU    cpu;
    PPU    ppu;
    HW     hw;
    static uint8_t rom[0x200];
    const char *tmp = "/tmp/opencode/gba-test.sav";

    printf("=== Cartridge backup tests ===\n");

    memory_init(&mem);
    cpu_init(&cpu);
    ppu_init(&ppu, &mem);
    hw_init(&hw, &mem, &cpu, &ppu);
    memory_set_io(&mem, &hw);

    /* --- plain SRAM --- */
    memset(rom, 0, sizeof(rom));
    rom[0xB2] = 0x09;                       /* 64 KB of SRAM */
    memory_load_rom_data(&mem, rom, sizeof(rom));
    t_check("header 09 selects SRAM", mem.save.kind == SAVE_SRAM);
    t_check("header 09 is 64 KB", mem.save.size == 0x10000u);

    memory_write8(&mem, 0x0A000000u, 0x42);
    memory_write8(&mem, 0x0A000123u, 0x99);
    t_check("SRAM read back", memory_read8(&mem, 0x0A000000u) == 0x42);
    t_check("SRAM read back at an offset", memory_read8(&mem, 0x0A000123u) == 0x99);
    /* A 16 bit store to the window has to reach both bytes. */
    memory_write16(&mem, 0x0A000200u, 0xBEEF);
    t_check("SRAM 16 bit store hits both bytes",
            memory_read8(&mem, 0x0A000200u) == 0xEF &&
            memory_read8(&mem, 0x0A000201u) == 0xBE);

    memory_store_save(&mem, tmp);
    memory_write8(&mem, 0x0A000000u, 0x00);
    t_check("SRAM save file round trip", memory_load_save(&mem, tmp) &&
            memory_read8(&mem, 0x0A000000u) == 0x42);
    remove(tmp);

    /* --- EEPROM ---
     * The bit stream has no register behind it, so a byte written to
     * 0x0D000000 does not land at an address the way SRAM does. Before this
     * the whole window read back as open bus and every EEPROM game saw a
     * blank save. */
    rom[0xB2] = 0x05;                       /* 16 KB EEPROM, 14 bit address */
    memory_load_rom_data(&mem, rom, sizeof(rom));
    t_check("header 05 selects EEPROM", mem.save.kind == SAVE_EEPROM);
    t_check("header 05 has a 14 bit address", mem.save.addr_bits == 14);

    eeprom_write_8(&mem, 0x00, 0xA5);      /* payload 1010 0101 at address 0 */
    t_check("EEPROM write is not plain memory",
            mem.save.data[0] == 0xA5);

    /* Read it back: address 0 again, then 8 reads. */
    eeprom_select(&mem, 0x00);
    t_check("EEPROM read returns the written byte",
            eeprom_read_bits(&mem, 8) == 0xA5);

    /* A second address has to land somewhere else, which is what shows the
     * address is being parsed rather than ignored. */
    eeprom_write_8(&mem, 0x02, 0x5C);
    t_check("EEPROM honours a second address", mem.save.data[2] == 0x5C);
    eeprom_select(&mem, 0x00);
    t_check("EEPROM address 0 is unchanged", eeprom_read_bits(&mem, 8) == 0xA5);
    eeprom_select(&mem, 0x02);
    t_check("EEPROM address 2 reads back", eeprom_read_bits(&mem, 8) == 0x5C);

    /* A cell erases to all ones and programming only ever drives bits to
     * zero, so a later write can clear a bit but can never bring one back.
     * That is the one behaviour a plain memory write cannot reproduce. */
    eeprom_write_8(&mem, 0x02, 0x0F);
    t_check("EEPROM a later write clears bits", mem.save.data[2] == 0x0C);
    eeprom_write_8(&mem, 0x02, 0xA5);
    t_check("EEPROM a cleared bit does not come back",
            mem.save.data[2] == 0x04);
    eeprom_write_8(&mem, 0x00, 0x00);
    t_check("EEPROM an all zero payload erases the cell",
            mem.save.data[0] == 0x00);

    /* A fresh chip is erased, so an unwritten byte reads as all ones. */
    eeprom_select(&mem, 0x40);
    t_check("an unwritten EEPROM byte reads as all ones",
            eeprom_read_bits(&mem, 8) == 0xFF);

    memory_store_save(&mem, tmp);
    memset(mem.save.data, 0, sizeof(mem.save.data));
    t_check("EEPROM save file round trip", memory_load_save(&mem, tmp) &&
            mem.save.data[0] == 0x00 && mem.save.data[2] == 0x04);
    remove(tmp);

    /* An erased load means all ones, not zeros: a zeroed EEPROM would look
     * like a chip full of zeroes rather than a blank one. */
    t_check("an absent EEPROM save loads erased", memory_load_save(&mem, tmp) == 0 &&
            mem.save.data[4] == 0xFF);

    /* --- Flash --- */
    rom[0xB2] = 0x0D;                       /* 128 KB flash */
    memory_load_rom_data(&mem, rom, sizeof(rom));
    t_check("header 0D selects Flash", mem.save.kind == SAVE_FLASH);
    t_check("header 0D is 128 KB", mem.save.size == 0x20000u);

    /* Program a byte the way a game does: 0xAA/0x55/0xAA, then 0xA0, then
     * the data byte at the address the 0xA0 went to. */
    memory_write8(&mem, 0x0A000000u, 0xFF);  /* erased */
    memory_write8(&mem, 0x0A005555u, 0xAA);
    memory_write8(&mem, 0x0A002AAAu, 0x55);
    memory_write8(&mem, 0x0A005555u, 0xAA);
    memory_write8(&mem, 0x0A000000u, 0xA0);
    memory_write8(&mem, 0x0A000000u, 0x5C);
    t_check("Flash programs a byte", memory_read8(&mem, 0x0A000000u) == 0x5C);

    /* Programming can only clear bits: 0xC3 over 0x5C leaves 0x40. */
    memory_write8(&mem, 0x0A005555u, 0xAA);
    memory_write8(&mem, 0x0A002AAAu, 0x55);
    memory_write8(&mem, 0x0A005555u, 0xAA);
    memory_write8(&mem, 0x0A000000u, 0xA0);
    memory_write8(&mem, 0x0A000000u, 0xC3);
    t_check("Flash programming only clears bits",
            memory_read8(&mem, 0x0A000000u) == 0x40);

    /* A chip erase puts everything back to 0xFF. */
    memory_write8(&mem, 0x0A005555u, 0xAA);
    memory_write8(&mem, 0x0A002AAAu, 0x55);
    memory_write8(&mem, 0x0A005555u, 0xAA);
    memory_write8(&mem, 0x0A005555u, 0x80);
    memory_write8(&mem, 0x0A002AAAu, 0xAA);
    memory_write8(&mem, 0x0A005555u, 0x10);
    t_check("a Flash chip erase restores 0xFF",
            memory_read8(&mem, 0x0A000000u) == 0xFF);

    /* Identification mode is how a game tells the chip apart. */
    memory_write8(&mem, 0x0A005555u, 0x90);
    t_check("Flash reports a vendor id", memory_read8(&mem, 0x0A000000u) == 0x32);
    t_check("Flash reports a device id", memory_read8(&mem, 0x0A000001u) == 0x62);
    t_check("Flash reports a size id", memory_read8(&mem, 0x0A000002u) == 0x13);
    memory_write8(&mem, 0x0A005555u, 0xF0);  /* back to normal reads */
    t_check("leaving ID mode restores normal reads",
            memory_read8(&mem, 0x0A000000u) == 0xFF);

    /* A stray 0xAA must not leave the chip half unlocked, or a later single
     * byte would be taken as a command. */
    memory_write8(&mem, 0x0A005555u, 0xAA);
    memory_write8(&mem, 0x0A005555u, 0xF0);
    t_check("an interrupted sequence is abandoned",
            memory_read8(&mem, 0x0A000000u) == 0xFF);

    /* The second 64 KB half is a separate bank. */
    memory_write8(&mem, 0x0A005555u, 0xAA);
    memory_write8(&mem, 0x0A002AAAu, 0x55);
    memory_write8(&mem, 0x0A005555u, 0xAA);
    memory_write8(&mem, 0x0A005555u, 0xB0);
    memory_write8(&mem, 0x0A000000u, 0x01);
    t_check("the bank select command switches banks", mem.save.bank == 1);
    t_check("bank 1 is a different half of the chip",
            memory_read8(&mem, 0x0A000000u) == 0xFF);
    memory_write8(&mem, 0x0A005555u, 0xAA);
    memory_write8(&mem, 0x0A002AAAu, 0x55);
    memory_write8(&mem, 0x0A005555u, 0xAA);
    memory_write8(&mem, 0x0A005555u, 0xB0);
    memory_write8(&mem, 0x0A000000u, 0x00);
    t_check("bank select goes back to bank 0", mem.save.bank == 0);

    /* --- an unreadable header ---
     * Plenty of dumps carry a wrong backup byte, and nothing else lives in
     * this window, so falling back to the common 8 KB EEPROM gives a working
     * save instead of an inert one. */
    rom[0xB2] = 0x00;
    memory_load_rom_data(&mem, rom, sizeof(rom));
    t_check("an unknown header falls back to EEPROM",
            mem.save.kind == SAVE_EEPROM);
    t_check("the fallback is 8 KB", mem.save.size == 0x2000u);
    eeprom_write_8(&mem, 0x10, 0x77);
    t_check("the fallback chip actually stores", mem.save.data[0x10] == 0x77);
    eeprom_select(&mem, 0x10);
    t_check("the fallback chip reads back", eeprom_read_bits(&mem, 8) == 0x77);
    /* The window is claimed whichever chip the header names, so a stray byte
     * written to it has to be consumed rather than fault, and the chip has to
     * still work afterwards. */
    memory_write8(&mem, 0x0A000000u, 0x11);
    eeprom_select(&mem, 0x20);
    t_check("the fallback chip survives a stray byte write",
            eeprom_read_bits(&mem, 8) == 0xFF);

    memory_set_io(&mem, NULL);
    memory_free(&mem);
}

/* ---- 8bpp transparency ---------------------------------------------------- */

/* Only the 4bpp forms have a transparent index. Every one of the 256 entries
 * in an 8bpp tile or bitmap is a colour, index 0 included, so treating 0 as
 * a hole punches transparent pixels through backgrounds and sprites that
 * should be fully opaque.
 *
 * Two things make this awkward to test by colour. BG palette entry 0 doubles
 * as the backdrop colour, so an 8bpp background's index 0 always looks like
 * the backdrop. And every background shares one palette, so the layer
 * underneath has to be given a palette bank of its own. With that done, the
 * same tile is rendered twice, once 8bpp and once 4bpp, and the only
 * difference in the result is index 0. */
static void test_8bpp_opaque(void) {
    Memory mem;
    CPU    cpu;
    PPU    ppu;
    HW     hw;

    printf("=== 8bpp index 0 is opaque ===\n");

    memory_init(&mem);
    cpu_init(&cpu);
    ppu_init(&ppu, &mem);
    hw_init(&hw, &mem, &cpu, &ppu);
    memory_set_io(&mem, &hw);

    /* --- 8bpp text background over a 4bpp one ---
     * BG0 is the layer under test at priority 0, drawing a tile of all zeroes.
     * BG1 sits under it at priority 1, solid blue, so every pixel it covers
     * shows through wherever BG0 is transparent. */
    memory_write16(&mem, 0x05000000, 0x001F);   /* BG0 index 0 and backdrop: red */
    memory_write16(&mem, 0x05000002, 0x03E0);   /* BG0 index 1: green */
    memory_write16(&mem, 0x0500003E, 0x7C00);   /* BG1 index 15 in bank 1: blue */

    hw_io_write(&hw, 0x00, 0x0300, 16);         /* mode 0, BG0 and BG1 on */
    hw_io_write(&hw, 0x08, 0x0F80, 16);         /* BG0: CBB 0, SBB 15, 8bpp, prio 0 */
    hw_io_write(&hw, 0x0A, 0x0E05, 16);         /* BG1: CBB 1, SBB 14, 4bpp, prio 1 */
    memory_write16(&mem, 0x06007C00, 0x0000);   /* BG0 entry (0,0): tile 0 */
    memory_write16(&mem, 0x06007000, 0x1000);   /* BG1 entry (0,0): tile 0, palette 1 */
    for (int i = 0; i < 8; i++) {
        memory_write8(&mem, 0x06000000 + i, 0x00);    /* BG0 tile 0: all index 0 */
        memory_write8(&mem, 0x06004000 + i / 2, 0xFF); /* BG1 tile 0: all index 15 */
    }
    ppu_render_scanline(&ppu, 0);
    /* Every pixel is BG0 index 0. If that were treated as a hole, BG1's blue
     * would show through instead. */
    t_check("8bpp text background index 0 is drawn", ppu.frame[0] == 0xFF0000);
    t_check("8bpp text background index 0 is drawn across the line",
            ppu.frame[1] == 0xFF0000 && ppu.frame[159] == 0xFF0000);

    /* The same tile as 4bpp, where index 0 is a hole. Nothing else about the
     * setup changed, so the difference between these two renders is exactly
     * the behaviour under test. */
    hw_io_write(&hw, 0x08, 0x0F00, 16);          /* BG0 back to 4bpp */
    ppu_render_scanline(&ppu, 0);
    t_check("4bpp index 0 is still transparent", ppu.frame[0] == 0x0000FF);
    /* The last pixel of the same tile, to catch a partial draw. Outside it
     * the backdrop shows through both backgrounds, which is the backdrop
     * colour again and so says nothing either way. */
    t_check("4bpp index 0 is still transparent across the tile",
            ppu.frame[7] == 0x0000FF);

    /* A 4bpp pixel of index 1 is opaque on top of the same background. */
    memory_write8(&mem, 0x06000000, 0x10);            /* px0: 0, px1: 1 */
    ppu_render_scanline(&ppu, 0);
    t_check("4bpp index 0 is still transparent next to a drawn pixel",
            ppu.frame[0] == 0x0000FF);
    t_check("4bpp index 1 is still drawn", ppu.frame[1] == 0x00FF00);

    /* --- mode 4 indexed bitmap ---
     * An 8bpp page of 0xA000 bytes, every entry a colour. BG2 is the only
     * background mode 4 can use, so the backdrop is the layer underneath,
     * and index 1 is used to tell BG2 apart from it. */
    hw_io_write(&hw, 0x00, 0x0404, 16);          /* mode 4, BG2 on, page 0 */
    memory_write16(&mem, 0x05000000, 0x03E0);    /* index 0 and backdrop: green */
    memory_write16(&mem, 0x05000002, 0x001F);    /* index 1: red */
    memset(mem.vram, 0, 0xA000);
    memory_write8(&mem, 0x06000000, 0x01);       /* px0: index 1 */
    ppu_render_scanline(&ppu, 0);
    t_check("mode 4 index 1 is drawn", ppu.frame[0] == 0xFF0000);
    /* With BG2 off the very same pixels come back as the backdrop, which is
     * what makes the check above meaningful. */
    hw_io_write(&hw, 0x00, 0x0004, 16);
    ppu_render_scanline(&ppu, 0);
    t_check("mode 4 with BG2 off falls back to the backdrop",
            ppu.frame[0] == 0x00FF00);
    /* Index 0 of the bitmap is the backdrop colour, so the way to show it is
     * drawn is to turn BG2 on and see that nothing changes. */
    memory_write8(&mem, 0x06000000, 0x00);
    hw_io_write(&hw, 0x00, 0x0404, 16);
    ppu_render_scanline(&ppu, 0);
    t_check("mode 4 index 0 matches the backdrop colour",
            ppu.frame[0] == 0x00FF00);
    hw_io_write(&hw, 0x00, 0x0004, 16);
    ppu_render_scanline(&ppu, 0);
    t_check("mode 4 index 0 is the same with BG2 off",
            ppu.frame[0] == 0x00FF00);

    /* --- 8bpp object over the backdrop ---
     * An object tile of 64 bytes holds 256 colours, so a tile that is all
     * zeroes is a solid block, not an empty one. The object palette is
     * separate from the BG one, so index 0 is directly comparable against
     * the backdrop here. */
    hw_io_write(&hw, 0x00, 0x1000, 16);          /* mode 0, OBJ on */
    hw_io_write(&hw, 0x22, 0x003F, 16);          /* WINOUT: show everything */
    for (int i = 0; i < 128; i++) {
        memory_write16(&mem, 0x07000000 + 8 * i, 0x0200);   /* park off screen */
    }
    memory_write16(&mem, 0x05000000, 0x001F);    /* backdrop: red */
    memory_write16(&mem, 0x05000200, 0x03E0);    /* object index 0: green */
    for (int i = 0; i < 64; i++) {
        memory_write8(&mem, 0x06010000 + i, 0x00);
    }
    memory_write16(&mem, 0x07000000, OBJ8);      /* y = 8, 8bpp, prio 0 */
    memory_write16(&mem, 0x07000002, 0x0000);   /* 8x8 */
    memory_write16(&mem, 0x07000004, 0x0000);   /* tile 0 */
    ppu_render_scanline(&ppu, 0);
    t_check("8bpp object index 0 is drawn", ppu.frame[0] == 0x00FF00);
    t_check("the whole 8bpp tile is solid",
            ppu.frame[1] == 0x00FF00 && ppu.frame[7] == 0x00FF00);

    /* A 4bpp object of all zeroes is still empty, so the backdrop shows. */
    memory_write16(&mem, 0x07000000, 0x0008);   /* 4bpp, y = 8 */
    memory_write16(&mem, 0x07000004, 0x0000);
    ppu_render_scanline(&ppu, 0);
    t_check("4bpp object index 0 is still transparent",
            ppu.frame[0] == 0xFF0000);

    memory_set_io(&mem, NULL);
    memory_free(&mem);
}

int main(void) {
    Memory memory;
    CPU    cpu;

    memory_init(&memory);
    cpu_init(&cpu);

    printf("=== ARM instruction tests ===\n");

    {   /* MOV R0, R1 */
        cpu.reg[1] = 0x12345678;
        run_one(&cpu, &memory, 0x02000000, 0xE1A00001);
        t_check("MOV R0, R1", cpu.reg[0] == 0x12345678);
        t_check("PC advanced by 4", cpu.reg[15] == 0x02000004);
    }

    {   /* MOV R2, #0xFF */
        run_one(&cpu, &memory, 0x02000000, 0xE3A020FF);
        t_check("MOV R2, #0xFF", cpu.reg[2] == 0xFF);
    }

    {   /* ADD R0, R1, R2 */
        cpu.reg[1] = 10;
        cpu.reg[2] = 20;
        run_one(&cpu, &memory, 0x02000010, 0xE0810002);
        t_check("ADD R0, R1, R2 = 30", cpu.reg[0] == 30);
    }

    {   /* SUB R3, R4, #5 */
        cpu.reg[4] = 10;
        run_one(&cpu, &memory, 0x02000020, 0xE2443005);
        t_check("SUB R3, R4, #5 = 5", cpu.reg[3] == 5);
    }

    {   /* CMP R0, R1 sets Z */
        cpu.reg[0] = 7;
        cpu.reg[1] = 7;
        cpu.cpsr = MODE_SVC;
        run_one(&cpu, &memory, 0x02000030, 0xE1500001);
        t_check("CMP r0==r1 sets Z", (cpu.cpsr & FLAG_Z) != 0);
    }

    {   /* SUB with borrow sets C clear */
        cpu.reg[0] = 5;
        cpu.reg[1] = 10;
        cpu.cpsr = MODE_SVC;
        run_one(&cpu, &memory, 0x02000040, 0xE050C001); /* SUBS R12, R0, R1 */
        t_check("SUBS 5-10 clears C", (cpu.cpsr & FLAG_C) == 0);
        t_check("SUBS result -5 = 0xFFFFFFFB", cpu.reg[12] == 0xFFFFFFFB);
    }

    {   /* MUL R0, R1, R2 */
        cpu.reg[1] = 1000;
        cpu.reg[2] = 2000;
        run_one(&cpu, &memory, 0x02000050, 0xE0000291);
        t_check("MUL R0 = 2000000", cpu.reg[0] == 2000000);
    }

    {   /* AND, ORR, MVN, EOR */
        cpu.reg[1] = 0x00FF00FF;
        cpu.reg[2] = 0x0F0F0F0F;
        run_one(&cpu, &memory, 0x02000060, 0xE0010002);
        t_check("AND R0 = 0x000F000F", cpu.reg[0] == 0x000F000F);
        run_one(&cpu, &memory, 0x02000060, 0xE1810002);
        t_check("ORR R0 = 0x0FFF0FFF", cpu.reg[0] == 0x0FFF0FFF);
        run_one(&cpu, &memory, 0x02000060, 0xE1E00000);
        t_check("MVN R0 = ~R0", cpu.reg[0] == ~0x0FFF0FFFu);
    }

    {   /* LDR R0, [R1, #2]: the 12 bit field is a byte offset, so this is +2 */
        memory_write32(&memory, 0x020000FA, 0xDEADBEEF);
        cpu.reg[1] = 0x020000F8;
        run_one(&cpu, &memory, 0x02000070, 0xE5910002);
        t_check("LDR R0 = 0xDEADBEEF", cpu.reg[0] == 0xDEADBEEF);
    }

    {   /* LDR R0, [R1, #8] is +8 bytes */
        memory_write32(&memory, 0x02000100, 0xFEEDFACE);
        cpu.reg[1] = 0x020000F8;
        run_one(&cpu, &memory, 0x02000070, 0xE5910008);
        t_check("LDR word offset in bytes", cpu.reg[0] == 0xFEEDFACE);
    }

    {   /* STR R0, [R1] */
        cpu.reg[1] = 0x02000200;
        cpu.reg[0] = 0xBADC0DE;
        run_one(&cpu, &memory, 0x02000080, 0xE5810000); /* STR r0, [r1] */
        t_check("STR stored 0xBADC0DE", memory_read32(&memory, 0x02000200) == 0xBADC0DE);
    }

    {   /* LDRH R0, [R1] (halfword) */
        memory_write16(&memory, 0x02000300, 0x1234);
        cpu.reg[1] = 0x02000300;
        run_one(&cpu, &memory, 0x02000090, 0xE1D100B0);
        t_check("LDRH R0 = 0x1234", cpu.reg[0] == 0x1234);
    }

    {   /* The halfword immediate is the flat 8 bit imm4H:imm4L field, so an
         * offset of 0x10 is imm4H=1, imm4L=0 rather than something the low
         * nibble alone could express. Decoding only imm4L silently turned
         * every offset of 16 or more into a much smaller one. */
        memory_write16(&memory, 0x02001000, 0x0000);
        memory_write16(&memory, 0x02001010, 0x1111);   /* +0x10 */
        memory_write16(&memory, 0x02001030, 0x3333);   /* +0x30 */
        memory_write16(&memory, 0x020010A0, 0xAAAA);   /* +0xA0 */
        memory_write16(&memory, 0x02000FFC, 0x4444);   /* -0x04 */
        cpu.reg[1] = 0x02001000;
        run_one(&cpu, &memory, 0x02000090, 0xE1D101B0); /* LDRH r0,[r1,#0x10] */
        t_check("LDRH +0x10 reads imm4H=1", cpu.reg[0] == 0x1111);
        run_one(&cpu, &memory, 0x02000090, 0xE1D103B0); /* LDRH r0,[r1,#0x30] */
        t_check("LDRH +0x30 reads imm4H=3", cpu.reg[0] == 0x3333);
        run_one(&cpu, &memory, 0x02000090, 0xE1D10AB0); /* LDRH r0,[r1,#0xA0] */
        t_check("LDRH +0xA0 reads imm4H=A", cpu.reg[0] == 0xAAAA);
        run_one(&cpu, &memory, 0x02000090, 0xE15100B4); /* LDRH r0,[r1,#-4] */
        t_check("LDRH -0x04 subtracts the offset", cpu.reg[0] == 0x4444);

        cpu.reg[0] = 0x5A5A;
        run_one(&cpu, &memory, 0x02000090, 0xE1C103B0); /* STRH r0,[r1,#0x30] */
        t_check("STRH +0x30 stores at the right place",
                memory_read16(&memory, 0x02001030) == 0x5A5A);
        run_one(&cpu, &memory, 0x02000090, 0xE1C10AB0); /* STRH r0,[r1,#0xA0] */
        t_check("STRH +0xA0 stores at the right place",
                memory_read16(&memory, 0x020010A0) == 0x5A5A);
        run_one(&cpu, &memory, 0x02000090, 0xE14100B4); /* STRH r0,[r1,#-4] */
        t_check("STRH -0x04 stores at the right place",
                memory_read16(&memory, 0x02000FFC) == 0x5A5A);

        /* LDRSB and LDRSH read the same flat field and are both sign
         * extending. */
        memory_write8(&memory, 0x02001025, 0xAD);
        memory_write8(&memory, 0x02001026, 0xFF);
        run_one(&cpu, &memory, 0x02000090, 0xE1D102D5); /* LDRSB r0,[r1,#0x25] */
        t_check("LDRSB +0x25 sign extends", cpu.reg[0] == 0xFFFFFFADu);
        run_one(&cpu, &memory, 0x02000090, 0xE1D102F5); /* LDRSH r0,[r1,#0x25] */
        t_check("LDRSH +0x25 reads the misaligned halfword and extends",
                cpu.reg[0] == 0xFFFFFFADu);

        /* The register offset form is unaffected and still adds Rm. */
        cpu.reg[4] = 0x10;
        run_one(&cpu, &memory, 0x02000090, 0xE19100B4); /* LDRH r0,[r1,r4] */
        t_check("LDRH register offset still works", cpu.reg[0] == 0x1111);
    }

    {   /* SWP and SWPB sit in the same instruction class as the halfword
         * transfers, and are reached through a different bit pattern rather
         * than being dead code. */
        memory_write32(&memory, 0x02000640, 0xA1B2C3D4u);
        cpu.reg[0] = 0x11111111u;
        cpu.reg[2] = 0x02000640;
        cpu.reg[5] = 0x0000FFFFu;
        run_one(&cpu, &memory, 0x020000A0, 0xE1020095); /* SWP r0,r5,[r2] */
        t_check("SWP loads the old word", cpu.reg[0] == 0xA1B2C3D4u);
        t_check("SWP stores the new word",
                memory_read32(&memory, 0x02000640) == 0x0000FFFFu);
        memory_write32(&memory, 0x02000640, 0xA1B2C3D4u);
        run_one(&cpu, &memory, 0x020000A0, 0xE1420095); /* SWPB r0,r5,[r2] */
        t_check("SWPB stores only the low byte",
                memory_read32(&memory, 0x02000640) == 0xA1B2C3FFu);
    }

    {   /* LDRB R0, [R1] and LDRSB sign extend */
        memory_write8(&memory, 0x02000400, 0xFA);
        cpu.reg[1] = 0x02000400;
        run_one(&cpu, &memory, 0x020000A0, 0xE5D10000);
        t_check("LDRB R0 = 0xFA", cpu.reg[0] == 0xFA);
        run_one(&cpu, &memory, 0x020000A0, 0xE1D100D0);
        t_check("LDRSB R0 = 0xFFFFFFFA", cpu.reg[0] == 0xFFFFFFFA);
    }

    {   /* STMIA R0!, {R1,R2} ; LDMIA R3!, {R1,R2} */
        cpu.reg[0] = 0x02000500;
        cpu.reg[1] = 0x11;
        cpu.reg[2] = 0x22;
        run_one(&cpu, &memory, 0x020000B0, 0xE8A00006); /* STMIA r0!, {r1,r2} */
        t_check("STMIA wrote R1", memory_read32(&memory, 0x02000500) == 0x11);
        t_check("STMIA wrote R2", memory_read32(&memory, 0x02000504) == 0x22);
        t_check("STMIA wrote back R0", cpu.reg[0] == 0x02000508);
        cpu.reg[3] = 0x02000500;
        cpu.reg[1] = cpu.reg[2] = 0;
        run_one(&cpu, &memory, 0x020000C0, 0xE8930006);
        t_check("LDMIA restored R1", cpu.reg[1] == 0x11);
        t_check("LDMIA restored R2", cpu.reg[2] == 0x22);
    }

    {   /* B +4 (skip one instruction) */
        uint32_t target = 0x02000100;
        cpu.reg[15] = 0x02000100 + 8; /* simulate that next executes at target */
        memory_write32(&memory, 0x02000100, 0xEA000010);
        run_one(&cpu, &memory, 0x02000100, 0xEA000010);
        /* offset 0x10 -> skips 4 instructions, target = 0x02000148 */
        t_check("B target", cpu.reg[15] == 0x02000148);
        (void)target;
    }

    {   /* BX R1 switches mode */
        cpu.reg[1] = 0x02000120 | 1;
        run_one(&cpu, &memory, 0x02000110, 0xE12FFF11);
        t_check("BX set T flag", (cpu.cpsr & FLAG_T) != 0);
        t_check("BX target = 0x02000120", cpu.reg[15] == 0x02000120);
    }

    {   /* A zero-offset B lands two instructions on: ARM reads R15 as addr+8 */
        cpu.cpsr = MODE_SVC;
        run_one(&cpu, &memory, 0x02000200, 0xEA000000);
        t_check("ARM B . targets addr+8", cpu.reg[15] == 0x02000208);
    }

    {   /* BX PC reads R15 as addr+4, so it targets addr+4. The target is even,
         * so BX also leaves Thumb state: only BX interworks. */
        cpu.cpsr = MODE_SVC;
        run_thumb(&cpu, &memory, 0x02000200, 0x4778);
        t_check("Thumb BX PC target = addr+4", cpu.reg[15] == 0x02000204);
        t_check("Thumb BX PC to even addr leaves Thumb", (cpu.cpsr & FLAG_T) == 0);
    }

    {   /* ARMv4T MOV PC,Rm branches but does not take T from bit 0 of Rm,
         * unlike BX. Bit 0 is simply ignored. */
        cpu.cpsr = MODE_SVC;
        cpu.reg[0] = 0x02000501;
        run_one(&cpu, &memory, 0x02000200, 0xE1A0F000);
        t_check("ARM MOV PC,Rm target", cpu.reg[15] == 0x02000500);
        t_check("ARM MOV PC,Rm stays ARM", (cpu.cpsr & FLAG_T) == 0);
    }

    {   /* BLX Rm: LR keeps the return address, T comes from bit 0 of Rm */
        cpu.cpsr = MODE_SVC;
        cpu.reg[3] = 0x02000701;
        run_one(&cpu, &memory, 0x02000200, 0xE12FFF33);
        t_check("ARM BLX LR = addr+4", cpu_read_reg(&cpu, 14) == 0x02000204);
        t_check("ARM BLX target", cpu.reg[15] == 0x02000700);
        t_check("ARM BLX set T", (cpu.cpsr & FLAG_T) != 0);

        /* An even Rm keeps ARM state. */
        cpu.cpsr = MODE_SVC;
        cpu.reg[3] = 0x02000700;
        run_one(&cpu, &memory, 0x02000200, 0xE12FFF33);
        t_check("ARM BLX even target", cpu.reg[15] == 0x02000700);
        t_check("ARM BLX even keeps ARM", (cpu.cpsr & FLAG_T) == 0);
    }

    {   /* Thumb BLX R3 (0x4798): LR is addr+2|1, T comes from bit 0 of Rm */
        cpu.cpsr = MODE_SVC;
        cpu.reg[3] = 0x02000701;
        run_thumb(&cpu, &memory, 0x02000200, 0x4798);
        t_check("Thumb BLX LR = addr+2 |1", cpu_read_reg(&cpu, 14) == 0x02000203);
        t_check("Thumb BLX target", cpu.reg[15] == 0x02000700);
        t_check("Thumb BLX set T", (cpu.cpsr & FLAG_T) != 0);

        cpu.cpsr = MODE_SVC;
        cpu.reg[3] = 0x02000700;
        run_thumb(&cpu, &memory, 0x02000200, 0x4798);
        t_check("Thumb BLX even clears T", (cpu.cpsr & FLAG_T) == 0);
    }

    {   /* SWI switches to SVC and vectors to 0x08 */
        cpu.cpsr = MODE_USR;
        run_one(&cpu, &memory, 0x02000130, 0xEF000000);
        t_check("SWI mode == SVC", (cpu.cpsr & 0x1F) == MODE_SVC);
        t_check("SWI I flag set", (cpu.cpsr & FLAG_I) != 0);
        t_check("SWI PC == 0x08", cpu.reg[15] == 0x08);
    }

    {   /* Conditional: NEQ add skipped when Z set */
        cpu.reg[0] = 5;
        cpu.reg[1] = 1;
        cpu.cpsr = MODE_SVC | FLAG_Z;
        run_one(&cpu, &memory, 0x02000140, 0x10800001); /* ADDNE R0, R0, R1 */
        t_check("ADDNE skipped when Z", cpu.reg[0] == 5);

        cpu.cpsr = MODE_SVC;
        run_one(&cpu, &memory, 0x02000140, 0x10800001);
        t_check("ADDNE runs when !Z", cpu.reg[0] == 6);
    }

    printf("\n=== Thumb instruction tests ===\n");

    {   /* MOVS R0, #5 */
        run_thumb(&cpu, &memory, 0x02000200, 0x2005);
        t_check("MOVS R0 = 5", cpu.reg[0] == 5);
        t_check("Thumb PC advanced by 2", cpu.reg[15] == 0x02000202);
    }

    {   /* ADDS R0, R1, R2 and SUBS R3, R4, #imm3 */
        cpu.reg[1] = 10;
        cpu.reg[2] = 20;
        run_thumb(&cpu, &memory, 0x02000200, 0x1888);
        t_check("ADDS R0 = 30", cpu.reg[0] == 30);
        cpu.reg[4] = 32;
        run_thumb(&cpu, &memory, 0x02000200, 0x1F23); /* SUBS R3, R4, #4 */
        t_check("SUBS R3 = 28", cpu.reg[3] == 28);
    }

    {   /* ADD R6, R6, #imm8 (format 3) */
        cpu.reg[6] = 100;
        run_thumb(&cpu, &memory, 0x02000200, 0x3006 | (6 << 8));
        t_check("ADD R6, #6 = 106", cpu.reg[6] == 106);
        cpu.reg[5] = 100;
        run_thumb(&cpu, &memory, 0x02000200, 0x3805 | (5 << 8));
        t_check("SUB R5, #5 = 95", cpu.reg[5] == 95);
    }

    {   /* MULS R0, R1, and LSLS R0, R0, #4 */
        cpu.reg[0] = 3;
        cpu.reg[1] = 7;
        run_thumb(&cpu, &memory, 0x02000200, 0x4348); /* MULS R0,R1,R0 */
        t_check("MULS R0 = 21", cpu.reg[0] == 21);
        run_thumb(&cpu, &memory, 0x02000200, 0x0100); /* LSLS R0, R0, #4 */
        t_check("LSLS R0 <<= 4 = 336", cpu.reg[0] == 336);
    }

    {   /* CMP R0, R1 (ALU) sets Z */
        cpu.cpsr = MODE_SVC | FLAG_T;
        cpu.reg[0] = 9;
        cpu.reg[1] = 9;
        run_thumb(&cpu, &memory, 0x02000200, 0x4288);
        t_check("Thumb CMP sets Z", (cpu.cpsr & FLAG_Z) != 0);
    }

    {   /* High register MOV R0, R1 */
        cpu.reg[1] = 0xDEAD0000;
        run_thumb(&cpu, &memory, 0x02000200, 0x4608);
        t_check("MOV R0, R1 = 0xDEAD0000", cpu.reg[0] == 0xDEAD0000);
        cpu.reg[8] = 0xC0FFEE;
        cpu.cpsr = MODE_USR | FLAG_T;
        run_thumb(&cpu, &memory, 0x02000200, 0x4641); /* MOV R1, R8 */
        t_check("MOV R1, R8 = 0xC0FFEE", cpu.reg[1] == 0xC0FFEE);
    }

    {   /* LDR/STR word immediate */
        memory_write32(&memory, 0x02000300, 0x11223344);
        cpu.reg[1] = 0x02000300;
        run_thumb(&cpu, &memory, 0x02000200, 0x6808); /* LDR R0,[R1,#0] */
        t_check("Thumb LDR R0 = 0x11223344", cpu.reg[0] == 0x11223344);
        cpu.reg[0] = 0xAA;
        run_thumb(&cpu, &memory, 0x02000200, 0x6008); /* STR R0,[R1,#0] */
        t_check("Thumb STR stored", memory_read32(&memory, 0x02000300) == 0xAA);
    }

    {   /* LDRB / LDRH byte and halfword immediate */
        memory_write8(&memory, 0x02000310, 0xFF);
        memory_write16(&memory, 0x02000310, 0x1234);
        cpu.reg[1] = 0x02000310;
        run_thumb(&cpu, &memory, 0x02000200, 0x7808); /* LDRB R0,[R1,#0] */
        t_check("Thumb LDRB R0 = 0x34", cpu.reg[0] == 0x34);
        memory_write8(&memory, 0x02000310, 0xFF);
        run_thumb(&cpu, &memory, 0x02000200, 0x8808); /* LDRH R0,[R1,#0] */
        t_check("Thumb LDRH R0 = 0x12FF", cpu.reg[0] == 0x12FF);
    }

    {   /* SP-relative LDR and ADR to SP */
        memory_write32(&memory, 0x03007F00, 0x02030405);
        cpu.cpsr = MODE_USR | FLAG_T;
        cpu_write_reg(&cpu, 13, 0x03007F00);
        run_thumb(&cpu, &memory, 0x02000200, 0x9800); /* LDR R0,[SP,#0] */
        t_check("Thumb LDR R0,[SP] = 0x02030405", cpu.reg[0] == 0x02030405);
        run_thumb(&cpu, &memory, 0x02000200, 0xA801); /* ADD R0, SP, #4 */
        t_check("Thumb ADR SP+4", cpu.reg[0] == 0x03007F04);
    }

    {   /* PUSH / POP */
        cpu.cpsr = MODE_USR | FLAG_T;
        cpu_write_reg(&cpu, 13, 0x03007E00);
        cpu.reg[0] = 0x5555;
        cpu_write_reg(&cpu, 14, 0x02000404);
        run_thumb(&cpu, &memory, 0x02000200, 0xB501); /* PUSH {R0, LR} */
        t_check("PUSH SP moved", cpu_read_reg(&cpu, 13) == 0x03007DF8);
        t_check("PUSH wrote LR", memory_read32(&memory, 0x03007DFC) == 0x02000404);
        cpu.reg[0] = 0;
        cpu_write_reg(&cpu, 14, 0);
        run_thumb(&cpu, &memory, 0x02000200, 0xBD01); /* POP {R0, PC} */
        t_check("POP R0 = 0x5555", cpu.reg[0] == 0x5555);
        t_check("POP PC = 0x02000404", cpu.reg[15] == 0x02000404);
    }

    {   /* STMIA / LDMIA */
        cpu.reg[1] = 0x02000500;
        cpu.reg[2] = 0x11;
        cpu.reg[3] = 0x22;
        run_thumb(&cpu, &memory, 0x02000200, 0xC10C); /* STMIA R1!, {R2,R3} */
        t_check("STMIA wrote R2", memory_read32(&memory, 0x02000500) == 0x11);
        t_check("STMIA wrote back", cpu.reg[1] == 0x02000508);
        cpu.reg[2] = cpu.reg[3] = 0;
        cpu.reg[1] = 0x02000500;
        run_thumb(&cpu, &memory, 0x02000200, 0xC90C); /* LDMIA R1!, {R2,R3} */
        t_check("LDMIA R2 = 0x11", cpu.reg[2] == 0x11);
        t_check("LDMIA R3 = 0x22", cpu.reg[3] == 0x22);
    }

    {   /* Conditional branch: BEQ taken / BNE skipped */
        cpu.cpsr = MODE_SVC | FLAG_T | FLAG_Z;
        run_thumb(&cpu, &memory, 0x02000200, 0xD001); /* BEQ +2 */
        t_check("BEQ taken", cpu.reg[15] == 0x02000206);
        cpu.cpsr = MODE_SVC | FLAG_T;
        run_thumb(&cpu, &memory, 0x02000200, 0xD001); /* BEQ +2, Z clear */
        t_check("BEQ skipped", cpu.reg[15] == 0x02000202);
    }

    {   /* Unconditional B +2 */
        run_thumb(&cpu, &memory, 0x02000210, 0xE001);
        t_check("B +2 target", cpu.reg[15] == 0x02000216);
    }

    {   /* BL pair: 11110 0 0000000000 : 11 1 1 1 00000000011 -> +6 */
        memory_write16(&memory, 0x02000240, 0xF000);
        memory_write16(&memory, 0x02000242, 0xF803);
        cpu.cpsr = MODE_USR | FLAG_T;
        cpu.reg[15] = 0x02000240;
        cpu_step(&cpu, &memory);
        t_check("BL target", cpu.reg[15] == 0x0200024A);
        t_check("BL LR = addr+4 |1", cpu_read_reg(&cpu, 14) == 0x02000245);
        t_check("BL stays in Thumb", (cpu.cpsr & FLAG_T) != 0);
    }

    {   /* BL with a negative offset: S = 1, imm10 = 0x3FF, J1 = J2 = 0 */
        memory_write16(&memory, 0x02000270, 0xF7FF);
        memory_write16(&memory, 0x02000272, 0xF800);
        cpu.cpsr = MODE_USR | FLAG_T;
        cpu.reg[15] = 0x02000270;
        cpu_step(&cpu, &memory);
        t_check("BL back target", cpu.reg[15] == 0x01FFF274);
    }

    {   /* BL pair from the devkitARM crt0: 0xF000 0xF82B -> +0x56 */
        memory_write16(&memory, 0x0200012C, 0xF000);
        memory_write16(&memory, 0x0200012E, 0xF82B);
        cpu.cpsr = MODE_USR | FLAG_T;
        cpu.reg[15] = 0x0200012C;
        cpu_step(&cpu, &memory);
        t_check("BL crt0 target", cpu.reg[15] == 0x02000186);
    }

    {   /* BX R3 to an ARM-mode address clears T */
        cpu.reg[3] = 0x02000800;
        run_thumb(&cpu, &memory, 0x02000250, 0x4718);
        t_check("Thumb BX set PC", cpu.reg[15] == 0x02000800);
        t_check("Thumb BX cleared T", (cpu.cpsr & FLAG_T) == 0);
    }

    {   /* SWI from Thumb enters SVC and vectors to 0x08 */
        cpu.cpsr = MODE_USR | FLAG_T;
        run_thumb(&cpu, &memory, 0x02000260, 0xDF00);
        t_check("Thumb SWI mode SVC", (cpu.cpsr & 0x1F) == MODE_SVC);
        t_check("Thumb SWI PC = 0x08", cpu.reg[15] == 0x08);
    }

    printf("\n=== Hardware tests ===\n");
    {
        PPU ppu;
        HW  hw;
        ppu_init(&ppu, &memory);
        hw_init(&hw, &memory, &cpu, &ppu);
        memory_set_io(&memory, &hw);

        /* --- timers --- */
        hw_io_write(&hw, 0x100, 0x1000, 16);          /* TM0 reload 0x1000 */
        hw_io_write(&hw, 0x102, 0x0080, 16);          /* enable, /1 */
        hw_tick(&hw, 0x700);
        t_check("Timer0 count = reload + cycles", hw_io_read(&hw, 0x100, 16) == 0x1700);

        hw_io_write(&hw, 0x100, 0x0800, 16);          /* reload while running */
        hw_tick(&hw, 0x100);
        t_check("Timer0 reload applied", hw_io_read(&hw, 0x100, 16) == 0x0800 + 0x100);

        hw_io_write(&hw, 0x102, 0x0000, 16);          /* disable */
        hw_tick(&hw, 0xFFFF);
        t_check("Timer0 disabled freezes count", hw_io_read(&hw, 0x100, 16) == 0x0800);

        hw_io_write(&hw, 0x104, 0xFFFF, 16);          /* TM1 reload 0xFFFF */
        hw_io_write(&hw, 0x106, 0x00C1, 16);          /* enable, /64 */
        hw_tick(&hw, 64);                             /* one tick -> wrap to 0 */
        t_check("Timer1 0xFFFF wraps to 0 in one tick", hw_timer_read(&hw, 1) == 0);
        t_check("Timer1 overflow raises IRQ", (hw.if_ & HW_IRQ_TIMER1) != 0);

        hw_io_write(&hw, 0x202, HW_IRQ_TIMER1, 16);   /* write-1-clear */
        t_check("IF write-1-clear", (hw.if_ & HW_IRQ_TIMER1) == 0);
        hw_tick(&hw, 64u * 0x10000u);   /* enough ticks to wrap again */
        t_check("Timer1 IRQ re-raised on next wrap", (hw.if_ & HW_IRQ_TIMER1) != 0);

        /* --- DMA --- */
        for (int i = 0; i < 4; i++) {
            memory_write16(&memory, 0x03000000 + 2 * i, (uint16_t)(0x1000 + i));
        }
        hw_io_write(&hw, 0xB0, 0x0000, 16);           /* DMA0 src = 0x03000000 */
        hw_io_write(&hw, 0xB2, 0x0300, 16);
        hw_io_write(&hw, 0xB4, 0x0000, 16);           /* DMA0 dst = 0x06000000 */
        hw_io_write(&hw, 0xB6, 0x0600, 16);
        hw_io_write(&hw, 0xB8, 3, 16);                /* 3 halfwords */
        hw.if_ = 0;
        hw_io_write(&hw, 0xBA, 0xC000, 16);           /* enable + IRQ, start=NOW */
        t_check("DMA0 copied CNT_L halfwords",
                memory_read16(&memory, 0x06000000) == 0x1000 &&
                memory_read16(&memory, 0x06000004) == 0x1002);
        t_check("DMA0 stopped after CNT_L units",
                memory_read16(&memory, 0x06000006) == 0);
        t_check("DMA0 raises DMA IRQ on end", (hw.if_ & HW_IRQ_DMA0) != 0);
        t_check("DMA0 one-shot clears enable", (hw.dma[0].cnt & 0x8000) == 0);

        hw_io_write(&hw, 0xBA, 0x8200, 16);           /* repeat + enable */
        t_check("DMA0 repeat stays enabled", (hw.dma[0].cnt & 0x8000) != 0);

        /* --- IRQ gating --- */
        hw_io_write(&hw, 0x200, HW_IRQ_TIMER0, 16);
        hw_io_write(&hw, 0x202, HW_IRQ_TIMER0, 16);   /* clear */
        hw_raise_irq(&hw, HW_IRQ_TIMER0);
        hw_io_write(&hw, 0x208, 0, 16);
        t_check("IRQ masked when IME=0", hw_irq_pending(&hw) == 0);
        hw_io_write(&hw, 0x208, 1, 16);
        t_check("IRQ pending when IME=1", hw_irq_pending(&hw) == 1);
        hw_io_write(&hw, 0x200, 0, 16);
        t_check("IRQ gated by IE", hw_irq_pending(&hw) == 0);

        /* --- keypad --- */
        hw_set_keys(&hw, HW_KEY_A, 0);                /* press A */
        t_check("KEYINPUT active low for A", hw_io_read(&hw, 0x130, 16) == 0xFFFE);
        hw_set_keys(&hw, HW_KEY_A, HW_KEY_A);         /* release A */
        t_check("KEYINPUT released", hw_io_read(&hw, 0x130, 16) == 0xFFFF);

        hw.if_ = 0;
        hw_io_write(&hw, 0x132, 0x4001, 16);          /* keycnt: enable + select A */
        hw_set_keys(&hw, HW_KEY_A, 0);
        t_check("KEYCNT IRQ on A press", (hw.if_ & HW_IRQ_KEYPAD) != 0);

        /* --- MMIO routing through memory --- */
        memory_write16(&memory, 0x04000200, 0x0005);
        t_check("memory routes MMIO write to HW", memory_read16(&memory, 0x04000200) == 0x0005);
        t_check("plain mmio array untouched", memory.mmio[0x200] == 0 && memory.mmio[0x201] == 0);

        memory_set_io(&memory, NULL);
    }

    printf("\n=== PPU tests ===\n");
    {
        PPU ppu;
        HW  hw;
        ppu_init(&ppu, &memory);
        hw_init(&hw, &memory, &cpu, &ppu);
        memory_set_io(&memory, &hw);

        /* --- mode 3 (bitmap) --- */
        hw_io_write(&hw, 0x00, 0x0403, 16);           /* mode 3, BG2 on */
        memory_write16(&memory, 0x06000000, 0x001F);  /* red   */
        memory_write16(&memory, 0x06000002, 0x03E0);  /* green */
        memory_write16(&memory, 0x06000004, 0x7FFF);  /* white */
        ppu_render_scanline(&ppu, 0);
        t_check("Mode3 pixel0 red",   ppu.frame[0] == 0xFF0000);
        t_check("Mode3 pixel1 green", ppu.frame[1] == 0x00FF00);
        t_check("Mode3 pixel2 white", ppu.frame[2] == 0xFFFFFF);

        /* --- mode 0, 4bpp text BG --- */
        hw_io_write(&hw, 0x00, 0x0100, 16);           /* mode 0, BG0 on */
        hw_io_write(&hw, 0x08, 0x1F00, 16);           /* BG0: CBB 0, SBB 31 */
        memory_write16(&memory, 0x0600F800, 0x0000);  /* entry (0,0): tile 0 */
        memory_write16(&memory, 0x0600F802, 0x0001);  /* entry (1,0): tile 1 (empty) */
        memory_write8(&memory, 0x06000000, 0x32);     /* tile 0 row 0: px0=2 px1=3 */
        memory_write16(&memory, 0x05000004, 0x001F);  /* palette[2] red */
        memory_write16(&memory, 0x05000006, 0x03E0);  /* palette[3] green */
        ppu_render_scanline(&ppu, 0);
        t_check("Text4bpp pixel0 red",   ppu.frame[0] == 0xFF0000);
        t_check("Text4bpp pixel1 green", ppu.frame[1] == 0x00FF00);
        t_check("Text4bpp pixel8 backdrop", ppu.frame[8] == 0x000000);

        /* --- VCOUNT match + IRQ flags --- */
        hw_io_write(&hw, 0x04, 0x6520, 16);           /* VCT=0x65, VCOUNT IRQ enable */
        ppu_set_vcount(&ppu, 0x65);
        t_check("VCOUNT register set", ppu.vcount == 0x65);
        t_check("VCOUNT match sets bit2", (ppu.dispstat & 0x04) != 0);
        t_check("VCOUNT IRQ flags", (ppu_irq_flags(&ppu) & HW_IRQ_VCOUNT) != 0);
        ppu_set_vcount(&ppu, 0x66);
        t_check("VCOUNT mismatch clears bit2", (ppu.dispstat & 0x04) == 0);

        /* --- objects ---
         * 8bpp object tiles (attribute 0 bit 13) hold a unique palette index
         * per (tile, row), so every rendered pixel says which tile and which
         * row of it it came from. That is what separates 1D mapping (a tile
         * strip) from 2D mapping (a 32 tile wide matrix). */
        hw_io_write(&hw, 0x00, 0x1000, 16);           /* mode 0, OBJ on, 2D mapping */
        hw_io_write(&hw, 0x22, 0x003F, 16);           /* WINOUT: show every layer */
        for (int i = 0; i < 128; i++) {
            memory_write16(&memory, 0x07000000 + 8 * i, 0x0200);    /* disable all */
        }
        for (int v = 0; v < 256; v++) {
            memory_write16(&memory, 0x05000200 + 2 * v,
                           (uint16_t)((v * 0x21) & 0x7FFF));
        }
        obj_fill(&memory, 40, 0);
        memory_write16(&memory, 0x07000000, OBJ8);             /* y = 8, 8bpp */
        memory_write16(&memory, 0x07000002, (uint16_t)(1 << 14));  /* 16x16 */
        memory_write16(&memory, 0x07000004, 0x0001);              /* tile 1 */
        ppu_render_scanline(&ppu, 0);
        t_check("Obj2D row0 samples tile 1 row 0",
                ppu.frame[0] == objpx(objidx(1, 0, 0, 0)));
        /* The right half of a 16 wide object comes from the next tile, not
         * from a repeat of the first one. */
        t_check("Obj2D column 1 advances to the next tile",
                ppu.frame[8] == objpx(objidx(2, 0, 0, 0)) &&
                ppu.frame[15] == objpx(objidx(2, 0, 7, 0)));
        ppu_render_scanline(&ppu, 1);
        t_check("Obj2D row1 samples tile 1 row 1",
                ppu.frame[240] == objpx(objidx(1, 1, 0, 0)));
        ppu_render_scanline(&ppu, 8);
        t_check("Obj2D tile row 1 starts 32 tiles on",
                ppu.frame[8 * 240] == objpx(objidx(33, 0, 0, 0)));
        ppu_render_scanline(&ppu, 16);
        t_check("Obj2D below a 16x16 sprite is backdrop", ppu.frame[16 * 240] == 0);
        memory_write16(&memory, 0x07000004, 0x0021);   /* page 1, offset 1 */
        ppu_render_scanline(&ppu, 0);
        t_check("Obj2D tile number is an offset in its page",
                ppu.frame[0] == objpx(objidx(33, 0, 0, 0)));

        /* DISPCNT bit 6 is set for one dimensional mapping, so clearing it
         * is what selects the two dimensional matrix. A 16x16 object at tile
         * 0 puts its second tile row at tile 32 in 2D and at tile 2 in 1D. */
        hw_io_write(&hw, 0x00, 0x1000, 16);
        memory_write16(&memory, 0x07000000, OBJ8);
        memory_write16(&memory, 0x07000002, (uint16_t)(1 << 14));  /* 16x16 */
        memory_write16(&memory, 0x07000004, 0x0000);
        ppu_render_scanline(&ppu, 8);
        t_check("Obj bit 6 clear is two dimensional mapping",
                ppu.frame[8 * 240 + 8] == objpx(objidx(33, 0, 0, 0)));
        hw_io_write(&hw, 0x00, 0x1040, 16);
        ppu_render_scanline(&ppu, 8);
        t_check("Obj bit 6 set is one dimensional mapping",
                ppu.frame[8 * 240 + 8] == objpx(objidx(3, 0, 0, 0)));

        /* The Y coordinate in OAM is the screen Y plus 8, so Y 8 puts the
         * top of the object on scanline 0. */
        memory_write16(&memory, 0x07000000, OBJ8);          /* y = 8 */
        memory_write16(&memory, 0x07000002, 0x0000);
        memory_write16(&memory, 0x07000004, 0x0000);
        ppu_render_scanline(&ppu, 0);
        t_check("Obj Y 8 lands on scanline 0", ppu.frame[0] == objpx(objidx(0, 0, 0, 0)));
        memory_write16(&memory, 0x07000000, OBJ8 + 1);      /* y = 9 */
        ppu_render_scanline(&ppu, 0);
        t_check("Obj Y 9 is one line lower", ppu.frame[0] == 0);
        ppu_render_scanline(&ppu, 1);
        t_check("Obj Y 9 lands on scanline 1", ppu.frame[240] == objpx(objidx(0, 0, 0, 0)));

        /* Leave the 8x8 object on screen Y 0, tile 0, for the 1D block. */
        memory_write16(&memory, 0x07000000, OBJ8);
        memory_write16(&memory, 0x07000002, 0x0000);
        memory_write16(&memory, 0x07000004, 0x0000);

        /* 1D mapping is a plain tile strip, so the next tile row starts the
         * shape width in tiles further along rather than 32. */
        hw_io_write(&hw, 0x00, 0x1040, 16);                       /* 1D mapping */
        memory_write16(&memory, 0x07000004, 0x0000);
        memory_write16(&memory, 0x07000002, 0x0000);              /* 8x8 square */
        ppu_render_scanline(&ppu, 0);
        t_check("Obj1D 8x8 samples tile 0 row 0",
                ppu.frame[0] == objpx(objidx(0, 0, 0, 0)));
        memory_write16(&memory, 0x07000002, (uint16_t)(1 << 14));  /* 16x8 */
        ppu_render_scanline(&ppu, 8);
        t_check("Obj1D 16x8 tile row 1 skips 2 tiles",
                ppu.frame[8 * 240] == objpx(objidx(2, 0, 0, 0)));
        t_check("Obj1D 16x8 column 1 advances to the next tile",
                ppu.frame[8 * 240 + 8] == objpx(objidx(3, 0, 0, 0)));
        memory_write16(&memory, 0x07000002, (uint16_t)(2 << 14));  /* 32x32 */
        ppu_render_scanline(&ppu, 8);
        t_check("Obj1D 32x32 tile row 1 skips 4 tiles",
                ppu.frame[8 * 240] == objpx(objidx(4, 0, 0, 0)));
        ppu_render_scanline(&ppu, 16);
        t_check("Obj1D 32x32 tile row 2 skips 8 tiles",
                ppu.frame[16 * 240] == objpx(objidx(8, 0, 0, 0)));
        ppu_render_scanline(&ppu, 24);
        t_check("Obj1D 32x32 tile row 3 skips 12 tiles",
                ppu.frame[24 * 240] == objpx(objidx(12, 0, 0, 0)));
        /* A vertical shape is one tile wide, so its tile rows are adjacent.
         * The shape is attribute 0 bits 14-15, the size attribute 1 bits
         * 14-15. */
        memory_write16(&memory, 0x07000000, OBJ8 | (2 << 14));  /* vertical */
        memory_write16(&memory, 0x07000002, (uint16_t)(1 << 14));  /* 8x32 */
        ppu_render_scanline(&ppu, 8);
        t_check("Obj1D 8x32 tile row 1 skips 1 tile",
                ppu.frame[8 * 240] == objpx(objidx(1, 0, 0, 0)));
        ppu_render_scanline(&ppu, 16);
        t_check("Obj1D 8x32 tile row 2 skips 2 tiles",
                ppu.frame[16 * 240] == objpx(objidx(2, 0, 0, 0)));
        ppu_render_scanline(&ppu, 24);
        t_check("Obj1D 8x32 tile row 3 skips 3 tiles",
                ppu.frame[24 * 240] == objpx(objidx(3, 0, 0, 0)));

        /* Horizontal addressing and the flips. */
        obj_fill(&memory, 8, 1);
        memory_write16(&memory, 0x07000000, OBJ8 | (1 << 14));  /* horizontal */
        memory_write16(&memory, 0x07000002, 0x0000);           /* 16x8 */
        memory_write16(&memory, 0x07000004, 0x0000);
        ppu_render_scanline(&ppu, 0);
        t_check("Obj col0 samples tile 0 col 0", ppu.frame[0] == objpx(objidx(0, 0, 0, 1)));
        t_check("Obj col1 samples tile 0 col 1", ppu.frame[1] == objpx(objidx(0, 0, 1, 1)));
        t_check("Obj col8 samples tile 1 col 0", ppu.frame[8] == objpx(objidx(1, 0, 0, 1)));
        t_check("Obj col9 samples tile 1 col 1", ppu.frame[9] == objpx(objidx(1, 0, 1, 1)));
        memory_write16(&memory, 0x07000002, 0x1000);           /* hflip */
        ppu_render_scanline(&ppu, 0);
        t_check("Obj hflip mirrors the columns",
                ppu.frame[0] == objpx(objidx(1, 0, 7, 1)) &&
                ppu.frame[15] == objpx(objidx(0, 0, 0, 1)));
        obj_fill(&memory, 8, 0);
        memory_write16(&memory, 0x07000002, 0x2000);           /* vflip */
        ppu_render_scanline(&ppu, 0);
        t_check("Obj vflip mirrors the tile row",
                ppu.frame[0] == objpx(objidx(0, 7, 0, 0)));
        ppu_render_scanline(&ppu, 7);
        t_check("Obj vflip mirrors the last row",
                ppu.frame[7 * 240] == objpx(objidx(0, 0, 0, 0)));

        /* 4bpp sprites add the palette bank to the tile index; 8bpp sprites
         * use all 256 entries and ignore the bank. */
        memory_write16(&memory, 0x07000000, 0x0008);              /* 4bpp, y = 8 */
        for (int i = 0; i < 32; i++) {
            memory_write8(&memory, 0x06010000 + i, 0x2A);
        }
        memory_write16(&memory, 0x05000200 + 2 * 0x0A, 0x7C00);   /* bank 0 */
        memory_write16(&memory, 0x05000200 + 2 * (0x10 + 0x0A), 0x03E0); /* bank 1 */
        memory_write16(&memory, 0x07000002, 0x0000);              /* 8x8 */
        memory_write16(&memory, 0x07000004, 0x0000);              /* tile 0 */
        ppu_render_scanline(&ppu, 0);
        t_check("Obj4bpp uses palette bank 0", ppu.frame[0] == rgb5(0x7C00));
        memory_write16(&memory, 0x07000004, 0x1000);
        ppu_render_scanline(&ppu, 0);
        t_check("Obj4bpp adds the palette bank", ppu.frame[0] == rgb5(0x03E0));
        memory_write16(&memory, 0x07000000, OBJ8 | 0x1000);   /* 8bpp, bank 1 */
        ppu_render_scanline(&ppu, 0);
        t_check("Obj8bpp ignores the palette bank",
                ppu.frame[0] == objpx(0x2A));

        /* Attribute 0 bit 9 disables a non-affine object; it is only the
         * double size flag when the affine bit is set. */
        memory_write16(&memory, 0x07000000, OBJ8);
        memory_write16(&memory, 0x07000002, 0x0000);
        memory_write16(&memory, 0x07000004, 0x0000);
        ppu_render_scanline(&ppu, 0);
        t_check("Obj drawn when not disabled", ppu.frame[0] == objpx(0x2A));
        memory_write16(&memory, 0x07000000, OBJ8 | 0x0200);
        ppu_render_scanline(&ppu, 0);
        t_check("Obj bit 9 hides a non-affine object", ppu.frame[0] == 0);

        /* The mode is attribute 0 bits 10-11, and the blending weights come
         * from BLDDALPHA. */
        hw_io_write(&hw, 0x50, 0x0000, 16);           /* BLDCNT: 1st target OBJ */
        hw_io_write(&hw, 0x52, 0x0808, 16);           /* BLDDALPHA: 50/50 */
        memory_write16(&memory, 0x07000000, OBJ8 | 0x0400);   /* semi transparent */
        ppu_render_scanline(&ppu, 0);
        t_check("Obj semi transparent blends with the backdrop",
                ppu.frame[0] == objpx(0x2A) / 2);
        memory_write16(&memory, 0x07000000, OBJ8 | 0x0800);   /* OBJ window */
        ppu_render_scanline(&ppu, 0);
        t_check("Obj window mode does not draw the object", ppu.frame[0] == 0);

        memory_set_io(&memory, NULL);
    }

    test_bios_services();
    test_boot_rom();
    test_long_multiply(&memory);
    test_sound();
    test_cartridge_backup();
    test_8bpp_opaque();

    printf("\n%d/%d passed\n", total_tests - failures, total_tests);

    memory_free(&memory);
    return failures ? 1 : 0;
}