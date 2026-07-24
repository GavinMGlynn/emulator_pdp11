#include "fp/fp.h"

// Faithful port of SimH pdp11_fp.c (MIT-licensed). The 64-bit fraction is held
// as two 32-bit halves {l, h} exactly as SimH does, so the guard-bit and
// rounding arithmetic matches bit-for-bit.
typedef struct { uint32_t l, h; } fpac_t;

#define FP_V_SIGN 31
#define FP_V_EXP  23
#define FP_V_HB   23
#define FP_M_EXP  0377u
#define FP_SIGN   (1u << FP_V_SIGN)
#define FP_EXP    (FP_M_EXP << FP_V_EXP)
#define FP_FRACH  ((1u << FP_V_HB) - 1u)
#define FP_FRACL  0xFFFFFFFFu
#define FP_HB     (1u << FP_V_HB)
#define FP_GUARD  3

#define FPS_T   0000040u
#define FPS_D   0000200u
#define FPS_V   0000002u
#define FPS_IV  0001000u // overflow interrupt enable
#define FPS_IU  0002000u // underflow interrupt enable

#define FEC_OVFLO 8
#define FEC_UNFLO 10

#define GET_EXP(h)   (((h) >> FP_V_EXP) & FP_M_EXP)
#define GET_SIGN(h)  (((h) >> FP_V_SIGN) & 1u)
#define GET_BIT(x, n) (((x) >> (n)) & 1u)

static const uint32_t and_mask[33] = {
    0, 0x1, 0x3, 0x7, 0xF, 0x1F, 0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF,
    0x1FFF, 0x3FFF, 0x7FFF, 0xFFFF, 0x1FFFF, 0x3FFFF, 0x7FFFF, 0xFFFFF,
    0x1FFFFF, 0x3FFFFF, 0x7FFFFF, 0xFFFFFF, 0x1FFFFFF, 0x3FFFFFF, 0x7FFFFFF,
    0xFFFFFFF, 0x1FFFFFFF, 0x3FFFFFFF, 0x7FFFFFFF, 0xFFFFFFFF};

#define F_ADD(s2, s1, ds) \
    (ds).l = ((s1).l + (s2).l) & 0xFFFFFFFFu; \
    (ds).h = ((s1).h + (s2).h + ((ds).l < (s2).l)) & 0xFFFFFFFFu
#define F_SUB(s2, s1, ds) \
    (ds).h = ((s1).h - (s2).h - ((s1).l < (s2).l)) & 0xFFFFFFFFu; \
    (ds).l = ((s1).l - (s2).l) & 0xFFFFFFFFu
#define F_LT_AP(x, y) \
    ((((x).h & ~FP_SIGN) < ((y).h & ~FP_SIGN)) || \
     ((((x).h & ~FP_SIGN) == ((y).h & ~FP_SIGN)) && ((x).l < (y).l)))
#define F_GET_FRAC(sr, ds) \
    (ds).l = (sr).l; (ds).h = ((sr).h & FP_FRACH) | FP_HB
#define F_RSH_V(sr, n, ds) \
    (ds).l = (((n) >= 32) ? ((sr).h >> ((n) - 32)) & and_mask[64 - (n)] \
             : (((sr).l >> (n)) & and_mask[32 - (n)]) | ((sr).h << (32 - (n)))) \
             & 0xFFFFFFFFu; \
    (ds).h = ((n) >= 32) ? 0 : ((sr).h >> (n)) & and_mask[32 - (n)]
#define F_LSH_1(ds) \
    (ds).h = (((ds).h << 1) | (((ds).l >> 31) & 1)) & 0xFFFFFFFFu; \
    (ds).l = ((ds).l << 1) & 0xFFFFFFFFu
#define F_RSH_1(ds) \
    (ds).l = (((ds).l >> 1) & 0x7FFFFFFFu) | (((ds).h & 1) << 31); \
    (ds).h = ((ds).h >> 1) & 0x7FFFFFFFu
#define F_LSH_K(sr, n, ds) \
    (ds).h = (((sr).h << (n)) | (((sr).l >> (32 - (n))) & and_mask[n])) \
             & 0xFFFFFFFFu; \
    (ds).l = ((sr).l << (n)) & 0xFFFFFFFFu
#define F_RSH_K(sr, n, ds) \
    (ds).l = ((((sr).l >> (n)) & and_mask[32 - (n)]) | ((sr).h << (32 - (n)))) \
             & 0xFFFFFFFFu; \
    (ds).h = ((sr).h >> (n)) & and_mask[32 - (n)]
#define F_LSH_GUARD(ds) F_LSH_K(ds, FP_GUARD, ds)
#define F_RSH_GUARD(ds) F_RSH_K(ds, FP_GUARD, ds)

static const fpac_t zero_fac = {0, 0};
static const fpac_t fround_guard_fac = {0, (1u << 2)};       // FP_V_FROUND+GUARD
static const fpac_t dround_guard_fac = {(1u << 2), 0};       // FP_V_DROUND+GUARD

// Normalize, round, and pack, mirroring SimH round_and_pack. Sets *fec on
// over/underflow; whether the result is zeroed depends on the trap enable.
static void round_and_pack(fpac_t *facp, int32_t exp, fpac_t *fracp, int r,
                           uint16_t fps, int *v_out, int *fec_out) {
    fpac_t frac = *fracp;
    if (r && ((fps & FPS_T) == 0)) {
        if (fps & FPS_D) {
            F_ADD(dround_guard_fac, frac, frac);
        } else {
            F_ADD(fround_guard_fac, frac, frac);
        }
        if (GET_BIT(frac.h, FP_V_HB + FP_GUARD + 1)) {
            F_RSH_1(frac);
            exp = exp + 1;
        }
    }
    F_RSH_GUARD(frac);
    facp->l = frac.l & FP_FRACL;
    facp->h = (uint32_t)((facp->h & FP_SIGN) | ((uint32_t)(exp & (int32_t)FP_M_EXP) << FP_V_EXP)
                         | (frac.h & FP_FRACH));
    if (exp > 0377) {
        if ((fps & FPS_IV) == 0) {
            *facp = zero_fac;
        }
        *fec_out = FEC_OVFLO;
        *v_out = FPS_V;
        return;
    }
    if (exp <= 0) {
        if ((fps & FPS_IU) == 0) {
            *facp = zero_fac;
        }
        *fec_out = FEC_UNFLO;
    }
}

// addfp11: fac += fsrc.
static fpac_t addfp11(fpac_t fac, fpac_t fsrc, uint16_t fps, int *v_out,
                      int *fec_out) {
    int32_t facexp, fsrcexp, ediff;
    fpac_t facfrac, fsrcfrac;

    if (F_LT_AP(fac, fsrc)) { // if |fac| < |fsrc|, swap
        fpac_t t = fac;
        fac = fsrc;
        fsrc = t;
    }
    facexp = (int32_t)GET_EXP(fac.h);
    fsrcexp = (int32_t)GET_EXP(fsrc.h);
    if (facexp == 0) {
        return fsrcexp ? fsrc : zero_fac;
    }
    if (fsrcexp == 0) {
        return fac;
    }
    ediff = facexp - fsrcexp;
    if (ediff >= 60) {
        return fac;
    }
    F_GET_FRAC(fac, facfrac);
    F_GET_FRAC(fsrc, fsrcfrac);
    F_LSH_GUARD(facfrac);
    F_LSH_GUARD(fsrcfrac);
    if (GET_SIGN(fac.h) != GET_SIGN(fsrc.h)) { // subtract
        if (ediff) {
            F_RSH_V(fsrcfrac, ediff, fsrcfrac);
        }
        F_SUB(fsrcfrac, facfrac, facfrac);
        if ((facfrac.h | facfrac.l) == 0) {
            return zero_fac;
        }
        if (ediff <= 1) {
            if ((facfrac.h & (0x00FFFFFFu << FP_GUARD)) == 0) {
                F_LSH_K(facfrac, 24, facfrac);
                facexp = facexp - 24;
            }
            if ((facfrac.h & (0x00FFF000u << FP_GUARD)) == 0) {
                F_LSH_K(facfrac, 12, facfrac);
                facexp = facexp - 12;
            }
            if ((facfrac.h & (0x00FC0000u << FP_GUARD)) == 0) {
                F_LSH_K(facfrac, 6, facfrac);
                facexp = facexp - 6;
            }
        }
        while (GET_BIT(facfrac.h, FP_V_HB + FP_GUARD) == 0) {
            F_LSH_1(facfrac);
            facexp = facexp - 1;
        }
    } else { // add
        if (ediff) {
            F_RSH_V(fsrcfrac, ediff, fsrcfrac);
        }
        F_ADD(fsrcfrac, facfrac, facfrac);
        if (GET_BIT(facfrac.h, FP_V_HB + FP_GUARD + 1)) {
            F_RSH_1(facfrac);
            facexp = facexp + 1;
        }
    }
    round_and_pack(&fac, facexp, &facfrac, 1, fps, v_out, fec_out);
    return fac;
}

uint64_t pdp11_fp_add(uint64_t fac_v, uint64_t fsrc_v, uint16_t fps,
                      int *v_out, int *fec_out) {
    *v_out = 0;
    *fec_out = 0;
    fpac_t fac = {(uint32_t)(fac_v & 0xFFFFFFFFu), (uint32_t)(fac_v >> 32)};
    fpac_t fsrc = {(uint32_t)(fsrc_v & 0xFFFFFFFFu), (uint32_t)(fsrc_v >> 32)};
    // Single precision ignores the low 32 bits.
    if ((fps & FPS_D) == 0) {
        fac.l = 0;
        fsrc.l = 0;
    }
    fpac_t r = addfp11(fac, fsrc, fps, v_out, fec_out);
    return ((uint64_t)r.h << 32) | r.l;
}
