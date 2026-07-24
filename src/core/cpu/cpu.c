#include "cpu/cpu.h"

#include <stdlib.h>

// ---------------------------------------------------------------------------
// Physical address mask. The 11/70 is a 22-bit machine, but until the MMU and
// Unibus map land (COMPLETION_PLAN P3/P4) we run unmapped in the low 16-bit
// space, so every effective address is masked to 16 bits.
// ---------------------------------------------------------------------------
#define ADDR_MASK 0177777u // 0xFFFF

pdp11_cpu *pdp11_cpu_create(void) {
    pdp11_cpu *cpu = calloc(1, sizeof *cpu);
    if (cpu == NULL) {
        return NULL;
    }
    cpu->mem = calloc(1, sizeof *cpu->mem);
    if (cpu->mem == NULL) {
        free(cpu);
        return NULL;
    }
    pdp11_cpu_reset(cpu);
    return cpu;
}

void pdp11_cpu_destroy(pdp11_cpu *cpu) {
    if (cpu != NULL) {
        free(cpu->mem);
        free(cpu);
    }
}

void pdp11_cpu_reset(pdp11_cpu *cpu) {
    for (int i = 0; i < 8; ++i) {
        cpu->r[i] = 0;
    }
    cpu->psw = 0;
    cpu->halted = false;
    cpu->instr_count = 0;
}

// --- Instruction fetch ------------------------------------------------------
// Read the word at the PC and advance the PC by two.
static uint16_t fetch(pdp11_cpu *cpu) {
    uint16_t word = pdp11_mem_read_word(cpu->mem, cpu->r[PDP11_PC]);
    cpu->r[PDP11_PC] = (uint16_t)(cpu->r[PDP11_PC] + 2u);
    return word;
}

// --- Operand resolution -----------------------------------------------------
// A resolved operand is either a register (is_reg) or a memory address. The
// autoincrement/autodecrement side effects happen exactly once, here, in the
// hardware's source-then-destination order (the caller decodes src before dst).
typedef struct {
    bool is_reg;
    uint8_t reg;   // valid when is_reg
    uint32_t addr; // valid when !is_reg (16-bit for now)
} operand;

// Autoincrement/decrement step: 2 for words; 1 for bytes, except SP and PC
// which always step by 2 to stay word-aligned.
static uint16_t operand_step(uint8_t reg, bool bytemode) {
    return (bytemode && reg < PDP11_SP) ? 1u : 2u;
}

static operand decode_operand(pdp11_cpu *cpu, uint8_t spec, bool bytemode) {
    uint8_t mode = (uint8_t)((spec >> 3) & 07u);
    uint8_t reg = (uint8_t)(spec & 07u);
    operand op = {0};

    switch (mode) {
    case 0: // Rn — register direct
        op.is_reg = true;
        op.reg = reg;
        break;
    case 1: // (Rn) — register deferred
        op.addr = cpu->r[reg];
        break;
    case 2: // (Rn)+ — autoincrement  (PC: immediate #n)
        op.addr = cpu->r[reg];
        cpu->r[reg] = (uint16_t)(cpu->r[reg] + operand_step(reg, bytemode));
        break;
    case 3: // @(Rn)+ — autoincrement deferred  (PC: absolute @#A)
        op.addr = pdp11_mem_read_word(cpu->mem, cpu->r[reg]);
        cpu->r[reg] = (uint16_t)(cpu->r[reg] + 2u);
        break;
    case 4: // -(Rn) — autodecrement
        cpu->r[reg] = (uint16_t)(cpu->r[reg] - operand_step(reg, bytemode));
        op.addr = cpu->r[reg];
        break;
    case 5: // @-(Rn) — autodecrement deferred
        cpu->r[reg] = (uint16_t)(cpu->r[reg] - 2u);
        op.addr = pdp11_mem_read_word(cpu->mem, cpu->r[reg]);
        break;
    case 6: { // X(Rn) — index  (PC: relative)
        uint16_t base = fetch(cpu);
        op.addr = (uint16_t)(base + cpu->r[reg]);
        break;
    }
    case 7: { // @X(Rn) — index deferred  (PC: relative deferred)
        uint16_t base = fetch(cpu);
        uint16_t ptr = (uint16_t)(base + cpu->r[reg]);
        op.addr = pdp11_mem_read_word(cpu->mem, ptr);
        break;
    }
    default: // unreachable: mode is masked to 3 bits
        break;
    }

    op.addr &= ADDR_MASK;
    return op;
}

static uint16_t read_word_operand(const pdp11_cpu *cpu, operand op) {
    return op.is_reg ? cpu->r[op.reg] : pdp11_mem_read_word(cpu->mem, op.addr);
}

static void write_word_operand(pdp11_cpu *cpu, operand op, uint16_t value) {
    if (op.is_reg) {
        cpu->r[op.reg] = value;
    } else {
        pdp11_mem_write_word(cpu->mem, op.addr, value);
    }
}

// --- Condition codes --------------------------------------------------------
static void set_flag(pdp11_cpu *cpu, uint16_t flag, bool on) {
    if (on) {
        cpu->psw |= flag;
    } else {
        cpu->psw = (uint16_t)(cpu->psw & ~flag);
    }
}

static void set_nz_word(pdp11_cpu *cpu, uint16_t result) {
    set_flag(cpu, PDP11_PSW_N, (result & 0100000u) != 0);
    set_flag(cpu, PDP11_PSW_Z, result == 0);
}

// --- Instructions -----------------------------------------------------------
// MOV: src -> dst. N,Z set from the value moved; V cleared; C unchanged.
static void op_mov(pdp11_cpu *cpu, uint16_t word) {
    operand src = decode_operand(cpu, (uint8_t)((word >> 6) & 077u), false);
    uint16_t value = read_word_operand(cpu, src);
    operand dst = decode_operand(cpu, (uint8_t)(word & 077u), false);
    write_word_operand(cpu, dst, value);
    set_nz_word(cpu, value);
    set_flag(cpu, PDP11_PSW_V, false);
}

// ADD: dst = dst + src. N,Z from result; V = signed overflow; C = carry out.
static void op_add(pdp11_cpu *cpu, uint16_t word) {
    operand src = decode_operand(cpu, (uint8_t)((word >> 6) & 077u), false);
    uint16_t s = read_word_operand(cpu, src);
    operand dst = decode_operand(cpu, (uint8_t)(word & 077u), false);
    uint16_t d = read_word_operand(cpu, dst);

    uint32_t sum = (uint32_t)d + (uint32_t)s;
    uint16_t result = (uint16_t)sum;
    write_word_operand(cpu, dst, result);

    set_nz_word(cpu, result);
    // Overflow when the operands share a sign that differs from the result's.
    set_flag(cpu, PDP11_PSW_V,
             (((~(s ^ d)) & (s ^ result)) & 0100000u) != 0);
    set_flag(cpu, PDP11_PSW_C, (sum & 0200000u) != 0);
}

// --- Step -------------------------------------------------------------------
void pdp11_cpu_step(pdp11_cpu *cpu) {
    if (cpu->halted) {
        return;
    }

    uint16_t word = fetch(cpu);
    uint8_t opcode = (uint8_t)((word >> 12) & 017u);

    switch (opcode) {
    case 001: // MOV (010000)
        op_mov(cpu, word);
        break;
    case 006: // ADD (060000)
        op_add(cpu, word);
        break;
    case 000:
        if (word == 0) { // HALT (000000)
            cpu->halted = true;
        }
        break;
    default:
        // Unimplemented opcodes are no-ops for this first slice; the real core
        // will trap to 4 (illegal/reserved instruction) once traps land (P2).
        break;
    }

    cpu->instr_count++;
}
