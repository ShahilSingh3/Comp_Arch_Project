#!/usr/bin/env python3
"""Writes the expected .txt for each custom test from HAND-DERIVED values.
Nothing here comes from running the simulator. Registers not listed are 0.
Counter model (Remarks.md 3.6, confirmed by the reference goldens):
  cycles = retired + 4 + 2*redirects + stalls ; redirects = mispredicts + jalr
"""
import os
HERE = os.path.dirname(os.path.abspath(__file__))
ABI = ["zero","ra","sp","gp","tp","t0","t1","t2","s0","s1","a0","a1","a2","a3","a4","a5",
       "a6","a7","s2","s3","s4","s5","s6","s7","s8","s9","s10","s11","t3","t4","t5","t6"]
FABI = ["ft0","ft1","ft2","ft3","ft4","ft5","ft6","ft7","fs0","fs1","fa0","fa1","fa2","fa3","fa4","fa5",
        "fa6","fa7","fs2","fs3","fs4","fs5","fs6","fs7","fs8","fs9","fs10","fs11","ft8","ft9","ft10","ft11"]

def dump(x, f, fcsr, pc, cnt, single=False):
    X = [x.get(ABI[i], 0) for i in range(32)]
    F = [f.get(i, 0) for i in range(32)]
    out = ["---------------------------- registers ----------------------------"]
    for r in range(8):
        out.append("  ".join("x%-2d/%-4s=0x%08x" % (i, ABI[i], X[i]) for i in (r, r+8, r+16, r+24)))
    out.append("------------------------- fp registers ----------------------------")
    for r in range(8):
        out.append("  ".join("f%-2d/%-4s=0x%08x" % (i, FABI[i], F[i]) for i in (r, r+8, r+16, r+24)))
    out.append("fcsr =0x%08x" % fcsr)
    out.append("pc = 0x%08x" % pc)
    out.append("status = ECALL_EXIT   cycles = %d" % cnt["cycles"])
    if not single:
        for k in ["retired","stalls","branches","mispredicts","jal","jalr","redirects","squashed"]:
            out.append("%-15s = %d" % (k, cnt[k]))
    return "\n".join(out) + "\n"

def counters(retired, stalls, branches, mispredicts, jal, jalr, squashed):
    redirects = mispredicts + jalr
    return dict(cycles=retired + 4 + 2*redirects + stalls, retired=retired, stalls=stalls,
                branches=branches, mispredicts=mispredicts, jal=jal, jalr=jalr,
                redirects=redirects, squashed=squashed)

tests = {}

# ---- m_edge: 20 instructions straight-line, ecall at 19*4. Stall unit: only
#      addi a7 -> ecall (distance 1) = 2 stalls.
tests["m_edge"] = dump(dict(
    s0=0x80000000, s1=0xffffffff, s2=7, s3=0xfffffffd,
    a0=0x80000000, a1=0, a2=0xffffffff, a3=0xffffffff, a4=0xfffffffd, a5=7, a6=0,
    t0=0xfffffffd, t1=0x40000000, t2=0xfffffffe, t3=0xffffffff, t4=0xffffffeb,
    t5=0x24924924, t6=3, a7=10), {}, 0, 0x4c,
    counters(retired=20, stalls=2, branches=0, mispredicts=0, jal=0, jalr=0, squashed=0))

# ---- branch program: retired = 3 + 4*3 + 3 + 1 (jal) + 2 (func) + 3 = 24
br_regs = dict(t0=4, t1=4, s0=6, s1=6, s2=11, ra=0x28, s3=111, s4=112, a7=10)
# stall+btfn: btfn predicts the backward blt taken -> only the last iteration
# mispredicts. Stalls: add<-addi s0 (2), blt<-addi t0 x4 iterations (8),
# addi s2<-lw (2), addi s3<-addi s2 across jal (1), ecall<-addi a7 (2) = 15.
tests["branch_stall_btfn"] = dump(br_regs, {}, 0, 0x30,
    counters(retired=24, stalls=15, branches=4, mispredicts=1, jal=1, jalr=1, squashed=4))
# forward+2bit: counter starts weakly-NT -> mispredict on 1st and last
# iteration. Stalls: only the lw -> addi s2 load-use (1).
tests["branch_fwd_2bit"] = dump(br_regs, {}, 0, 0x30,
    counters(retired=24, stalls=1, branches=4, mispredicts=2, jal=1, jalr=1, squashed=6))

# ---- fp_hand: 72 instructions straight-line (li = 2), ecall at 71*4 = 0x11c.
fp_x = dict(t0=0x80000000, a0=1, a1=1, a2=0, a3=0x200, a4=0x8, a5=0xfffffffe, a6=0xfffffffe,
            s1=0, s2=0x7fffffff, s3=0xfffffff9, s4=0x3dcccccd, s5=0x1d, s6=0x1d, s7=3, s8=0,
            s9=3, s10=0x61, s11=1, t3=5, t4=3, t5=0x25, t6=0x426fffff, a7=10)
fp_f = {0: 0x426fffff, 1: 0x3fc00000, 2: 0xc0100000, 3: 0x40400000, 4: 0x3dcccccd, 5: 0x7149f2ca,
        6: 0x80000000, 7: 0xbf400000, 8: 0x3fb33333, 9: 0x7f800000, 10: 0x41f00000, 11: 0xff800000,
        12: 0x3fddb3d7, 13: 0x7fc00000, 14: 0x80000000, 15: 0x3fc00000, 16: 0xbec00000,
        17: 0xc0cc0000, 18: 0x3ec00000, 19: 0x40cc0000, 20: 0xbfc00000, 21: 0x3fc00000,
        22: 0x40100000, 23: 0x3dcccccd, 24: 0xbdcccccd, 25: 0x40100000, 26: 0xc0e00000,
        27: 0x4f800000, 28: 0xfffffff9, 29: 0x40466667, 30: 0x41efffff, 31: 0x41efffff}
FCSR = (1 << 5) | 0x05          # frm = RTZ, flags = OF | NX
tests["fp_hand"] = dump(fp_x, fp_f, FCSR, 0x11c,
    counters(retired=72, stalls=1, branches=0, mispredicts=0, jal=0, jalr=0, squashed=0))
tests["fp_hand_sc"] = dump(fp_x, fp_f, FCSR, 0x11c, dict(cycles=72), single=True)

for name, text in tests.items():
    with open(os.path.join(HERE, name + ".txt"), "w") as fh:
        fh.write(text)
    print("wrote", name + ".txt")
