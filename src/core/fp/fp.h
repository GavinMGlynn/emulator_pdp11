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

// Arithmetic: result = fac <op> fsrc. `v_out` receives FPS_V (overflow) and
// `fec_out` the posted FP exception code (0 = none). DIV's caller must first
// check fsrc for zero (FEC_DZRO always traps and is posted by the caller).
uint64_t pdp11_fp_add(uint64_t fac, uint64_t fsrc, uint16_t fps,
                      int *v_out, int *fec_out);
uint64_t pdp11_fp_mul(uint64_t fac, uint64_t fsrc, uint16_t fps,
                      int *v_out, int *fec_out);
uint64_t pdp11_fp_div(uint64_t fac, uint64_t fsrc, uint16_t fps,
                      int *v_out, int *fec_out);

// MODf: fac*fsrc split into an integer part (returned via *int_out, destined for
// FR[ac|1]) and a fraction (the return value, destined for FR[ac]).
uint64_t pdp11_fp_mod(uint64_t fac, uint64_t fsrc, uint16_t fps,
                      uint64_t *int_out, int *v_out, int *fec_out);

// CMPf: compare fsrc against fac. Returns the FPS N/Z condition bits; *zero_ac
// is set when the accumulator should be replaced by true zero (both operands 0).
uint16_t pdp11_fp_cmp(uint64_t fac, uint64_t fsrc, uint16_t fps, int *zero_ac);

// Round a value to single precision (LDCff'/STCff'). Returns the rounded value;
// *v_out receives FPS_V on overflow.
uint64_t pdp11_fp_round(uint64_t val, uint16_t fps, int *v_out, int *fec_out);

// Integer -> float (LDCif). `facl` is the source integer positioned in the high
// half as SimH does (word << 16, or the full 32-bit long).
uint64_t pdp11_fp_ldcif(uint32_t facl, uint16_t fps);

// Float -> integer (STCfi). Returns the 32-bit integer result (high word first).
// *c_out is set (C flag) on a conversion error; *fec_out receives FEC_ICVT.
uint32_t pdp11_fp_stcfi(uint64_t val, uint16_t fps, int *c_out, int *fec_out);

#endif // PDP11_FP_H
