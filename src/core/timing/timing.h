#ifndef PDP11_TIMING_H
#define PDP11_TIMING_H

#include <stdint.h>

// KB11-C (PDP-11/70) instruction timing.
//
// The timing oracle for this project is the DEC documentation, since SimH
// models no timing and the 11/70 has no software-readable cycle counter. All
// numbers here are transcribed from the PDP-11/70 Processor Handbook (1977-78),
// Appendix C "Instruction Timing" (pages C-1..C-6 / PDF 269-275), and every one
// cites its table in tools/simh-oracle/FINDINGS.md.
//
// Instr Time = SRC Time + DST Time + EF (Execute/Fetch) Time, per the per-class
// rules in C.1.1. Times are in nanoseconds (the handbook gives microseconds in
// multiples of 0.15 us; ns keeps them integer and exact). The chart times assume
// all read cycles hit the cache; `read_cycles` counts the reads so a cache model
// (P4c) can add the 1.02 us miss penalty. This covers the integer instruction
// set; EIS/FP operand-dependent timing and branch/jump refinements are P4b.
typedef struct {
    uint32_t ns;          // execution time assuming all cache hits
    uint32_t read_cycles; // number of read memory cycles (for miss accounting)
} pdp11_timing;

// Timing for a decoded instruction word (all-hits case).
pdp11_timing pdp11_instr_timing(uint16_t word);

#endif // PDP11_TIMING_H
