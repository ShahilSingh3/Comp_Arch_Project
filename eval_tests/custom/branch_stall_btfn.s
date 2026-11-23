# Loop + load-use + call/return, stall unit with BTFN.
# Counters derived by hand (see eval_tests/README.md).
# RUN: --hazard stall --predictor btfn
# GRADE: int
.text
.globl main
main:
    addi t0, x0, 0           # i = 0
    addi t1, x0, 4           # n = 4
    addi s0, x0, 0           # sum = 0
loop:
    add  s0, s0, t0          # sum += i
    addi t0, t0, 1           # i++
    blt  t0, t1, loop        # backward branch: T,T,T,NT
    sw   s0, 1024(x0)
    lw   s1, 1024(x0)
    addi s2, s1, 5           # load-use
    jal  ra, func            # jal: redirected in IF (free)
    addi s4, s3, 1           # return lands here (0x28)
    addi a7, x0, 10
    ecall
func:
    addi s3, s2, 100
    jalr x0, 0(ra)           # jalr: always a redirect
