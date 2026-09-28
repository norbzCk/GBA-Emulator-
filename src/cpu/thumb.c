#include "cpu_priv.h"

/* Thumb shift-by-register/immediate semantics.
 *
 * For immediate shifts an encoded amount of 0 means "shift by 32" for
 * LSR/ASR (0 for LSL). For register shifts an amount of 0 is a no-op that
 * leaves the carry flag untouched. */

static uint32_t t_shift(uint32_t value, int type, uint32_t amount, int is_reg,
                        uint32_t carry_in, uint32_t *c_out) {
    *c_out = carry_in;

    switch (type) {
        case CPU_SHIFT_LSL:
            if (amount == 0) return value;
            if (amount < 32) {
                *c_out = (value >> (32 - amount)) & 1;
                return value << amount;
            }
            if (amount == 32) {
                *c_out = value & 1;
            } else {
                *c_out = 0;
            }
            return 0;

        case CPU_SHIFT_LSR:
            if (amount == 0 && !is_reg) amount = 32;
            if (amount == 0) return value;
            if (amount < 32) {
                *c_out = (value >> (amount - 1)) & 1;
                return value >> amount;
            }
            *c_out = (value >> 31) & 1;
            return 0;

        case CPU_SHIFT_ASR:
            if (amount == 0 && !is_reg) amount = 32;
            if (amount == 0) return value;
            if (amount < 32) {
                *c_out = (value >> (amount - 1)) & 1;
                return (uint32_t)((int32_t)value >> amount);
            }
            *c_out = (value >> 31) & 1;
            return (uint32_t)((int32_t)value >> 31);

        case CPU_SHIFT_ROR:
        default:
            amount &= 31;
            if (amount == 0) return value;
            *c_out = (value >> (amount - 1)) & 1;
            return (value >> amount) | (value << (32 - amount));
    }
}

static uint32_t t_reg8(CPU *cpu, uint32_t idx) {
    return cpu_read_reg(cpu, idx) & 0xFF;
}

/* ---- format 1: shift by immediate (0x0000..0x17FF) ----------------------- */

static uint32_t t_shift_imm(CPU *cpu, uint16_t insn) {
    uint32_t type = (insn >> 11) & 3;
    uint32_t imm = (insn >> 6) & 0x1F;
    uint32_t rm = (insn >> 3) & 7;
    uint32_t rd = insn & 7;
    uint32_t value = cpu_read_reg(cpu, rm);
    uint32_t carry;
    uint32_t res = t_shift(value, type, imm, 0, cpu_flag_c(cpu), &carry);
    cpu_set_flags(cpu, res & 0x80000000u, res == 0, carry, cpu_flag_v(cpu));
    cpu_write_reg(cpu, rd, res);
    return 1;
}

/* ---- format 2: add/subtract, 3-reg or immediate (0x1800..0x1FFF) --------- */

static uint32_t t_add_sub(CPU *cpu, uint16_t insn) {
    uint32_t is_imm = (insn >> 10) & 1;
    uint32_t sub = (insn >> 9) & 1;
    uint32_t rn = (insn >> 3) & 7;
    uint32_t rd = insn & 7;
    uint32_t op2 = is_imm ? ((insn >> 6) & 7) : cpu_read_reg(cpu, (insn >> 6) & 7);
    uint32_t a = cpu_read_reg(cpu, rn);
    uint32_t c_out, v_out, res;

    if (sub) {
        res = cpu_add_carry(a, ~op2, 1, &c_out, &v_out);
    } else {
        res = cpu_add_carry(a, op2, 0, &c_out, &v_out);
    }
    cpu_set_flags(cpu, res & 0x80000000u, res == 0, c_out, v_out);
    cpu_write_reg(cpu, rd, res);
    return 1;
}

/* ---- format 3: MOV/CMP/ADD/SUB immediate (0x2000..0x3FFF) ---------------- */

static uint32_t t_imm8(CPU *cpu, uint16_t insn) {
    uint32_t op = (insn >> 11) & 3;
    uint32_t rd = (insn >> 8) & 7;
    uint32_t imm = insn & 0xFF;
    uint32_t a = cpu_read_reg(cpu, rd);
    uint32_t c_out, v_out, res;

    switch (op) {
        case 0: /* MOV */
            res = imm;
            cpu_set_flags(cpu, res & 0x80000000u, res == 0, cpu_flag_c(cpu), cpu_flag_v(cpu));
            break;
        case 1: /* CMP */
            res = cpu_add_carry(a, ~imm, 1, &c_out, &v_out);
            cpu_set_flags(cpu, res & 0x80000000u, res == 0, c_out, v_out);
            return 1;
        case 2: /* ADD */
            res = cpu_add_carry(a, imm, 0, &c_out, &v_out);
            cpu_set_flags(cpu, res & 0x80000000u, res == 0, c_out, v_out);
            break;
        default: /* SUB */
            res = cpu_add_carry(a, ~imm, 1, &c_out, &v_out);
            cpu_set_flags(cpu, res & 0x80000000u, res == 0, c_out, v_out);
            break;
    }
    cpu_write_reg(cpu, rd, res);
    return 1;
}

/* ---- format 4: ALU operations (0x4000..0x41FF) --------------------------- */

static uint32_t t_alu(CPU *cpu, uint16_t insn) {
    uint32_t op = (insn >> 6) & 0xF;
    uint32_t rm = (insn >> 3) & 7;
    uint32_t rdn = insn & 7;
    uint32_t a = cpu_read_reg(cpu, rdn);
    uint32_t b = cpu_read_reg(cpu, rm);
    uint32_t carry, c_out, v_out, res;
    uint32_t old_c = cpu_flag_c(cpu);

    switch (op) {
        case 0: /* ANDS */
            res = a & b;
            cpu_set_flags(cpu, res & 0x80000000u, res == 0, old_c, cpu_flag_v(cpu));
            break;
        case 1: /* EORS */
            res = a ^ b;
            cpu_set_flags(cpu, res & 0x80000000u, res == 0, old_c, cpu_flag_v(cpu));
            break;
        case 2: /* LSLS by register */
            res = t_shift(a, CPU_SHIFT_LSL, t_reg8(cpu, rm), 1, old_c, &carry);
            cpu_set_flags(cpu, res & 0x80000000u, res == 0, carry, cpu_flag_v(cpu));
            break;
        case 3: /* LSRS by register */
            res = t_shift(a, CPU_SHIFT_LSR, t_reg8(cpu, rm), 1, old_c, &carry);
            cpu_set_flags(cpu, res & 0x80000000u, res == 0, carry, cpu_flag_v(cpu));
            break;
        case 4: /* ASRS by register */
            res = t_shift(a, CPU_SHIFT_ASR, t_reg8(cpu, rm), 1, old_c, &carry);
            cpu_set_flags(cpu, res & 0x80000000u, res == 0, carry, cpu_flag_v(cpu));
            break;
        case 5: /* ADCS */
            res = cpu_add_carry(a, b, old_c, &c_out, &v_out);
            cpu_set_flags(cpu, res & 0x80000000u, res == 0, c_out, v_out);
            break;
        case 6: /* SBCS */
            res = cpu_add_carry(a, ~b, old_c, &c_out, &v_out);
            cpu_set_flags(cpu, res & 0x80000000u, res == 0, c_out, v_out);
            break;
        case 7: /* RORS by register */
            res = t_shift(a, CPU_SHIFT_ROR, t_reg8(cpu, rm), 1, old_c, &carry);
            cpu_set_flags(cpu, res & 0x80000000u, res == 0, carry, cpu_flag_v(cpu));
            break;
        case 8: /* TST */
            res = a & b;
            cpu_set_flags(cpu, res & 0x80000000u, res == 0, old_c, cpu_flag_v(cpu));
            return 1;
        case 9: /* NEG (RSBS) */
            res = cpu_add_carry(0, ~b, 1, &c_out, &v_out);
            cpu_set_flags(cpu, res & 0x80000000u, res == 0, c_out, v_out);
            break;
        case 10: /* CMP */
            res = cpu_add_carry(a, ~b, 1, &c_out, &v_out);
            cpu_set_flags(cpu, res & 0x80000000u, res == 0, c_out, v_out);
            return 1;
        case 11: /* CMN */
            res = cpu_add_carry(a, b, 0, &c_out, &v_out);
            cpu_set_flags(cpu, res & 0x80000000u, res == 0, c_out, v_out);
            return 1;
        case 12: /* ORRS */
            res = a | b;
            cpu_set_flags(cpu, res & 0x80000000u, res == 0, old_c, cpu_flag_v(cpu));
            break;
        case 13: /* MULS */
            res = a * b;
            cpu_set_flags(cpu, res & 0x80000000u, res == 0, old_c, cpu_flag_v(cpu));
            break;
        case 14: /* BICS */
            res = a & ~b;
            cpu_set_flags(cpu, res & 0x80000000u, res == 0, old_c, cpu_flag_v(cpu));
            break;
        default: /* MVNS */
            res = ~b;
            cpu_set_flags(cpu, res & 0x80000000u, res == 0, old_c, cpu_flag_v(cpu));
            break;
    }

    cpu_write_reg(cpu, rdn, res);
    return 1;
}

/* ---- format 5: high register ops and BX (0x4400..0x47FF) -----------------
 *
 * bits[9:8] identify the operation:
 *   00 ADD  01 CMP  10 MOV  11 BX
 * bit[7]    = H1, the high bit of Rd
 * bits[6:3] = Rm (4 bits; bit[6] is Rm's high bit)
 * bits[2:0] = Rd low bits
 * For BX the destination field is unused and Rm = bits[6:3]. */

static uint32_t t_high(CPU *cpu, uint16_t insn) {
    uint32_t op = (insn >> 8) & 3;
    uint32_t h1 = (insn >> 7) & 1;
    uint32_t rm = (insn >> 3) & 0xF;
    uint32_t rd = (insn & 7) | (h1 << 3);

    if (op == 3) { /* BX / BLX */
        uint32_t value = cpu_read_reg(cpu, rm);
        if (insn & 0x0080) {
            /* BLX: return to the Thumb instruction after this one. */
            cpu_write_reg(cpu, 14, (cpu->reg[15] - 2) | 1u);
        }
        if (value & 1) {
            cpu->cpsr |= FLAG_T;
        } else {
            cpu->cpsr &= ~FLAG_T;
        }
        cpu_set_pc(cpu, value & ~1u);
        return 3;
    }

    uint32_t a = cpu_read_reg(cpu, rd);
    uint32_t b = cpu_read_reg(cpu, rm);
    uint32_t c_out, v_out, res;

    switch (op) {
        case 0: /* ADD */
            res = a + b;
            cpu_write_reg(cpu, rd, res);
            if (rd == 15) {
                /* ADD pc, rm: the state is kept and bit 0 of the result is
                 * ignored. Only BX interworks on the ARM7TDMI. */
                cpu->reg[15] &= ~1u;
                return 3;
            }
            return 1;
        case 1: /* CMP */
            res = cpu_add_carry(a, ~b, 1, &c_out, &v_out);
            cpu_set_flags(cpu, res & 0x80000000u, res == 0, c_out, v_out);
            return 1;
        default: /* MOV */
            cpu_write_reg(cpu, rd, b);
            if (rd == 15) {
                /* MOV pc, rm does not interwork on the ARM7TDMI: the ARMv5T
                 * rule of taking the instruction set from bit 0 of the source
                 * register does not exist here, so devkitARM code uses
                 * "mov pc, reg" to call Thumb functions. Bit 0 is ignored and
                 * the Thumb state is kept. */
                cpu->reg[15] &= ~1u;
                return 3;
            }
            return 1;
    }
}

/* ---- format 7: load/store with register offset (0x5000..0x5FFF) ---------- */

static uint32_t t_load_store_reg(CPU *cpu, Memory *mem, uint16_t insn, int width,
                                 int sign, int load) {
    uint32_t ro = cpu_read_reg(cpu, (insn >> 6) & 7);
    uint32_t rn = (insn >> 3) & 7;
    uint32_t rd = insn & 7;
    uint32_t addr = cpu_read_reg(cpu, rn) + ro;

    if (load) {
        uint32_t value;
        if (width == 1) {
            value = memory_read8(mem, addr);
        } else if (width == 2) {
            value = memory_read16(mem, addr);
        } else {
            value = memory_read32(mem, addr);
        }
        if (sign) {
            value = (width == 1) ? (uint32_t)(int32_t)(int8_t)value
                                 : (uint32_t)(int32_t)(int16_t)value;
        }
        cpu_write_reg(cpu, rd, value);
    } else {
        uint32_t value = cpu_read_reg(cpu, rd);
        if (width == 1) {
            memory_write8(mem, addr, (uint8_t)value);
        } else if (width == 2) {
            memory_write16(mem, addr, (uint16_t)value);
        } else {
            memory_write32(mem, addr, value);
        }
    }
    return 2;
}

/* ---- formats 8/9/10: load/store byte/halfword/word, immediate offset ----- */

static uint32_t t_load_store_imm(CPU *cpu, Memory *mem, uint16_t insn, int width,
                                 int load) {
    uint32_t imm = (insn >> 6) & 0x1F;
    uint32_t rn = (insn >> 3) & 7;
    uint32_t rd = insn & 7;
    uint32_t shift = (width == 1) ? 0 : (width == 2) ? 1 : 2;
    uint32_t addr = cpu_read_reg(cpu, rn) + (imm << shift);

    if (load) {
        if (width == 1) {
            cpu_write_reg(cpu, rd, memory_read8(mem, addr));
        } else if (width == 2) {
            cpu_write_reg(cpu, rd, memory_read16(mem, addr));
        } else {
            cpu_write_reg(cpu, rd, memory_read32(mem, addr));
        }
    } else {
        uint32_t value = cpu_read_reg(cpu, rd);
        if (width == 1) {
            memory_write8(mem, addr, (uint8_t)value);
        } else if (width == 2) {
            memory_write16(mem, addr, (uint16_t)value);
        } else {
            memory_write32(mem, addr, value);
        }
    }
    return 2;
}

/* ---- format 11: LDR/STR SP-relative (0x9000..0x9FFF) ---------------------- */

static uint32_t t_sp_rel(CPU *cpu, Memory *mem, uint16_t insn) {
    uint32_t rd = (insn >> 8) & 7;
    uint32_t addr = cpu_read_reg(cpu, 13) + ((insn & 0xFF) << 2);
    if (insn & 0x0800) {
        cpu_write_reg(cpu, rd, memory_read32(mem, addr));
    } else {
        memory_write32(mem, addr, cpu_read_reg(cpu, rd));
    }
    return 2;
}

/* ---- format 6: PC-relative LDR literal (0x4800..0x4FFF) ------------------ */

static uint32_t t_ldr_literal(CPU *cpu, Memory *mem, uint16_t insn) {
    uint32_t rd = (insn >> 8) & 7;
    uint32_t addr = (cpu->reg[15] & ~3u) + ((insn & 0xFF) << 2);
    cpu_write_reg(cpu, rd, memory_read32(mem, addr));
    return 2;
}

/* ---- format 12: ADR, add address of PC/SP (0xA000..0xA7FF PC) ------------ */

static uint32_t t_adr(CPU *cpu, uint16_t insn) {
    uint32_t rd = (insn >> 8) & 7;
    uint32_t imm = (insn & 0xFF) << 2;
    if (insn & 0x0800) {
        cpu_write_reg(cpu, rd, cpu_read_reg(cpu, 13) + imm);
    } else {
        cpu_write_reg(cpu, rd, (cpu->reg[15] & ~3u) + imm);
    }
    return 1;
}

/* ---- format 13: add/subtract SP by immediate (0xB000..0xB3FF) ------------ */

static uint32_t t_sp_adjust(CPU *cpu, uint16_t insn) {
    uint32_t imm = (insn & 0x7F) << 2;
    uint32_t sp = cpu_read_reg(cpu, 13);
    if (insn & 0x80) {
        cpu_write_reg(cpu, 13, sp - imm);
    } else {
        cpu_write_reg(cpu, 13, sp + imm);
    }
    return 1;
}

/* ---- format 14: PUSH (0xB400..0xB7FF) ------------------------------------- */

static uint32_t t_push(CPU *cpu, Memory *mem, uint16_t insn) {
    uint32_t list = insn & 0xFF;
    unsigned count = 0;

    for (unsigned i = 0; i < 8; i++) {
        count += (list >> i) & 1;
    }
    if (insn & 0x0100) count++; /* LR */

    uint32_t addr = cpu_read_reg(cpu, 13) - 4 * count;
    for (unsigned i = 0; i < 8; i++) {
        if (list & (1u << i)) {
            memory_write32(mem, addr, cpu_read_reg(cpu, i));
            addr += 4;
        }
    }
    if (insn & 0x0100) {
        memory_write32(mem, addr, cpu_read_reg(cpu, 14));
    }
    cpu_write_reg(cpu, 13, cpu_read_reg(cpu, 13) - 4 * count);
    return 1 + count;
}

/* ---- format 15: POP (0xBC00..0xBFFF) -------------------------------------- */

static uint32_t t_pop(CPU *cpu, Memory *mem, uint16_t insn) {
    uint32_t list = insn & 0xFF;
    unsigned count = 0;
    uint32_t addr = cpu_read_reg(cpu, 13);

    for (unsigned i = 0; i < 8; i++) {
        if (list & (1u << i)) {
            count++;
            cpu_write_reg(cpu, i, memory_read32(mem, addr));
            addr += 4;
        }
    }
    if (insn & 0x0100) {
        count++;
        uint32_t pc = memory_read32(mem, addr);
        if (pc & 1) {
            cpu->cpsr |= FLAG_T;
        } else {
            cpu->cpsr &= ~FLAG_T;
        }
        cpu_set_pc(cpu, pc & ~1u);
    }
    cpu_write_reg(cpu, 13, cpu_read_reg(cpu, 13) + 4 * count);
    return 2 + count;
}

/* ---- format 16: LDMIA/STMIA (0xC000..0xCFFF) ------------------------------ */

static uint32_t t_ldm_stm(CPU *cpu, Memory *mem, uint16_t insn) {
    uint32_t load = (insn >> 11) & 1;
    uint32_t rn = (insn >> 8) & 7;
    uint32_t list = insn & 0xFF;
    unsigned count = 0;

    for (int i = 0; i < 8; i++) {
        count += (list >> i) & 1;
    }

    uint32_t base = cpu_read_reg(cpu, rn);
    uint32_t addr = base;

    if (load) {
        for (unsigned i = 0; i < 8; i++) {
            if (list & (1u << i)) {
                cpu_write_reg(cpu, i, memory_read32(mem, addr));
                addr += 4;
            }
        }
        if (!(list & (1u << rn))) {
            cpu_write_reg(cpu, rn, base + 4 * count);
        }
    } else {
        for (unsigned i = 0; i < 8; i++) {
            if (list & (1u << i)) {
                memory_write32(mem, addr, cpu_read_reg(cpu, i));
                addr += 4;
            }
        }
        cpu_write_reg(cpu, rn, base + 4 * count);
    }
    return 1 + count;
}

/* ---- branches ------------------------------------------------------------ */

static int t_cond_passed(CPU *cpu, uint32_t cond) {
    int passed = 0;

    switch (cond) {
        case 0:  passed = cpu_flag_z(cpu); break;
        case 1:  passed = !cpu_flag_z(cpu); break;
        case 2:  passed = cpu_flag_c(cpu); break;
        case 3:  passed = !cpu_flag_c(cpu); break;
        case 4:  passed = cpu_flag_n(cpu); break;
        case 5:  passed = !cpu_flag_n(cpu); break;
        case 6:  passed = cpu_flag_v(cpu); break;
        case 7:  passed = !cpu_flag_v(cpu); break;
        case 8:  passed = cpu_flag_c(cpu) && !cpu_flag_z(cpu); break;
        case 9:  passed = !cpu_flag_c(cpu) || cpu_flag_z(cpu); break;
        case 10: passed = cpu_flag_n(cpu) == cpu_flag_v(cpu); break;
        case 11: passed = cpu_flag_n(cpu) != cpu_flag_v(cpu); break;
        case 12: passed = !cpu_flag_z(cpu) && (cpu_flag_n(cpu) == cpu_flag_v(cpu)); break;
        case 13: passed = cpu_flag_z(cpu) || (cpu_flag_n(cpu) != cpu_flag_v(cpu)); break;
        default: passed = 1; break;
    }

    return passed;
}

static uint32_t t_branch_cond(CPU *cpu, uint16_t insn) {
    if (!t_cond_passed(cpu, (insn >> 8) & 0xF)) {
        return 1;
    }

    int32_t off = (int8_t)(insn & 0xFF) << 1;
    cpu_set_pc(cpu, cpu->reg[15] + off);
    return 3;
}

static uint32_t t_branch(CPU *cpu, uint16_t insn) {
    int32_t off = (int16_t)((insn & 0x7FF) << 5) >> 4;
    cpu_set_pc(cpu, cpu->reg[15] + off);
    return 3;
}

static uint32_t t_swi(CPU *cpu, uint16_t insn) {
    if (cpu->swi_hook) {
        cpu->swi_hook(cpu->swi_ctx, insn & 0xFF);
        return 3;
    }

    cpu_write_reg(cpu, 14, cpu->reg[15] - 2);
    cpu->spsr[BANK_SVC] = cpu->cpsr;
    /* The BIOS SWI entry at 0x00000008 is ARM code; it branches back into
     * Thumb itself when the caller was Thumb. */
    cpu->cpsr = (cpu->cpsr & ~(0x1Fu | FLAG_T)) | MODE_SVC | FLAG_I;
    cpu_set_pc(cpu, 0x00000008);
    return 3;
}

/* BL is a pair of halfwords: 11110 S imm10 : 11 J1 1 J2 imm11. The 24-bit
 * offset is formed as S:I1:I2:imm10:imm11:'0' with I1/I2 the complements of
 * J1/J2 xored with S, so both halfwords are needed before the target is
 * known. Fetching the suffix here keeps that in one place; the link register
 * ends up holding the address after the pair, as on hardware. */
static uint32_t t_bl(CPU *cpu, Memory *mem, uint16_t insn) {
    uint16_t insn2 = memory_read16(mem, cpu->reg[15] - 2);
    uint32_t s = (insn >> 10) & 1;
    uint32_t i1 = 1u ^ ((((insn2 >> 13) & 1) ^ s));
    uint32_t i2 = 1u ^ ((((insn2 >> 11) & 1) ^ s));
    uint32_t imm = (s << 24) | (i1 << 23) | (i2 << 22) | ((insn & 0x3FF) << 12) |
                   ((insn2 & 0x7FF) << 1);
    int32_t off = (imm & (1u << 24)) ? (int32_t)imm - (1 << 25) : (int32_t)imm;

    cpu_write_reg(cpu, 14, cpu->reg[15] | 1);
    cpu_set_pc(cpu, cpu->reg[15] + (uint32_t)off);
    return 4;
}

/* ---- main Thumb dispatch -------------------------------------------------- */

uint32_t thumb_execute(CPU *cpu, Memory *mem, uint16_t insn) {
    if ((insn & 0xF800) == 0xF000) {
        return t_bl(cpu, mem, insn);
    }

    switch (insn >> 10) {
        /* 000xx : shift by immediate */
        case 0x00: case 0x01: case 0x02:
        case 0x03: case 0x04: case 0x05:
            return t_shift_imm(cpu, insn);

        /* 00011x : add/subtract */
        case 0x06: case 0x07:
            return t_add_sub(cpu, insn);

        /* 001xx : MOV/CMP/ADD/SUB immediate */
        case 0x08: case 0x09: case 0x0A: case 0x0B:
        case 0x0C: case 0x0D: case 0x0E: case 0x0F:
            return t_imm8(cpu, insn);

        /* 010000 : ALU operations */
        case 0x10:
            return t_alu(cpu, insn);

        /* 010001 : high register ops / BX */
        case 0x11:
            return t_high(cpu, insn);

        /* 01001x : LDR literal (PC-relative) */
        case 0x12: case 0x13:
            return t_ldr_literal(cpu, mem, insn);

        /* 0101xx : load/store register offset */
        case 0x14: case 0x15: case 0x16: case 0x17: {
            uint32_t op = (insn >> 9) & 7;
            switch (op) {
                case 0: return t_load_store_reg(cpu, mem, insn, 4, 0, 0);
                case 1: return t_load_store_reg(cpu, mem, insn, 2, 0, 0);
                case 2: return t_load_store_reg(cpu, mem, insn, 1, 0, 0);
                case 3: return t_load_store_reg(cpu, mem, insn, 1, 1, 1);
                case 4: return t_load_store_reg(cpu, mem, insn, 4, 0, 1);
                case 5: return t_load_store_reg(cpu, mem, insn, 2, 0, 1);
                case 6: return t_load_store_reg(cpu, mem, insn, 1, 0, 1);
                default: return t_load_store_reg(cpu, mem, insn, 2, 1, 1);
            }
        }

        /* 0110x0 : STR word / 0110x1 : LDR word immediate */
        case 0x18: case 0x19:
            return t_load_store_imm(cpu, mem, insn, 4, 0);
        case 0x1A: case 0x1B:
            return t_load_store_imm(cpu, mem, insn, 4, 1);

        /* 0111: STRB / 0111x1 : LDRB immediate */
        case 0x1C: case 0x1D:
            return t_load_store_imm(cpu, mem, insn, 1, 0);
        case 0x1E: case 0x1F:
            return t_load_store_imm(cpu, mem, insn, 1, 1);

        /* 1000x0 : STRH / 1000x1 : LDRH immediate */
        case 0x20: case 0x21:
            return t_load_store_imm(cpu, mem, insn, 2, 0);
        case 0x22: case 0x23:
            return t_load_store_imm(cpu, mem, insn, 2, 1);

        /* 1001x0 : STR SP-relative / 1001x1 : LDR SP-relative */
        case 0x24: case 0x25:
            return t_sp_rel(cpu, mem, insn);
        case 0x26: case 0x27:
            return t_sp_rel(cpu, mem, insn);

        /* 101000 : ADR to PC / 10101 : ADR to SP */
        case 0x28: case 0x29:
            return t_adr(cpu, insn);
        case 0x2A: case 0x2B:
            return t_adr(cpu, insn);

        /* 1011000 : ADD/SUB SP by immediate */
        case 0x2C:
            return t_sp_adjust(cpu, insn);

        /* 101101x : PUSH */
        case 0x2D:
            return t_push(cpu, mem, insn);

        /* 101111x : POP */
        case 0x2F:
            return t_pop(cpu, mem, insn);

        /* 1100xx : STMIA / LDMIA */
        case 0x30: case 0x31:
            return t_ldm_stm(cpu, mem, insn);
        case 0x32: case 0x33:
            return t_ldm_stm(cpu, mem, insn);

        /* 1101xx : conditional branch / SWI */
        case 0x34: case 0x35: case 0x36: case 0x37:
            if ((insn & 0xFF00) == 0xDF00) {
                return t_swi(cpu, insn);
            }
            return t_branch_cond(cpu, insn);

        /* 11100x : unconditional branch */
        case 0x38: case 0x39:
            return t_branch(cpu, insn);

        /* 0xE800..0xEFFF is reserved for the 32-bit Thumb space, which the
         * ARM7TDMI does not implement, and 0xF800..0xFFFF only appears as the
         * second halfword of a BL (both are consumed by the default case). */
        case 0x3A: case 0x3B:
            return 1;

        default:
            /* Undefined 16-bit encoding: treated as a no-op. */
            return 1;
    }
}