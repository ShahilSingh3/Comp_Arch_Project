# test_rv32m_broad.s - Exercises all 8 RV32M instructions
.text
.globl main
main:
    addi s0, x0, 15         # s0 = 15
    addi s1, x0, 6          # s1 = 6
    addi s2, x0, -15        # s2 = -15 (0xFFFFFFF1)

    mul    t0, s0, s1       # 15 * 6 = 90 (0x5A)
    mulh   t1, s0, s1       # high word = 0
    mulhsu t2, s2, s1       # -15 (signed) * 6 (unsigned) = -90 -> high word = 0xFFFFFFFF
    mulhu  t3, s0, s1       # 15 * 6 unsigned -> high word = 0
    div    t4, s0, s1       # 15 / 6 = 2
    divu   t5, s0, s1       # 15 / 6 unsigned = 2
    rem    t6, s0, s1       # 15 % 6 = 3
    remu   a2, s0, s1       # 15 % 6 unsigned = 3

    addi a7, x0, 10         # Exit ecall
    ecall
