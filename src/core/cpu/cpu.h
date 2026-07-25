#ifndef PDP11_CPU_H
#define PDP11_CPU_H

#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>

#include "cache/cache.h"
#include "devices/rk11.h"
#include "devices/rp11.h"
#include "devices/tm11.h"
#include "memory/memory.h"

// PDP-11 processor state.
//
// This is the seed of the KB11-C (11/70) core. It currently models the general
// registers, the processor status word, and a single-instruction step for a
// first slice of instructions (MOV, ADD, HALT) exercising the full addressing-
// mode machinery. Modes/dual register sets, the MMU, traps and interrupts, EIS,
// FP11-C, cache and bus timing arrive in later phases (docs/COMPLETION_PLAN.md).

// General registers. R6 is the stack pointer, R7 is the program counter.
enum { PDP11_R0, PDP11_R1, PDP11_R2, PDP11_R3,
       PDP11_R4, PDP11_R5, PDP11_SP, PDP11_PC };

// Processor Status Word condition codes and trace bit.
#define PDP11_PSW_C 0000001u // carry
#define PDP11_PSW_V 0000002u // overflow
#define PDP11_PSW_Z 0000004u // zero
#define PDP11_PSW_N 0000010u // negative
#define PDP11_PSW_T 0000020u // trace trap

// Model range (P10). The 11/70 (KB11-C) is the full superset built first; every
// other model subsets it. Order mirrors SimH's cpu_tab (pdp11_cpumod.c), the
// architectural oracle for which options each model carries.
typedef enum {
    PDP11_MODEL_1103, PDP11_MODEL_1104, PDP11_MODEL_1105, PDP11_MODEL_1120,
    PDP11_MODEL_1123, PDP11_MODEL_1123P, PDP11_MODEL_1124, PDP11_MODEL_1134,
    PDP11_MODEL_1140, PDP11_MODEL_1144, PDP11_MODEL_1145, PDP11_MODEL_1153,
    PDP11_MODEL_1160, PDP11_MODEL_1170, PDP11_MODEL_1173, PDP11_MODEL_1173B,
    PDP11_MODEL_1183, PDP11_MODEL_1184, PDP11_MODEL_1193, PDP11_MODEL_1194,
    PDP11_MODEL_COUNT
} pdp11_model;

// Per-model capability descriptor. A feature is present iff it is in the model's
// *standard* option set (SimH SOP_*, which cpu_set_model loads as cpu_opt); the
// SimH `.opt` field lists only user-toggleable extras, not the power-up state.
typedef struct {
    const char *name;   // canonical model name ("11/70")
    bool has_eis;       // MUL/DIV/ASH/ASHC (KE11-E / integral EIS)
    bool has_fpp;       // FP11 floating-point processor
    bool has_mmu;       // KT11 memory management
    bool has_ubm;       // 18-bit Unibus map (22-bit Unibus models)
    // Base-instruction-set additions absent on the earliest machines (SimH
    // HAS_SXS / HAS_MARK / HAS_RTT / HAS_SPL). SXS = SXT, SOB and XOR.
    bool has_sxs;       // SXT / SOB / XOR (absent on 11/04, 11/05, 11/20)
    bool has_mark;      // MARK          (absent on 11/04, 11/05, 11/20)
    bool has_rtt;       // RTT           (absent on 11/05, 11/20)
    bool has_spl;       // SPL           (only 11/44, 11/45, 11/70, J-class)
    uint16_t mfpt_code; // MFPT type code returned in R0, or 0 if MFPT is absent
    uint32_t max_mem;   // physical memory ceiling in bytes (64 KiB / 256 KiB / 4 MiB)
    uint16_t psw_mask;  // writable PSW bits (narrower on modeless low-end machines)
    // KT11 register bit-masks (SimH cpu_tab mm0/mm3/par/pdr). A mask of 0 means
    // the register does not exist on this model (no-MMU machines, and MMR3 on the
    // models that predate it): writes are dropped, reads return 0.
    uint16_t mmr0_mask, mmr3_mask, par_mask, pdr_mask;
} pdp11_model_info;

// The descriptor for a model (never NULL for a valid enum value).
const pdp11_model_info *pdp11_model_lookup(pdp11_model model);

typedef struct pdp11_cpu {
    // Model identity + cached capability flags (create-time config, not runtime
    // state — excluded from the state hash). Default create() is the 11/70.
    pdp11_model model;
    bool has_eis, has_fpp, has_mmu, has_ubm;
    bool has_sxs, has_mark, has_rtt, has_spl;
    uint16_t mfpt_code;
    uint16_t psw_mask; // writable PSW bits for this model
    uint16_t mmr0_mask, mmr3_mask, par_mask, pdr_mask; // KT11 register masks

    uint16_t r[8]; // R0-R7 (kernel set; alternate set + modes land later)
    uint16_t psw;  // processor status word
    bool halted;   // set by HALT until the next reset
    bool waiting;  // set by WAIT; cleared when an interrupt is granted
    bool trace_pending; // a T-bit trace trap is due before the next instruction
    // Set when an instruction writes the PSW as its destination (via 0177776):
    // the written value defines the condition codes, so the instruction's own
    // CC update is suppressed for the rest of that instruction (matches SimH,
    // where the explicit PSW store is the last operation and wins).
    bool cc_frozen;
    uint16_t pirq; // program interrupt request register (0177772)

    // Device interrupts (P6). int_req is a bit per interrupting device (see the
    // PDP11_INT_* ids); each device's BR level and vector live in a table in
    // cpu.c. The KW11-L line clock is the first. clk_csr is its status register
    // (LKS, 0177546); clk_tick_ns is the line-clock period in emulated ns (0
    // disables it) and clk_next_ns is the emulated time of the next tick.
    uint32_t int_req;
    uint16_t clk_csr;
    uint32_t clk_tick_ns;
    uint64_t clk_next_ns;

    // DL11 console SLU (P6). Receiver (RCSR/RBUF) and transmitter (XCSR/XBUF)
    // register pairs; the transmitter models a character-time busy window
    // (tto_busy until tto_done_ns) after XBUF is written. Transmitted characters
    // go to the console_out sink (set by the frontend), if any.
    uint16_t tti_csr, tti_buf;
    uint16_t tto_csr, tto_buf;
    bool tto_busy;
    uint64_t tto_done_ns;
    void (*console_out)(void *ctx, uint8_t ch);
    void *console_ctx;

    // RK11 disk controller (P6).
    pdp11_rk11 rk;

    // RH70 Massbus adapter + RP04 disk (P6).
    pdp11_rp11 rp;

    // TM11 / TU10 magnetic tape (P6).
    pdp11_tm11 tm;

    // KT11 memory management (P3). MMR0<0> enables relocation; MMR3<M22E>
    // selects 22-bit. The PAR/PDR file is indexed (mode<<4)|(dspace<<3)|page,
    // mode 0=Kernel 1=Super 3=User (2 unused).
    uint16_t mmr0;
    uint16_t mmr3;
    uint16_t par[64];
    uint16_t pdr[64];

    // Banked registers. R0-R5 have two sets (PSW<11>); the inactive set lives in
    // regfile. R6 (SP) is banked per mode (Kernel/Super/User); the inactive SPs
    // live in stackfile. The active set/SP is always in r[]. (P3b)
    uint16_t regfile[6][2];
    uint16_t stackfile[4];

    // FP11-C floating point (P5). Six 64-bit accumulators, the floating-point
    // status word, and the exception code/address registers.
    uint64_t fac[6];
    uint16_t fps;
    uint16_t fec;
    uint16_t fea;

    // Mid-instruction fault handling. A bus/odd-address (later MMU) fault
    // longjmps to abort_env, set up at the top of each instruction, and the
    // step loop then traps through abort_vec.
    jmp_buf abort_env;
    uint16_t abort_vec;
    int abort_depth; // consecutive faults in one step (red-stack runaway guard)

    // Accounting. instr_count is a simple executed-instruction counter;
    // time_ns accumulates KB11-C instruction execution time in nanoseconds
    // (all-cache-hits; the cache miss model lands at P4c).
    uint64_t instr_count;
    uint64_t time_ns; // KB11-C all-cache-hits execution time (ns)

    // Cache model (timing only). Total execution time is
    // time_ns + cache.misses * 1020 (1.02 us per read miss).
    pdp11_cache cache;

    pdp11_mem *mem;

    // Installed main-memory size in bytes. The backing store is the full 22-bit
    // space, but a relocated physical reference at or above this (and below the
    // I/O page) is non-existent memory: it aborts through vector 4, exactly as
    // SimH does when the relocated PA is >= MEMSIZE and < IOPAGEBASE
    // (pdp11_cpu.c ReadW/WriteW). Unix sizes core by walking it until this NXM
    // trap fires, so the value must match the oracle's `set cpu <n>k`.
    uint32_t mem_top;

    // KT11 Unibus Map: 31 usable mapping registers (a 32nd slot exists but the
    // top page bypasses to the I/O page). Each holds a 22-bit even physical
    // base. An 18-bit Unibus DMA address is relocated to physical through this
    // map when MMR3<BME> is set, so a device can reach core the CPU placed
    // outside the 18-bit Unibus window. Registers live at 0170200-0170377.
    uint32_t ub_map[32];
} pdp11_cpu;

// Allocate a CPU with its own 4 MiB physical memory (heap — the memory array is
// far too large for the stack). Returns NULL on allocation failure.
pdp11_cpu *pdp11_cpu_create(void);
// Allocate a CPU configured as a specific model (subsetting the 11/70 superset).
// pdp11_cpu_create() is pdp11_cpu_create_model(PDP11_MODEL_1170). Returns NULL
// on allocation failure or an out-of-range model.
pdp11_cpu *pdp11_cpu_create_model(pdp11_model model);
void pdp11_cpu_destroy(pdp11_cpu *cpu);

// Clear registers, PSW, and the halted flag. Does not clear memory.
void pdp11_cpu_reset(pdp11_cpu *cpu);

// Deterministic 64-bit hash of all architectural + timing state (registers,
// banked files, MMU, FP, interrupt/clock/console, device registers, cache,
// instruction/time accounting, all of physical memory, and attached media),
// excluding host pointers. The P9 identity oracle: a verified fast mode must
// produce a bit-identical hash to the reference core at every checkpoint.
uint64_t pdp11_state_hash(const pdp11_cpu *cpu);

// The earliest emulated time (ns) at which a scheduled subsystem event is due
// (line clock, console transmit, or disk/tape completion), or UINT64_MAX if
// nothing is pending. During WAIT the CPU idle-skips straight to this time
// instead of spinning; exposed so a frontend/harness can reason about idle.
uint64_t pdp11_next_event_ns(const pdp11_cpu *cpu);

// Fetch, decode, and execute exactly one instruction. A HALT sets `halted` and
// leaves the PC past the HALT word; further steps while halted are no-ops.
void pdp11_cpu_step(pdp11_cpu *cpu);

// Device interrupt ids (bit positions in int_req). Each maps to a BR level and
// vector in cpu.c's interrupt table.
enum { PDP11_INT_CLK = 0, PDP11_INT_TTI = 1, PDP11_INT_TTO = 2,
       PDP11_INT_RK = 3, PDP11_INT_RP = 4, PDP11_INT_TM = 5 };

// Raise/lower a device interrupt request. The CPU grants the highest-BR pending
// request whose level exceeds the current PSW priority at an instruction
// boundary, vectoring through the device's vector.
void pdp11_set_int(pdp11_cpu *cpu, int dev);
void pdp11_clr_int(pdp11_cpu *cpu, int dev);

// Relocate an 18-bit Unibus DMA byte address to a physical address. When the
// Unibus Map is enabled (MMR3<BME>) the address is mapped through ub_map;
// otherwise the 18-bit address is already physical and returned unchanged.
// A Unibus device calls this for each transferred word. (The RH70 Massbus path,
// e.g. RP04, uses a native 22-bit address and does NOT go through here.)
uint32_t pdp11_unibus_map(const pdp11_cpu *cpu, uint32_t uba);

#endif // PDP11_CPU_H
