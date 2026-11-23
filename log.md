# RV32IMF Simulator — Test Results

**Last run:** 23 Nov 2026 · **Code under test:** `riscv_sim_base_updated/` · **Evaluator:** `student_package_updated/evaluate.py` (unmodified, with `ref_ui/`)

## Summary

| Area | Status |
|---|---|
| Build: clean copy + reference `ui/` | ✅ Builds, 0 warnings (also 0 under `-pedantic -Wshadow -Wstrict-prototypes`) |
| Official `sample.s` | ✅ register 100% (65/65), counter 100% |
| Submission zip (`Final_<IDs>.zip` layout) | ✅ Builds and scores 100% / 100% on `sample.s` and `fp_hand.s` |
| Custom hand-checked tests (5) | ✅ 5/5 at 100% / 100% |
| Reference goldens, Lab 4 + Lab 5 (23) | ✅ 23/23 at 100% / 100% |
| Reference goldens, Lab 2 + Lab 3 (18) | ⚠️ 13/18 exact. The other 5 match all registers; their counters differ only because those goldens predate free `jal` (explained below, no fix needed) |
| Run time | ✅ 1,000,000 cycles in 0.32 s (limit 10 s) |
| CLI options (`--batch --single-cycle --hazard --predictor --max-cycles`) | ✅ |
| `make test` (RV32M unit tests) | ✅ passes |

**Code changes needed: none.**

## How to re-run

```bash
python3 eval_tests/run_all.py                      # all 46 tests through evaluate.py
python3 eval_tests/run_all.py Final_<IDs>.zip      # same, on the submission zip
python3 student_package_updated/evaluate.py riscv_sim_base_updated \
        student_package_updated/sample.s student_package_updated/sample.txt --verbose
```

- `eval_tests/custom/`: 5 new tests. Expected `.txt` files are produced by `make_expected.py` from **hand-derived** values (RISC-V spec + the cycle model below), not from the simulator.
- `eval_tests/goldens/`: the 41 `sample_tests` goldens, with `# RUN:`/`# GRADE:` headers added (`make_goldens.py`).
- `eval_tests/` lies outside `riscv_sim_base_updated/`, so it is not part of the submission zip.

## 1. Official sample

```
running: rvsim sample.s --batch --hazard forward --predictor 2bit
graded registers: x1..x31, pc, f0..f31, fcsr (65 values)
register state match= 100% ,counter match = 100%
```

## 2. Custom tests (hand-derived expected values)

| Test | Options | Covers | Regs | Counters |
|---|---|---|---|---|
| `m_edge` | stall, 2bit | div/rem by 0, `INT_MIN/-1`, mulh/mulhu/mulhsu, divu/remu | 32/32 | 9/9 |
| `branch_stall_btfn` | stall, btfn | loop branch (T,T,T,NT), load-use, `jal` call, `jalr` return; 15 stalls, 47 cycles | 32/32 | 9/9 |
| `branch_fwd_2bit` | forward, 2bit | same program; 2 mispredicts, 1 load-use stall, 35 cycles | 32/32 | 9/9 |
| `fp_hand` | forward, 1bit | all F groups, FMA, RTZ/RUP (static and dynamic), canonical NaN, ±inf, −0, fclass, saturating fcvt, all 5 flags, all Zicsr forms incl. `frflags/fsflags/fsrm/frrm/frcsr`, FP load-use stall | 65/65 | 9/9 |
| `fp_hand_sc` | single-cycle | same program | 65/65 | 1/1 |

Cycle model used for the hand-derived counters:
`cycles = retired + 4 + 2×redirects + stalls`, `redirects = mispredicts + jalr` (`jal` is redirected in IF at no cost). `sample.txt` agrees: `jal=1, jalr=1, mispredicts=2, redirects=3`.

## 3. Reference goldens (`sample_tests/`, 41)

| Set | Result |
|---|---|
| `lab5_fp` (8: forward / stall / single-cycle, 65 regs) | ✅ 8/8 exact |
| `lab4_branch` (15: 4 predictors + single-cycle) | ✅ 15/15 exact |
| `lab3_hazards` (12) | ✅ 10/12 exact; `h3_kernel` forward and stall ⚠️ |
| `lab2_pipeline` (6) | ✅ 3/6 exact (all single-cycle); 3 pipeline runs ⚠️ |

### ⚠️ The 5 non-exact goldens: expected, no fix needed

All 5 match on every register. Their counters differ by exactly the number of `jal`s in the program:

| Golden | Expected → got | `jal`s |
|---|---|---|
| `p1_broad.pipeline` | cycles 113→111, redirects 9→8, squashed 18→16 | 1 |
| `p2_edge.pipeline` | cycles 57→51, redirects 5→2, squashed 10→4 | 3 |
| `p3_kernel.pipeline` | cycles 762→748, redirects 50→43, squashed 100→86 | 7 |
| `h3_kernel.forward` | cycles 461→447, redirects 50→43, squashed 100→86 | 7 |
| `h3_kernel.stall` | cycles 749→742, redirects 50→43, squashed 100→86, stalls 324→331 | 7 |

These goldens were recorded on the Lab 2/3 pipeline, which resolved `jal` in EX (2-cycle penalty). The final reference resolves `jal` in IF for free. Evidence: the Lab 4 not-taken goldens (`b3_applied`: `jal=336, jalr=191, mispredicts=145, redirects=336`) and `sample.txt` both follow `redirects = mispredicts + jalr`. The final evaluation runs the Lab 5 reference, so the current behaviour is the right one. Changing it would break `sample.txt` and all Lab 4/5 goldens.

## 4. Open items / before submitting

- [ ] Run `make clean` and delete `rvsim_asan.d` in `riscv_sim_base_updated/` (build leftovers; the evaluator ignores them, but they bloat the zip).
- [ ] Zip the folder as `Final_<ID1_ID2>.zip`:
  `zip -r Final_<IDs>.zip riscv_sim_base_updated -x "*.o" "*.d" "*/rvsim" "*/tests/test_rv32m"`
- [ ] Re-run `python3 eval_tests/run_all.py Final_<IDs>.zip` on the final zip.
- Your own test programs need a `main:` label. Without it, execution starts at address 0, which is the data section when the program has `.data`.
- Not checkable locally: MOSS similarity (must be ≤ 95%) and the 5 hidden tests.
