#include "rv32f.h"
#include "isa_registry.h"

#include <fenv.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/*
 * RV32F on top of the host's IEEE 754 hardware.
 *
 * The register file holds raw bit patterns (uint32_t). A value only
 * becomes a C `float` for the duration of an arithmetic operation, via
 * memcpy (never a pointer cast: that would break strict aliasing).
 *
 * Arithmetic (add/sub/mul/div/sqrt/fma, int->float) runs on the host FPU
 * with the rounding mode set through <fenv.h>, and the host's exception
 * flags are translated into fcsr.fflags. That gives correctly rounded
 * results and exact flags, including subnormals and underflow (x86 SSE,
 * like RISC-V, detects tininess after rounding).
 *
 * Everything where RISC-V's rules differ from what C/the host does is
 * done by hand on the bits:
 *   - NaN results are always the canonical NaN 0x7fc00000 (x86 would
 *     produce 0xffc00000; C makes no promise at all).
 *   - fmin/fmax: a NaN operand is ignored if the other is a number;
 *     -0 < +0; signalling NaNs raise NV.
 *   - feq is quiet (NV only for signalling NaNs); flt/fle signal NV for
 *     any NaN.
 *   - float->int conversions saturate and raise NV when out of range or
 *     NaN (C's cast is undefined behaviour there).
 *   - fsgnj*, fmv, fclass, flw/fsw never touch a float at all.
 *   - Round-to-nearest-max-magnitude (RMM) has no <fenv.h> equivalent.
 *     It only differs from round-to-nearest-even when the exact result
 *     lies exactly halfway between two floats; see d2f() and fp_arith().
 *   - inf * 0 in a fused multiply-add raises NV even when the addend is a
 *     quiet NaN (IEEE leaves that case to the implementation; RISC-V
 *     requires it).
 *
 * Rounding mode: the instruction's rm field (funct3), or fcsr.frm when
 * rm = 7 (dyn). rm values 5 and 6, or dyn with an invalid frm, make the
 * instruction illegal.
 */

#define OP_LOAD_FP  0x07
#define OP_STORE_FP 0x27
#define OP_FP       0x53
#define OP_FMADD    0x43
#define OP_FMSUB    0x47
#define OP_FNMSUB   0x4B
#define OP_FNMADD   0x4F

#define CANONICAL_NAN 0x7fc00000u
#define SIGN_BIT      0x80000000u

/* ------------------------------------------------------------------ */
/* Helpers you may use (static inline: no warning while unused)        */
/* ------------------------------------------------------------------ */

static inline float f_of(uint32_t bits) { float f; memcpy(&f, &bits, sizeof f); return f; }
static inline uint32_t bits_of(float f) { uint32_t b; memcpy(&b, &f, sizeof b); return b; }

static inline int is_nan_bits(uint32_t b)  { return (b & 0x7f800000u) == 0x7f800000u && (b & 0x007fffffu) != 0; }
static inline int is_snan_bits(uint32_t b) { return is_nan_bits(b) && (b & 0x00400000u) == 0; }

#define FREG(idx) cpu_read_freg(c, (int)(idx))

/* ================================================================== */
/* Helper Functions for RV32F IEEE 754 Operations                    */
/* ================================================================== */

typedef enum { AR_ADD, AR_SUB, AR_MUL, AR_DIV, AR_SQRT, AR_FMA } ArithOp;

/* Resolves instruction rounding mode (funct3 or cpu_frm). Returns -1 on illegal. */
static inline int resolve_rm(CPUState *c, const DecodedInstr *i) {
    int rm = (int)i->funct3;
    if (rm == RM_DYN) {
        rm = (int)cpu_frm(c);
        if (rm > 4) {
            c->status = CPU_ILLEGAL_INSTRUCTION;
            c->fault_addr = c->pc;
            snprintf(c->fault_msg, sizeof(c->fault_msg), "illegal dynamic rounding mode %d", rm);
            return -1;
        }
        return rm;
    }
    if (rm > 4) {
        c->status = CPU_ILLEGAL_INSTRUCTION;
        c->fault_addr = c->pc;
        snprintf(c->fault_msg, sizeof(c->fault_msg), "illegal static rounding mode %d", rm);
        return -1;
    }
    return rm;
}

/* Maps RISC-V rounding mode (0..3) to C fenv FE_* constant. */
static inline int host_mode(int rm) {
    switch (rm) {
        case RM_RNE: return FE_TONEAREST;
        case RM_RTZ: return FE_TOWARDZERO;
        case RM_RDN: return FE_DOWNWARD;
        case RM_RUP: return FE_UPWARD;
        default:     return FE_TONEAREST;
    }
}

/* Translates host fetestexcept() bits into RISC-V FFLAG_* bits. */
static inline uint32_t fflags_of(int fe) {
    uint32_t f = 0;
    if (fe & FE_INVALID)   f |= FFLAG_NV;
    if (fe & FE_DIVBYZERO) f |= FFLAG_DZ;
    if (fe & FE_OVERFLOW)  f |= FFLAG_OF;
    if (fe & FE_UNDERFLOW) f |= FFLAG_UF;
    if (fe & FE_INEXACT)   f |= FFLAG_NX;
    return f;
}

/* Converts exact double to float in RMM mode (ties round away from zero). */
static inline float d2f_rmm(double d) {
    if (isnan(d) || isinf(d)) return (float)d;
    fesetround(FE_TONEAREST);
    float r = (float)d;
    double diff = fabs(d - (double)r);
    if (diff == 0.0) return r;
    float r_away = nextafterf(r, (d > 0) ? INFINITY : -INFINITY);
    double diff_away = fabs(d - (double)r_away);
    if (diff == diff_away) {
        return r_away; /* Exact tie: break away from zero */
    }
    return r;
}

/*
 * RMM support. Round-to-nearest-max-magnitude gives the same result AND the
 * same flags as round-to-nearest-even except when the exact result lies
 * exactly halfway between two floats; then RMM takes the neighbour with the
 * larger magnitude. (Flags agree: NX is set either way, overflow happens at
 * the same tie, and in a tie below FLT_MIN the exact value has at most 24
 * significant bits, so it is tiny under both modes.)
 *
 * A halfway value has at most 25 significant bits and lies well inside the
 * double range, so if the exact result is NOT representable as a double it
 * cannot be a tie. exact_double() returns the exact result as a double when
 * it is one, using error-free transformations (must run in FE_TONEAREST):
 *   add/sub: TwoSum        mul: float*float is always exact in double
 *   fma:     exact product, then TwoSum      div: residual a - q*b via fma
 * sqrt never ties (its result is always a normal number).
 */
static int exact_double(ArithOp op, float fa, float fb, float fc, double *out) {
    double a = fa, b = fb, c = fc, s, e;
    switch (op) {
        case AR_SUB: b = -b; /* fall through */
        case AR_ADD: { s = a + b; double bv = s - a; e = (a - (s - bv)) + (b - bv); break; }
        case AR_MUL: s = a * b; e = 0.0; break;
        case AR_FMA: { double p = a * b; s = p + c; double bv = s - p; e = (p - (s - bv)) + (c - bv); break; }
        case AR_DIV: s = a / b; e = isfinite(s) ? fma(-s, b, a) : 1.0; break;
        default: return 0;
    }
    if (!isfinite(s) || e != 0.0) return 0;
    *out = s;
    return 1;
}

/* Turns the round-to-nearest-even result `r` of the exact value `exact`
 * into the round-to-nearest-max-magnitude result. */
static float rne_to_rmm(float r, double exact) {
    if (isnan(r) || isinf(r) || (double)r == exact) return r;
    float other = nextafterf(r, (exact > (double)r) ? INFINITY : -INFINITY);
    if (fabs(exact - (double)r) != fabs(exact - (double)other)) return r; /* not a tie */
    return (fabsf(other) > fabsf(r)) ? other : r;
}

/* Unified floating-point arithmetic evaluation. */
static uint32_t fp_arith(ArithOp op, uint32_t A, uint32_t B, uint32_t C, int rm, uint32_t *flags) {
    /* 1. Signalling NaN operand always triggers invalid operation (NV) */
    if (is_snan_bits(A) || is_snan_bits(B) || (op == AR_FMA && is_snan_bits(C))) {
        *flags |= FFLAG_NV;
    }

    float fa = f_of(A), fb = f_of(B), fc = f_of(C);

    /* 2. RISC-V rule: inf * 0 in FMA triggers NV regardless of addend */
    if (op == AR_FMA && ((isinf(fa) && fb == 0.0f) || (fa == 0.0f && isinf(fb)))) {
        *flags |= FFLAG_NV;
    }

    /* 3. Execute on the host FPU. RMM runs as RNE and is corrected below. */
    fesetround(host_mode(rm == RM_RMM ? RM_RNE : rm));
    feclearexcept(FE_ALL_EXCEPT);
    float r = 0.0f;
    switch (op) {
        case AR_ADD:  r = fa + fb; break;
        case AR_SUB:  r = fa - fb; break;
        case AR_MUL:  r = fa * fb; break;
        case AR_DIV:  r = fa / fb; break;
        case AR_SQRT: r = sqrtf(fa); break;
        case AR_FMA:  r = fmaf(fa, fb, fc); break;
    }
    int ex = fetestexcept(FE_ALL_EXCEPT);
    fesetround(FE_TONEAREST); /* Defensive restore (exact_double needs it too) */
    *flags |= fflags_of(ex);
    if (isnan(r)) return CANONICAL_NAN;

    if (rm == RM_RMM) {
        double exact;
        if (exact_double(op, fa, fb, fc, &exact)) r = rne_to_rmm(r, exact);
    }
    return bits_of(r);
}

/* Helper for standard arithmetic dispatch */
static void do_arith(CPUState *c, DecodedInstr *i, ArithOp op, uint32_t A, uint32_t B, uint32_t C) {
    int rm = resolve_rm(c, i);
    if (rm < 0) return;
    uint32_t flags = 0;
    uint32_t r = fp_arith(op, A, B, C, rm, &flags);
    cpu_raise_fflags(c, flags);
    cpu_write_freg(c, i->rd, r);
}

/* ---- loads / stores: raw 32-bit moves ---- */

static void exec_flw(CPUState *c, Memory *m, DecodedInstr *i) {
    uint32_t addr = cpu_read_reg(c, i->rs1) + (uint32_t)i->imm;
    uint32_t v;
    if (memory_read_word(m, addr, &v) != MEM_OK) {
        c->status = CPU_MEM_FAULT;
        c->fault_addr = addr;
        snprintf(c->fault_msg, sizeof(c->fault_msg), "flw at 0x%08x out of bounds", addr);
        return;
    }
    cpu_write_freg(c, i->rd, v);
}

static void exec_fsw(CPUState *c, Memory *m, DecodedInstr *i) {
    uint32_t addr = cpu_read_reg(c, i->rs1) + (uint32_t)i->imm;
    uint32_t v = FREG(i->rs2);
    if (memory_write_word(m, addr, v) != MEM_OK) {
        c->status = CPU_MEM_FAULT;
        c->fault_addr = addr;
        snprintf(c->fault_msg, sizeof(c->fault_msg), "fsw at 0x%08x out of bounds", addr);
        return;
    }
}

/* ---- arithmetic ---- */

static void exec_fadd(CPUState *c, Memory *m, DecodedInstr *i) { (void)m; do_arith(c, i, AR_ADD, FREG(i->rs1), FREG(i->rs2), 0); }
static void exec_fsub(CPUState *c, Memory *m, DecodedInstr *i) { (void)m; do_arith(c, i, AR_SUB, FREG(i->rs1), FREG(i->rs2), 0); }
static void exec_fmul(CPUState *c, Memory *m, DecodedInstr *i) { (void)m; do_arith(c, i, AR_MUL, FREG(i->rs1), FREG(i->rs2), 0); }
static void exec_fdiv(CPUState *c, Memory *m, DecodedInstr *i) { (void)m; do_arith(c, i, AR_DIV, FREG(i->rs1), FREG(i->rs2), 0); }
static void exec_fsqrt(CPUState *c, Memory *m, DecodedInstr *i) { (void)m; do_arith(c, i, AR_SQRT, FREG(i->rs1), 0, 0); }

/* Fused multiply-add family: one rounding of (+-rs1 * rs2) +- rs3.
 * Negating = flipping the sign bit, which never signals. */
static void exec_fmadd(CPUState *c, Memory *m, DecodedInstr *i) {
    (void)m; do_arith(c, i, AR_FMA, FREG(i->rs1), FREG(i->rs2), FREG(i->rs3));
}
static void exec_fmsub(CPUState *c, Memory *m, DecodedInstr *i) {
    (void)m; do_arith(c, i, AR_FMA, FREG(i->rs1), FREG(i->rs2), FREG(i->rs3) ^ SIGN_BIT);
}
static void exec_fnmsub(CPUState *c, Memory *m, DecodedInstr *i) {
    (void)m; do_arith(c, i, AR_FMA, FREG(i->rs1) ^ SIGN_BIT, FREG(i->rs2), FREG(i->rs3));
}
static void exec_fnmadd(CPUState *c, Memory *m, DecodedInstr *i) {
    (void)m; do_arith(c, i, AR_FMA, FREG(i->rs1) ^ SIGN_BIT, FREG(i->rs2), FREG(i->rs3) ^ SIGN_BIT);
}

/* ---- min / max ---- */

static void exec_fmin(CPUState *c, Memory *m, DecodedInstr *i) {
    (void)m;
    uint32_t A = FREG(i->rs1), B = FREG(i->rs2);
    if (is_snan_bits(A) || is_snan_bits(B)) cpu_raise_fflags(c, FFLAG_NV);
    if (is_nan_bits(A) && is_nan_bits(B)) { cpu_write_freg(c, i->rd, CANONICAL_NAN); return; }
    if (is_nan_bits(A)) { cpu_write_freg(c, i->rd, B); return; }
    if (is_nan_bits(B)) { cpu_write_freg(c, i->rd, A); return; }
    /* Both zeros: -0.0 | +0.0 = -0.0 */
    if ((A & ~SIGN_BIT) == 0 && (B & ~SIGN_BIT) == 0) {
        cpu_write_freg(c, i->rd, A | B);
        return;
    }
    cpu_write_freg(c, i->rd, (f_of(A) < f_of(B)) ? A : B);
}

static void exec_fmax(CPUState *c, Memory *m, DecodedInstr *i) {
    (void)m;
    uint32_t A = FREG(i->rs1), B = FREG(i->rs2);
    if (is_snan_bits(A) || is_snan_bits(B)) cpu_raise_fflags(c, FFLAG_NV);
    if (is_nan_bits(A) && is_nan_bits(B)) { cpu_write_freg(c, i->rd, CANONICAL_NAN); return; }
    if (is_nan_bits(A)) { cpu_write_freg(c, i->rd, B); return; }
    if (is_nan_bits(B)) { cpu_write_freg(c, i->rd, A); return; }
    /* Both zeros: -0.0 & +0.0 = +0.0 */
    if ((A & ~SIGN_BIT) == 0 && (B & ~SIGN_BIT) == 0) {
        cpu_write_freg(c, i->rd, A & B);
        return;
    }
    cpu_write_freg(c, i->rd, (f_of(A) > f_of(B)) ? A : B);
}

/* ---- sign injection (pure bit operations, no flags, no NaN rules) ---- */

static void exec_fsgnj(CPUState *c, Memory *m, DecodedInstr *i) {
    (void)m;
    cpu_write_freg(c, i->rd, (FREG(i->rs1) & ~SIGN_BIT) | (FREG(i->rs2) & SIGN_BIT));
}
static void exec_fsgnjn(CPUState *c, Memory *m, DecodedInstr *i) {
    (void)m;
    cpu_write_freg(c, i->rd, (FREG(i->rs1) & ~SIGN_BIT) | (~FREG(i->rs2) & SIGN_BIT));
}
static void exec_fsgnjx(CPUState *c, Memory *m, DecodedInstr *i) {
    (void)m;
    cpu_write_freg(c, i->rd, FREG(i->rs1) ^ (FREG(i->rs2) & SIGN_BIT));
}

/* ---- comparisons (result in an x register) ---- */

static void exec_feq(CPUState *c, Memory *m, DecodedInstr *i) {
    (void)m;
    uint32_t A = FREG(i->rs1), B = FREG(i->rs2);
    if (is_snan_bits(A) || is_snan_bits(B)) cpu_raise_fflags(c, FFLAG_NV);
    int r = !is_nan_bits(A) && !is_nan_bits(B) && (f_of(A) == f_of(B));
    cpu_write_reg(c, i->rd, r ? 1 : 0);
}
static void exec_flt(CPUState *c, Memory *m, DecodedInstr *i) {
    (void)m;
    uint32_t A = FREG(i->rs1), B = FREG(i->rs2);
    if (is_nan_bits(A) || is_nan_bits(B)) {
        cpu_raise_fflags(c, FFLAG_NV);
        cpu_write_reg(c, i->rd, 0);
        return;
    }
    cpu_write_reg(c, i->rd, (f_of(A) < f_of(B)) ? 1 : 0);
}
static void exec_fle(CPUState *c, Memory *m, DecodedInstr *i) {
    (void)m;
    uint32_t A = FREG(i->rs1), B = FREG(i->rs2);
    if (is_nan_bits(A) || is_nan_bits(B)) {
        cpu_raise_fflags(c, FFLAG_NV);
        cpu_write_reg(c, i->rd, 0);
        return;
    }
    cpu_write_reg(c, i->rd, (f_of(A) <= f_of(B)) ? 1 : 0);
}

/* Helper for rounding float to integer in double */
static inline double round_by_mode(double d, int rm) {
    switch (rm) {
        case RM_RNE: fesetround(FE_TONEAREST); return nearbyint(d);
        case RM_RTZ: return trunc(d);
        case RM_RDN: return floor(d);
        case RM_RUP: return ceil(d);
        case RM_RMM: return round(d);
        default:     return d;
    }
}

/* ---- conversions ---- */

static void exec_fcvt_w_s(CPUState *c, Memory *m, DecodedInstr *i) {
    (void)m;
    int rm = resolve_rm(c, i);
    if (rm < 0) return;
    uint32_t A = FREG(i->rs1);
    if (is_nan_bits(A)) {
        cpu_raise_fflags(c, FFLAG_NV);
        cpu_write_reg(c, i->rd, 0x7fffffffu);
        return;
    }
    double d = (double)f_of(A);
    double r = round_by_mode(d, rm);
    if (r >= 2147483648.0) { /* Overflow >= 2^31 */
        cpu_raise_fflags(c, FFLAG_NV);
        cpu_write_reg(c, i->rd, 0x7fffffffu);
    } else if (r < -2147483648.0) { /* Underflow < -2^31 */
        cpu_raise_fflags(c, FFLAG_NV);
        cpu_write_reg(c, i->rd, 0x80000000u);
    } else {
        if (r != d) cpu_raise_fflags(c, FFLAG_NX);
        cpu_write_reg(c, i->rd, (uint32_t)(int32_t)r);
    }
}

static void exec_fcvt_wu_s(CPUState *c, Memory *m, DecodedInstr *i) {
    (void)m;
    int rm = resolve_rm(c, i);
    if (rm < 0) return;
    uint32_t A = FREG(i->rs1);
    if (is_nan_bits(A)) {
        cpu_raise_fflags(c, FFLAG_NV);
        cpu_write_reg(c, i->rd, 0xffffffffu);
        return;
    }
    double d = (double)f_of(A);
    double r = round_by_mode(d, rm);
    if (r >= 4294967296.0) { /* Overflow >= 2^32 */
        cpu_raise_fflags(c, FFLAG_NV);
        cpu_write_reg(c, i->rd, 0xffffffffu);
    } else if (r < 0.0) { /* Negative: clipped to 0, NV only */
        cpu_raise_fflags(c, FFLAG_NV);
        cpu_write_reg(c, i->rd, 0);
    } else {
        if (r != d) cpu_raise_fflags(c, FFLAG_NX);
        cpu_write_reg(c, i->rd, (uint32_t)r);
    }
}

static void exec_fcvt_s_w(CPUState *c, Memory *m, DecodedInstr *i) {
    (void)m;
    int rm = resolve_rm(c, i);
    if (rm < 0) return;
    int32_t val = (int32_t)cpu_read_reg(c, i->rs1);
    if (rm == RM_RMM) {
        float f = d2f_rmm((double)val);
        if ((double)f != (double)val) cpu_raise_fflags(c, FFLAG_NX);
        cpu_write_freg(c, i->rd, bits_of(f));
    } else {
        fesetround(host_mode(rm));
        feclearexcept(FE_ALL_EXCEPT);
        float f = (float)val;
        int ex = fetestexcept(FE_ALL_EXCEPT);
        fesetround(FE_TONEAREST);
        cpu_raise_fflags(c, fflags_of(ex));
        cpu_write_freg(c, i->rd, bits_of(f));
    }
}

static void exec_fcvt_s_wu(CPUState *c, Memory *m, DecodedInstr *i) {
    (void)m;
    int rm = resolve_rm(c, i);
    if (rm < 0) return;
    uint32_t val = cpu_read_reg(c, i->rs1);
    if (rm == RM_RMM) {
        float f = d2f_rmm((double)val);
        if ((double)f != (double)val) cpu_raise_fflags(c, FFLAG_NX);
        cpu_write_freg(c, i->rd, bits_of(f));
    } else {
        fesetround(host_mode(rm));
        feclearexcept(FE_ALL_EXCEPT);
        float f = (float)val;
        int ex = fetestexcept(FE_ALL_EXCEPT);
        fesetround(FE_TONEAREST);
        cpu_raise_fflags(c, fflags_of(ex));
        cpu_write_freg(c, i->rd, bits_of(f));
    }
}

/* ---- moves between register files (bits unchanged) ---- */

static void exec_fmv_x_w(CPUState *c, Memory *m, DecodedInstr *i) {
    (void)m;
    cpu_write_reg(c, i->rd, FREG(i->rs1));
}
static void exec_fmv_w_x(CPUState *c, Memory *m, DecodedInstr *i) {
    (void)m;
    cpu_write_freg(c, i->rd, cpu_read_reg(c, i->rs1));
}

/* ---- classify: exactly one bit set ---- */

static void exec_fclass(CPUState *c, Memory *m, DecodedInstr *i) {
    (void)m;
    uint32_t b = FREG(i->rs1);
    uint32_t sign = (b >> 31) & 1;
    uint32_t exp  = (b >> 23) & 0xFF;
    uint32_t frac = b & 0x7FFFFF;
    uint32_t res  = 0;

    if (exp == 0xFF) {
        if (frac == 0) {
            res = sign ? (1 << 0) : (1 << 7); /* -inf / +inf */
        } else {
            res = (frac & 0x00400000) ? (1 << 9) : (1 << 8); /* qNaN / sNaN */
        }
    } else if (exp == 0) {
        if (frac == 0) {
            res = sign ? (1 << 3) : (1 << 4); /* -0 / +0 */
        } else {
            res = sign ? (1 << 2) : (1 << 5); /* -subnormal / +subnormal */
        }
    } else {
        res = sign ? (1 << 1) : (1 << 6); /* -normal / +normal */
    }
    cpu_write_reg(c, i->rd, res);
}

/* ------------------------------------------------------------------ */
/* Registration                                                       */
/* ------------------------------------------------------------------ */

#define FFF (IF_RD_FP | IF_RS1_FP | IF_RS2_FP)   /* fd, fs1, fs2 */

static void reg(const char *mn, uint32_t opcode, int32_t f3, int32_t f7, InstrFormat fmt,
                ExecuteFn fn, uint32_t flags, uint32_t rs2_sel) {
    register_instruction_ex((InstructionHandler){mn, opcode, f3, f7, fmt, fn}, flags, rs2_sel);
}

void register_rv32f(void) {
    reg("flw",       OP_LOAD_FP,  0x2, -1,   FMT_I_LOAD, exec_flw,  IF_RD_FP,  0);
    reg("fsw",       OP_STORE_FP, 0x2, -1,   FMT_S,      exec_fsw,  IF_RS2_FP, 0);

    reg("fadd.s",    OP_FP, -1, 0x00, FMT_R,  exec_fadd,  FFF | IF_HAS_RM, 0);
    reg("fsub.s",    OP_FP, -1, 0x04, FMT_R,  exec_fsub,  FFF | IF_HAS_RM, 0);
    reg("fmul.s",    OP_FP, -1, 0x08, FMT_R,  exec_fmul,  FFF | IF_HAS_RM, 0);
    reg("fdiv.s",    OP_FP, -1, 0x0C, FMT_R,  exec_fdiv,  FFF | IF_HAS_RM, 0);
    reg("fsqrt.s",   OP_FP, -1, 0x2C, FMT_R2, exec_fsqrt, IF_RD_FP | IF_RS1_FP | IF_HAS_RM | IF_RS2_FIXED, 0);

    reg("fsgnj.s",   OP_FP, 0x0, 0x10, FMT_R, exec_fsgnj,  FFF, 0);
    reg("fsgnjn.s",  OP_FP, 0x1, 0x10, FMT_R, exec_fsgnjn, FFF, 0);
    reg("fsgnjx.s",  OP_FP, 0x2, 0x10, FMT_R, exec_fsgnjx, FFF, 0);
    reg("fmin.s",    OP_FP, 0x0, 0x14, FMT_R, exec_fmin,   FFF, 0);
    reg("fmax.s",    OP_FP, 0x1, 0x14, FMT_R, exec_fmax,   FFF, 0);

    reg("feq.s",     OP_FP, 0x2, 0x50, FMT_R, exec_feq, IF_RS1_FP | IF_RS2_FP, 0);
    reg("flt.s",     OP_FP, 0x1, 0x50, FMT_R, exec_flt, IF_RS1_FP | IF_RS2_FP, 0);
    reg("fle.s",     OP_FP, 0x0, 0x50, FMT_R, exec_fle, IF_RS1_FP | IF_RS2_FP, 0);

    reg("fcvt.w.s",  OP_FP, -1, 0x60, FMT_R2, exec_fcvt_w_s,  IF_RS1_FP | IF_HAS_RM | IF_RS2_FIXED, 0);
    reg("fcvt.wu.s", OP_FP, -1, 0x60, FMT_R2, exec_fcvt_wu_s, IF_RS1_FP | IF_HAS_RM | IF_RS2_FIXED, 1);
    reg("fcvt.s.w",  OP_FP, -1, 0x68, FMT_R2, exec_fcvt_s_w,  IF_RD_FP  | IF_HAS_RM | IF_RS2_FIXED, 0);
    reg("fcvt.s.wu", OP_FP, -1, 0x68, FMT_R2, exec_fcvt_s_wu, IF_RD_FP  | IF_HAS_RM | IF_RS2_FIXED, 1);

    reg("fmv.x.w",   OP_FP, 0x0, 0x70, FMT_R2, exec_fmv_x_w, IF_RS1_FP | IF_RS2_FIXED, 0);
    reg("fclass.s",  OP_FP, 0x1, 0x70, FMT_R2, exec_fclass,  IF_RS1_FP | IF_RS2_FIXED, 0);
    reg("fmv.w.x",   OP_FP, 0x0, 0x78, FMT_R2, exec_fmv_w_x, IF_RD_FP  | IF_RS2_FIXED, 0);
    /* Older spellings of the two moves (accepted by RARS too). Registered
     * after the official names, so decoding finds those first. */
    reg("fmv.x.s",   OP_FP, 0x0, 0x70, FMT_R2, exec_fmv_x_w, IF_RS1_FP | IF_RS2_FIXED, 0);
    reg("fmv.s.x",   OP_FP, 0x0, 0x78, FMT_R2, exec_fmv_w_x, IF_RD_FP  | IF_RS2_FIXED, 0);

    /* R4: funct7 holds rs3 and the 2-bit format field (00 = single), so
     * it is "don't care" for the registry. */
    reg("fmadd.s",   OP_FMADD,  -1, -1, FMT_R4, exec_fmadd,  FFF | IF_RS3_FP | IF_HAS_RM, 0);
    reg("fmsub.s",   OP_FMSUB,  -1, -1, FMT_R4, exec_fmsub,  FFF | IF_RS3_FP | IF_HAS_RM, 0);
    reg("fnmsub.s",  OP_FNMSUB, -1, -1, FMT_R4, exec_fnmsub, FFF | IF_RS3_FP | IF_HAS_RM, 0);
    reg("fnmadd.s",  OP_FNMADD, -1, -1, FMT_R4, exec_fnmadd, FFF | IF_RS3_FP | IF_HAS_RM, 0);
}
