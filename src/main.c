#include <stdio.h>

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

    memory_set_io(&memory, NULL);
    memory_free(&memory);
}

/* ---- end to end ---------------------------------------------------------- */

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
        hw_io_write(&hw, 0xB8, 3, 16);                /* count-1 = 3 */
        hw.if_ = 0;
        hw_io_write(&hw, 0xBA, 0xC000, 16);           /* enable + IRQ, start=NOW */
        t_check("DMA0 copied CNT_L+1 halfwords",
                memory_read16(&memory, 0x06000000) == 0x1000 &&
                memory_read16(&memory, 0x06000006) == 0x1003);
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

    printf("\n%d/%d passed\n", total_tests - failures, total_tests);

    memory_free(&memory);
    return failures ? 1 : 0;
}