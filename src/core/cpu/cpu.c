#include "cpu/cpu.h"

#include <stdlib.h>

#include "clk/clk.h"
#include "console/console.h"
#include "devices/rk11.h"
#include "devices/rp11.h"
#include "devices/tm11.h"
#include "fp/fp.h"
#include "timing/timing.h"

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

// Per-model capability table. Transcribed from SimH's cpu_tab (pdp11_cpumod.c):
// a feature is present iff it is in the model's standard option set (SOP_*),
// which cpu_set_model loads as the power-up cpu_opt. Memory ceilings: 64 KiB
// (16-bit), 256 KiB (18-bit Unibus), 4 MiB (22-bit). Order matches pdp11_model.
#define MEM_64K  0000200000u //  64 KiB (2**16)
#define MEM_256K 0001000000u // 256 KiB (2**18)
#define MEM_4M   0020000000u //   4 MiB (2**22)
static const pdp11_model_info k_model_tab[PDP11_MODEL_COUNT] = {
    //  name       eis    fpp    mmu    ubm    sxs    mark   rtt    spl   mfpt max_mem   psw_mask
    { "11/03",  false, false, false, false, true,  true,  true,  false, 0, MEM_64K,  0000377u },
    { "11/04",  false, false, false, false, false, false, true,  false, 0, MEM_64K,  0000377u },
    { "11/05",  false, false, false, false, false, false, false, false, 0, MEM_64K,  0000377u },
    { "11/20",  false, false, false, false, false, false, false, false, 0, MEM_64K,  0000377u },
    { "11/23",  true,  true,  true,  false, true,  true,  true,  false, 3, MEM_4M,   0170777u },
    { "11/23+", true,  true,  true,  false, true,  true,  true,  false, 3, MEM_4M,   0170777u },
    { "11/24",  true,  true,  true,  true,  true,  true,  true,  false, 3, MEM_4M,   0170777u },
    { "11/34",  true,  false, true,  false, true,  true,  true,  false, 0, MEM_256K, 0170377u },
    { "11/40",  true,  false, true,  false, true,  true,  true,  false, 0, MEM_256K, 0170377u },
    { "11/44",  true,  true,  true,  true,  true,  true,  true,  true,  1, MEM_4M,   0170777u },
    { "11/45",  true,  true,  true,  false, true,  true,  true,  true,  0, MEM_256K, 0174377u },
    { "11/53",  true,  true,  true,  false, true,  true,  true,  true,  5, MEM_4M,   0174777u },
    { "11/60",  true,  true,  true,  false, true,  true,  true,  false, 0, MEM_256K, 0170377u },
    { "11/70",  true,  true,  true,  true,  true,  true,  true,  true,  0, MEM_4M,   0174377u },
    { "11/73",  true,  true,  true,  false, true,  true,  true,  true,  5, MEM_4M,   0174777u },
    { "11/73B", true,  true,  true,  false, true,  true,  true,  true,  5, MEM_4M,   0174777u },
    { "11/83",  true,  true,  true,  false, true,  true,  true,  true,  5, MEM_4M,   0174777u },
    { "11/84",  true,  true,  true,  false, true,  true,  true,  true,  5, MEM_4M,   0174777u },
    { "11/93",  true,  true,  true,  false, true,  true,  true,  true,  5, MEM_4M,   0174777u },
    { "11/94",  true,  true,  true,  false, true,  true,  true,  true,  5, MEM_4M,   0174777u },
};

const pdp11_model_info *pdp11_model_lookup(pdp11_model model) {
    if (model < 0 || model >= PDP11_MODEL_COUNT) {
        return NULL;
    }
    return &k_model_tab[model];
}

pdp11_cpu *pdp11_cpu_create_model(pdp11_model model) {
    const pdp11_model_info *info = pdp11_model_lookup(model);
    if (info == NULL) {
        return NULL;
    }
    pdp11_cpu *cpu = calloc(1, sizeof *cpu);
    if (cpu == NULL) {
        return NULL;
    }
    cpu->mem = calloc(1, sizeof *cpu->mem);
    if (cpu->mem == NULL) {
        free(cpu);
        return NULL;
    }
    cpu->model = model;
    cpu->has_eis = info->has_eis;
    cpu->has_fpp = info->has_fpp;
    cpu->has_mmu = info->has_mmu;
    cpu->has_ubm = info->has_ubm;
    cpu->has_sxs = info->has_sxs;
    cpu->has_mark = info->has_mark;
    cpu->has_rtt = info->has_rtt;
    cpu->has_spl = info->has_spl;
    cpu->mfpt_code = info->mfpt_code;
    pdp11_cpu_reset(cpu);
    // Installed memory: 256 KiB by default (matching the oracle's `set cpu 256k`,
    // the V6 boot config), capped at the model's physical ceiling — so an 11/20
    // sees its true 64 KiB. A relocated physical reference at or above mem_top and
    // below the I/O page is non-existent memory and aborts through vector 4.
    // Create-time configuration, so reset (runtime state only) leaves it alone.
    cpu->mem_top = info->max_mem < 01000000u ? info->max_mem : 01000000u;
    return cpu;
}

pdp11_cpu *pdp11_cpu_create(void) {
    return pdp11_cpu_create_model(PDP11_MODEL_1170);
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
    cpu->abort_depth = 0;
    cpu->mmr0 = 0;
    cpu->mmr3 = 0;
    for (int i = 0; i < 32; ++i) {
        cpu->ub_map[i] = 0;
    }
    for (int i = 0; i < 64; ++i) {
        cpu->par[i] = 0;
        cpu->pdr[i] = 0;
    }
    for (int i = 0; i < 6; ++i) {
        cpu->regfile[i][0] = 0;
        cpu->regfile[i][1] = 0;
    }
    for (int i = 0; i < 4; ++i) {
        cpu->stackfile[i] = 0;
    }
    for (int i = 0; i < 6; ++i) {
        cpu->fac[i] = 0;
    }
    cpu->fps = 0;
    cpu->fec = 0;
    cpu->fea = 0;
    cpu->instr_count = 0;
    cpu->time_ns = 0;
    // KW11-L line clock: default 60 Hz line frequency (16.667 ms period). INIT
    // clears the status register and pending device interrupts.
    cpu->int_req = 0;
    cpu->clk_csr = KW11L_DONE; // the monitor bit powers up set (SimH clk_reset)
    cpu->clk_tick_ns = 1000000000u / 60u;
    cpu->clk_next_ns = cpu->clk_tick_ns;
    // DL11 console: the receiver powers up idle (no character), the transmitter
    // ready (DONE set) so software can send at once. (SimH tti/tto_reset.)
    cpu->tti_csr = 0;
    cpu->tti_buf = 0;
    cpu->tto_csr = DL11_DONE;
    cpu->tto_buf = 0;
    cpu->tto_busy = false;
    // RK11: registers to their reset state; DONE ready. The attached disk buffer
    // (set by the frontend) is preserved across reset.
    cpu->rk.rkcs = 0000200u; // RKCS_DONE
    cpu->rk.rker = 0;
    cpu->rk.rkwc = 0;
    cpu->rk.rkba = 0;
    cpu->rk.rkda = 0;
    cpu->rk.rkds = 0;
    cpu->rk.busy = false;
    // RH70 + RP04: registers to reset; CS1 ready. Disk buffer preserved.
    cpu->rp.cs1 = 0000200u; // CS1_DONE
    cpu->rp.wc = cpu->rp.ba = cpu->rp.cs2 = cpu->rp.bae = 0;
    cpu->rp.da = cpu->rp.dc = cpu->rp.cc = cpu->rp.ds = cpu->rp.er1 = 0;
    cpu->rp.busy = false;
    // TM11 tape: MTC ready; tape image preserved across reset.
    cpu->tm.cmd = 0000200u; // MTC_DONE
    cpu->tm.sta = cpu->tm.bc = cpu->tm.ca = cpu->tm.db = cpu->tm.rdl = 0;
    cpu->tm.busy = false;
    pdp11_cache_reset(&cpu->cache);
}

// --- Deterministic state hash (P9 identity harness) -------------------------
// A 64-bit FNV-1a over every bit of *architectural and timing* state that the
// reference core must reproduce exactly: registers, banked files, MMU, FP,
// interrupt/clock/console state, device controller registers, cache-timing
// state, the instruction/time accounting, all of physical memory, and the
// contents of any attached disk/tape media. Host-specific pointers (mem,
// console_out/ctx, disk/tape backing pointers) and the abort setjmp buffer are
// excluded — they are not machine state and vary run to run. This is the oracle
// a verified fast mode must match bit-for-bit against the reference core.
static void fnv1a(uint64_t *h, const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; ++i) {
        *h ^= b[i];
        *h *= 0x100000001b3ull;
    }
}

uint64_t pdp11_state_hash(const pdp11_cpu *cpu) {
    uint64_t h = 0xcbf29ce484222325ull; // FNV offset basis

    // CPU visible + hidden architectural state.
    fnv1a(&h, cpu->r, sizeof cpu->r);
    fnv1a(&h, &cpu->psw, sizeof cpu->psw);
    fnv1a(&h, &cpu->halted, sizeof cpu->halted);
    fnv1a(&h, &cpu->waiting, sizeof cpu->waiting);
    fnv1a(&h, &cpu->trace_pending, sizeof cpu->trace_pending);
    fnv1a(&h, &cpu->cc_frozen, sizeof cpu->cc_frozen);
    fnv1a(&h, &cpu->pirq, sizeof cpu->pirq);
    fnv1a(&h, cpu->regfile, sizeof cpu->regfile);
    fnv1a(&h, cpu->stackfile, sizeof cpu->stackfile);

    // MMU (KT11).
    fnv1a(&h, &cpu->mmr0, sizeof cpu->mmr0);
    fnv1a(&h, &cpu->mmr3, sizeof cpu->mmr3);
    fnv1a(&h, cpu->par, sizeof cpu->par);
    fnv1a(&h, cpu->pdr, sizeof cpu->pdr);
    fnv1a(&h, cpu->ub_map, sizeof cpu->ub_map);

    // FP11-C.
    fnv1a(&h, cpu->fac, sizeof cpu->fac);
    fnv1a(&h, &cpu->fps, sizeof cpu->fps);
    fnv1a(&h, &cpu->fec, sizeof cpu->fec);
    fnv1a(&h, &cpu->fea, sizeof cpu->fea);

    // Interrupts, KW11-L clock, DL11 console registers.
    fnv1a(&h, &cpu->int_req, sizeof cpu->int_req);
    fnv1a(&h, &cpu->clk_csr, sizeof cpu->clk_csr);
    fnv1a(&h, &cpu->clk_tick_ns, sizeof cpu->clk_tick_ns);
    fnv1a(&h, &cpu->clk_next_ns, sizeof cpu->clk_next_ns);
    fnv1a(&h, &cpu->tti_csr, sizeof cpu->tti_csr);
    fnv1a(&h, &cpu->tti_buf, sizeof cpu->tti_buf);
    fnv1a(&h, &cpu->tto_csr, sizeof cpu->tto_csr);
    fnv1a(&h, &cpu->tto_buf, sizeof cpu->tto_buf);
    fnv1a(&h, &cpu->tto_busy, sizeof cpu->tto_busy);
    fnv1a(&h, &cpu->tto_done_ns, sizeof cpu->tto_done_ns);

    // Device controller registers (media pointers excluded; the media contents
    // are folded in separately below).
    fnv1a(&h, &cpu->rk.rkcs, 6u * sizeof(uint16_t)); // rkcs..rkds contiguous
    fnv1a(&h, &cpu->rk.disk_words, sizeof cpu->rk.disk_words);
    fnv1a(&h, &cpu->rk.busy, sizeof cpu->rk.busy);
    fnv1a(&h, &cpu->rk.done_ns, sizeof cpu->rk.done_ns);

    fnv1a(&h, &cpu->rp.cs1, 10u * sizeof(uint16_t)); // cs1..er1 contiguous
    fnv1a(&h, &cpu->rp.disk_words, sizeof cpu->rp.disk_words);
    fnv1a(&h, &cpu->rp.busy, sizeof cpu->rp.busy);
    fnv1a(&h, &cpu->rp.done_ns, sizeof cpu->rp.done_ns);

    fnv1a(&h, &cpu->tm.sta, 6u * sizeof(uint16_t)); // sta..rdl contiguous
    fnv1a(&h, &cpu->tm.tape_len, sizeof cpu->tm.tape_len);
    fnv1a(&h, &cpu->tm.pos, sizeof cpu->tm.pos);
    fnv1a(&h, &cpu->tm.wrp, sizeof cpu->tm.wrp);
    fnv1a(&h, &cpu->tm.busy, sizeof cpu->tm.busy);
    fnv1a(&h, &cpu->tm.done_ns, sizeof cpu->tm.done_ns);

    // Cache tags/valid/victim + miss count (timing state).
    fnv1a(&h, &cpu->cache, sizeof cpu->cache);

    // Timing + accounting.
    fnv1a(&h, &cpu->time_ns, sizeof cpu->time_ns);
    fnv1a(&h, &cpu->instr_count, sizeof cpu->instr_count);

    // All of physical memory.
    if (cpu->mem != NULL) {
        fnv1a(&h, cpu->mem->words, sizeof cpu->mem->words);
    }

    // Attached media contents (disk writes are machine state a fast mode must
    // reproduce). Hashed only when attached; size is already folded in above.
    if (cpu->rk.disk != NULL) {
        fnv1a(&h, cpu->rk.disk, (size_t)cpu->rk.disk_words * sizeof(uint16_t));
    }
    if (cpu->rp.disk != NULL) {
        fnv1a(&h, cpu->rp.disk, (size_t)cpu->rp.disk_words * sizeof(uint16_t));
    }
    if (cpu->tm.tape != NULL) {
        fnv1a(&h, cpu->tm.tape, (size_t)cpu->tm.tape_len);
    }

    return h;
}

// CPU memory access goes through these (MMU relocation + I/O-page decode),
// defined below. Word accesses fault on odd addresses.
static uint16_t cpu_read_word(pdp11_cpu *cpu, uint32_t va);
static void cpu_write_word(pdp11_cpu *cpu, uint32_t va, uint16_t value);
static uint16_t cpu_fetch_word(pdp11_cpu *cpu, uint32_t va);
static uint8_t cpu_read_byte(pdp11_cpu *cpu, uint32_t va);
static void cpu_write_byte(pdp11_cpu *cpu, uint32_t va, uint8_t value);
static void push_word(pdp11_cpu *cpu, uint16_t value);
static uint16_t pop_word(pdp11_cpu *cpu);
static void do_trap(pdp11_cpu *cpu, uint16_t vector); // trap through a vector

// --- Instruction fetch ------------------------------------------------------
// Read the word at the PC and advance the PC by two.
static uint16_t fetch(pdp11_cpu *cpu) {
    uint16_t word = cpu_fetch_word(cpu, cpu->r[PDP11_PC]); // instruction space
    cpu->r[PDP11_PC] = (uint16_t)(cpu->r[PDP11_PC] + 2u);
    return word;
}

// --- Operand resolution -----------------------------------------------------
// A resolved operand is either a register (is_reg) or a memory address. The
// autoincrement/autodecrement side effects happen exactly once, here, in the
// hardware's source-then-destination order (the caller decodes src before dst).
typedef struct {
    bool is_reg;
    bool is_imm;   // immediate (#n): value fetched from the instruction stream
    uint8_t reg;   // valid when is_reg
    uint16_t imm;  // valid when is_imm
    uint32_t addr; // valid when !is_reg && !is_imm (16-bit for now)
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
    case 2: // (Rn)+ — autoincrement  (PC: immediate #n, from I-space)
        if (reg == PDP11_PC) {
            op.addr = cpu->r[PDP11_PC]; // inline location (for a dest write)
            op.is_imm = true;
            op.imm = fetch(cpu);        // value from the instruction stream
        } else {
            op.addr = cpu->r[reg];
            cpu->r[reg] = (uint16_t)(cpu->r[reg] + operand_step(reg, bytemode));
        }
        break;
    case 3: // @(Rn)+ — autoincrement deferred  (PC: absolute @#A, A from I-space)
        if (reg == PDP11_PC) {
            op.addr = fetch(cpu); // the address word A is in the instruction stream
        } else {
            op.addr = cpu_read_word(cpu, cpu->r[reg]); // pointer is data (D-space)
            cpu->r[reg] = (uint16_t)(cpu->r[reg] + 2u);
        }
        break;
    case 4: // -(Rn) — autodecrement
        cpu->r[reg] = (uint16_t)(cpu->r[reg] - operand_step(reg, bytemode));
        op.addr = cpu->r[reg];
        break;
    case 5: // @-(Rn) — autodecrement deferred
        cpu->r[reg] = (uint16_t)(cpu->r[reg] - 2u);
        op.addr = cpu_read_word(cpu, cpu->r[reg]);
        break;
    case 6: { // X(Rn) — index  (PC: relative)
        uint16_t base = fetch(cpu);
        op.addr = (uint16_t)(base + cpu->r[reg]);
        break;
    }
    case 7: { // @X(Rn) — index deferred  (PC: relative deferred)
        uint16_t base = fetch(cpu);
        uint16_t ptr = (uint16_t)(base + cpu->r[reg]);
        op.addr = cpu_read_word(cpu, ptr);
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

// Device interrupt table: each PDP11_INT_* id maps to a BR level and vector.
static const struct { uint8_t ipl; uint16_t vec; } int_tab[] = {
    [PDP11_INT_CLK] = {KW11L_IPL, KW11L_VEC},
    [PDP11_INT_TTI] = {DL11_IPL, DL11_RVEC},
    [PDP11_INT_TTO] = {DL11_IPL, DL11_XVEC},
    [PDP11_INT_RK] = {RK_IPL, RK_VEC},
    [PDP11_INT_RP] = {RP_IPL, RP_VEC},
    [PDP11_INT_TM] = {TM_IPL, TM_VEC},
};
#define NUM_INT (sizeof int_tab / sizeof int_tab[0])

void pdp11_set_int(pdp11_cpu *cpu, int dev) {
    cpu->int_req |= (uint32_t)(1u << dev);
}

void pdp11_clr_int(pdp11_cpu *cpu, int dev) {
    cpu->int_req &= ~(uint32_t)(1u << dev);
}

// The pending device interrupt with the highest BR level (ties: lowest id, i.e.
// the earlier bit), or -1 if none is pending.
static int highest_int(const pdp11_cpu *cpu, int *ipl_out) {
    int best = -1, bestipl = 0;
    for (size_t d = 0; d < NUM_INT; ++d) {
        if ((cpu->int_req & (1u << d)) && int_tab[d].ipl > bestipl) {
            bestipl = int_tab[d].ipl;
            best = (int)d;
        }
    }
    *ipl_out = bestipl;
    return best;
}

// PIRQ write (put_PIRQ): keep the request bits and echo the highest pending
// level into bits 7-5 and 3-1, matching the hardware read-back.
static void put_pirq(pdp11_cpu *cpu, uint16_t value) {
    uint16_t req = (uint16_t)(value & 0177000u);
    int level = highest_pir_level(req);
    uint16_t encoded = level ? (uint16_t)((level << 5) | (level << 1)) : 0u;
    cpu->pirq = (uint16_t)(req | encoded);
}

// Load a new PSW, banking the general registers and stack pointer if the
// register set (PSW<11>) or current mode (PSW<15:14>) changed. All PSW changes
// that can alter those fields (traps, RTI, writes to 0177776) go through here.
static void put_psw(pdp11_cpu *cpu, uint16_t new_psw) {
    int old_rs = (cpu->psw >> 11) & 01;
    int new_rs = (new_psw >> 11) & 01;
    if (new_rs != old_rs) {
        for (int i = 0; i < 6; ++i) {
            cpu->regfile[i][old_rs] = cpu->r[i];
            cpu->r[i] = cpu->regfile[i][new_rs];
        }
    }
    int old_cm = (cpu->psw >> 14) & 03;
    int new_cm = (new_psw >> 14) & 03;
    if (new_cm != old_cm) {
        cpu->stackfile[old_cm] = cpu->r[PDP11_SP];
        cpu->r[PDP11_SP] = cpu->stackfile[new_cm];
    }
    cpu->psw = new_psw;
}

// --- KT11 memory management -------------------------------------------------
#define MMR0_MME   0000001u // management enable
#define MMR0_IC    0000200u // instruction complete (set on trap dispatch)
#define MMR0_PAGE  0000176u // faulting page field (apridx << 1)
#define MMR0_RO    0020000u // read-only violation
#define MMR0_PL    0040000u // page-length error
#define MMR0_NR    0100000u // non-resident (no access)
#define MMR0_FREEZE 0160000u // any error bit -> MMR0 frozen
#define MMR3_UDS   0000001u // user D-space enable
#define MMR3_SDS   0000002u // supervisor D-space enable
#define MMR3_KDS   0000004u // kernel D-space enable
#define MMR3_M22E  0000020u // 22-bit enable
#define MMR3_BME   0000040u // DMA bus (Unibus) map enable
#define PDR_ACF    0000007u // access-control field
#define PDR_ED     0000010u // expansion direction (1 = downward)
#define PDR_W      0000100u // written flag
#define PDR_A      0000200u // accessed flag
#define PDR_PLF    0077400u // page-length field
#define PAMASK22   017777777u // 22-bit physical address mask
#define IOPAGE_TOP 017760000u // physical base of the 8 KB I/O page
#define VEC_MME    0250u    // memory-management abort

// Which MMR0 error bit (if any) an access-control field raises for a given
// direction. ACF: 0/1/3/7 non-resident, 2 read-only, 4/5/6 read-write-ish. The
// 11/70 "trap" codes (1,4,5) permit the access here; the MMU-trap-vs-abort
// refinement (MMR0_TRAP/TENB) is a P3c tail.
static uint16_t acf_violation(uint16_t acf, bool is_write) {
    if (is_write) {
        switch (acf) {
        case 4: case 5: case 6: return 0;        // writable
        case 2:                 return MMR0_RO;  // read-only
        default:                return MMR0_NR;  // 0,1,3,7 non-resident
        }
    }
    switch (acf) {
    case 1: case 2: case 4: case 5: case 6: return 0; // readable
    default:                                return MMR0_NR; // 0,3,7
    }
}

// Per-mode MMR3 D-space enable bits (mode 2 is unused).
static const uint16_t dsmask[4] = {MMR3_KDS, MMR3_SDS, 0, MMR3_UDS};

// Relocate a 16-bit virtual address to a physical address in a given processor
// mode (0=Kernel 1=Super 3=User) and space (dspace = data reference). Enforces
// the PDR access-control and page-length checks (aborting through vector 0250 on
// a violation, after recording the page and error in MMR0). When MMR3 D-space is
// disabled for the mode, data references fall back to the I-space registers.
// Management off is the identity in the low 56 KB, folding the top 8 KB onto the
// I/O page (SimH relocR).
static uint32_t mmu_relocate(pdp11_cpu *cpu, uint16_t va, bool is_write,
                             int mode, bool dspace) {
    if (cpu->mmr0 & MMR0_MME) {
        int ds = (dspace && (cpu->mmr3 & dsmask[mode])) ? 1 : 0;
        int idx = (mode << 4) | (ds << 3) | ((va >> 13) & 07);
        uint16_t pdr = cpu->pdr[idx];

        uint16_t err = acf_violation(pdr & PDR_ACF, is_write);
        uint16_t dbn = (uint16_t)(va & 017700u);          // block number
        uint16_t plf = (uint16_t)((pdr & PDR_PLF) >> 2);
        if ((pdr & PDR_ED) ? (dbn < plf) : (dbn > plf)) { // page-length error
            err |= MMR0_PL;
        }
        if (err) {
            if (!(cpu->mmr0 & MMR0_FREEZE)) { // capture page + error once
                cpu->mmr0 = (uint16_t)((cpu->mmr0 & ~MMR0_PAGE)
                                       | ((unsigned)idx << 1));
                cpu->mmr0 |= err;
            }
            cpu_bus_fault(cpu, VEC_MME);
        }
        cpu->pdr[idx] |= PDR_A;               // accessed
        if (is_write) {
            cpu->pdr[idx] |= PDR_W;           // written
        }

        uint32_t pa = ((uint32_t)(va & 017777u)
                       + ((uint32_t)cpu->par[idx] << 6)) & PAMASK22;
        if (!(cpu->mmr3 & MMR3_M22E)) { // 18-bit relocation
            pa &= 0777777u;
            if (pa >= 0760000u) {
                pa = 017000000u | pa;
            }
        }
        return pa;
    }
    uint32_t pa = va & 0177777u;
    if (pa >= 0160000u) {
        pa = 017600000u | pa;
    }
    return pa;
}

static bool is_iopage(uint32_t pa) { return pa >= IOPAGE_TOP; }

// Unibus Map registers: 32 double-words at 0170200-0170377.
#define UBM_BASE   0170200u
#define UBM_END    0170377u

// Relocate an 18-bit Unibus DMA byte address to physical. Mirrors SimH Map_Addr
// (pdp11_io.c): the top 8 KB page (31) bypasses to the I/O page; every other
// page adds the mapping register's 22-bit base to the 13-bit offset. With the
// map disabled the 18-bit address is already the physical address.
uint32_t pdp11_unibus_map(const pdp11_cpu *cpu, uint32_t uba) {
    if (!(cpu->mmr3 & MMR3_BME)) {
        return uba & 0777777u; // 18-bit direct (no relocation)
    }
    uint32_t pg = (uba >> 13) & 037u;   // one of 32 8 KB pages
    uint32_t off = uba & 017777u;       // offset within the page
    if (pg == 037u) {                   // last page bypasses to the I/O page
        return (IOPAGE_TOP + off) & PAMASK22;
    }
    return (cpu->ub_map[pg] + off) & PAMASK22;
}

// Non-existent memory: a relocated physical reference at or above installed
// memory (and below the I/O page, which is dispatched separately) aborts
// through vector 4, as SimH does (pdp11_cpu.c ReadW/WriteW: pa >= MEMSIZE &&
// pa < IOPAGEBASE). Unix sizes core by walking it until this trap fires.
static void check_nxm(pdp11_cpu *cpu, uint32_t pa) {
    if (pa >= cpu->mem_top) {
        cpu_bus_fault(cpu, VEC_BUS);
    }
}

// Map an I/O-page register address (pa & 0177777, i.e. 0160000-0177777) to its
// PAR/PDR slot, or return -1. The 12 blocks of 8 cover Kernel/Super/User × I/D.
static int apr_index(uint16_t a, bool *is_par) {
    static const struct { uint16_t base; bool par; int idx0; } blk[] = {
        {0172300u, false, 0},  {0172320u, false, 8},  // Kernel I/D PDR
        {0172340u, true, 0},   {0172360u, true, 8},   // Kernel I/D PAR
        {0172200u, false, 16}, {0172220u, false, 24}, // Super I/D PDR
        {0172240u, true, 16},  {0172260u, true, 24},  // Super I/D PAR
        {0177600u, false, 48}, {0177620u, false, 56}, // User I/D PDR
        {0177640u, true, 48},  {0177660u, true, 56},  // User I/D PAR
    };
    for (size_t i = 0; i < sizeof blk / sizeof blk[0]; ++i) {
        if (a >= blk[i].base && a <= (uint16_t)(blk[i].base + 016u)) {
            *is_par = blk[i].par;
            return blk[i].idx0 + ((a - blk[i].base) >> 1);
        }
    }
    return -1;
}

// Unibus Map register access. Each register is a double-word: the low word
// (0170200+4n) holds physical bits 15:1 (bit 0 forced even), the high word
// (0170202+4n) holds bits 21:16. Matches SimH ubmap_rd/ubmap_wr (pdp11_io.c).
static uint16_t ubm_read(const pdp11_cpu *cpu, uint16_t a) {
    uint32_t off = (uint32_t)(a - UBM_BASE);
    uint32_t pg = (off >> 2) & 037u;
    return (off & 2u) ? (uint16_t)((cpu->ub_map[pg] >> 16) & 077u)
                      : (uint16_t)(cpu->ub_map[pg] & 0177776u);
}

static void ubm_write(pdp11_cpu *cpu, uint16_t a, uint16_t value) {
    uint32_t off = (uint32_t)(a - UBM_BASE);
    uint32_t pg = (off >> 2) & 037u;
    if (off & 2u) {
        cpu->ub_map[pg] = (cpu->ub_map[pg] & 0177777u)
                          | (((uint32_t)value & 077u) << 16);
    } else {
        cpu->ub_map[pg] = (cpu->ub_map[pg] & ~0177777u) | (value & 0177776u);
    }
    cpu->ub_map[pg] &= 017777776u; // 22-bit even base
}

static uint16_t io_read(pdp11_cpu *cpu, uint16_t a) {
    bool is_par;
    int idx;
    if (a >= UBM_BASE && a <= UBM_END) {
        return ubm_read(cpu, a);
    }
    switch (a) {
    case IOPAGE_PSW:  return cpu->psw;
    case IOPAGE_PIRQ: return cpu->pirq;
    case 0177572u:    return cpu->mmr0; // MMR0
    case 0172516u:    return cpu->mmr3; // MMR3
    case KW11L_LKS:   return pdp11_clk_read(cpu); // KW11-L line clock
    case DL11_RCSR: case DL11_RBUF:
    case DL11_XCSR: case DL11_XBUF:
        return pdp11_console_read(cpu, a); // DL11 console
    case RK_RKDS: case RK_RKER: case RK_RKCS: case RK_RKWC:
    case RK_RKBA: case RK_RKDA: case RK_RKMR: case RK_RKDB:
        return pdp11_rk_read(cpu, a); // RK11 disk
    case TM_MTS: case TM_MTC: case TM_MTBRC:
    case TM_MTCMA: case TM_MTD: case TM_MTRD:
        return pdp11_tm_read(cpu, a); // TM11 tape
    default:
        if (a >= RP_CSR && a <= RP_END) {
            return pdp11_rp_read(cpu, a); // RH70 + RP04 disk
        }
        break;
    }
    idx = apr_index(a, &is_par);
    if (idx >= 0) {
        return is_par ? cpu->par[idx] : cpu->pdr[idx];
    }
    return 0; // unmapped I/O register (proper NXM trap arrives with the Unibus)
}

static void io_write(pdp11_cpu *cpu, uint16_t a, uint16_t value) {
    bool is_par;
    int idx;
    if (a >= UBM_BASE && a <= UBM_END) {
        ubm_write(cpu, a, value);
        return;
    }
    switch (a) {
    case IOPAGE_PSW:  put_psw(cpu, value); cpu->cc_frozen = true; return;
    case IOPAGE_PIRQ: put_pirq(cpu, value); return;
    case 0177572u:    cpu->mmr0 = value; return;
    case 0172516u:    cpu->mmr3 = value; return;
    case KW11L_LKS:   pdp11_clk_write(cpu, value); return; // KW11-L line clock
    case DL11_RCSR: case DL11_RBUF:
    case DL11_XCSR: case DL11_XBUF:
        pdp11_console_write(cpu, a, value); return; // DL11 console
    case RK_RKDS: case RK_RKER: case RK_RKCS: case RK_RKWC:
    case RK_RKBA: case RK_RKDA: case RK_RKMR: case RK_RKDB:
        pdp11_rk_write(cpu, a, value); return; // RK11 disk
    case TM_MTS: case TM_MTC: case TM_MTBRC:
    case TM_MTCMA: case TM_MTD: case TM_MTRD:
        pdp11_tm_write(cpu, a, value); return; // TM11 tape
    default:
        if (a >= RP_CSR && a <= RP_END) {
            pdp11_rp_write(cpu, a, value); return; // RH70 + RP04 disk
        }
        break;
    }
    idx = apr_index(a, &is_par);
    if (idx >= 0) {
        if (is_par) {
            cpu->par[idx] = value;
        } else {
            cpu->pdr[idx] = value;
        }
    }
    // else: unmapped — dropped for now (NXM trap with the Unibus at P6).
}

static int cur_mode(const pdp11_cpu *cpu) { return (cpu->psw >> 14) & 03; }

// Word access in an explicit mode and space (used by MFPx/MTPx to reach the
// previous mode's I- or D-space); the plain versions use the current mode and
// data (D) space, and cpu_fetch_word uses the current mode's instruction (I)
// space for the instruction stream.
static uint16_t cpu_read_word_gen(pdp11_cpu *cpu, uint32_t va, int mode,
                                  bool dspace) {
    if (va & 1u) { // a word reference to an odd address traps through vector 4
        cpu_bus_fault(cpu, VEC_BUS);
    }
    uint32_t pa = mmu_relocate(cpu, (uint16_t)va, false, mode, dspace);
    if (is_iopage(pa)) {
        return io_read(cpu, (uint16_t)(pa & 0177777u)); // I/O page bypasses cache
    }
    check_nxm(cpu, pa);
    pdp11_cache_read(&cpu->cache, pa); // read-miss counting for timing
    return pdp11_mem_read_word(cpu->mem, pa);
}

static void cpu_write_word_mode(pdp11_cpu *cpu, uint32_t va, int mode,
                                bool dspace, uint16_t value) {
    if (va & 1u) {
        cpu_bus_fault(cpu, VEC_BUS);
    }
    uint32_t pa = mmu_relocate(cpu, (uint16_t)va, true, mode, dspace);
    if (is_iopage(pa)) {
        io_write(cpu, (uint16_t)(pa & 0177777u), value);
    } else {
        check_nxm(cpu, pa);
        pdp11_mem_write_word(cpu->mem, pa, value);
    }
}

// MFPx reaches the previous mode's I-space (MFPI) or D-space (MFPD).
static uint16_t cpu_read_word_mode(pdp11_cpu *cpu, uint32_t va, int mode,
                                   bool dspace) {
    return cpu_read_word_gen(cpu, va, mode, dspace);
}

static uint16_t cpu_fetch_word(pdp11_cpu *cpu, uint32_t va) {
    return cpu_read_word_gen(cpu, va, cur_mode(cpu), false); // I-space
}

static uint16_t cpu_read_word(pdp11_cpu *cpu, uint32_t va) {
    return cpu_read_word_gen(cpu, va, cur_mode(cpu), true); // D-space
}

static void cpu_write_word(pdp11_cpu *cpu, uint32_t va, uint16_t value) {
    cpu_write_word_mode(cpu, va, cur_mode(cpu), true, value); // D-space
}

static uint8_t cpu_read_byte(pdp11_cpu *cpu, uint32_t va) {
    uint32_t pa = mmu_relocate(cpu, (uint16_t)va, false, cur_mode(cpu), true);
    if (is_iopage(pa)) {
        uint16_t w = io_read(cpu, (uint16_t)(pa & 0177776u));
        return (uint8_t)((pa & 1u) ? (w >> 8) : (w & 0377u));
    }
    check_nxm(cpu, pa);
    pdp11_cache_read(&cpu->cache, pa); // read-miss counting for timing
    return pdp11_mem_read_byte(cpu->mem, pa);
}

static void cpu_write_byte(pdp11_cpu *cpu, uint32_t va, uint8_t value) {
    uint32_t pa = mmu_relocate(cpu, (uint16_t)va, true, cur_mode(cpu), true);
    if (is_iopage(pa)) {
        uint16_t reg = (uint16_t)(pa & 0177776u);
        uint16_t w = io_read(cpu, reg);
        w = (pa & 1u) ? (uint16_t)((w & 0377u) | ((uint32_t)value << 8))
                      : (uint16_t)((w & 0177400u) | value);
        io_write(cpu, reg, w);
    } else {
        check_nxm(cpu, pa);
        pdp11_mem_write_byte(cpu->mem, pa, value);
    }
}

// Read/write an operand at the natural width. A byte read yields 0..255; a byte
// write to a register touches only the low byte (MOVB's sign-extension is the
// one exception, handled in op_mov).
static uint16_t read_operand(pdp11_cpu *cpu, operand op, bool bytemode) {
    if (op.is_reg) {
        return bytemode ? (uint16_t)(cpu->r[op.reg] & 0377u) : cpu->r[op.reg];
    }
    if (op.is_imm) {
        return bytemode ? (uint16_t)(op.imm & 0377u) : op.imm;
    }
    return bytemode ? cpu_read_byte(cpu, op.addr)
                    : cpu_read_word(cpu, op.addr);
}

static void write_operand(pdp11_cpu *cpu, operand op, bool bytemode,
                          uint16_t value) {
    if (op.is_reg) {
        cpu->r[op.reg] = bytemode
            ? (uint16_t)((cpu->r[op.reg] & 0177400u) | (value & 0377u))
            : value;
    } else if (bytemode) {
        cpu_write_byte(cpu, op.addr, (uint8_t)value);
    } else {
        cpu_write_word(cpu, op.addr, value);
    }
}

// --- Condition codes --------------------------------------------------------
static void set_flag(pdp11_cpu *cpu, uint16_t flag, bool on) {
    if (cpu->cc_frozen) { // an explicit PSW write already set the codes
        return;
    }
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
        // Overflow only when a borrow actually flips 0100000 -> 0077777; with no
        // carry in there is no subtraction and thus no overflow (SimH pdp11_cpu).
        set_flag(cpu, PDP11_PSW_V, c && (d & mask) == msb);
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

// MFPI/MFPD (0065): read the source operand from the *previous* mode's space
// and push it onto the current stack. MTPI/MTPD (0066): pop from the current
// stack and store into the previous mode. R6 refers to the previous mode's
// stack pointer when the modes differ. (I and D spaces coincide while MMR3
// D-space is disabled — the distinction is a P3b tail.)
static void op_mfp(pdp11_cpu *cpu, uint16_t word, bool dspace) {
    int pm = (cpu->psw >> 12) & 03;
    int cm = cur_mode(cpu);
    operand src = decode_operand(cpu, (uint8_t)(word & 077u), false);
    uint16_t value;
    if (src.is_reg) {
        value = (src.reg == PDP11_SP && cm != pm) ? cpu->stackfile[pm]
                                                  : cpu->r[src.reg];
    } else {
        value = cpu_read_word_mode(cpu, src.addr, pm, dspace);
    }
    set_nz(cpu, value, false);
    set_flag(cpu, PDP11_PSW_V, false);
    push_word(cpu, value);
}

static void op_mtp(pdp11_cpu *cpu, uint16_t word, bool dspace) {
    int pm = (cpu->psw >> 12) & 03;
    int cm = cur_mode(cpu);
    uint16_t value = pop_word(cpu); // pop from the current stack first
    set_nz(cpu, value, false);
    set_flag(cpu, PDP11_PSW_V, false);
    operand dst = decode_operand(cpu, (uint8_t)(word & 077u), false);
    if (dst.is_reg) {
        if (dst.reg == PDP11_SP && cm != pm) {
            cpu->stackfile[pm] = value;
        } else {
            cpu->r[dst.reg] = value;
        }
    } else {
        cpu_write_word_mode(cpu, dst.addr, pm, dspace, value);
    }
}

static bool try_single_op(pdp11_cpu *cpu, uint16_t word) {
    uint16_t code = (uint16_t)((word >> 6) & 01777u); // bits 15-6
    bool bytemode = (code & 01000u) != 0;
    uint16_t base = (uint16_t)(code & 0777u);

    if (base == 0003 && !bytemode) { // SWAB (word only)
        single_op(cpu, word, 0003, false);
        return true;
    }
    if (base == 0064 && !bytemode) { // MARK (absent on 11/04, 11/05, 11/20)
        if (cpu->has_mark) { op_mark(cpu, word); } else { do_trap(cpu, VEC_RESERVED); }
        return true;
    }
    if (base == 0065) { // MFPI (I-space) / MFPD (D-space, byte-flagged)
        op_mfp(cpu, word, bytemode);
        return true;
    }
    if (base == 0066) { // MTPI (I-space) / MTPD (D-space, byte-flagged)
        op_mtp(cpu, word, bytemode);
        return true;
    }
    if (base >= 0050 && base <= 0063) { // CLR..ASL (word + byte)
        single_op(cpu, word, base, bytemode);
        return true;
    }
    if (!bytemode && base == 0067) { // SXT (absent on 11/04, 11/05, 11/20)
        if (cpu->has_sxs) { single_op(cpu, word, 0067, false); }
        else { do_trap(cpu, VEC_RESERVED); }
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
    cpu_write_word(cpu, cpu->r[PDP11_SP], value);
}

static uint16_t pop_word(pdp11_cpu *cpu) {
    uint16_t value = cpu_read_word(cpu, cpu->r[PDP11_SP]);
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
    uint16_t old_pc = cpu->r[PDP11_PC];
    uint16_t old_mode = (uint16_t)((old_psw >> 14) & 03u);
    // Trap vectors are always read in Kernel D-space, whatever mode we trap from.
    uint16_t new_pc = cpu_read_word_gen(cpu, vector, 0, true);
    uint16_t new_psw = cpu_read_word_gen(cpu, (uint16_t)(vector + 2u), 0, true);
    // The new PSW's previous-mode field records the mode we trapped from.
    new_psw = (uint16_t)((new_psw & ~0030000u) | ((uint32_t)old_mode << 12));
    // Switch mode/register set first, then push the old state on the new stack.
    put_psw(cpu, new_psw);
    push_word(cpu, old_psw);
    push_word(cpu, old_pc);
    cpu->r[PDP11_PC] = new_pc;
    cpu->mmr0 |= MMR0_IC; // instruction-complete bit, set on trap dispatch
}

// RTI/RTT: pop PC then PSW. On the 11/70 an RTI (but not RTT) that restores the
// T bit takes the trace trap *immediately*, before the instruction at the
// restored PC; RTT defers to the normal after-instruction rule. This is the one
// documented behavioural difference between the two (KB11-C; verified vs SimH).
static void op_rti(pdp11_cpu *cpu, bool is_rtt) {
    cpu->r[PDP11_PC] = pop_word(cpu); // pop from the current stack, then switch
    put_psw(cpu, pop_word(cpu));
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
    } else if (word >= 0000230 && word <= 0000237) {
        // SPL: set the CPU priority (PSW<7:5>) from the low 3 bits. Privileged —
        // it only takes effect in Kernel mode; elsewhere it is a no-op. Present
        // only on the 11/44, 11/45, 11/70 and J-class; reserved elsewhere.
        if (!cpu->has_spl) {
            do_trap(cpu, VEC_RESERVED);
        } else if (((cpu->psw >> 14) & 03u) == 0) {
            cpu->psw = (uint16_t)((cpu->psw & ~0340u) | ((word & 07u) << 5));
        }
    } else if (word >= 0000240 && word <= 0000277) {
        op_ccops(cpu, word);
    } else if ((word & 0177000u) == 0004000) {
        op_jsr(cpu, word);
    } else if ((word & 0177000u) == 0070000) {
        if (cpu->has_eis) { op_mul(cpu, word); } else { do_trap(cpu, VEC_RESERVED); }
    } else if ((word & 0177000u) == 0071000) {
        if (cpu->has_eis) { op_div(cpu, word); } else { do_trap(cpu, VEC_RESERVED); }
    } else if ((word & 0177000u) == 0072000) {
        if (cpu->has_eis) { op_ash(cpu, word); } else { do_trap(cpu, VEC_RESERVED); }
    } else if ((word & 0177000u) == 0073000) {
        if (cpu->has_eis) { op_ashc(cpu, word); } else { do_trap(cpu, VEC_RESERVED); }
    } else if ((word & 0177000u) == 0074000) {
        if (cpu->has_sxs) { op_xor(cpu, word); } else { do_trap(cpu, VEC_RESERVED); }
    } else if ((word & 0177000u) == 0077000) {
        if (cpu->has_sxs) { op_sob(cpu, word); } else { do_trap(cpu, VEC_RESERVED); }
    } else if ((word & 0177400u) == 0104000) {
        do_trap(cpu, VEC_EMT);  // EMT (104000-104377)
    } else if ((word & 0177400u) == 0104400) {
        do_trap(cpu, VEC_TRAP); // TRAP (104400-104777)
    } else if (word == 0000002) {
        op_rti(cpu, false);     // RTI
    } else if (word == 0000006) {
        // RTT (absent on 11/05, 11/20 — reserved there)
        if (cpu->has_rtt) { op_rti(cpu, true); } else { do_trap(cpu, VEC_RESERVED); }
    } else if (word == 0000007) {
        // MFPT: return the processor-type code in R0. Present only on the
        // F-class, 11/44 and J-class; reserved (incl. on the 11/70) otherwise.
        if (cpu->mfpt_code != 0) {
            cpu->r[PDP11_R0] = cpu->mfpt_code;
        } else {
            do_trap(cpu, VEC_RESERVED);
        }
    } else if (word == 0000003) {
        do_trap(cpu, VEC_BPT);  // BPT
    } else if (word == 0000004) {
        do_trap(cpu, VEC_IOT);  // IOT
    } else if (word == 0) {
        cpu->halted = true;     // HALT
    } else if (word == 0000001) {
        cpu->waiting = true;    // WAIT for an interrupt
    } else if (word == 0000005) {
        // RESET: bus INIT clears device state (PIRQ, device interrupts, and the
        // KW11-L status register).
        cpu->pirq = 0;
        cpu->int_req = 0;
        cpu->clk_csr = KW11L_DONE; // INIT sets the monitor bit (SimH clk_reset)
        cpu->tti_csr = 0;          // DL11 receiver idle
        cpu->tto_csr = DL11_DONE;  // DL11 transmitter ready
        cpu->tto_busy = false;
        cpu->rk.rkcs = 0000200u;   // RK11 controller ready
        cpu->rk.rker = 0;
        cpu->rk.busy = false;
        cpu->rp.cs1 = 0000200u;    // RH70/RP04 controller ready
        cpu->rp.er1 = 0;
        cpu->rp.busy = false;
        cpu->tm.cmd = 0000200u;    // TM11 tape controller ready
        cpu->tm.sta = 0;
        cpu->tm.busy = false;
    } else if ((word & 0177700u) == 0106400u  // MTPS  (LSI-only)
               || (word & 0177700u) == 0106700u  // MFPS  (LSI-only)
               || (word & 0177000u) == 0075000u  // FIS   (not on 11/70)
               || (word & 0177000u) == 0076000u) { // CIS  (not on 11/70)
        do_trap(cpu, VEC_RESERVED); // illegal instruction on the 11/70
    }
    // The illegal-instruction net is not yet exhaustive: some unassigned
    // encodings still fall through as no-ops rather than trapping to vector 10.
}

// --- FP11-C floating point (P5) ---------------------------------------------
#define FPS_CC 0000017u // condition codes (N Z V C), same positions as the PSW
#define FPS_N  0000010u
#define FPS_Z  0000004u
#define FPS_V  0000002u
#define FPS_C  0000001u
#define FPS_T  0000040u // truncate (no rounding)
#define FPS_L  0000100u // 0 = short integer, 1 = long
#define FPS_D  0000200u // 0 = single, 1 = double
#define FPS_IV 0001000u // interrupt on overflow
#define FPS_IU 0002000u // interrupt on underflow
#define FPS_ID 0040000u // interrupt disable
#define FPS_ER 0100000u // error
#define FPS_RW 0147757u // writable FPS bits (SimH FPS_RW)
#define FP_SIGNBIT (1ULL << 63)

// FP number fields, as they sit in the high 32 bits of the packed accumulator.
#define FP_V_EXP  23
#define FP_M_EXP  0377u
#define FP_EXP_MASK (FP_M_EXP << FP_V_EXP)
#define FP_BIAS   0200u

#define FEC_DZRO  4  // divide by zero
#define VEC_FPE   0244u // floating-point exception trap vector

// Post an FP exception: set FPS_ER, FEC, FEA and (unless FPS_ID inhibits it)
// trap through the FPE vector. `entry_pc` is the PC just past the FP opcode
// word (SimH backup_PC), so FEA — the faulting instruction's address — is
// entry_pc - 2. Called after the instruction's own side effects are committed,
// matching SimH, where fpnotrap arms a trap serviced at the next dispatch.
static void post_fpe(pdp11_cpu *cpu, int fec, uint16_t entry_pc) {
    cpu->fps |= FPS_ER;
    cpu->fec = (uint16_t)fec;
    cpu->fea = (uint16_t)(entry_pc - 2u);
    if ((cpu->fps & FPS_ID) == 0) {
        do_trap(cpu, VEC_FPE);
    }
}

// A floating value is held in a 64-bit accumulator as word0<<48 | word1<<32 |
// word2<<16 | word3, i.e. big-endian memory-word order (word0 = lowest address).
// Single precision uses the high 32 bits; the exponent is bits 62-55 (8 bits).
static uint16_t fp_exp(uint64_t v) { return (uint16_t)((v >> 55) & 0377u); }

// Update the FP condition codes from a result: N = sign, Z = exponent 0.
static uint16_t fp_setcc(uint16_t fps, uint64_t v, uint16_t newv) {
    fps = (uint16_t)((fps & ~FPS_CC) | newv);
    if (v & FP_SIGNBIT) {
        fps |= FPS_N;
    }
    if (fp_exp(v) == 0) {
        fps |= FPS_Z;
    }
    return fps;
}

// A resolved FP operand: a register (FR), an immediate (one word), or memory.
// Autoincrement/decrement steps by the operand length (4 bytes single / 8 byte
// double); deferred modes step the pointer by 2.
typedef struct { bool is_reg; bool imm; uint8_t reg; uint16_t immw; uint32_t addr; } fp_op;

static fp_op decode_fp(pdp11_cpu *cpu, uint8_t spec, int len_words) {
    uint8_t mode = (uint8_t)((spec >> 3) & 07u);
    uint8_t reg = (uint8_t)(spec & 07u);
    uint16_t step = (uint16_t)(len_words * 2);
    fp_op o = {0};
    switch (mode) {
    case 0: o.is_reg = true; o.reg = reg; break;
    case 1: o.addr = cpu->r[reg]; break;
    case 2:
        if (reg == PDP11_PC) { o.imm = true; o.immw = fetch(cpu); }
        else { o.addr = cpu->r[reg]; cpu->r[reg] = (uint16_t)(cpu->r[reg] + step); }
        break;
    case 3:
        if (reg == PDP11_PC) { o.addr = fetch(cpu); }
        else { o.addr = cpu_read_word(cpu, cpu->r[reg]);
               cpu->r[reg] = (uint16_t)(cpu->r[reg] + 2u); }
        break;
    case 4: cpu->r[reg] = (uint16_t)(cpu->r[reg] - step); o.addr = cpu->r[reg]; break;
    case 5: cpu->r[reg] = (uint16_t)(cpu->r[reg] - 2u);
            o.addr = cpu_read_word(cpu, cpu->r[reg]); break;
    case 6: { uint16_t b = fetch(cpu); o.addr = (uint16_t)(b + cpu->r[reg]); break; }
    case 7: { uint16_t b = fetch(cpu);
              o.addr = cpu_read_word(cpu, (uint16_t)(b + cpu->r[reg])); break; }
    default: break;
    }
    o.addr &= ADDR_MASK;
    return o;
}

static uint64_t read_fp(pdp11_cpu *cpu, fp_op o, int len_words) {
    if (o.is_reg) {
        return (o.reg < 6) ? cpu->fac[o.reg] : 0;
    }
    if (o.imm) {
        return (uint64_t)o.immw << 48; // immediate: one word in the high position
    }
    uint64_t h = ((uint64_t)cpu_read_word(cpu, o.addr) << 16)
                 | cpu_read_word(cpu, (uint16_t)(o.addr + 2u));
    uint64_t l = 0;
    if (len_words == 4) {
        l = ((uint64_t)cpu_read_word(cpu, (uint16_t)(o.addr + 4u)) << 16)
            | cpu_read_word(cpu, (uint16_t)(o.addr + 6u));
    }
    return (h << 32) | l;
}

static void write_fp(pdp11_cpu *cpu, fp_op o, int len_words, uint64_t v) {
    if (o.is_reg) {
        if (o.reg < 6) {
            cpu->fac[o.reg] = v;
        }
        return;
    }
    cpu_write_word(cpu, o.addr, (uint16_t)((v >> 48) & 0177777u));
    cpu_write_word(cpu, (uint16_t)(o.addr + 2u), (uint16_t)((v >> 32) & 0177777u));
    if (len_words == 4) {
        cpu_write_word(cpu, (uint16_t)(o.addr + 4u), (uint16_t)((v >> 16) & 0177777u));
        cpu_write_word(cpu, (uint16_t)(o.addr + 6u), (uint16_t)(v & 0177777u));
    }
}

// FP11 instruction decode (top nibble 017). P5a: control group. P5b: load/store
// (LDf/STf) and CLR/TST/ABS/NEG. P5c: full arithmetic (ADD/SUB/MUL/DIV/MOD/CMP),
// the LDC/STC/LDEXP/STEXP conversions, and the FEC/FEA + FPE-trap exception model.
static void op_fp11(pdp11_cpu *cpu, uint16_t word) {
    int major = (word >> 8) & 017;
    int subop = (word >> 6) & 03;
    uint8_t spec = (uint8_t)(word & 077u);
    int len = (cpu->fps & FPS_D) ? 4 : 2;
    int olen = (len == 4) ? 2 : 4;              // the "other" precision length
    int ilen = (cpu->fps & FPS_L) ? 2 : 1;      // integer length, in words
    uint16_t entry_pc = cpu->r[PDP11_PC];       // SimH backup_PC (past opcode)

    switch (major) {
    case 000:
        switch (subop) {
        case 0: // specials
            switch (word) {
            case 0170000: // CFCC
                cpu->psw = (uint16_t)((cpu->psw & ~017u) | (cpu->fps & FPS_CC));
                break;
            case 0170001: cpu->fps = (uint16_t)(cpu->fps & ~FPS_D); break;
            case 0170002: cpu->fps = (uint16_t)(cpu->fps & ~FPS_L); break;
            case 0170011: cpu->fps |= FPS_D; break;
            case 0170012: cpu->fps |= FPS_L; break;
            default: break;
            }
            break;
        case 1: { // LDFPS
            operand s = decode_operand(cpu, spec, false);
            cpu->fps = (uint16_t)(read_operand(cpu, s, false) & FPS_RW);
            break;
        }
        case 2: { // STFPS
            cpu->fps &= FPS_RW;
            operand d = decode_operand(cpu, spec, false);
            write_operand(cpu, d, false, cpu->fps);
            break;
        }
        case 3: { // STST
            operand d = decode_operand(cpu, spec, false);
            if (d.is_reg) {
                cpu->r[d.reg] = cpu->fec;
            } else {
                cpu_write_word(cpu, d.addr, cpu->fec);
                cpu_write_word(cpu, (uint16_t)(d.addr + 2u), cpu->fea);
            }
            break;
        }
        default: break;
        }
        break;

    case 001: { // CLR (0) / TST (1) / ABS (2) / NEG (3)
        fp_op o = decode_fp(cpu, spec, len);
        if (subop == 0) { // CLRf
            write_fp(cpu, o, len, 0);
            cpu->fps = (uint16_t)((cpu->fps & ~FPS_CC) | FPS_Z);
        } else {
            uint64_t v = read_fp(cpu, o, len);
            if (subop == 2) { // ABSf
                v = (fp_exp(v) == 0) ? 0 : (v & ~FP_SIGNBIT);
                write_fp(cpu, o, len, v);
            } else if (subop == 3) { // NEGf
                v = (fp_exp(v) == 0) ? 0 : (v ^ FP_SIGNBIT);
                write_fp(cpu, o, len, v);
            }
            cpu->fps = fp_setcc(cpu->fps, v, 0);
        }
        break;
    }

    case 005: { // LDf
        int ac = (word >> 6) & 03;
        fp_op o = decode_fp(cpu, spec, len);
        uint64_t v = read_fp(cpu, o, len);
        cpu->fac[ac] = v;
        cpu->fps = fp_setcc(cpu->fps, v, 0);
        break;
    }

    case 010: { // STf
        int ac = (word >> 6) & 03;
        fp_op o = decode_fp(cpu, spec, len);
        write_fp(cpu, o, len, cpu->fac[ac]);
        break;
    }

    case 002:   // MULf
    case 004:   // ADDf
    case 006: { // SUBf
        int ac = (word >> 6) & 03;
        fp_op o = decode_fp(cpu, spec, len);
        uint64_t fsrc = read_fp(cpu, o, len);
        // SUB negates the source (unless it is zero) then adds.
        if (major == 006 && fp_exp(fsrc) != 0) {
            fsrc ^= FP_SIGNBIT;
        }
        int vflag = 0, fec = 0;
        uint64_t r = (major == 002)
                         ? pdp11_fp_mul(cpu->fac[ac], fsrc, cpu->fps, &vflag, &fec)
                         : pdp11_fp_add(cpu->fac[ac], fsrc, cpu->fps, &vflag, &fec);
        cpu->fac[ac] = r;
        cpu->fps = fp_setcc(cpu->fps, r, (uint16_t)(vflag ? FPS_V : 0));
        if (fec) {
            post_fpe(cpu, fec, entry_pc);
        }
        break;
    }

    case 011: { // DIVf
        int ac = (word >> 6) & 03;
        fp_op o = decode_fp(cpu, spec, len);
        uint64_t fsrc = read_fp(cpu, o, len);
        if (fp_exp(fsrc) == 0) { // divide by zero: always traps
            post_fpe(cpu, FEC_DZRO, entry_pc);
        } else {
            int vflag = 0, fec = 0;
            uint64_t r = pdp11_fp_div(cpu->fac[ac], fsrc, cpu->fps, &vflag, &fec);
            cpu->fac[ac] = r;
            cpu->fps = fp_setcc(cpu->fps, r, (uint16_t)(vflag ? FPS_V : 0));
            if (fec) {
                post_fpe(cpu, fec, entry_pc);
            }
        }
        break;
    }

    case 003: { // MODf: integer part -> FR[ac|1], fraction -> FR[ac]
        int ac = (word >> 6) & 03;
        fp_op o = decode_fp(cpu, spec, len);
        uint64_t fsrc = read_fp(cpu, o, len);
        uint64_t intpart = 0;
        int vflag = 0, fec = 0;
        uint64_t frac =
            pdp11_fp_mod(cpu->fac[ac], fsrc, cpu->fps, &intpart, &vflag, &fec);
        cpu->fac[ac | 1] = intpart;
        cpu->fac[ac] = frac;
        cpu->fps = fp_setcc(cpu->fps, frac, (uint16_t)(vflag ? FPS_V : 0));
        if (fec) {
            post_fpe(cpu, fec, entry_pc);
        }
        break;
    }

    case 007: { // CMPf: compare fsrc against fac
        int ac = (word >> 6) & 03;
        fp_op o = decode_fp(cpu, spec, len);
        uint64_t fsrc = read_fp(cpu, o, len);
        int zero_ac = 0;
        uint16_t cc = pdp11_fp_cmp(cpu->fac[ac], fsrc, cpu->fps, &zero_ac);
        cpu->fps = (uint16_t)((cpu->fps & ~FPS_CC) | cc);
        if (zero_ac) {
            cpu->fac[ac] = 0;
        }
        break;
    }

    case 017: { // LDCff': load, converting from the other precision
        int ac = (word >> 6) & 03;
        fp_op o = decode_fp(cpu, spec, olen);
        uint64_t v = read_fp(cpu, o, olen);
        if (fp_exp(v) == 0) {
            v = 0;
        }
        int vflag = 0, fec = 0;
        // Round only when narrowing double -> single (single mode, not truncate).
        if ((cpu->fps & (FPS_D | FPS_T)) == 0) {
            v = pdp11_fp_round(v, cpu->fps, &vflag, &fec);
        }
        cpu->fac[ac] = v;
        cpu->fps = fp_setcc(cpu->fps, v, (uint16_t)(vflag ? FPS_V : 0));
        if (fec) {
            post_fpe(cpu, fec, entry_pc);
        }
        break;
    }

    case 014: { // STCff': store, converting to the other precision
        int ac = (word >> 6) & 03;
        uint64_t v = cpu->fac[ac];
        if ((cpu->fps & FPS_D) == 0) {
            v &= 0xFFFFFFFF00000000ULL; // single: low words are zero
        }
        if (fp_exp(v) == 0) {
            v = 0;
        }
        int vflag = 0, fec = 0;
        // Round only when narrowing double -> single (double mode, not truncate).
        if ((cpu->fps & (FPS_D | FPS_T)) == FPS_D) {
            v = pdp11_fp_round(v, cpu->fps, &vflag, &fec);
        }
        fp_op o = decode_fp(cpu, spec, olen);
        write_fp(cpu, o, olen, v);
        cpu->fps = fp_setcc(cpu->fps, v, (uint16_t)(vflag ? FPS_V : 0));
        if (fec) {
            post_fpe(cpu, fec, entry_pc);
        }
        break;
    }

    case 015: { // LDEXP: load exponent from an integer word operand
        int ac = (word >> 6) & 03;
        operand s = decode_operand(cpu, spec, false);
        uint16_t dst = read_operand(cpu, s, false);
        uint64_t fac = cpu->fac[ac];
        uint32_t h = (uint32_t)(fac >> 32);
        h = (h & ~FP_EXP_MASK)
            | ((((uint32_t)dst + FP_BIAS) & FP_M_EXP) << FP_V_EXP);
        uint64_t v = ((uint64_t)h << 32) | (fac & 0xFFFFFFFFu);
        uint16_t newv = 0;
        int fec = 0;
        // dst is a signed exponent; out-of-range values over/underflow.
        if (dst > 0177u && dst <= 0177600u) {
            if (dst < 0100000u) { // too-large positive exponent -> overflow
                if ((cpu->fps & FPS_IV) == 0) {
                    v = 0;
                } else {
                    fec = 8; // FEC_OVFLO
                }
                newv = FPS_V;
            } else { // too-negative exponent -> underflow
                if ((cpu->fps & FPS_IU) == 0) {
                    v = 0;
                } else {
                    fec = 10; // FEC_UNFLO
                }
            }
        }
        cpu->fac[ac] = v;
        cpu->fps = fp_setcc(cpu->fps, v, newv);
        if (fec) {
            post_fpe(cpu, fec, entry_pc);
        }
        break;
    }

    case 012: { // STEXP: store exponent to an integer word operand
        int ac = (word >> 6) & 03;
        uint16_t dst = (uint16_t)((fp_exp(cpu->fac[ac]) - FP_BIAS) & 0177777u);
        uint16_t cc = 0;
        if (dst & 0100000u) {
            cc |= FPS_N;
        }
        if (dst == 0) {
            cc |= FPS_Z;
        }
        // STEXP writes both the FP and the CPU condition codes (SimH sets the
        // N/Z/V/C globals here as well as FPS).
        cpu->fps = (uint16_t)((cpu->fps & ~FPS_CC) | cc);
        cpu->psw = (uint16_t)((cpu->psw & ~017u) | cc);
        operand d = decode_operand(cpu, spec, false);
        write_operand(cpu, d, false, dst);
        break;
    }

    case 016: { // LDCif: integer -> float
        int ac = (word >> 6) & 03;
        fp_op o = decode_fp(cpu, spec, ilen);
        uint32_t ival;
        if (o.is_reg) {
            ival = (uint32_t)cpu->r[o.reg] << 16; // register source: word only
        } else if (o.imm) {
            ival = (uint32_t)o.immw << 16;
        } else {
            uint32_t hi = cpu_read_word(cpu, o.addr);
            uint32_t lo =
                (ilen == 2) ? cpu_read_word(cpu, (uint16_t)(o.addr + 2u)) : 0;
            ival = (hi << 16) | lo;
        }
        uint64_t v = pdp11_fp_ldcif(ival, cpu->fps);
        cpu->fac[ac] = v;
        cpu->fps = fp_setcc(cpu->fps, v, 0);
        break;
    }

    case 013: { // STCfi: float -> integer
        int ac = (word >> 6) & 03;
        int cflag = 0, fec = 0;
        uint32_t dst = pdp11_fp_stcfi(cpu->fac[ac], cpu->fps, &cflag, &fec);
        uint16_t cc = 0;
        if (dst & 0x80000000u) {
            cc |= FPS_N;
        }
        if (dst == 0) {
            cc |= FPS_Z;
        }
        if (cflag) {
            cc |= FPS_C;
        }
        // STCfi writes both the FP and the CPU condition codes (SimH sets the
        // N/Z/V/C globals here as well as FPS).
        cpu->fps = (uint16_t)((cpu->fps & ~FPS_CC) | cc);
        cpu->psw = (uint16_t)((cpu->psw & ~017u) | cc);
        fp_op o = decode_fp(cpu, spec, ilen);
        if (o.is_reg) {
            cpu->r[o.reg] = (uint16_t)((dst >> 16) & 0177777u);
        } else {
            cpu_write_word(cpu, o.addr, (uint16_t)((dst >> 16) & 0177777u));
            if (ilen == 2) {
                cpu_write_word(cpu, (uint16_t)(o.addr + 2u),
                               (uint16_t)(dst & 0177777u));
            }
        }
        if (fec) {
            post_fpe(cpu, fec, entry_pc);
        }
        break;
    }

    default:
        break;
    }
}

// --- Scheduled-event servicing / idle-skip (P9b) ----------------------------
// Service every subsystem whose scheduled completion is due at the current
// emulated time: the KW11-L line clock, the DL11 transmitter, and the RK/RP/TM
// controllers. Each poll is a no-op unless time_ns has reached its deadline, so
// this is safe to call after every instruction and after an idle-skip jump. The
// clock ticks at most once per call (matching the pre-idle-skip behaviour — an
// instruction never spans more than one line-clock period).
static void service_due_events(pdp11_cpu *cpu) {
    if (cpu->clk_tick_ns && cpu->time_ns >= cpu->clk_next_ns) {
        pdp11_clk_tick(cpu);
        cpu->clk_next_ns += cpu->clk_tick_ns;
    }
    if (cpu->tto_busy) {
        pdp11_console_tx_poll(cpu);
    }
    if (cpu->rk.busy) {
        pdp11_rk_poll(cpu);
    }
    if (cpu->rp.busy) {
        pdp11_rp_poll(cpu);
    }
    if (cpu->tm.busy) {
        pdp11_tm_poll(cpu);
    }
}

// The earliest emulated time at which a scheduled subsystem event is due: the
// next line-clock tick, a pending disk/tape transfer completion, or a console-
// transmit completion. UINT64_MAX means nothing is scheduled — in that state a
// WAIT can only be broken by fresh external input, so emulated time does not
// advance. This is the fast-mode scheduler's next_event(): a WAIT jumps straight
// here instead of spinning one instruction-time at a time. Servicing the event
// it names, then re-stepping, is bit-identical to advancing in tiny increments
// (the polls gate on time_ns >= deadline, and nothing else changes while idle).
uint64_t pdp11_next_event_ns(const pdp11_cpu *cpu) {
    uint64_t t = UINT64_MAX;
    if (cpu->clk_tick_ns && cpu->clk_next_ns < t) {
        t = cpu->clk_next_ns;
    }
    if (cpu->tto_busy && cpu->tto_done_ns < t) {
        t = cpu->tto_done_ns;
    }
    if (cpu->rk.busy && cpu->rk.done_ns < t) {
        t = cpu->rk.done_ns;
    }
    if (cpu->rp.busy && cpu->rp.done_ns < t) {
        t = cpu->rp.done_ns;
    }
    if (cpu->tm.busy && cpu->tm.done_ns < t) {
        t = cpu->tm.done_ns;
    }
    return t;
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
    // follows the trace check. Both program-interrupt (PIR) and device (BR)
    // requests compete here; a device (Unibus) request wins a tie at the same
    // level. The granted vector's own PSW re-raises priority to mask the source.
    {
        int ipl = (int)((cpu->psw >> 5) & 07u);
        int pir = highest_pir_level(cpu->pirq);
        int devipl = 0;
        int dev = highest_int(cpu, &devipl);
        if (dev >= 0 && devipl > ipl && devipl >= pir) {
            cpu->waiting = false; // an interrupt ends the wait state
            // Acknowledging the grant drops the device's bus request: the
            // Unibus BG cycle clears the request latch, so a level that stays
            // asserted (DONE & IE both still set) does not re-interrupt until
            // the device raises a fresh edge. Mirrors SimH get_vector, which
            // clears int_req for the acknowledged device (pdp11_io.c). Without
            // this a still-DONE device (e.g. RK after a completed read) storms
            // the same vector every instruction.
            cpu->int_req &= (uint16_t)~(1u << dev);
            do_trap(cpu, int_tab[dev].vec);
            cpu->instr_count++;
            return;
        }
        if (pir > ipl) {
            cpu->waiting = false;
            do_trap(cpu, VEC_PIRQ);
            cpu->instr_count++;
            return;
        }
    }
    // WAIT idles the processor until an interrupt arrives. Rather than spin one
    // instruction-time at a time, jump emulated time straight to the earliest
    // scheduled event of ANY subsystem (idle-skip) and service it; the interrupt
    // it raises is granted at the top of the next step, breaking the wait. If
    // nothing is scheduled, only fresh external input can release the wait, so
    // stay idle without advancing time.
    if (cpu->waiting) {
        uint64_t next = pdp11_next_event_ns(cpu);
        if (next != UINT64_MAX) {
            if (next > cpu->time_ns) {
                cpu->time_ns = next;
            }
            service_due_events(cpu);
        }
        return;
    }
    // Arm a trace trap to fire after this instruction if T is set going in.
    if (cpu->psw & PDP11_PSW_T) {
        cpu->trace_pending = true;
    }

    // A bus/odd-address/MMU fault during the instruction longjmps back here and
    // traps through the recorded vector. If the trap's own stack pushes keep
    // faulting (a red-stack runaway — e.g. the kernel stack page is not
    // resident), halt rather than spin forever. (Full red-stack-abort-to-4
    // semantics are a P3c tail.)
    if (setjmp(cpu->abort_env)) {
        if (++cpu->abort_depth > 8) {
            cpu->halted = true;
            return;
        }
        do_trap(cpu, cpu->abort_vec);
        cpu->instr_count++;
        return;
    }
    cpu->abort_depth = 0;
    cpu->cc_frozen = false; // fresh for this instruction

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
        decode_misc(cpu, word);
        break;
    case 017: // FP11-C floating point (traps as reserved on a model without FPP)
        if (cpu->has_fpp) {
            op_fp11(cpu, word);
        } else {
            do_trap(cpu, VEC_RESERVED);
        }
        break;
    default:
        break;
    }

    // KB11-C timing. Branches and SOB depend on whether the branch was taken —
    // condition codes are unchanged by a branch, so re-evaluating gives the same
    // result, and SOB's counter holds its post-decrement value.
    uint16_t hb = (uint16_t)(word >> 8);
    uint32_t ns;
    if (is_branch(hb)) {
        ns = branch_taken(cpu, hb) ? 600u : 300u; // taken .60 / not .30
    } else if ((word & 0177000u) == 0077000u) {   // SOB: taken .60 / not .75
        ns = (cpu->r[(word >> 6) & 07u] != 0) ? 600u : 750u;
    } else {
        ns = pdp11_instr_timing(word).ns;
    }
    cpu->time_ns += ns;
    cpu->instr_count++;

    // Service any subsystem completion (clock tick, console transmit, disk/tape
    // transfer) that came due during this instruction.
    service_due_events(cpu);
}
