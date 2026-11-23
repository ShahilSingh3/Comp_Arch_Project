# M-extension edge cases (div/rem by zero, INT_MIN/-1, high products).
# Expected values derived by hand from the RISC-V M spec.
# RUN: --hazard stall --predictor 2bit
# GRADE: int
.text
.globl main
main:
    lui    s0, 0x80000        # s0 = 0x80000000 (INT_MIN)
    addi   s1, x0, -1         # s1 = -1
    addi   s2, x0, 7
    addi   s3, x0, -3
    div    a0, s0, s1         # INT_MIN / -1  = INT_MIN
    rem    a1, s0, s1         # INT_MIN % -1  = 0
    div    a2, s2, x0         # 7 / 0         = -1
    divu   a3, s2, x0         # 7 /u 0        = 0xffffffff
    rem    a4, s3, x0         # -3 % 0        = -3
    remu   a5, s2, x0         # 7 %u 0        = 7
    div    a6, s3, s2         # -3 / 7        = 0 (truncates)
    rem    t0, s3, s2         # -3 % 7        = -3
    mulh   t1, s0, s0         # (2^62) >> 32  = 0x40000000
    mulhu  t2, s1, s1         # 0xfffffffe
    mulhsu t3, s1, s2         # -1 * 7u = -7  -> 0xffffffff
    mul    t4, s3, s2         # -21
    divu   t5, s1, s2         # 0xffffffff / 7 = 0x24924924
    remu   t6, s1, s2         # 3
    addi   a7, x0, 10         # 2 stalls before ecall (stall unit)
    ecall
