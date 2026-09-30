#include "cpu_priv.h"

#include <stdint.h>
#include <stdio.h>

/* Barrel shifter shift types (operand2 bits 6-5). */
enum {
    SHIFT_LSL = 0,
    SHIFT_LSR = 1,
    SHIFT_ASR = 2,
    SHIFT_ROR = 3
};

/* ---- CPSR helpers ------------------------------------------------------- */

static int flag_n(const CPU *cpu) { return cpu_flag_n(cpu); }
static int flag_z(const CPU *cpu) { return cpu_flag_z(cpu); }
static int flag_c(const CPU *cpu) { return cpu_flag_c(cpu); }
static int flag_v(const CPU *cpu) { return cpu_flag_v(cpu); }

int cpu_flag_n(const CPU *cpu) { return (cpu->cpsr & FLAG_N) != 0; }
int cpu_flag_z(const CPU *cpu) { return (cpu->cpsr & FLAG_Z) != 0; }
int cpu_flag_c(const CPU *cpu) { return (cpu->cpsr & FLAG_C) != 0; }
int cpu_flag_v(const CPU *cpu) { return (cpu->cpsr & FLAG_V) != 0; }

void cpu_set_flags(CPU *cpu, uint32_t n, uint32_t z, uint32_t c, uint32_t v) {
    cpu->cpsr &= ~(FLAG_N | FLAG_Z | FLAG_C | FLAG_V);
    cpu->cpsr |= (n ? FLAG_N : 0) | (z ? FLAG_Z : 0)
               | (c ? FLAG_C : 0) | (v ? FLAG_V : 0);
}

/* ---- register banks ----------------------------------------------------- */

static int mode_to_bank(const CPU *cpu) {
    switch (cpu->cpsr & 0x1F) {
        case MODE_FIQ: return BANK_FIQ;
        case MODE_SVC: return BANK_SVC;
        case MODE_ABT: return BANK_ABT;
        case MODE_IRQ: return BANK_IRQ;
        case MODE_UND: return BANK_UND;
        default:       return BANK_USR;
    }
}

uint32_t cpu_read_reg(const CPU *cpu, uint32_t r) {
    if (r == 15) {
        return cpu->reg[15];
    }
    if (r >= 8 && r <= 14) {
        int bank = mode_to_bank(cpu);
        if (r <= 12 && bank == BANK_FIQ) {
            return cpu->r8_12_fiq[r - 8];
        }
        if (r == 13) {
            return cpu->r13[bank];
        }
        if (r == 14) {
            return cpu->r14[bank];
        }
    }
    return cpu->reg[r];
}

void cpu_set_pc(CPU *cpu, uint32_t value) {
    cpu->reg[15] = value;
    cpu->pc_written = true;
}

void cpu_write_reg(CPU *cpu, uint32_t r, uint32_t value) {
    if (r == 15) {
        cpu_set_pc(cpu, value);
        return;
    }
    if (r >= 8 && r <= 14) {
        int bank = mode_to_bank(cpu);
        if (r <= 12 && bank == BANK_FIQ) {
            cpu->r8_12_fiq[r - 8] = value;
            return;
        }
        if (r == 13) {
            cpu->r13[bank] = value;
            return;
        }
        if (r == 14) {
            cpu->r14[bank] = value;
            return;
        }
    }
    cpu->reg[r] = value;
}

/* ---- condition codes ---------------------------------------------------- */

static int condition_passed(const CPU *cpu, uint32_t cond) {
    switch (cond) {
        case 0:  return flag_z(cpu);
        case 1:  return !flag_z(cpu);
        case 2:  return flag_c(cpu);
        case 3:  return !flag_c(cpu);
        case 4:  return flag_n(cpu);
        case 5:  return !flag_n(cpu);
        case 6:  return flag_v(cpu);
        case 7:  return !flag_v(cpu);
        case 8:  return flag_c(cpu) && !flag_z(cpu);
        case 9:  return !flag_c(cpu) || flag_z(cpu);
        case 10: return flag_n(cpu) == flag_v(cpu);
        case 11: return flag_n(cpu) != flag_v(cpu);
        case 12: return !flag_z(cpu) && (flag_n(cpu) == flag_v(cpu));
        case 13: return flag_z(cpu) || (flag_n(cpu) != flag_v(cpu));
        case 14: return 1;
        default: return 0; /* NV unexecuted in ARMv4 */
    }
}

/* ---- arithmetic --------------------------------------------------------- */

/* a + b + carry, reporting unsigned carry-out and signed overflow. */
uint32_t cpu_add_carry(uint32_t a, uint32_t b, uint32_t carry_in,
                       uint32_t *carry_out, uint32_t *overflow_out) {
    uint64_t sum = (uint64_t)a + b + carry_in;
    uint32_t res = (uint32_t)sum;
    uint32_t m = 0x80000000u;

    *carry_out = (uint32_t)(sum >> 32);
    *overflow_out = (((a ^ b) & m) == 0 && ((a ^ res) & m) != 0);
    return res;
}

/* ---- barrel shifter ----------------------------------------------------- */

static uint32_t rot_r(uint32_t value, uint32_t amount) {
    amount &= 31;
    if (amount == 0) {
        return value;
    }
    return (value >> amount) | (value << (32 - amount));
}

uint32_t cpu_shift_c(CPU *cpu, uint32_t type, uint32_t value, uint32_t amount,
                     uint32_t carry_in, uint32_t *carry_out) {
    uint32_t res;
    *carry_out = carry_in;

    switch (type) {
        case SHIFT_LSL:
            if (amount == 0) return value;
            if (amount < 32) {
                *carry_out = (value >> (32 - amount)) & 1;
                return value << amount;
            }
            if (amount == 32) {
                *carry_out = value & 1;
                return 0;
            }
            *carry_out = 0;
            return 0;

        case SHIFT_LSR:
            if (amount == 0) amount = 32;
            if (amount < 32) {
                *carry_out = (value >> (amount - 1)) & 1;
                return value >> amount;
            }
            *carry_out = (value >> 31) & 1;
            return 0;

        case SHIFT_ASR:
            if (amount == 0) amount = 32;
            if (amount < 32) {
                *carry_out = (value >> (amount - 1)) & 1;
                return (uint32_t)((int32_t)value >> amount);
            }
            *carry_out = (value >> 31) & 1;
            return (uint32_t)((int32_t)value >> 31);

        case SHIFT_ROR:
        default:
            if (amount == 0) {
                /* RRX: rotate right through carry. */
                *carry_out = value & 1;
                return (carry_in << 31) | (value >> 1);
            }
            amount &= 31;
            if (amount == 0) {
                return value;
            }
            *carry_out = (value >> (amount - 1)) & 1;
            return (value >> amount) | (value << (32 - amount));
    }

    (void)cpu;
    return res;
}

/* Decode operand2 in a data-processing instruction. */
static uint32_t operand2(CPU *cpu, uint32_t insn, uint32_t *carry) {
    uint32_t carry_in = flag_c(cpu);
    *carry = carry_in;

    if (insn & (1 << 25)) {
        /* Immediate: imm8 rotated right by 2 * rot. */
        uint32_t imm8 = insn & 0xFF;
        uint32_t rot = (insn >> 8) & 0xF;
        if (rot == 0) {
            return imm8;
        }
        *carry = ((imm8 >> (rot * 2)) | (imm8 << (32 - rot * 2))) >> 31;
        return rot_r(imm8, rot * 2);
    }

    /* Register, optionally shifted. */
    uint32_t rm = insn & 0xF;
    uint32_t type = (insn >> 5) & 3;
    uint32_t value = cpu_read_reg(cpu, rm);
    uint32_t amount;

    if (insn & (1 << 4)) {
        /* Shift amount from register Rs (low 8 bits). */
        amount = cpu_read_reg(cpu, (insn >> 8) & 0xF) & 0xFF;
        if (amount == 0) {
            if (type == SHIFT_ROR) {
                return value; /* ROR Rs with Rs&0xFF==0: no-op */
            }
        }
        return cpu_shift_c(cpu, type, value, amount, carry_in, carry);
    }

    amount = (insn >> 7) & 0x1F;
    return cpu_shift_c(cpu, type, value, amount, carry_in, carry);
}

/* ---- data processing ---------------------------------------------------- */

static void data_processing(CPU *cpu, uint32_t insn) {
    uint32_t opcode = (insn >> 21) & 0xF;
    uint32_t s = (insn >> 20) & 1;
    uint32_t rn = (insn >> 16) & 0xF;
    uint32_t rd = (insn >> 12) & 0xF;
    uint32_t carry;
    uint32_t result = 0;
    uint32_t c_out = 0, v_out = 0;
    int updates_flags = s;

    uint32_t op2 = operand2(cpu, insn, &carry);

    uint32_t rn_val = (rn == 15) ? cpu->reg[15] : cpu_read_reg(cpu, rn);

    switch (opcode) {
        case 0x0: /* AND */
        case 0x8: /* TST */
            result = rn_val & op2;
            break;
        case 0x1: /* EOR */
        case 0x9: /* TEQ */
            result = rn_val ^ op2;
            break;
        case 0x2: /* SUB */
            result = cpu_add_carry(rn_val, ~op2, 1, &c_out, &v_out);
            break;
        case 0x3: /* RSB */
            result = cpu_add_carry(op2, ~rn_val, 1, &c_out, &v_out);
            break;
        case 0x4: /* ADD */
            result = cpu_add_carry(rn_val, op2, 0, &c_out, &v_out);
            break;
        case 0x5: /* ADC */
            result = cpu_add_carry(rn_val, op2, flag_c(cpu), &c_out, &v_out);
            break;
        case 0x6: /* SBC */
            result = cpu_add_carry(rn_val, ~op2, flag_c(cpu), &c_out, &v_out);
            break;
        case 0x7: /* RSC */
            result = cpu_add_carry(op2, ~rn_val, flag_c(cpu), &c_out, &v_out);
            break;
        case 0xA: /* CMP */
            result = cpu_add_carry(rn_val, ~op2, 1, &c_out, &v_out);
            break;
        case 0xB: /* CMN */
            result = cpu_add_carry(rn_val, op2, 0, &c_out, &v_out);
            break;
        case 0xC: /* ORR */
            result = rn_val | op2;
            break;
        case 0xD: /* MOV */
            result = op2;
            break;
        case 0xE: /* BIC */
            result = rn_val & ~op2;
            break;
        case 0xF: /* MVN */
            result = ~op2;
            break;
    }

    /* Arithmetic ops feed C/V from the adder; logic ops keep V and use the
     * shifter carry in C. */
    if (updates_flags) {
        int is_logic = (opcode <= 0x1) || opcode == 0x9 ||
                       (opcode >= 0xC && opcode <= 0xF) ||
                       opcode == 0x8;
        uint32_t c = is_logic ? carry : c_out;
        uint32_t v = is_logic ? (flag_v(cpu) ? 1u : 0u) : v_out;
        cpu_set_flags(cpu, result & (1u << 31), result == 0, c, v);
    }

    if (opcode >= 0x8 && opcode <= 0xB) {
        return; /* test ops write no register */
    }

    if (rd == 15) {
        if (s) {
            int bank = mode_to_bank(cpu);
            if (bank != BANK_USR) {
                cpu->cpsr = cpu->spsr[bank];
            }
        }
        /* On the ARM7TDMI a data processing instruction that writes R15 is a
         * branch: bit 0 of the result is ignored and T is left alone. Only
         * BX interworks, so the state does not follow bit 0. */
        cpu_set_pc(cpu, result & ~1u);
    } else {
        cpu_write_reg(cpu, rd, result);
    }
}

/* ---- single data transfer (LDR/STR) ------------------------------------- */

static void single_data_transfer(CPU *cpu, Memory *mem, uint32_t insn) {
    uint32_t p = (insn >> 24) & 1; /* pre/post index */
    uint32_t u = (insn >> 23) & 1; /* up/down */
    uint32_t b = (insn >> 22) & 1; /* byte/word */
    uint32_t w = (insn >> 21) & 1; /* write-back */
    uint32_t l = (insn >> 20) & 1; /* load/store */
    uint32_t rn = (insn >> 16) & 0xF;
    uint32_t rd = (insn >> 12) & 0xF;
    uint32_t offset;
    uint32_t base = (rn == 15) ? cpu->reg[15] : cpu_read_reg(cpu, rn);
    uint32_t addr;

    if (insn & (1 << 25)) {
        /* Register offset, optionally shifted. */
        uint32_t carry;
        offset = cpu_shift_c(cpu, (insn >> 5) & 3, cpu_read_reg(cpu, insn & 0xF),
                         (insn >> 7) & 0x1F, flag_c(cpu), &carry);
    } else {
        offset = insn & 0xFFF; /* the 12 bit field is a plain byte offset */
    }

    if (p) {
        addr = u ? base + offset : base - offset;
        if (w) {
            cpu_write_reg(cpu, rn, addr);
        }
    } else {
        addr = base;
        cpu_write_reg(cpu, rn, u ? base + offset : base - offset);
    }

    if (l) {
        uint32_t value = b ? memory_read8(mem, addr) : memory_read32(mem, addr);
        if (rd == 15) {
            cpu_set_pc(cpu, value & ~3u);
            if (value & 1) {
                cpu->cpsr |= FLAG_T;
            }
        } else {
            cpu_write_reg(cpu, rd, value);
        }
    } else {
        /* Rd == 15 stores PC + 12. */
        uint32_t value = (rd == 15) ? cpu->reg[15] + 4 : cpu_read_reg(cpu, rd);
        if (b) {
            memory_write8(mem, addr, (uint8_t)value);
        } else {
            memory_write32(mem, addr, value);
        }
    }
}

/* ---- extra load/store (halfword, signed byte, …) ------------------------ */

static void extra_transfer(CPU *cpu, Memory *mem, uint32_t insn, int width, int sign) {
    uint32_t p = (insn >> 24) & 1;
    uint32_t u = (insn >> 23) & 1;
uint32_t i = (insn >> 22) & 1; /* 1=imm offset, 0=register offset */
    uint32_t w = (insn >> 21) & 1;
    uint32_t l = (insn >> 20) & 1;
    uint32_t rn = (insn >> 16) & 0xF;
    uint32_t rd = (insn >> 12) & 0xF;
    uint32_t offset;
    uint32_t base = (rn == 15) ? cpu->reg[15] : cpu_read_reg(cpu, rn);
    uint32_t addr;

    if (i) {
        /* The immediate is the flat 8 bit imm4H:imm4L field, a byte offset
         * for LDRH/STRH, LDRSB and LDRSH alike. There is no scaled form on
         * this architecture: an offset of 0x10 is imm4H=1, imm4L=0. */
        offset = (((insn >> 8) & 0xF) << 4) | (insn & 0xF);
    } else {
        offset = cpu_read_reg(cpu, insn & 0xF);
    }

    if (p) {
        addr = u ? base + offset : base - offset;
        if (w) {
            cpu_write_reg(cpu, rn, addr);
        }
    } else {
        addr = base;
        cpu_write_reg(cpu, rn, u ? base + offset : base - offset);
    }

    if (l) {
        uint32_t value = (width == 1) ? memory_read8(mem, addr)
                                      : (uint32_t)memory_read16(mem, addr);
        if (sign) {
            value = (width == 1) ? (uint32_t)(int32_t)(int8_t)value
                                 : (uint32_t)(int32_t)(int16_t)value;
        }
        if (rd == 15) {
            cpu_set_pc(cpu, value & ~3u);
        } else {
            cpu_write_reg(cpu, rd, value);
        }
    } else {
        uint32_t value = (rd == 15) ? cpu->reg[15] + 4 : cpu_read_reg(cpu, rd);
        if (width == 1) {
            memory_write8(mem, addr, (uint8_t)value);
        } else {
            memory_write16(mem, addr, (uint16_t)value);
        }
    }
}

/* ---- block transfer (LDM/STM) ------------------------------------------- */

static unsigned popcount16(uint32_t x) {
    unsigned c = 0;
    for (int i = 0; i < 16; i++) {
        c += (x >> i) & 1;
    }
    return c;
}

static void block_transfer(CPU *cpu, Memory *mem, uint32_t insn) {
    uint32_t p = (insn >> 24) & 1;
    uint32_t u = (insn >> 23) & 1;
    uint32_t s = (insn >> 22) & 1;
    uint32_t w = (insn >> 21) & 1;
    uint32_t l = (insn >> 20) & 1;
    uint32_t rn = (insn >> 16) & 0xF;
    uint32_t rlist = insn & 0xFFFF;
    unsigned count = popcount16(rlist);

    uint32_t base = cpu_read_reg(cpu, rn);
    uint32_t start;

    if (u) {
        start = base;
        if (p) {
            start += 4;
        }
    } else {
        start = base - count * 4;
        if (!p) {
            start += 4;
        }
    }

    int force_user = (s && !l) || (s && l && !(rlist & (1u << 15)));

    uint32_t addr = start;
    for (int i = 0; i < 16; i++) {
        if (!(rlist & (1u << i))) {
            continue;
        }

        if (l) {
            uint32_t value = memory_read32(mem, addr);
            if (i == 15) {
                /* Loading PC. With S the saved PSR is restored first, and if
                 * the instruction was ARM the target's bit 0 selects Thumb -
                 * this is the longjmp / ARM->Thumb trampoline. */
                if (s) {
                    int bank = mode_to_bank(cpu);
                    if (bank != BANK_USR) {
                        int was_thumb = (cpu->cpsr & FLAG_T) != 0;
                        cpu->cpsr = cpu->spsr[bank];
                        if (was_thumb) {
                            cpu_set_pc(cpu, value & ~1u);
                        } else {
                            cpu_set_pc(cpu, value & ~3u);
                            if (value & 1) {
                                cpu->cpsr |= FLAG_T;
                            }
                        }
                    } else {
                        cpu_set_pc(cpu, value & ~3u);
                    }
                } else {
                    cpu_set_pc(cpu, value & ~3u);
                    if (value & 1) {
                        cpu->cpsr |= FLAG_T;
                    }
                }
            } else if (force_user && i >= 8) {
                /* write to user bank */
                uint32_t saved = cpu->cpsr;
                cpu->cpsr = (saved & ~0x1Fu) | MODE_USR;
                cpu_write_reg(cpu, i, value);
                cpu->cpsr = saved;
            } else {
                cpu_write_reg(cpu, i, value);
            }
        } else {
            uint32_t value;
            if (i == 15) {
                value = cpu->reg[15] + 4;
            } else if (force_user && i >= 8) {
                value = cpu_read_reg(cpu, i);
            } else {
                value = cpu_read_reg(cpu, i);
            }
            memory_write32(mem, addr, value);
        }
        addr += 4;
    }

    if (w) {
        cpu_write_reg(cpu, rn, u ? base + count * 4 : base - count * 4);
    }
}

/* ---- multiplier --------------------------------------------------------- */

static void multiply(CPU *cpu, uint32_t insn) {
    uint32_t a = (insn >> 21) & 1;
    uint32_t s = (insn >> 20) & 1;
    uint32_t rd = (insn >> 16) & 0xF;
    uint32_t rn = (insn >> 12) & 0xF;
    uint32_t rs = (insn >> 8) & 0xF;
    uint32_t rm = insn & 0xF;

    if (insn & (1 << 23)) {
        /* Long multiply (UMULL/SMULL/UMLAL/SMLAL). Bit 23 marks the long
         * form; bit 22 selects signed (1) or unsigned (0). The source
         * operands sit in the opposite fields from the 32-bit form:
         * Rm is in bits 11-8 and Rs is in bits 3-0. */
        int us = (insn >> 22) & 1;
        uint32_t rm_long = rs;   /* bits 11-8 */
        uint32_t rs_long = rm;   /* bits 3-0  */
        uint64_t m = cpu_read_reg(cpu, rm_long);
        uint64_t s2 = cpu_read_reg(cpu, rs_long);
        uint64_t product;
        if (us) {
            product = (uint64_t)((int64_t)(int32_t)m * (int64_t)(int32_t)s2);
        } else {
            product = m * s2;
        }
        if (a) {
            /* The 64-bit accumulator is RdHi:RdLo, i.e. {rd, rn}. */
            product += ((uint64_t)cpu_read_reg(cpu, rd) << 32) |
                       cpu_read_reg(cpu, rn);
        }
        cpu_write_reg(cpu, rd, (uint32_t)(product >> 32));
        cpu_write_reg(cpu, rn, (uint32_t)product);
        if (s) {
            cpu_set_flags(cpu, (product >> 63) & 1, (product >> 32) == 0,
                      flag_c(cpu), flag_v(cpu));
        }
    } else {
        uint32_t product = cpu_read_reg(cpu, rm) * cpu_read_reg(cpu, rs);
        if (a) {
            product += cpu_read_reg(cpu, rn);
        }
        cpu_write_reg(cpu, rd, product);
        if (s) {
            cpu_set_flags(cpu, product & (1u << 31), product == 0,
                      flag_c(cpu), flag_v(cpu));
        }
    }
}

/* ---- swap --------------------------------------------------------------- */

static void swap(CPU *cpu, Memory *mem, uint32_t insn) {
    int byte = (insn >> 22) & 1;
    uint32_t rn = (insn >> 16) & 0xF;
    uint32_t rd = (insn >> 12) & 0xF;
    uint32_t rm = insn & 0xF;
    uint32_t addr = cpu_read_reg(cpu, rn);
    uint32_t value = byte ? memory_read8(mem, addr) : memory_read32(mem, addr);
    cpu_write_reg(cpu, rd, value);
    if (byte) {
        memory_write8(mem, addr, cpu_read_reg(cpu, rm));
    } else {
        memory_write32(mem, addr, cpu_read_reg(cpu, rm));
    }
}

/* ---- PSR transfer (MRS/MSR) --------------------------------------------- */

/* MRS: 00010 R00 (10) R(15)(0) S(1111) Rd(0000) */
static int is_mrs(uint32_t insn) {
    return (insn & 0x0FFF0FFF) == 0x010F0000 ||
           (insn & 0x0FFF0FFF) == 0x014F0000;
}

/* MSR: 00010 R00 (1) 0 R(10) S(1111) Rd(0000) plus the register forms
 * 00010 R00 I P(0)(0)(1)(0)(0)(0)(0) S Rm(0000). */
static int is_msr(uint32_t insn) {
    return (insn & 0x0FF0FFF0) == 0x0120F000 ||
           (insn & 0x0FF0FFF0) == 0x0160F000 ||
           (insn & 0x0FF0FF00) == 0x0320F000 ||
           (insn & 0x0FF0FF00) == 0x0360F000;
}

static void mrs(CPU *cpu, uint32_t insn) {
    uint32_t rd = (insn >> 12) & 0xF;
    uint32_t value = (insn & (1 << 22))
                         ? cpu->spsr[mode_to_bank(cpu)]
                         : cpu->cpsr;
    cpu_write_reg(cpu, rd, value);
}

static void msr(CPU *cpu, uint32_t insn) {
    uint32_t mask = (insn >> 16) & 0xF; /* f s x c */
    uint32_t value;
    uint32_t to_spsr = (insn & (1 << 22)) != 0;
    uint32_t *target = to_spsr ? &cpu->spsr[mode_to_bank(cpu)] : &cpu->cpsr;
    uint32_t real_mask = 0;

    if (insn & (1 << 25)) {
        uint32_t carry;
        value = operand2(cpu, insn, &carry);
    } else {
        value = cpu_read_reg(cpu, insn & 0xF);
    }

    if (mask & 0x8) {
        real_mask |= 0xFF000000u;
    }
    if (mask & 0x1) {
        real_mask |= 0x000000FFu;
    }

    *target = (*target & ~real_mask) | (value & real_mask);
}

/* ---- branch ------------------------------------------------------------- */

static void branch(CPU *cpu, uint32_t insn) {
    uint32_t link = (insn >> 24) & 1;
    uint32_t offset = insn & 0xFFFFFF;
    if (offset & 0x800000) {
        offset -= 0x1000000; /* sign-extend 24-bit */
    }
    uint32_t target = cpu->reg[15] + (offset << 2);
    if (link) {
        cpu_write_reg(cpu, 14, cpu->reg[15] - 4);
    }
    cpu_set_pc(cpu, target);
}

static void bx(CPU *cpu, uint32_t insn) {
    uint32_t rm = insn & 0xF;
    uint32_t value = cpu_read_reg(cpu, rm);
    if (value & 1) {
        cpu->cpsr |= FLAG_T;
    } else {
        cpu->cpsr &= ~FLAG_T;
    }
    cpu_set_pc(cpu, value & ~1u);
}

/* ---- SWI ---------------------------------------------------------------- */

/* A software interrupt either goes to the BIOS vector (when a real BIOS image
 * is mapped) or to the high-level handler installed by the emulator. The hook
 * sees the SWI number and the return address in reg[15]. */
static void swi(CPU *cpu, uint32_t number) {
    if (cpu->swi_hook) {
        cpu->swi_hook(cpu->swi_ctx, number);
        return;
    }

    cpu->r14[BANK_SVC] = cpu->reg[15] - 4;
    cpu->spsr[BANK_SVC] = cpu->cpsr;
    cpu->cpsr = (cpu->cpsr & ~(0x1Fu | FLAG_T)) | MODE_SVC | FLAG_I;
    cpu_set_pc(cpu, 0x00000008);
}

/* ---- ARM instruction dispatch ------------------------------------------- */

static unsigned popcount16_insn(uint32_t x) {
    unsigned c = 0;
    for (int i = 0; i < 16; i++) {
        c += (x >> i) & 1;
    }
    return c;
}

static uint32_t arm_execute(CPU *cpu, Memory *mem, uint32_t insn) {
    uint32_t cond = insn >> 28;
    if (!condition_passed(cpu, cond)) {
        return 1;
    }

    /* BX:  cond 0001 0010 1111 1111 1111 0001 Rm
     * BLX: cond 0001 0010 1111 1111 1111 0011 Rm
     * BLX additionally stores the return address in LR. The BLX is an ARM
     * instruction, so the return address is the next ARM instruction and its
     * bit 0 is clear. */
    if ((insn & 0x0FFFFFF0) == 0x012FFF10) {
        bx(cpu, insn);
        return 3;
    }
    if ((insn & 0x0FFFFFF0) == 0x012FFF30) {
        cpu_write_reg(cpu, 14, cpu->reg[15] - 4);
        bx(cpu, insn);
        return 3;
    }

    switch ((insn >> 25) & 0x7) {
        case 0: /* 000 */
            if ((insn & 0x00000090) == 0x00000090) {
                /* multiply, extra load/store, or swap */
                uint32_t sh = insn & 0x00000060;
                if (sh == 0x20) {
                    extra_transfer(cpu, mem, insn, 2, 0);
                    return 2;
                } else if (sh == 0x40) {
                    extra_transfer(cpu, mem, insn, 1, 1);
                    return 2;
                } else if (sh == 0x60) {
                    extra_transfer(cpu, mem, insn, 2, 1);
                    return 2;
                } else if (insn & (1 << 24)) {
                    swap(cpu, mem, insn);
                    return 5;
                } else {
                    multiply(cpu, insn);
                    return 4;
                }
            } else if (is_mrs(insn)) {
                mrs(cpu, insn);
                return 1;
            } else if (is_msr(insn)) {
                msr(cpu, insn);
                return 1;
            } else {
                data_processing(cpu, insn);
                return 1;
            }

        case 1: /* 001: data processing, immediate operand2 */
            data_processing(cpu, insn);
            return 1;

        case 2: /* 010: LDR/STR class */
        case 3:
            single_data_transfer(cpu, mem, insn);
            return 2;

        case 4: /* 100: LDM/STM */
            block_transfer(cpu, mem, insn);
            return 1 + popcount16_insn(insn & 0xFFFF);

        case 5: /* 101: B/BL */
            branch(cpu, insn);
            return 3;

        case 6: /* 110: coprocessor data ops (NOP on ARM7TDMI) */
        case 7: /* 111: MCR/MRC/SWI */
            if ((insn & 0x0F000000) == 0x0F000000) {
                /* ARM carries the service number in the low byte of the 24 bit
                 * immediate (swi 0xNN assembles to 0xEF0000NN). */
                swi(cpu, insn & 0xFF);
                return 3;
            }
            /* else: coprocessor access, ignore */
            return 1;
    }

    return 1;
}

/* ---- public API --------------------------------------------------------- */

void cpu_init(CPU *cpu) {
    cpu_reset(cpu);
}

void cpu_reset(CPU *cpu) {
    for (int i = 0; i < 16; i++) {
        cpu->reg[i] = 0;
    }
    for (int i = 0; i < 5; i++) {
        cpu->r8_12_fiq[i] = 0;
    }
    for (int i = 0; i < BANK_COUNT; i++) {
        cpu->r13[i] = 0;
        cpu->r14[i] = 0;
        cpu->spsr[i] = 0;
    }
    cpu->cpsr = FLAG_I | MODE_SVC;
    cpu->halted = false;
    cpu->wait_mode = CPU_RUN;
}

void cpu_set_wait(CPU *cpu, int mode) {
    cpu->wait_mode = mode;
    cpu->halted = (mode != CPU_RUN);
}

uint32_t cpu_step(CPU *cpu, Memory *mem) {
    uint32_t addr = cpu->reg[15];

    if (cpu->halted) {
        return 1;
    }

    if (cpu->cpsr & FLAG_T) {
        uint16_t insn = memory_read16(mem, addr);
        cpu->pc_written = false;
        cpu->reg[15] = addr + 4;
        uint32_t cycles = thumb_execute(cpu, mem, insn);

        /* If execution did not branch, advance to the next instruction. */
        if (!cpu->pc_written) {
            cpu->reg[15] = addr + 2;
        }
        return cycles;
    }

    uint32_t insn = memory_read32(mem, addr);
    cpu->pc_written = false;
    cpu->reg[15] = addr + 8;
    uint32_t cycles = arm_execute(cpu, mem, insn);

    /* If execution did not branch, advance to the next instruction. */
    if (!cpu->pc_written) {
        cpu->reg[15] = addr + 4;
    }
    return cycles;
}

void cpu_irq(CPU *cpu) {
    cpu->halted = false;
    cpu->wait_mode = CPU_RUN;
    cpu->r14[BANK_IRQ] = cpu->reg[15] + 4;
    cpu->spsr[BANK_IRQ] = cpu->cpsr;
    cpu->cpsr = (cpu->cpsr & ~(0x1Fu | FLAG_T)) | MODE_IRQ | FLAG_I;
    cpu->reg[15] = 0x00000018;
}