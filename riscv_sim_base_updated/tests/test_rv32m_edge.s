# test_rv32m_edge.s - Corner cases (div-by-zero, overflow, INT_MIN)
.text
.globl main
main:
    addi s0, x0, 121
    addi s1, x0, 0          # divisor = 0
    addi s2, x0, -1         # divisor = -1 (0xFFFFFFFF)
    lui  s3, 0x80000        # s3 = 0x80000000 (INT32_MIN)

    # Division by zero
    div  t0, s0, s1         # 121 / 0 -> -1 (0xFFFFFFFF)
    divu t1, s0, s1         # 121 / 0 unsigned -> 2^32-1 (0xFFFFFFFF)
    rem  t2, s0, s1         # 121 % 0 -> dividend (121 = 0x79)
    remu t3, s0, s1         # 121 % 0 unsigned -> dividend (121 = 0x79)

    # Overflow: INT32_MIN / -1
    div  t4, s3, s2         # INT32_MIN / -1 -> INT32_MIN (0x80000000)
    rem  t5, s3, s2         # INT32_MIN % -1 -> 0

    # Negative remainder sign
    addi s4, x0, -20
    addi s5, x0, 7
    rem  t6, s4, s5         # -20 % 7 -> -6 (0xFFFFFFFA)

    # x0 write discard
    mul  x0, s0, s0         # Must not modify x0

    addi a7, x0, 10
    ecall
