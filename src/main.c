#include <stdio.h>

#include "cpu/cpu.h"
#include "memory/memory.h"

static int failures    = 0;
static int total_tests = 0;

static void t_check(const char *what, int ok) {
    total_tests++;
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}

/* Write one instruction at `addr`, set PC there, execute a single step. */
static void run_one(CPU *cpu, Memory *mem, uint32_t addr, uint32_t insn) {
    memory_write32(mem, addr, insn);
    cpu->reg[15] = addr;
    cpu_step(cpu, mem);
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

    {   /* LDR R0, [R1, #8] */
        memory_write32(&memory, 0x02000100, 0xDEADBEEF);
        cpu.reg[1] = 0x020000F8;
        run_one(&cpu, &memory, 0x02000070, 0xE5910008);
        t_check("LDR R0 = 0xDEADBEEF", cpu.reg[0] == 0xDEADBEEF);
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

    printf("\n%d/%d passed\n", total_tests - failures, total_tests);

    memory_free(&memory);
    return failures ? 1 : 0;
}