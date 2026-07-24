#include "fp/fp.h"

// Faithful port of SimH pdp11_fp.c (MIT-licensed). The 64-bit fraction is held
// as two 32-bit halves {l, h} exactly as SimH does, so the guard-bit and
// rounding arithmetic matches bit-for-bit. The packed public value is
// word0<<48 | word1<<32 | word2<<16 | word3, i.e. h = high 32 bits (sign, exp,
// fraction high) and l = low 32 bits — the same decomposition SimH's fpac_t uses.
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
#define FP_BIAS   0200
#define PSW_V_N   3

#define FPS_V   0000002u // the V condition-code bit
#define FPS_Z   0000004u
#define FPS_N   0000010u
#define FPS_T   0000040u // truncate (no rounding)
#define FPS_L   0000100u // 0 = word integer, 1 = long integer
#define FPS_D   0000200u // 0 = single, 1 = double
#define FPS_IC  0000400u // interrupt on conversion error
#define FPS_IV  0001000u // interrupt on overflow
#define FPS_IU  0002000u // interrupt on underflow
#define FPS_IUV 0004000u // interrupt on undefined variable
#define FPS_ID  0040000u // interrupt disable
#define FPS_ER  0100000u // error

#define FEC_OP    2  // illegal op/mode
#define FEC_DZRO  4  // divide by zero
#define FEC_ICVT  6  // conversion error
#define FEC_OVFLO 8
#define FEC_UNFLO 10
#define FEC_UNDFV 12 // undefined variable

#define GET_EXP(h)    (((h) >> FP_V_EXP) & FP_M_EXP)
#define GET_SIGN(h)   (((h) >> FP_V_SIGN) & 1u)
#define GET_SIGN_L(l) (((l) >> 31) & 1u)
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
#define F_LT(x, y) (((x).h < (y).h) || (((x).h == (y).h) && ((x).l < (y).l)))
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
#define F_LSH_V(sr, n, ds) \
    (ds).h = (((n) >= 32) ? ((sr).l << ((n) - 32)) \
             : ((sr).h << (n)) | (((sr).l >> (32 - (n))) & and_mask[n])) \
             & 0xFFFFFFFFu; \
    (ds).l = ((n) >= 32) ? 0 : ((sr).l << (n)) & 0xFFFFFFFFu
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
static const fpac_t fround_fac = {(1u << 31), 0};           // FP_V_FROUND+32
static const fpac_t fround_guard_fac = {0, (1u << 2)};      // FP_V_FROUND+GUARD
static const fpac_t dround_guard_fac = {(1u << 2), 0};      // FP_V_DROUND+GUARD
static const fpac_t fmask_fac = {0xFFFFFFFFu, (1u << (FP_V_HB + FP_GUARD + 1)) - 1u};

static inline fpac_t to_fac(uint64_t v, uint16_t fps) {
    fpac_t f = {(uint32_t)(v & 0xFFFFFFFFu), (uint32_t)(v >> 32)};
    if ((fps & FPS_D) == 0) { // single precision ignores the low 32 bits
        f.l = 0;
    }
    return f;
}

static inline uint64_t from_fac(fpac_t f) {
    return ((uint64_t)f.h << 32) | f.l;
}

// SimH fpnotrap: decide whether an FP exception is silently suppressed. Returns
// 1 (suppress: caller zeroes the result, nothing is posted) when the code is one
// of the maskable exceptions and its interrupt-enable is clear. Otherwise the
// exception is "real": *fec_out receives the code (the caller sets FPS_ER, FEC,
// FEA and raises the FPE trap unless FPS_ID). FEC_OP and FEC_DZRO always post.
static int fp_notrap(uint16_t fps, int code, int *fec_out) {
    static const uint16_t test_code[] = {0, 0, 0, FPS_IC, FPS_IV, FPS_IU, FPS_IUV};
    if (code >= FEC_ICVT && code <= FEC_UNDFV &&
        (fps & test_code[code >> 1]) == 0) {
        return 1;
    }
    *fec_out = code;
    return 0;
}

// Normalize, round, and pack, mirroring SimH round_and_pack. Returns FPS_V on
// overflow. Sets *fec_out on a posted over/underflow.
static int round_and_pack(fpac_t *facp, int32_t exp, fpac_t *fracp, int r,
                          uint16_t fps, int *fec_out) {
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
    facp->h = (uint32_t)((facp->h & FP_SIGN)
                         | ((uint32_t)(exp & (int32_t)FP_M_EXP) << FP_V_EXP)
                         | (frac.h & FP_FRACH));
    if (exp > 0377) {
        if (fp_notrap(fps, FEC_OVFLO, fec_out)) {
            *facp = zero_fac;
        }
        return FPS_V;
    }
    if (exp <= 0 && fp_notrap(fps, FEC_UNFLO, fec_out)) {
        *facp = zero_fac;
    }
    return 0;
}

// roundfp11: round a value to single precision. Returns FPS_V on overflow.
static int roundfp11(fpac_t *fptr, uint16_t fps, int *fec_out) {
    fpac_t outf = *fptr;
    F_ADD(fround_fac, outf, outf);
    if (GET_SIGN(outf.h ^ fptr->h)) { // rounding flipped the sign -> overflow
        outf.h = outf.h ^ FP_SIGN;
        if (fp_notrap(fps, FEC_OVFLO, fec_out)) {
            *fptr = zero_fac;
        } else {
            *fptr = outf;
        }
        return FPS_V;
    }
    *fptr = outf;
    return 0;
}

// addfp11: fac += fsrc.
static int addfp11(fpac_t *facp, fpac_t fsrc, uint16_t fps, int *fec_out) {
    int32_t facexp, fsrcexp, ediff;
    fpac_t fac = *facp;
    fpac_t facfrac, fsrcfrac;

    if (F_LT_AP(fac, fsrc)) { // if |fac| < |fsrc|, swap
        fpac_t t = fac;
        fac = fsrc;
        fsrc = t;
    }
    facexp = (int32_t)GET_EXP(fac.h);
    fsrcexp = (int32_t)GET_EXP(fsrc.h);
    if (facexp == 0) {
        *facp = fsrcexp ? fsrc : zero_fac;
        return 0;
    }
    if (fsrcexp == 0) {
        *facp = fac;
        return 0;
    }
    ediff = facexp - fsrcexp;
    if (ediff >= 60) {
        *facp = fac;
        return 0;
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
            *facp = zero_fac;
            return 0;
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
    int v = round_and_pack(&fac, facexp, &facfrac, 1, fps, fec_out);
    *facp = fac;
    return v;
}

// frac_mulfp11: classic shift-and-add fraction multiply. Inputs unguarded,
// output guarded.
static void frac_mulfp11(fpac_t *f1p, fpac_t *f2p) {
    fpac_t result = zero_fac;
    fpac_t mpy = *f1p;
    fpac_t mpc = *f2p;
    int i;
    F_LSH_GUARD(mpc);
    if ((mpy.l | mpc.l) == 0) { // 24b x 24b
        for (i = 0; i < 24; i++) {
            if (mpy.h & 1) {
                result.h = result.h + mpc.h;
            }
            F_RSH_1(result);
            mpy.h = mpy.h >> 1;
        }
    } else {
        if (mpy.l != 0) { // 24b x 56b
            for (i = 0; i < 32; i++) {
                if (mpy.l & 1) {
                    F_ADD(mpc, result, result);
                }
                F_RSH_1(result);
                mpy.l = mpy.l >> 1;
            }
        }
        for (i = 0; i < 24; i++) {
            if (mpy.h & 1) {
                F_ADD(mpc, result, result);
            }
            F_RSH_1(result);
            mpy.h = mpy.h >> 1;
        }
    }
    *f1p = result;
}

// mulfp11: fac *= fsrc.
static int mulfp11(fpac_t *facp, fpac_t fsrc, uint16_t fps, int *fec_out) {
    int32_t facexp = (int32_t)GET_EXP(facp->h);
    int32_t fsrcexp = (int32_t)GET_EXP(fsrc.h);
    fpac_t facfrac, fsrcfrac;
    if (facexp == 0 || fsrcexp == 0) {
        *facp = zero_fac;
        return 0;
    }
    F_GET_FRAC(*facp, facfrac);
    F_GET_FRAC(fsrc, fsrcfrac);
    facexp = facexp + fsrcexp - FP_BIAS;
    facp->h = facp->h ^ fsrc.h; // sign
    frac_mulfp11(&facfrac, &fsrcfrac);
    if (GET_BIT(facfrac.h, FP_V_HB + FP_GUARD) == 0) {
        F_LSH_1(facfrac);
        facexp = facexp - 1;
    }
    return round_and_pack(facp, facexp, &facfrac, 1, fps, fec_out);
}

// divfp11: fac /= fsrc. Caller must have checked fsrc != 0.
static int divfp11(fpac_t *facp, fpac_t fsrc, uint16_t fps, int *fec_out) {
    int32_t facexp = (int32_t)GET_EXP(facp->h);
    int32_t fsrcexp = (int32_t)GET_EXP(fsrc.h);
    int32_t i, count, qd;
    fpac_t facfrac, fsrcfrac, quo;
    if (facexp == 0) {
        *facp = zero_fac;
        return 0;
    }
    F_GET_FRAC(*facp, facfrac);
    F_GET_FRAC(fsrc, fsrcfrac);
    F_LSH_GUARD(facfrac);
    F_LSH_GUARD(fsrcfrac);
    facexp = facexp - fsrcexp + FP_BIAS + 1;
    facp->h = facp->h ^ fsrc.h; // sign
    qd = fps & FPS_D;
    count = FP_V_HB + FP_GUARD + (qd ? 33 : 1); // 56b / 24b
    quo = zero_fac;
    for (i = count; (i > 0) && ((facfrac.h | facfrac.l) != 0); i--) {
        F_LSH_1(quo);
        if (!F_LT(facfrac, fsrcfrac)) { // dividend >= divisor?
            F_SUB(fsrcfrac, facfrac, facfrac);
            if (qd) {
                quo.l = quo.l | 1;
            } else {
                quo.h = quo.h | 1;
            }
        }
        F_LSH_1(facfrac);
    }
    if (i > 0) { // early exit: left-justify the quotient
        F_LSH_V(quo, i, quo);
    }
    if (GET_BIT(quo.h, FP_V_HB + FP_GUARD) == 0) {
        F_LSH_1(quo);
        facexp = facexp - 1;
    }
    return round_and_pack(facp, facexp, &quo, 1, fps, fec_out);
}

// modfp11: split fac*fsrc into integer part (*intp) and fraction (returned).
static int modfp11(fpac_t *facp, fpac_t fsrc, fpac_t *fracp, uint16_t fps,
                   int *fec_out) {
    int32_t facexp = (int32_t)GET_EXP(facp->h);
    int32_t fsrcexp = (int32_t)GET_EXP(fsrc.h);
    int32_t fsrcexp2;
    fpac_t facfrac, fsrcfrac, fmask;
    if (facexp == 0 || fsrcexp == 0) {
        *fracp = zero_fac;
        *facp = zero_fac;
        return 0;
    }
    F_GET_FRAC(*facp, facfrac);
    F_GET_FRAC(fsrc, fsrcfrac);
    facexp = facexp + fsrcexp - FP_BIAS;
    fracp->h = facp->h = facp->h ^ fsrc.h; // sign
    frac_mulfp11(&facfrac, &fsrcfrac);
    if (GET_BIT(facfrac.h, FP_V_HB + FP_GUARD) == 0) {
        F_LSH_1(facfrac);
        facexp = facexp - 1;
    }
    if (facexp <= FP_BIAS) { // case 1: all fraction
        *facp = zero_fac;
        return round_and_pack(fracp, facexp, &facfrac, 1, fps, fec_out);
    }
    if (facexp > ((fps & FPS_D) ? FP_BIAS + 56 : FP_BIAS + 24)) {
        *fracp = zero_fac; // case 2: all integer
        return round_and_pack(facp, facexp, &facfrac, 0, fps, fec_out);
    }
    // case 3: split integer and fraction
    F_RSH_V(fmask_fac, facexp - FP_BIAS, fmask);
    fsrcfrac.l = facfrac.l & fmask.l;
    fsrcfrac.h = facfrac.h & fmask.h;
    if ((fsrcfrac.h | fsrcfrac.l) == 0) {
        *fracp = zero_fac;
    } else {
        F_LSH_V(fsrcfrac, facexp - FP_BIAS, fsrcfrac);
        fsrcexp2 = FP_BIAS;
        if ((fsrcfrac.h & (0x00FFFFFFu << FP_GUARD)) == 0) {
            F_LSH_K(fsrcfrac, 24, fsrcfrac);
            fsrcexp2 = fsrcexp2 - 24;
        }
        if ((fsrcfrac.h & (0x00FFF000u << FP_GUARD)) == 0) {
            F_LSH_K(fsrcfrac, 12, fsrcfrac);
            fsrcexp2 = fsrcexp2 - 12;
        }
        if ((fsrcfrac.h & (0x00FC0000u << FP_GUARD)) == 0) {
            F_LSH_K(fsrcfrac, 6, fsrcfrac);
            fsrcexp2 = fsrcexp2 - 6;
        }
        while (GET_BIT(fsrcfrac.h, FP_V_HB + FP_GUARD) == 0) {
            F_LSH_1(fsrcfrac);
            fsrcexp2 = fsrcexp2 - 1;
        }
        round_and_pack(fracp, fsrcexp2, &fsrcfrac, 1, fps, fec_out);
    }
    facfrac.l = facfrac.l & ~fmask.l;
    facfrac.h = facfrac.h & ~fmask.h;
    return round_and_pack(facp, facexp, &facfrac, 0, fps, fec_out);
}

// ---- Public API -----------------------------------------------------------

uint64_t pdp11_fp_add(uint64_t fac_v, uint64_t fsrc_v, uint16_t fps,
                      int *v_out, int *fec_out) {
    *v_out = 0;
    *fec_out = 0;
    fpac_t fac = to_fac(fac_v, fps);
    fpac_t fsrc = to_fac(fsrc_v, fps);
    *v_out = addfp11(&fac, fsrc, fps, fec_out);
    return from_fac(fac);
}

uint64_t pdp11_fp_mul(uint64_t fac_v, uint64_t fsrc_v, uint16_t fps,
                      int *v_out, int *fec_out) {
    *v_out = 0;
    *fec_out = 0;
    fpac_t fac = to_fac(fac_v, fps);
    fpac_t fsrc = to_fac(fsrc_v, fps);
    *v_out = mulfp11(&fac, fsrc, fps, fec_out);
    return from_fac(fac);
}

uint64_t pdp11_fp_div(uint64_t fac_v, uint64_t fsrc_v, uint16_t fps,
                      int *v_out, int *fec_out) {
    *v_out = 0;
    *fec_out = 0;
    fpac_t fac = to_fac(fac_v, fps);
    fpac_t fsrc = to_fac(fsrc_v, fps);
    *v_out = divfp11(&fac, fsrc, fps, fec_out);
    return from_fac(fac);
}

uint64_t pdp11_fp_mod(uint64_t fac_v, uint64_t fsrc_v, uint16_t fps,
                      uint64_t *int_out, int *v_out, int *fec_out) {
    *v_out = 0;
    *fec_out = 0;
    fpac_t fac = to_fac(fac_v, fps);
    fpac_t fsrc = to_fac(fsrc_v, fps);
    fpac_t frac;
    *v_out = modfp11(&fac, fsrc, &frac, fps, fec_out);
    *int_out = from_fac(fac); // integer part -> FR[ac|1]
    return from_fac(frac);    // fraction -> FR[ac]
}

uint16_t pdp11_fp_cmp(uint64_t fac_v, uint64_t fsrc_v, uint16_t fps,
                      int *zero_ac) {
    fpac_t fac = to_fac(fac_v, fps);
    fpac_t fsrc = to_fac(fsrc_v, fps);
    uint16_t cc;
    *zero_ac = 0;
    if (GET_EXP(fsrc.h) == 0) {
        fsrc = zero_fac;
    }
    if (GET_EXP(fac.h) == 0) {
        fac = zero_fac;
    }
    if (fsrc.h == fac.h && fsrc.l == fac.l) { // equal
        cc = FPS_Z;
        if ((fsrc.h | fsrc.l) == 0) { // both zero
            *zero_ac = 1;
        }
    } else { // unequal: N from the sign of fsrc, flipped when fsrc < fac
        cc = (uint16_t)((fsrc.h >> (FP_V_SIGN - PSW_V_N)) & FPS_N);
        if (GET_SIGN(fsrc.h ^ fac.h) == 0 && fac.h != 0 && F_LT(fsrc, fac)) {
            cc ^= FPS_N;
        }
    }
    return cc;
}

uint64_t pdp11_fp_round(uint64_t val, uint16_t fps, int *v_out, int *fec_out) {
    *v_out = 0;
    *fec_out = 0;
    fpac_t f = {(uint32_t)(val & 0xFFFFFFFFu), (uint32_t)(val >> 32)};
    *v_out = roundfp11(&f, fps, fec_out);
    return from_fac(f);
}

uint64_t pdp11_fp_ldcif(uint32_t facl, uint16_t fps) {
    fpac_t fac;
    fac.l = facl;
    fac.h = 0;
    if (fac.l) {
        uint32_t sign = GET_SIGN_L(fac.l);
        int i;
        if (sign) {
            fac.l = (fac.l ^ 0xFFFFFFFFu) + 1;
        }
        for (i = 0; GET_SIGN_L(fac.l) == 0; i++) {
            fac.l = fac.l << 1;
        }
        int32_t exp = ((fps & FPS_L) ? FP_BIAS + 32 : FP_BIAS + 16) - i;
        fac.h = (sign << FP_V_SIGN) | ((uint32_t)exp << FP_V_EXP)
                | ((fac.l >> (31 - FP_V_HB)) & FP_FRACH);
        fac.l = (fac.l << (FP_V_HB + 1)) & FP_FRACL;
        if ((fps & (FPS_D | FPS_T)) == 0) {
            int fec = 0;
            roundfp11(&fac, fps, &fec);
        }
    }
    return from_fac(fac);
}

uint32_t pdp11_fp_stcfi(uint64_t val, uint16_t fps, int *c_out, int *fec_out) {
    // 0x80000000 is the negative overflow limit; the positive limit is one
    // less magnitude and depends on word/long. [long?][sign].
    static const uint32_t i_limit[2][2] = {
        {0x80000000u, 0x80010000u},
        {0x80000000u, 0x80000001u}};
    fpac_t fr = {(uint32_t)(val & 0xFFFFFFFFu), (uint32_t)(val >> 32)};
    uint32_t sign = GET_SIGN(fr.h);
    int32_t exp = (int32_t)GET_EXP(fr.h);
    int is_long = (fps & FPS_L) ? 1 : 0;
    fpac_t fac;
    fac.h = (fr.h & FP_FRACH) | FP_HB; // fraction with hidden bit
    fac.l = (fps & FPS_D) ? fr.l : 0;
    int32_t limit_exp = is_long ? FP_BIAS + 32 : FP_BIAS + 16;
    uint32_t dst;
    int c = 0;
    if (exp <= FP_BIAS) {
        dst = 0;
    } else if (exp > limit_exp) {
        dst = 0;
        c = 1;
    } else {
        fpac_t shifted;
        F_RSH_V(fac, FP_V_HB + 1 + limit_exp - exp, shifted);
        if (!is_long) {
            shifted.l = shifted.l & ~0177777u;
        }
        if (shifted.l >= i_limit[is_long][sign]) {
            dst = 0;
            c = 1;
        } else {
            dst = shifted.l;
            if (sign) {
                dst = (uint32_t)(-(int32_t)dst);
            }
        }
    }
    *c_out = c;
    *fec_out = 0;
    if (c) {
        fp_notrap(fps, FEC_ICVT, fec_out);
    }
    return dst;
}
