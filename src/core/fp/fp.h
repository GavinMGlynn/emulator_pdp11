#ifndef PDP11_FP_H
#define PDP11_FP_H

#include <stdint.h>

// FP11-C floating-point arithmetic (P5c). The value is the 64-bit packed
// accumulator (word0<<48..word3). These routines are a faithful port of SimH's
// pdp11_fp.c arithmetic so the results match the oracle bit-for-bit; the exact
// guard-bit/rounding behaviour is what makes the port necessary. `fps` supplies
// the rounding mode (FPS_D double, FPS_T truncate) and the exception enables.
// `v_out` receives the overflow (V) condition; `fec_out` receives the FP
// exception code (0 none, 8 overflow, 10 underflow) for the caller to post.

uint64_t pdp11_fp_add(uint64_t fac, uint64_t fsrc, uint16_t fps,
                      int *v_out, int *fec_out);

#endif // PDP11_FP_H
