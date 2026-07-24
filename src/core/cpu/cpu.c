#include "cpu/cpu.h"

#include <stdlib.h>

// ---------------------------------------------------------------------------
// Physical address mask. The 11/70 is a 22-bit machine, but until the MMU and
// Unibus map land (COMPLETION_PLAN P3/P4) we run unmapped in the low 16-bit
// space, so every effective address is masked to 16 bits.
// ---------------------------------------------------------------------------
#define ADDR_MASK 0177777u // 0xFFFF

// Trap/interrupt vectors (kernel-mode only until the MMU adds modes at P3).
#define VEC_BUS      0004u // odd address / bus error / stack limit / nxm
#define VEC_RESERVED 0010u // reserved / illegal instruction
#define VEC_BPT      0014u // BPT and the T-bit trace trap
#define VEC_IOT      0020u // IOT
#define VEC_EMT      0030u // EMT
#define VEC_TRAP     0034u // TRAP
#define VEC_PIRQ     0240u // program interrupt request

#define IOPAGE_PIRQ 0177772u // program interrupt request register

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
    cpu->waiting = false;
    cpu->trace_pending = false;
    cpu->pirq = 0;
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

// Width-dependent constants: byte ops work in an 8-bit field, words in 16.
static uint16_t width_msb(bool bytemode) { return bytemode ? 0200u : 0100000u; }
static uint16_t width_mask(bool bytemode) { return bytemode ? 0377u : 0177777u; }
static uint32_t width_carry(bool bytemode) { return bytemode ? 0400u : 0200000u; }

// The Processor Status word is memory-mapped at the top of the I/O page. Full
// Unibus I/O-page decoding arrives with the devices (P6); for now only the PSW
// register is decoded so software (and probes) can read/write it the hardware
// way. The address is the 16-bit alias of 17777776.
#define IOPAGE_PSW 0177776u

// Raise a bus/odd-address (or, later, MMU) fault: record the vector and unwind
// to the setjmp at the top of pdp11_cpu_step, aborting the instruction.
static void cpu_bus_fault(pdp11_cpu *cpu, uint16_t vector) {
    cpu->abort_vec = vector;
    longjmp(cpu->abort_env, 1);
}

// Highest program-interrupt-request level pending (7..1), or 0 if none.
// PIR1..PIR7 occupy PIRQ bits 9..15.
static int highest_pir_level(uint16_t pirq) {
    for (int level = 7; level >= 1; --level) {
        if (pirq & (uint16_t)(1u << (level + 8))) {
            return level;
        }
    }
    return 0;
}

// PIRQ write (put_PIRQ): keep the request bits and echo the highest pending
// level into bits 7-5 and 3-1, matching the hardware read-back.
static void put_pirq(pdp11_cpu *cpu, uint16_t value) {
    uint16_t req = (uint16_t)(value & 0177000u);
    int level = highest_pir_level(req);
    uint16_t encoded = level ? (uint16_t)((level << 5) | (level << 1)) : 0u;
    cpu->pirq = (uint16_t)(req | encoded);
}

static uint16_t cpu_read_word(pdp11_cpu *cpu, uint32_t addr) {
    if (addr & 1u) { // a word reference to an odd address traps through vector 4
        cpu_bus_fault(cpu, VEC_BUS);
    }
    switch (addr & 0177777u) {
    case IOPAGE_PSW:  return cpu->psw;
    case IOPAGE_PIRQ: return cpu->pirq;
    default:          return pdp11_mem_read_word(cpu->mem, addr);
    }
}

static void cpu_write_word(pdp11_cpu *cpu, uint32_t addr, uint16_t value) {
    if (addr & 1u) {
        cpu_bus_fault(cpu, VEC_BUS);
    }
    switch (addr & 0177777u) {
    case IOPAGE_PSW:  cpu->psw = value; break; // T-bit rules land with full I/O
    case IOPAGE_PIRQ: put_pirq(cpu, value); break;
    default:          pdp11_mem_write_word(cpu->mem, addr, value); break;
    }
}

// Read/write an operand at the natural width. A byte read yields 0..255; a byte
// write to a register touches only the low byte (MOVB's sign-extension is the
// one exception, handled in op_mov).
static uint16_t read_operand(pdp11_cpu *cpu, operand op, bool bytemode) {
    if (op.is_reg) {
        return bytemode ? (uint16_t)(cpu->r[op.reg] & 0377u) : cpu->r[op.reg];
    }
    return bytemode ? pdp11_mem_read_byte(cpu->mem, op.addr)
                    : cpu_read_word(cpu, op.addr);
}

static void write_operand(pdp11_cpu *cpu, operand op, bool bytemode,
                          uint16_t value) {
    if (op.is_reg) {
        cpu->r[op.reg] = bytemode
            ? (uint16_t)((cpu->r[op.reg] & 0177400u) | (value & 0377u))
            : value;
    } else if (bytemode) {
        pdp11_mem_write_byte(cpu->mem, op.addr, (uint8_t)value);
    } else {
        cpu_write_word(cpu, op.addr, value);
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

static bool flag_set(const pdp11_cpu *cpu, uint16_t flag) {
    return (cpu->psw & flag) != 0;
}

static void set_nz(pdp11_cpu *cpu, uint16_t result, bool bytemode) {
    set_flag(cpu, PDP11_PSW_N, (result & width_msb(bytemode)) != 0);
    set_flag(cpu, PDP11_PSW_Z, (result & width_mask(bytemode)) == 0);
}

// N ^ C, the overflow rule shared by the shift/rotate instructions.
static void set_v_from_nc(pdp11_cpu *cpu) {
    set_flag(cpu, PDP11_PSW_V,
             flag_set(cpu, PDP11_PSW_N) != flag_set(cpu, PDP11_PSW_C));
}

// --- Arithmetic primitives (produce result + set N,Z,V,C) -------------------
// x + y. Carry = carry out of the MSB; V = signed overflow.
static uint16_t alu_add(pdp11_cpu *cpu, uint16_t x, uint16_t y, bool bytemode) {
    uint16_t mask = width_mask(bytemode);
    uint32_t sum = (uint32_t)(x & mask) + (uint32_t)(y & mask);
    uint16_t result = (uint16_t)(sum & mask);
    set_nz(cpu, result, bytemode);
    set_flag(cpu, PDP11_PSW_V,
             (((~(x ^ y)) & (x ^ result)) & width_msb(bytemode)) != 0);
    set_flag(cpu, PDP11_PSW_C, (sum & width_carry(bytemode)) != 0);
    return result;
}

// x - y computed as x + ~y + 1. On the PDP-11, C is the *borrow*: set when
// there is no carry out of the MSB. V = signed overflow of the subtraction.
static uint16_t alu_sub(pdp11_cpu *cpu, uint16_t x, uint16_t y, bool bytemode) {
    uint16_t mask = width_mask(bytemode);
    uint32_t sum = (uint32_t)(x & mask) + (uint32_t)(~y & mask) + 1u;
    uint16_t result = (uint16_t)(sum & mask);
    set_nz(cpu, result, bytemode);
    set_flag(cpu, PDP11_PSW_V,
             (((x ^ y) & (x ^ result)) & width_msb(bytemode)) != 0);
    set_flag(cpu, PDP11_PSW_C, (sum & width_carry(bytemode)) == 0);
    return result;
}

// --- Double-operand instructions --------------------------------------------
// Decode src then dst (side effects in that order), returning the read values.
typedef struct { operand src, dst; uint16_t s, d; } double_op;

static double_op decode_double(pdp11_cpu *cpu, uint16_t word, bool bytemode,
                               bool read_dst) {
    double_op o;
    o.src = decode_operand(cpu, (uint8_t)((word >> 6) & 077u), bytemode);
    o.s = read_operand(cpu, o.src, bytemode);
    o.dst = decode_operand(cpu, (uint8_t)(word & 077u), bytemode);
    o.d = read_dst ? read_operand(cpu, o.dst, bytemode) : 0u;
    return o;
}

static void op_mov(pdp11_cpu *cpu, uint16_t word, bool bytemode) {
    operand src = decode_operand(cpu, (uint8_t)((word >> 6) & 077u), bytemode);
    uint16_t value = read_operand(cpu, src, bytemode);
    operand dst = decode_operand(cpu, (uint8_t)(word & 077u), bytemode);

    // MOVB into a register sign-extends the byte through all 16 bits.
    if (bytemode && dst.is_reg) {
        uint16_t ext = (value & 0200u) ? (uint16_t)(value | 0177400u)
                                       : (uint16_t)(value & 0377u);
        cpu->r[dst.reg] = ext;
    } else {
        write_operand(cpu, dst, bytemode, value);
    }
    set_nz(cpu, value, bytemode);
    set_flag(cpu, PDP11_PSW_V, false);
}

static void op_cmp(pdp11_cpu *cpu, uint16_t word, bool bytemode) {
    double_op o = decode_double(cpu, word, bytemode, true);
    alu_sub(cpu, o.s, o.d, bytemode); // src - dst; result discarded
}

static void op_add(pdp11_cpu *cpu, uint16_t word) {
    double_op o = decode_double(cpu, word, false, true);
    write_operand(cpu, o.dst, false, alu_add(cpu, o.d, o.s, false));
}

static void op_sub(pdp11_cpu *cpu, uint16_t word) {
    double_op o = decode_double(cpu, word, false, true);
    write_operand(cpu, o.dst, false, alu_sub(cpu, o.d, o.s, false)); // dst - src
}

// BIT/BIC/BIS share the N,Z-from-result / V=0 / C-unchanged pattern.
static void op_bit(pdp11_cpu *cpu, uint16_t word, bool bytemode) {
    double_op o = decode_double(cpu, word, bytemode, true);
    set_nz(cpu, (uint16_t)(o.s & o.d), bytemode);
    set_flag(cpu, PDP11_PSW_V, false);
}

static void op_bic(pdp11_cpu *cpu, uint16_t word, bool bytemode) {
    double_op o = decode_double(cpu, word, bytemode, true);
    uint16_t result = (uint16_t)(o.d & ~o.s);
    write_operand(cpu, o.dst, bytemode, result);
    set_nz(cpu, result, bytemode);
    set_flag(cpu, PDP11_PSW_V, false);
}

static void op_bis(pdp11_cpu *cpu, uint16_t word, bool bytemode) {
    double_op o = decode_double(cpu, word, bytemode, true);
    uint16_t result = (uint16_t)(o.d | o.s);
    write_operand(cpu, o.dst, bytemode, result);
    set_nz(cpu, result, bytemode);
    set_flag(cpu, PDP11_PSW_V, false);
}

// --- Single-operand instructions --------------------------------------------
// `base` is the 9-bit opcode with the byte flag stripped (0050..0067, 0003).
static void single_op(pdp11_cpu *cpu, uint16_t word, uint16_t base,
                      bool bytemode) {
    operand dst = decode_operand(cpu, (uint8_t)(word & 077u), bytemode);
    uint16_t mask = width_mask(bytemode);
    uint16_t msb = width_msb(bytemode);
    uint16_t d = read_operand(cpu, dst, bytemode);
    uint16_t result = 0;
    bool store = true;

    switch (base) {
    case 0050: // CLR
        result = 0;
        set_flag(cpu, PDP11_PSW_N, false);
        set_flag(cpu, PDP11_PSW_Z, true);
        set_flag(cpu, PDP11_PSW_V, false);
        set_flag(cpu, PDP11_PSW_C, false);
        break;
    case 0051: // COM: ones-complement
        result = (uint16_t)(~d & mask);
        set_nz(cpu, result, bytemode);
        set_flag(cpu, PDP11_PSW_V, false);
        set_flag(cpu, PDP11_PSW_C, true);
        break;
    case 0052: // INC: V set only on max-positive -> min-negative
        result = (uint16_t)((d + 1u) & mask);
        set_nz(cpu, result, bytemode);
        set_flag(cpu, PDP11_PSW_V, (d & mask) == (uint16_t)(msb - 1u));
        break;
    case 0053: // DEC: V set only on min-negative -> max-positive
        result = (uint16_t)((d - 1u) & mask);
        set_nz(cpu, result, bytemode);
        set_flag(cpu, PDP11_PSW_V, (d & mask) == msb);
        break;
    case 0054: // NEG: two's-complement negate
        result = (uint16_t)((0u - d) & mask);
        set_nz(cpu, result, bytemode);
        set_flag(cpu, PDP11_PSW_V, result == msb);
        set_flag(cpu, PDP11_PSW_C, result != 0);
        break;
    case 0055: { // ADC: add carry in
        uint16_t c = flag_set(cpu, PDP11_PSW_C) ? 1u : 0u;
        result = (uint16_t)((d + c) & mask);
        set_nz(cpu, result, bytemode);
        set_flag(cpu, PDP11_PSW_V, c && (d & mask) == (uint16_t)(msb - 1u));
        set_flag(cpu, PDP11_PSW_C, c && (d & mask) == mask);
        break;
    }
    case 0056: { // SBC: subtract carry (borrow) in
        uint16_t c = flag_set(cpu, PDP11_PSW_C) ? 1u : 0u;
        result = (uint16_t)((d - c) & mask);
        set_nz(cpu, result, bytemode);
        set_flag(cpu, PDP11_PSW_V, (d & mask) == msb);
        set_flag(cpu, PDP11_PSW_C, c && (d & mask) == 0u);
        break;
    }
    case 0057: // TST: compare against 0, no store
        result = d;
        store = false;
        set_nz(cpu, d, bytemode);
        set_flag(cpu, PDP11_PSW_V, false);
        set_flag(cpu, PDP11_PSW_C, false);
        break;
    case 0060: { // ROR: rotate right through carry
        uint16_t cin = flag_set(cpu, PDP11_PSW_C) ? msb : 0u;
        set_flag(cpu, PDP11_PSW_C, (d & 1u) != 0);
        result = (uint16_t)(((d & mask) >> 1) | cin);
        set_nz(cpu, result, bytemode);
        set_v_from_nc(cpu);
        break;
    }
    case 0061: { // ROL: rotate left through carry
        uint16_t cin = flag_set(cpu, PDP11_PSW_C) ? 1u : 0u;
        set_flag(cpu, PDP11_PSW_C, (d & msb) != 0);
        result = (uint16_t)(((d << 1) | cin) & mask);
        set_nz(cpu, result, bytemode);
        set_v_from_nc(cpu);
        break;
    }
    case 0062: // ASR: arithmetic shift right (sign-preserving)
        set_flag(cpu, PDP11_PSW_C, (d & 1u) != 0);
        result = (uint16_t)(((d & mask) >> 1) | (d & msb));
        set_nz(cpu, result, bytemode);
        set_v_from_nc(cpu);
        break;
    case 0063: // ASL: arithmetic shift left
        set_flag(cpu, PDP11_PSW_C, (d & msb) != 0);
        result = (uint16_t)((d << 1) & mask);
        set_nz(cpu, result, bytemode);
        set_v_from_nc(cpu);
        break;
    case 0003: // SWAB: swap bytes (word only); N,Z from the new low byte
        result = (uint16_t)((((uint32_t)d << 8) | ((uint32_t)d >> 8)) & 0177777u);
        set_flag(cpu, PDP11_PSW_N, (result & 0200u) != 0);
        set_flag(cpu, PDP11_PSW_Z, (result & 0377u) == 0);
        set_flag(cpu, PDP11_PSW_V, false);
        set_flag(cpu, PDP11_PSW_C, false);
        break;
    case 0067: // SXT: fill dst with the N bit; Z = !N
        result = flag_set(cpu, PDP11_PSW_N) ? mask : 0u;
        set_flag(cpu, PDP11_PSW_Z, !flag_set(cpu, PDP11_PSW_N));
        set_flag(cpu, PDP11_PSW_V, false);
        break;
    default: // 0064/0065/0066 (MARK/MTPS/MFP*) land in later phases
        store = false;
        break;
    }

    if (store) {
        write_operand(cpu, dst, bytemode, result);
    }
}

// --- Decode & step ----------------------------------------------------------
// Returns true if this word was a recognised single-operand instruction.
// MARK nn (0064nn): subroutine-return stack cleanup.
//   i = PC + 2*nn;  PC = R5;  R5 = mem[i];  SP = i + 2
static void op_mark(pdp11_cpu *cpu, uint16_t word) {
    uint16_t nn = (uint16_t)(word & 077u);
    uint16_t i = (uint16_t)(cpu->r[PDP11_PC] + 2u * nn);
    cpu->r[PDP11_PC] = cpu->r[5];
    cpu->r[5] = cpu_read_word(cpu, i);
    cpu->r[PDP11_SP] = (uint16_t)(i + 2u);
}

static bool try_single_op(pdp11_cpu *cpu, uint16_t word) {
    uint16_t code = (uint16_t)((word >> 6) & 01777u); // bits 15-6
    bool bytemode = (code & 01000u) != 0;
    uint16_t base = (uint16_t)(code & 0777u);

    if (base == 0003 && !bytemode) { // SWAB (word only)
        single_op(cpu, word, 0003, false);
        return true;
    }
    if (base == 0064 && !bytemode) { // MARK
        op_mark(cpu, word);
        return true;
    }
    if (base >= 0050 && base <= 0063) { // CLR..ASL (word + byte)
        single_op(cpu, word, base, bytemode);
        return true;
    }
    if (!bytemode && base == 0067) { // SXT (word)
        single_op(cpu, word, 0067, false);
        return true;
    }
    // Not on the 11/70 / handled elsewhere: MARK (word 0064), MFPI/MTPI/MFPD/
    // MTPD (0065/0066, MMU — P3), MTPS/MFPS (byte 0064/0067 — LSI models only,
    // illegal on the 11/70). These currently fall through; a reserved-
    // instruction trap for the truly-illegal ones lands in P2b.
    return false;
}

// --- Stack (R6) -------------------------------------------------------------
static void push_word(pdp11_cpu *cpu, uint16_t value) {
    cpu->r[PDP11_SP] = (uint16_t)(cpu->r[PDP11_SP] - 2u);
    pdp11_mem_write_word(cpu->mem, cpu->r[PDP11_SP] & ADDR_MASK, value);
}

static uint16_t pop_word(pdp11_cpu *cpu) {
    uint16_t value = pdp11_mem_read_word(cpu->mem, cpu->r[PDP11_SP] & ADDR_MASK);
    cpu->r[PDP11_SP] = (uint16_t)(cpu->r[PDP11_SP] + 2u);
    return value;
}

// --- Branches ---------------------------------------------------------------
// A branch is identified by its high byte; the low byte is a signed word offset.
static bool is_branch(uint16_t hb) {
    return (hb >= 0001 && hb <= 0007) || (hb >= 0200 && hb <= 0207);
}

static bool branch_taken(const pdp11_cpu *cpu, uint16_t hb) {
    bool n = flag_set(cpu, PDP11_PSW_N);
    bool z = flag_set(cpu, PDP11_PSW_Z);
    bool v = flag_set(cpu, PDP11_PSW_V);
    bool c = flag_set(cpu, PDP11_PSW_C);
    switch (hb) {
    case 0001: return true;             // BR
    case 0002: return !z;               // BNE
    case 0003: return z;                // BEQ
    case 0004: return (n ^ v) == 0;     // BGE
    case 0005: return (n ^ v) != 0;     // BLT
    case 0006: return !(z || (n ^ v));  // BGT
    case 0007: return z || (n ^ v);     // BLE
    case 0200: return !n;               // BPL
    case 0201: return n;                // BMI
    case 0202: return !c && !z;         // BHI
    case 0203: return c || z;           // BLOS
    case 0204: return !v;               // BVC
    case 0205: return v;                // BVS
    case 0206: return !c;               // BCC / BHIS
    case 0207: return c;                // BCS / BLO
    default:   return false;
    }
}

static void op_branch(pdp11_cpu *cpu, uint16_t word, uint16_t hb) {
    if (branch_taken(cpu, hb)) {
        int offset = (int8_t)(word & 0377u); // sign-extend the byte
        cpu->r[PDP11_PC] = (uint16_t)(cpu->r[PDP11_PC] + (uint16_t)(offset * 2));
    }
}

static void do_trap(pdp11_cpu *cpu, uint16_t vector); // defined below

// --- Control transfer -------------------------------------------------------
// JMP dst: PC := effective address of dst. A register operand has no address, so
// on the 11/70 (which lacks HAS_JREG4) JMP/JSR to a register is illegal and
// traps through vector 010.
static void op_jmp(pdp11_cpu *cpu, uint16_t word) {
    operand dst = decode_operand(cpu, (uint8_t)(word & 077u), false);
    if (dst.is_reg) {
        do_trap(cpu, VEC_RESERVED);
    } else {
        cpu->r[PDP11_PC] = (uint16_t)dst.addr;
    }
}

static void op_jsr(pdp11_cpu *cpu, uint16_t word) {
    uint8_t reg = (uint8_t)((word >> 6) & 07u);
    operand dst = decode_operand(cpu, (uint8_t)(word & 077u), false);
    if (dst.is_reg) {
        do_trap(cpu, VEC_RESERVED); // JSR to a register is illegal on the 11/70
        return;
    }
    push_word(cpu, cpu->r[reg]);
    cpu->r[reg] = cpu->r[PDP11_PC];
    cpu->r[PDP11_PC] = (uint16_t)dst.addr;
}

static void op_rts(pdp11_cpu *cpu, uint16_t word) {
    uint8_t reg = (uint8_t)(word & 07u);
    cpu->r[PDP11_PC] = cpu->r[reg];
    cpu->r[reg] = pop_word(cpu);
}

// SOB reg,offset: decrement reg; if non-zero, branch back by offset words.
static void op_sob(pdp11_cpu *cpu, uint16_t word) {
    uint8_t reg = (uint8_t)((word >> 6) & 07u);
    uint16_t offset = (uint16_t)(word & 077u);
    cpu->r[reg] = (uint16_t)(cpu->r[reg] - 1u);
    if (cpu->r[reg] != 0) {
        cpu->r[PDP11_PC] = (uint16_t)(cpu->r[PDP11_PC] - offset * 2u);
    }
}

// Condition-code operators (0002 40-77): bit 4 selects set vs clear, the low
// nibble names the flags — which map directly onto the PSW's low four bits.
static void op_ccops(pdp11_cpu *cpu, uint16_t word) {
    uint16_t nibble = (uint16_t)(word & 017u);
    if (word & 020u) {
        cpu->psw |= nibble;
    } else {
        cpu->psw = (uint16_t)(cpu->psw & ~nibble);
    }
}

// --- Traps ------------------------------------------------------------------
// Enter a trap: push the old PSW then the old PC on the current stack, and load
// the new PC/PSW from the vector. PC is pushed last so RTI/RTT pop it first.
static void do_trap(pdp11_cpu *cpu, uint16_t vector) {
    uint16_t old_psw = cpu->psw;
    uint16_t new_pc = pdp11_mem_read_word(cpu->mem, vector);
    uint16_t new_psw = pdp11_mem_read_word(cpu->mem, vector + 2u);
    push_word(cpu, old_psw);
    push_word(cpu, cpu->r[PDP11_PC]);
    cpu->r[PDP11_PC] = new_pc;
    cpu->psw = new_psw;
}

// RTI/RTT: pop PC then PSW. On the 11/70 an RTI (but not RTT) that restores the
// T bit takes the trace trap *immediately*, before the instruction at the
// restored PC; RTT defers to the normal after-instruction rule. This is the one
// documented behavioural difference between the two (KB11-C; verified vs SimH).
static void op_rti(pdp11_cpu *cpu, bool is_rtt) {
    cpu->r[PDP11_PC] = pop_word(cpu);
    cpu->psw = pop_word(cpu);
    if (!is_rtt && (cpu->psw & PDP11_PSW_T)) {
        cpu->trace_pending = true;
    }
}

// --- EIS: MUL, DIV, ASH, ASHC, XOR ------------------------------------------
// Register is bits 8-6; the operand is the low 6 bits. Semantics mirror the
// KB11-C exactly (SimH pdp11_cpu.c case 007), including the J11/11-70 divide
// error compatibility cases. Verified against the oracle by the `eis` probe.
static int16_t sext16(uint16_t v) { return (int16_t)v; }

static void op_mul(pdp11_cpu *cpu, uint16_t word) {
    uint8_t reg = (uint8_t)((word >> 6) & 07u);
    operand s = decode_operand(cpu, (uint8_t)(word & 077u), false);
    int32_t src2 = sext16(read_operand(cpu, s, false));
    int32_t src = sext16(cpu->r[reg]);
    int32_t dst = src * src2;
    cpu->r[reg] = (uint16_t)((uint32_t)dst >> 16);
    cpu->r[reg | 1] = (uint16_t)((uint32_t)dst & 0177777u);
    set_flag(cpu, PDP11_PSW_N, dst < 0);
    set_flag(cpu, PDP11_PSW_Z, dst == 0);
    set_flag(cpu, PDP11_PSW_V, false);
    set_flag(cpu, PDP11_PSW_C, (dst > 32767) || (dst < -32768));
}

static void op_div(pdp11_cpu *cpu, uint16_t word) {
    uint8_t reg = (uint8_t)((word >> 6) & 07u);
    operand s = decode_operand(cpu, (uint8_t)(word & 077u), false);
    uint16_t src2u = read_operand(cpu, s, false);
    uint32_t srcu = ((uint32_t)cpu->r[reg] << 16) | cpu->r[reg | 1];

    if (src2u == 0) { // divide by zero (J11/11-70: N=0, Z=V=C=1)
        set_flag(cpu, PDP11_PSW_N, false);
        set_flag(cpu, PDP11_PSW_Z, true);
        set_flag(cpu, PDP11_PSW_V, true);
        set_flag(cpu, PDP11_PSW_C, true);
        return;
    }
    if (srcu == 020000000000u && src2u == 0177777u) { // -2^31 / -1 overflow
        set_flag(cpu, PDP11_PSW_V, true);
        set_flag(cpu, PDP11_PSW_N, false);
        set_flag(cpu, PDP11_PSW_Z, false);
        set_flag(cpu, PDP11_PSW_C, false);
        return;
    }
    int32_t src2 = sext16(src2u);
    int32_t src = (int32_t)srcu; // sign already in bit 31
    int32_t dst = src / src2;
    set_flag(cpu, PDP11_PSW_N, dst < 0);
    if (dst > 32767 || dst < -32768) { // quotient doesn't fit in 16 bits
        set_flag(cpu, PDP11_PSW_V, true);
        set_flag(cpu, PDP11_PSW_Z, false);
        set_flag(cpu, PDP11_PSW_C, false);
        return;
    }
    int32_t rem = src - src2 * dst;
    cpu->r[reg] = (uint16_t)((uint32_t)dst & 0177777u);
    cpu->r[reg | 1] = (uint16_t)((uint32_t)rem & 0177777u);
    set_flag(cpu, PDP11_PSW_Z, dst == 0);
    set_flag(cpu, PDP11_PSW_V, false);
    set_flag(cpu, PDP11_PSW_C, false);
}

static void op_ash(pdp11_cpu *cpu, uint16_t word) {
    uint8_t reg = (uint8_t)((word >> 6) & 07u);
    operand s = decode_operand(cpu, (uint8_t)(word & 077u), false);
    int32_t sh = (int32_t)(read_operand(cpu, s, false) & 077u); // 0..63 shift code
    int32_t sign = (cpu->r[reg] & 0100000u) ? 1 : 0;
    int32_t src = sign ? (int32_t)(cpu->r[reg] | ~077777u) : (int32_t)cpu->r[reg];
    int32_t dst;
    bool v = false, c = false;

    if (sh == 0) {
        dst = src;
    } else if (sh <= 15) { // left 1..15
        dst = (int32_t)((uint32_t)src << sh);
        int32_t i = (src >> (16 - sh)) & 0177777;
        v = (i != ((dst & 0100000) ? 0177777 : 0));
        c = (i & 1) != 0;
    } else if (sh <= 31) { // left 16..31 -> all bits out
        dst = 0;
        v = (src != 0);
        c = ((uint32_t)src << (sh - 16) & 1u) != 0;
    } else if (sh == 32) { // right 32
        dst = -sign;
        c = sign != 0;
    } else { // right 31..1  (sh 33..63)
        dst = (src >> (64 - sh)) | (int32_t)((uint32_t)(-sign) << (sh - 32));
        c = ((src >> (63 - sh)) & 1) != 0;
    }
    cpu->r[reg] = (uint16_t)((uint32_t)dst & 0177777u);
    set_nz(cpu, cpu->r[reg], false);
    set_flag(cpu, PDP11_PSW_V, v);
    set_flag(cpu, PDP11_PSW_C, c);
}

static void op_ashc(pdp11_cpu *cpu, uint16_t word) {
    uint8_t reg = (uint8_t)((word >> 6) & 07u);
    operand s = decode_operand(cpu, (uint8_t)(word & 077u), false);
    int32_t sh = (int32_t)(read_operand(cpu, s, false) & 077u);
    int32_t sign = (cpu->r[reg] & 0100000u) ? 1 : 0;
    int32_t src = (int32_t)(((uint32_t)cpu->r[reg] << 16) | cpu->r[reg | 1]);
    int32_t dst;
    bool v = false, c = false;

    if (sh == 0) {
        dst = src;
    } else if (sh <= 31) { // left 1..31
        dst = (int32_t)((uint32_t)src << sh);
        int32_t i = (int32_t)(((uint32_t)(src >> (32 - sh)))
                              | ((uint32_t)(-sign) << sh));
        v = (i != (dst < 0 ? -1 : 0));
        c = (i & 1) != 0;
    } else if (sh == 32) { // right 32
        dst = -sign;
        c = sign != 0;
    } else { // right 31..1
        dst = (int32_t)(((uint32_t)(src >> (64 - sh)))
                        | ((uint32_t)(-sign) << (sh - 32)));
        c = ((src >> (63 - sh)) & 1) != 0;
    }
    uint16_t hi = (uint16_t)((uint32_t)dst >> 16);
    uint16_t lo = (uint16_t)((uint32_t)dst & 0177777u);
    cpu->r[reg] = hi;
    cpu->r[reg | 1] = lo;
    set_flag(cpu, PDP11_PSW_N, (hi & 0100000u) != 0);
    set_flag(cpu, PDP11_PSW_Z, (hi | lo) == 0);
    set_flag(cpu, PDP11_PSW_V, v);
    set_flag(cpu, PDP11_PSW_C, c);
}

static void op_xor(pdp11_cpu *cpu, uint16_t word) {
    uint8_t reg = (uint8_t)((word >> 6) & 07u);
    operand d = decode_operand(cpu, (uint8_t)(word & 077u), false);
    uint16_t result = (uint16_t)(read_operand(cpu, d, false) ^ cpu->r[reg]);
    write_operand(cpu, d, false, result);
    set_nz(cpu, result, false);
    set_flag(cpu, PDP11_PSW_V, false);
}

// Decode the groups that aren't clean double-operand opcodes: single-operand
// instructions, branches, JMP/JSR/RTS/SOB, condition-code ops, EIS, HALT.
static void decode_misc(pdp11_cpu *cpu, uint16_t word) {
    if (try_single_op(cpu, word)) {
        return;
    }
    uint16_t hb = (uint16_t)(word >> 8);
    if (is_branch(hb)) {
        op_branch(cpu, word, hb);
    } else if (word >= 0000100 && word <= 0000177) {
        op_jmp(cpu, word);
    } else if (word >= 0000200 && word <= 0000207) {
        op_rts(cpu, word);
    } else if (word >= 0000240 && word <= 0000277) {
        op_ccops(cpu, word);
    } else if ((word & 0177000u) == 0004000) {
        op_jsr(cpu, word);
    } else if ((word & 0177000u) == 0070000) {
        op_mul(cpu, word);
    } else if ((word & 0177000u) == 0071000) {
        op_div(cpu, word);
    } else if ((word & 0177000u) == 0072000) {
        op_ash(cpu, word);
    } else if ((word & 0177000u) == 0073000) {
        op_ashc(cpu, word);
    } else if ((word & 0177000u) == 0074000) {
        op_xor(cpu, word);
    } else if ((word & 0177000u) == 0077000) {
        op_sob(cpu, word);
    } else if ((word & 0177400u) == 0104000) {
        do_trap(cpu, VEC_EMT);  // EMT (104000-104377)
    } else if ((word & 0177400u) == 0104400) {
        do_trap(cpu, VEC_TRAP); // TRAP (104400-104777)
    } else if (word == 0000002) {
        op_rti(cpu, false);     // RTI
    } else if (word == 0000006) {
        op_rti(cpu, true);      // RTT
    } else if (word == 0000003) {
        do_trap(cpu, VEC_BPT);  // BPT
    } else if (word == 0000004) {
        do_trap(cpu, VEC_IOT);  // IOT
    } else if (word == 0) {
        cpu->halted = true;     // HALT
    } else if (word == 0000001) {
        cpu->waiting = true;    // WAIT for an interrupt
    } else if (word == 0000005) {
        cpu->pirq = 0;          // RESET: bus INIT clears device state (+ PIRQ)
    } else if ((word & 0177700u) == 0106400u  // MTPS  (LSI-only)
               || (word & 0177700u) == 0106700u  // MFPS  (LSI-only)
               || (word & 0177000u) == 0075000u  // FIS   (not on 11/70)
               || (word & 0177000u) == 0076000u) { // CIS  (not on 11/70)
        do_trap(cpu, VEC_RESERVED); // illegal instruction on the 11/70
    }
    // Still no-ops (legal on the 11/70, implemented later): WAIT/RESET/MFPT,
    // MARK, MFPI/MTPI/MFPD/MTPD (P3), FP11 (P5). A full illegal-instruction
    // net tightens as those land.
}

void pdp11_cpu_step(pdp11_cpu *cpu) {
    if (cpu->halted) {
        return;
    }

    // Take a trace trap that came due before this instruction (SimH ordering:
    // pending traps are serviced at the top of the loop, before the fetch).
    if (cpu->trace_pending) {
        cpu->trace_pending = false;
        do_trap(cpu, VEC_BPT);
        return;
    }
    // Hardware interrupt: grant the highest pending request whose level exceeds
    // the current CPU priority (PSW bits 7-5). Traps outrank interrupts, so this
    // follows the trace check. Device interrupts join this path at P6.
    if (highest_pir_level(cpu->pirq) > (int)((cpu->psw >> 5) & 07u)) {
        cpu->waiting = false; // an interrupt ends the wait state
        do_trap(cpu, VEC_PIRQ);
        cpu->instr_count++;
        return;
    }
    // WAIT idles the processor until an interrupt arrives (checked above).
    if (cpu->waiting) {
        return;
    }
    // Arm a trace trap to fire after this instruction if T is set going in.
    if (cpu->psw & PDP11_PSW_T) {
        cpu->trace_pending = true;
    }

    // A bus/odd-address fault during the instruction longjmps back here and
    // traps through the recorded vector.
    if (setjmp(cpu->abort_env)) {
        do_trap(cpu, cpu->abort_vec);
        cpu->instr_count++;
        return;
    }

    uint16_t word = fetch(cpu);
    uint8_t top = (uint8_t)((word >> 12) & 017u);

    switch (top) {
    case 001: op_mov(cpu, word, false); break; // MOV
    case 002: op_cmp(cpu, word, false); break; // CMP
    case 003: op_bit(cpu, word, false); break; // BIT
    case 004: op_bic(cpu, word, false); break; // BIC
    case 005: op_bis(cpu, word, false); break; // BIS
    case 006: op_add(cpu, word);        break; // ADD
    case 011: op_mov(cpu, word, true);  break; // MOVB
    case 012: op_cmp(cpu, word, true);  break; // CMPB
    case 013: op_bit(cpu, word, true);  break; // BITB
    case 014: op_bic(cpu, word, true);  break; // BICB
    case 015: op_bis(cpu, word, true);  break; // BISB
    case 016: op_sub(cpu, word);        break; // SUB
    case 000: // single-op / branches / JMP / RTS / cc-ops / JSR / HALT
    case 007: // SOB (+ EIS/FIS in P2/P5)
    case 010: // branches / byte single-op / EMT / TRAP (P2)
    case 017: // FP11 (P5)
        decode_misc(cpu, word);
        break;
    default:
        break;
    }

    cpu->instr_count++;
}
