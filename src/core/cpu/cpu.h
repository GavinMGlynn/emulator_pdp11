#ifndef PDP11_CPU_H
#define PDP11_CPU_H

#include <stdbool.h>
#include <stdint.h>

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

typedef struct pdp11_cpu {
    uint16_t r[8]; // R0-R7 (kernel set; alternate set + modes land later)
    uint16_t psw;  // processor status word
    bool halted;   // set by HALT until the next reset
    bool trace_pending; // a T-bit trace trap is due before the next instruction

    // Cycle accounting. The reference core will drive this from the KB11-C
    // timing model once cache/bus timing lands (COMPLETION_PLAN P4); until then
    // it counts executed instructions so the harness has something to diff.
    uint64_t instr_count;

    pdp11_mem *mem;
} pdp11_cpu;

// Allocate a CPU with its own 4 MiB physical memory (heap — the memory array is
// far too large for the stack). Returns NULL on allocation failure.
pdp11_cpu *pdp11_cpu_create(void);
void pdp11_cpu_destroy(pdp11_cpu *cpu);

// Clear registers, PSW, and the halted flag. Does not clear memory.
void pdp11_cpu_reset(pdp11_cpu *cpu);

// Fetch, decode, and execute exactly one instruction. A HALT sets `halted` and
// leaves the PC past the HALT word; further steps while halted are no-ops.
void pdp11_cpu_step(pdp11_cpu *cpu);

#endif // PDP11_CPU_H
