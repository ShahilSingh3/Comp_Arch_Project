# Same program as fp_hand.s, single-cycle (only cycles is a graded counter).
# RUN: --single-cycle
# GRADE: int+fp
.text
.globl main
main:
    li   t0, 0x3fc00000
    fmv.w.x f1, t0            # 1.5
    li   t0, 0xc0100000
    fmv.w.x f2, t0            # -2.25
    li   t0, 0x40400000
    fmv.w.x f3, t0            # 3.0
    li   t0, 0x3dcccccd
    fmv.w.x f4, t0            # 0.1f
    li   t0, 0x7149f2ca
    fmv.w.x f5, t0            # 1e30f
    li   t0, 0x80000000
    fmv.w.x f6, t0            # -0.0
    fadd.s   f7, f1, f2       # -0.75 exact
    fsub.s   f8, f1, f4, rtz  # 1.3999999 (RTZ)            NX
    fmul.s   f9, f5, f5       # +inf                       OF NX
    fdiv.s   f10, f3, f4      # 30.0 (RNE)                 NX
    fdiv.s   f11, f1, f6      # -inf                       DZ
    fsqrt.s  f12, f3          # 1.7320508                  NX
    fsqrt.s  f13, f2          # canonical NaN              NV
    fmin.s   f14, f1, f6      # -0.0
    fmax.s   f15, f13, f1     # 1.5 (qNaN ignored, no NV)
    fmadd.s  f16, f1, f2, f3  # -0.375
    fmsub.s  f17, f1, f2, f3  # -6.375
    fnmadd.s f18, f1, f2, f3  # 0.375
    fnmsub.s f19, f1, f2, f3  # 6.375
    fsgnj.s  f20, f1, f2      # -1.5
    fsgnjn.s f21, f1, f2      # 1.5
    fsgnjx.s f22, f2, f2      # 2.25
    fmv.s    f23, f4
    fneg.s   f24, f4
    fabs.s   f25, f2
    feq.s    a0, f1, f1       # 1
    flt.s    a1, f2, f1       # 1
    fle.s    a2, f13, f1      # 0, NV
    fclass.s a3, f13          # 0x200 quiet NaN
    fclass.s a4, f6           # 0x008 -0
    fcvt.w.s a5, f2           # -2 (RNE)
    fcvt.w.s a6, f2, rup      # -2
    fcvt.wu.s s1, f2          # 0, NV
    fcvt.w.s s2, f5           # 0x7fffffff, NV
    li       s3, -7
    fcvt.s.w  f26, s3         # -7.0
    fcvt.s.wu f27, s3         # 4294967296.0 NX
    fmv.x.w  s4, f4
    fmv.w.x  f28, s3
    frflags  s5               # 0x1d (NV DZ OF NX)
    fsflags  s6, zero         # s6 = 0x1d, flags cleared
    li       s7, 3
    fsrm     s8, s7           # s8 = 0, frm = RUP
    fadd.s   f29, f4, f3      # 3.1000001 (RUP)            NX
    frrm     s9               # 3
    frcsr    s10              # 0x61
    csrrsi   s11, fflags, 4   # s11 = 1, flags = 5
    csrrci   t3, fflags, 1    # t3 = 5, flags = 4
    csrrwi   t4, frm, 1       # t4 = 3, frm = RTZ
    fdiv.s   f30, f3, f4      # 29.999998 (RTZ)            NX
    csrr     t5, fcsr         # 0x25
    fsw      f30, 1024(x0)
    flw      f31, 1024(x0)
    fadd.s   f0, f31, f31     # load-use: 1 stall; 59.999996 exact
    fmv.x.w  t6, f0
    li       a7, 10
    ecall
