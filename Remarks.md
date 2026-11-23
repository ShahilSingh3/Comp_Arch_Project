# Remarks — CS F342 Computer Architecture Lab Project

> **Course**: CS F342 — Computer Architecture, BITS Pilani
> **Submission deadline**: 25/11/2026 23:59
> **Submission format**: `Final_<STUDENTID1_STUDENTID2>.zip` (see the naming note under *Submission checklist*)
> **Status as of**: 23/11/2026, ~03:30 IST, `riscv_sim_base_updated/` (Phases 1–4 last changed 02:25, Zicsr 02:27, F 03:18 after the RMM fix)

---

## 1. At a glance

| Phase | Lab tag in code | What it covers | Status |
|---|---|---|---|
| 1 | `TODO(lab1)` | 5-stage pipeline core: `stage_if/id/ex/mem/wb`, `control_from_instr()` | ✅ Done, verified |
| 2 | `TODO(lab2)` | Hazard unit: `forward_mux()`, `hazard_stall.c`, `hazard_forward.c` | ✅ Done, verified |
| 3 | `TODO(lab3)` | Branch prediction: `jal` pre-decode in `predict_next()`, btfn, 1-bit, 2-bit | ✅ Done, verified |
| 4 | `TODO(lab4)` | M extension: 8 handlers in `isa/rv32m.c` | ✅ Done, verified |
| 5 | `TODO(lab5)` | F extension (`isa/rv32f.c`, 27 handlers) + Zicsr (`isa/zicsr.c`, 6 handlers) | ✅ Done, verified |

**Official self-check** (`student_package_updated/evaluate.py` on `sample.s`, current tree, re-run 23/11 02:40):

```
running: rvsim sample.s --batch --hazard forward --predictor 2bit
graded registers: x1..x31, pc, f0..f31, fcsr (65 values)
register state match= 100% ,counter match = 100%
```

- **Counters: 100%.** All 9 graded counters match the reference.
- **Registers: 100%.** All 65 graded values (`x1..x31`, `pc`, `f0..f31`, and `fcsr`) match bit-exact.
- All 15 sample programs give the same registers as `--single-cycle` under `stall` and `forward`. All 23 goldens recorded on the final design (15 `lab4_branch`, 8 `lab5_fp`) match exactly. 5 older Lab 1/2-level goldens differ only by the expected "free `jal`" saving (see the note in §4).

---

## 2. Requirements (source of truth: `readme1.txt`, `evaluation.txt`, `student_package_updated/README.md`)

- **Scope of the final evaluation:** RV32I + 5-stage pipeline + hazards/forwarding + branch prediction + M + F with `fcsr`.
- **5 hidden tests:**
  - 3 cover RV32I, the pipeline and M (`# GRADE: int`: `x1–x31`, `pc`, 32 values).
  - 2 use F (`# GRADE: int+fp`: also `f0–f31` and `fcsr`, 65 values).
- **How it is run:** the evaluator copies the folder, deletes `*.o *.d rvsim`, replaces `ui/` with `ref_ui/`, then runs `make` and `./rvsim test.s --batch <options from # RUN:>`. Any `--hazard`/`--predictor` combination or `--single-cycle` may be used.
- **Marks per test:**
  - Registers: 12 if every graded register is correct, otherwise 3.
  - Counters: 2 + 6 × (fraction correct). The graded counters are `cycles retired stalls branches mispredicts jal jalr redirects squashed`.
  - The final mark is the average register mark plus the average counter mark, rounded up, out of 20.
- **Hard failures:**
  - A build failure gives 0 on all 5 tests.
  - MOSS similarity above 95% gives 0/20.
  - A successful build with similarity at or below 95% guarantees at least 5.
- **Limits:** each run is capped at 10 s, and `--batch` must never wait for input.

---

## 3. How the simulator works

### 3.1 Layout
```
riscv_sim_base_updated/
├── main.c, Makefile          CLI + build (Makefile also has a `make test` target for the M tests)
├── core/                     CPU state (x0–x31, f0–f31, fcsr), memory, decoder, ISA registry,
│                             single-cycle executor (the oracle), SimEngine interface — given
├── assembler/                two-pass assembler (li/la = lui+addi, .float, FP/CSR pseudo-ops) — given
├── isa/                      rv32i_base.c (given), rv32m.c, rv32f.c, zicsr.c (all done)
├── pipeline/                 pipeline.c, control.c, hazard_*.c, bp_*.c — our work
├── ui/                       replaced by ref_ui/ at evaluation time — never edit
└── tests/                    our M-extension tests (unit test + 3 asm programs)
```

### 3.2 One clock cycle (`pipeline_step()`, `pipeline/pipeline.c:449`)
1. **Snapshot.** The latches are copied into `cur`, so hazard decisions and forwarded values use start-of-cycle contents.
2. **Hazard unit.** The selected `HazardDetector` sees the control signals of ID, EX, MEM and WB. It returns `stall` and `fwd1/fwd2/fwd3`.
3. **Stage order.** The stages run in reverse (WB, MEM, EX, ID, IF), so each one reads its input latch before the stage behind it overwrites it.
   - **WB** (`stage_wb`, l.67): writes `ctrl.dst` and counts `retired`. A fault carried down the pipe is raised here, which keeps exceptions precise. An `ecall` exit stops the machine here, and `pc` is set to the ecall's pc.
   - **MEM** (`stage_mem`, l.119): does the load or store using `mem_size`/`mem_unsigned`. An out-of-bounds access becomes a carried fault, and the younger instruction in EX is discarded.
   - **EX** (`stage_ex`, l.186):
     - Operands pass through `forward_mux()` (l.173), which picks EX/MEM `alu_result` or MEM/WB `wb_val`.
     - A load or store only computes its address here.
     - Every other instruction runs its ISA handler on a **scratch `CPUState`** that holds just the forwarded operands, `pc` and `fcsr`. The result is read back from `dst`, and `fcsr` is copied back to the real CPU.
     - Branches and jumps resolve here. If the real next pc differs from `pred_next`, EX redirects the pipeline: IF and ID are squashed, giving a 2-cycle penalty.
   - **ID** (`stage_id`, l.289): decodes, calls `control_from_instr()`, and reads sources from the register file. WB has already written this cycle, so a producer 3 instructions ahead is visible without forwarding. A stall freezes IF/ID and sends a bubble into EX.
   - **IF** (`stage_if`, l.382): fetches at `fetch_pc` and calls `predict_next()` (l.365):
     - A conditional branch asks the predictor (target = `pc + imm_b`).
     - A `jal` goes straight to `pc + imm_j`, so it costs nothing.
     - Everything else, including `jalr`, continues at `pc + 4`.

### 3.3 Control signals (`pipeline/control.c:39`)
- **Unified register numbers:** `x1–x31` map to 1–31, `f0–f31` map to 32–63, and `x0` maps to `REG_NONE`. The hazard unit and forwarding therefore handle FP registers with no extra code, and `x0` is never treated as a dependency.
- **Derived from the instruction format** (`FMT_*` plus the `IF_*_FP` flags): `src1/src2/src3/dst`, `mem_op/mem_size/mem_unsigned`, `is_branch/is_jump/is_ecall`. `ecall` reads `a7` (src1) and `a0` (src2).

### 3.4 Hazard units (`--hazard`)
| Unit | Behaviour |
|---|---|
| `none` | No stalls and no forwarding, so RAW hazards read stale values (the Lab 1 behaviour, kept for comparison) |
| `stall` | Stalls ID while an instruction in EX or MEM writes one of ID's sources |
| `forward` (default) | EX/MEM→EX has priority over MEM/WB→EX. A load sitting in MEM is never forwarded. A 1-cycle stall when a load in EX feeds the instruction in ID |

### 3.5 Branch predictors (`--predictor`; 64-entry table indexed by `pc[7:2]`)
| Predictor | Rule |
|---|---|
| `not-taken` | always predicts not taken |
| `btfn` | predicts taken when `target < pc` |
| `1bit` | predicts the last outcome; table starts all not-taken |
| `2bit` (default) | saturating counter 0–3, predicts taken at ≥2, starts at 1 (weakly not taken) |

### 3.6 Counter identities (these hold on every run)
```
cycles    = retired + 4 + 2 × redirects + stalls
redirects = mispredicts + jalr        (jal is free: it is redirected in IF)
squashed  = 2 × redirects             (one fewer when the slot in ID was already a bubble)
```

### 3.7 M extension (`isa/rv32m.c`)
- **No undefined behaviour:**
  - Low products are computed in `uint32_t`.
  - High products use exact 64-bit intermediates.
  - Division by zero and `INT32_MIN / -1` are handled before the C division, using the values the spec requires:

    | Case | div | divu | rem | remu |
    |---|---|---|---|---|
    | `x / 0` | `-1` | `0xFFFFFFFF` | `x` | `x` |
    | `INT32_MIN / -1` | `INT32_MIN` | n/a | `0` | n/a |
- **No pipeline changes needed:** every M instruction is `FMT_R`, so the pipeline treats it like any other ALU operation.

---

## 4. Verification performed

All checks ran on a clean copy built with the reference `ui/`, never on the working tree.

| Check | Result |
|---|---|
| Clean `make` with `-Wall -Wextra` and the reference `ui/` | ✅ builds with **0 warnings** |
| Golden outputs, `sample_tests/lab2_pipeline` (6) | ✅ 6/6, using `--hazard none --predictor not-taken`, the configuration these goldens were recorded with (before `jal` pre-decode) |
| Golden outputs, `sample_tests/lab3_hazards` (12) | ✅ 12/12 before `jal` pre-decode was added. After it, `h3_kernel` stall/forward differ only by the expected "free `jal`" saving (see the note below) |
| Golden outputs, `sample_tests/lab4_branch` (15): all 4 predictors + single-cycle | ✅ **15/15** (registers, status, every counter) |
| `Cycles references.pdf`: pipeline column for p1/p2/p3 | ✅ 113 / 57 / 762 with `jal` treated as before (Lab 1/2 level) |
| `make test`: M unit assertions + 3 M programs | ✅ pass |
| M handlers against an independent `__int128` reference: 8 ops × 25,800 operand pairs, plus aliasing and `rd = x0` | ✅ 0 mismatches |
| Random programs (ALU, M, all loads/stores, branches, `jal`/`jalr`, loops, prints) vs `--single-cycle`, 2 hazard units × 4 predictors | ✅ 4,400 runs, 0 failures. Registers, output, status and `retired` match, and the cycle identity holds every time |
| All 10 counters (including `forwards`) compared against an independently written hazard/predictor implementation: random programs plus all sample tests, 3 hazard units × 4 predictors | ✅ 11,580 runs, 0 differences |
| Fault paths (out-of-bounds load/store, illegal instruction, a faulting instruction on a squashed wrong path, `--max-cycles` on an infinite loop) | ✅ same registers, pc and status as the oracle. Only the fault-message text differs; it comes from the skeleton and is not graded |
| Golden outputs, `sample_tests/lab5_fp` (8): forward, stall, single-cycle | ✅ **8/8** (all 65 registers incl. `fcsr`, every counter) |
| Every F handler against an independent exact model (Python `Fraction` arithmetic, all 5 rounding modes + dynamic `frm`, all 5 flags, tininess after rounding), on edge values, NaN/sNaN/inf/subnormals, constructed ties and random bits | ✅ 210,055 cases, 0 mismatches (after the RMM fix below) |
| Illegal rounding modes (`rm` = 5/6, or dynamic with `frm` = 5–7) | ✅ `ILLEGAL_INSTRUCTION`, no register or flag written |
| Zicsr: all 6 instructions on `fflags`/`frm`/`fcsr` against hand-computed values, incl. masking, no-write for `x0`/`uimm 0`, unknown CSR | ✅ correct in single-cycle, `stall` and `forward` |
| Random FP + CSR programs (FP↔FP/int dependences, `flw` load-use, `fsw` data, FMA `rs3`, `fcvt`, `fmv`, CSR read/write of flags and `frm`, branches) vs `--single-cycle`: 2 hazard units × 4 predictors | ✅ 420 programs, 3,360 runs, 0 failures (registers, `f` registers, `fcsr`, output, status, `retired`, cycle identity) |
| `evaluate.py sample.s sample.txt` (current tree) | ✅ **100% registers, 100% counters** |

> The integer random-program and counter cross-check rows were run on the 02:25 snapshot. The Phase 1–4 files are unchanged since; only `isa/rv32f.c` changed (the fix below).

### Bug fixed on 23/11 (03:18): RMM rounding in `isa/rv32f.c`
The exact-model test found 1,270 mismatches out of 60,055, **all with rounding mode RMM** (`rm=4` or dynamic `frm=4`). Every other mode and every other instruction was already exact.
- **Missing NX.** `fadd`/`fsub`/`fmadd` family computed in `double` and then rounded to float. When the operand magnitudes differ widely (e.g. `1.0 + 2⁻⁸⁰`), the double rounding lost the sticky bits, so NX was not raised. FMA could also tie-break wrong.
- **Wrong fdiv result.** `fdiv` assumed "division never ties", which is false for subnormal results (e.g. `5·2⁻¹⁴⁹ / 2` = 2.5 ulp must round to 3 ulp, but gave 2).
- **Missing UF.** A hand-written underflow check missed the tininess-after-rounding edge (e.g. a product of exactly `2⁻¹²⁶ − 2⁻¹⁵⁰`).

**Fix.** RMM now runs on the host FPU as RNE, which gives the right result and flags except at an exact tie. An error-free check (TwoSum for add/sub/FMA, exact product for mul, FMA residual for div) recovers the exact result as a `double` when one exists. If it lies exactly halfway, the result is replaced by the neighbour with the larger magnitude (`exact_double()` + `rne_to_rmm()`). After the fix: 210,055 cases with 0 mismatches, `sample.s` still at 100%/100%, and all 23 final-design goldens pass.

> **Note on old goldens.** The `lab2_pipeline` goldens (and `h3_kernel` in `lab3_hazards`) were recorded on a pipeline where `jal` was resolved in EX. With the Phase 3 pre-decode, every `jal` saves exactly 2 cycles, one redirect and two squashes. For example, p1 drops from 113 to 111 cycles (1 `jal`) and p3 from 762 to 748 (7 `jal`s). This is the behaviour the final evaluator expects: `sample.txt` counts `jal = 1` with no redirect.

---

## 5. Phase 5 (F extension + Zicsr): spec rules the implementation must follow

The pipeline handles FP registers, `src3` and `fcsr` with no changes. Phase 5 lives entirely in the handler bodies, and this list doubles as a review checklist for them:

- **`isa/rv32f.c`:**
  - Loads/stores: `flw fsw`
  - Arithmetic: `fadd fsub fmul fdiv fsqrt`
  - Fused multiply-add: `fmadd fmsub fnmsub fnmadd`
  - Min/max: `fmin fmax`
  - Sign injection: `fsgnj fsgnjn fsgnjx`
  - Comparisons: `feq flt fle`
  - Conversions: `fcvt.w.s fcvt.wu.s fcvt.s.w fcvt.s.wu`
  - Moves: `fmv.x.w fmv.w.x`
  - Classify: `fclass`
- **`isa/zicsr.c`:** `csrrw csrrs csrrc csrrwi csrrsi csrrci` for `fflags` (0x001), `frm` (0x002) and `fcsr` (0x003). The handler writes the old value to `rd`, then writes, sets or clears the CSR. `rs1 = x0` or `uimm = 0` means no write for the set and clear forms.

**RISC-V rules that differ from plain C (each one is easy to get wrong):**
1. **NaN results:** every NaN result must be the canonical `0x7fc00000`. x86 produces `0xffc00000`.
2. **Moving bits:** use `memcpy` between `float` and `uint32_t`. Pointer casts break strict aliasing.
3. **Flags:** raise them from the host FPU with `feclearexcept` / `fetestexcept` and map them onto `FFLAG_NV/DZ/OF/UF/NX`.
4. **Rounding:**
   - `rm = 7` means use `frm` from `fcsr`.
   - RNE, RTZ, RDN and RUP map to `fesetround`.
   - **RMM has no `<fenv.h>` mode**, so handle it by hand.
   - A reserved rounding mode is an illegal instruction.
5. **`fmin`/`fmax`:**
   - If one operand is NaN, return the other.
   - If both are NaN, return the canonical NaN.
   - A signalling NaN raises NV.
   - `-0.0 < +0.0`.
6. **Comparisons:** `feq` is quiet (NV only for a signalling NaN). `flt` and `fle` raise NV for any NaN.
7. **`fcvt.w[u].s`:** saturate on overflow and NaN and raise NV (NaN converts to the maximum value). This replaces C's undefined behaviour.
8. **No canonicalisation for these:** `fsgnj*`, `fmv.*`, `flw` and `fsw` move raw bits.
9. **Fused multiply-add:** use `fmaf` for a single rounding. Compute the negated forms by flipping signs, not by negating the result.
10. **`fclass`:** returns a one-hot 10-bit mask.

Test against `sample_tests/lab5_fp/` (`f1_broad`, `f2_special`, `f3_kernel` have goldens for forward, stall and single-cycle) and `sample.s`. **The goal is `register state match= 100% ,counter match = 100%`.** Cross-check values with RARS 1.6; where RARS deviates on NaN cases, the spec wins.

---

## 6. Known notes and caveats (none affect the current tests)

- **Branch to the next instruction:** a taken branch whose target is `pc + 4` is recorded as not taken, because `stage_ex` infers *taken* from `next_pc != pc + 4`. It makes no difference to timing and only touches the predictor table in a pathological case.
- **`--hazard none`:** gives wrong results by design whenever a RAW hazard exists. This includes every `li`/`la`, because each expands to a dependent `lui`+`addi` pair.
- **Lab sheet design differs from the skeleton:** Lab Sheets 2–4 describe an older design (own `pipeline_state.h`, a `--pipeline` flag, a `flushes` counter, `cycles = retired + stalls + flushes + 4`). The skeleton's interfaces, counters and identities in §3 are what the evaluator uses.
- **Lab numbering offset:** the code's `labN` tags are one lower than the lab sheet numbers. The pipeline is Sheet 2 / `lab1`, hazards are Sheet 3 / `lab2`, branch prediction is Sheet 4 / `lab3`, and F is Sheet 5 / `lab5`. M is `lab4`.
- **`Makefile`:** has an extra `test` target. It is harmless because the evaluator only runs `make`.

---

## 7. Build, run, test

```bash
cd riscv_sim_base_updated
make                                                  # builds ./rvsim
./rvsim prog.s --batch --single-cycle                 # reference oracle
./rvsim prog.s --batch                                # pipeline, defaults: --hazard forward --predictor 2bit
./rvsim prog.s --batch --hazard stall --predictor btfn
./rvsim prog.s --batch --trace                        # per-cycle stage occupancy
make test                                             # M-extension tests
python3 ../student_package_updated/evaluate.py . ../student_package_updated/sample.s \
        ../student_package_updated/sample.txt --verbose
```

---

## 8. Submission checklist

- [x] A plain `make` builds `rvsim` from a clean copy with the reference `ui/`, with no warnings
- [x] `cpu_read_freg()`, `cpu_freg_abi_name()`, `cpu_frm()`, `cpu->fcsr` and `FFLAG_*` are present (from the skeleton)
- [x] All CLI flags work: `--batch --single-cycle --hazard none|stall|forward --predictor not-taken|btfn|1bit|2bit --max-cycles N`
- [x] Statistics text in `pipeline_format_stats()` is unchanged (the evaluator parses it)
- [x] Counter match is 100% on `sample.s`
- [x] **Phase 5:** F + Zicsr handlers, verified 100% register match on `sample.s` and all `lab5_fp` goldens passing
- [ ] Remove build artefacts and junk before zipping: `*.o`, `*.d`, `rvsim`, `tests/test_rv32m`, and the stray `-.o` in the project root
- [ ] Run `evaluate.py` on the **final zip itself** (`python3 evaluate.py Final_<IDs>.zip sample.s sample.txt`)
- [ ] Naming: `readme1.txt` says the zip is `Final_<STUDENTID1_STUDENTID2>.zip`, while `Steps to follow.pdf` shows a folder and zip named `FA_<ID1>_<ID2>`. Follow the instructor announcement (`Final_…zip`), with the source folder (containing the `Makefile`) as the single top-level folder inside the zip.
