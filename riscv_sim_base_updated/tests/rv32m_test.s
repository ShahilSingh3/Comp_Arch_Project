# rv32m_test.s - Comprehensive self-checking verification for all 8 RV32M instructions
# Exits with gp = 0 (and a0 = 0) on success, or gp = test_id on failure.

.text
.globl main
main:
    # 1. mul test: 15 * 6 = 90
    addi s0, x0, 15
    addi s1, x0, 6
    mul  t0, s0, s1
    addi t1, x0, 90
    bne  t0, t1, fail_1

    # 2. mulh test: -15 * 6 = -90 -> high word 0xFFFFFFFF (-1)
    addi s2, x0, -15
    mulh t0, s2, s1
    addi t1, x0, -1
    bne  t0, t1, fail_2

    # 3. mulhsu test: -15 (signed) * 6 (unsigned) = -90 -> high word 0xFFFFFFFF (-1)
    mulhsu t0, s2, s1
    addi t1, x0, -1
    bne  t0, t1, fail_3

    # 4. mulhu test: 15 * 6 = 90 -> high word 0
    mulhu t0, s0, s1
    bne  t0, x0, fail_4

    # 5. div test: 15 / 6 = 2
    div  t0, s0, s1
    addi t1, x0, 2
    bne  t0, t1, fail_5

    # 6. divu test: 15 / 6 = 2
    divu t0, s0, s1
    addi t1, x0, 2
    bne  t0, t1, fail_6

    # 7. rem test: 15 % 6 = 3
    rem  t0, s0, s1
    addi t1, x0, 3
    bne  t0, t1, fail_7

    # 8. remu test: 15 % 6 = 3
    remu t0, s0, s1
    addi t1, x0, 3
    bne  t0, t1, fail_8

    # 9. div by zero: 15 / 0 -> -1 (0xFFFFFFFF)
    div  t0, s0, x0
    addi t1, x0, -1
    bne  t0, t1, fail_9

    # 10. divu by zero: 15 / 0 -> 0xFFFFFFFF
    divu t0, s0, x0
    addi t1, x0, -1
    bne  t0, t1, fail_10

    # 11. rem by zero: 15 % 0 -> dividend (15)
    rem  t0, s0, x0
    bne  t0, s0, fail_11

    # 12. remu by zero: 15 % 0 -> dividend (15)
    remu t0, s0, x0
    bne  t0, s0, fail_12

    # 13. div overflow: INT32_MIN / -1 -> INT32_MIN
    lui  s3, 0x80000
    addi s4, x0, -1
    div  t0, s3, s4
    bne  t0, s3, fail_13

    # 14. rem overflow: INT32_MIN % -1 -> 0
    rem  t0, s3, s4
    bne  t0, x0, fail_14

    # 15. negative remainder: -20 % 7 -> -6
    addi s5, x0, -20
    addi s6, x0, 7
    rem  t0, s5, s6
    addi t1, x0, -6
    bne  t0, t1, fail_15

    # 16. x0 write discard: mul x0, s0, s0 must leave x0 == 0
    mul  x0, s0, s0
    bne  x0, x0, fail_16

    # Success: gp = 0, a0 = 0
    addi gp, x0, 0
    addi a0, x0, 0
    addi a7, x0, 10
    ecall

fail_1:
    addi gp, x0, 1
    addi a7, x0, 10
    ecall
fail_2:
    addi gp, x0, 2
    addi a7, x0, 10
    ecall
fail_3:
    addi gp, x0, 3
    addi a7, x0, 10
    ecall
fail_4:
    addi gp, x0, 4
    addi a7, x0, 10
    ecall
fail_5:
    addi gp, x0, 5
    addi a7, x0, 10
    ecall
fail_6:
    addi gp, x0, 6
    addi a7, x0, 10
    ecall
fail_7:
    addi gp, x0, 7
    addi a7, x0, 10
    ecall
fail_8:
    addi gp, x0, 8
    addi a7, x0, 10
    ecall
fail_9:
    addi gp, x0, 9
    addi a7, x0, 10
    ecall
fail_10:
    addi gp, x0, 10
    addi a7, x0, 10
    ecall
fail_11:
    addi gp, x0, 11
    addi a7, x0, 10
    ecall
fail_12:
    addi gp, x0, 12
    addi a7, x0, 10
    ecall
fail_13:
    addi gp, x0, 13
    addi a7, x0, 10
    ecall
fail_14:
    addi gp, x0, 14
    addi a7, x0, 10
    ecall
fail_15:
    addi gp, x0, 15
    addi a7, x0, 10
    ecall
fail_16:
    addi gp, x0, 16
    addi a7, x0, 10
    ecall
