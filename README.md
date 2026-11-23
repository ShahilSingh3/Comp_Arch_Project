# RV32IMF Pipelined Processor Simulator

A cycle-accurate simulator for a **5-stage pipelined RISC-V processor** (RV32I + M + F + Zicsr), written in C.
Built for the *CS F342 – Computer Architecture* lab project at BITS Pilani.

The simulator runs RISC-V assembly (`.s`) programs directly: it includes its own two-pass assembler, a
single-cycle reference executor, and a pipelined engine with selectable hazard handling and branch prediction.

## Features

| Area | What is implemented |
|---|---|
| **Pipeline** | Classic 5 stages: IF → ID → EX → MEM → WB, with precise exceptions (faults travel down the pipe and are raised in WB) |
| **Hazards** | `none`, `stall` (interlock), `forward` (EX/MEM→EX and MEM/WB→EX forwarding plus a 1-cycle load-use stall) |
| **Branch prediction** | `not-taken`, `btfn`, `1bit`, `2bit` (64-entry table indexed by `pc[7:2]`); `jal` is redirected in IF at no cost |
| **ISA** | RV32I, **M** (mul/div, spec-exact corner cases), **F** (27 instructions, all 5 rounding modes, all 5 exception flags), **Zicsr** (`fflags`, `frm`, `fcsr`) |
| **Execution modes** | Pipelined, or `--single-cycle` (the functional oracle) |
| **Statistics** | cycles, retired, stalls, branches, mispredicts, jal, jalr, redirects, squashed, forwards, CPI, predictor accuracy |

## Quick start

Requires `gcc` and `make` (Python 3.8+ for the test scripts).

```bash
cd riscv_sim_base_updated
make                                                  # builds ./rvsim

./rvsim prog.s --batch                                # pipelined, defaults: --hazard forward --predictor 2bit
./rvsim prog.s --batch --single-cycle                 # reference oracle
./rvsim prog.s --batch --hazard stall --predictor btfn
./rvsim prog.s --batch --trace                        # per-cycle stage occupancy
make test                                             # RV32M unit tests + M programs
```

### Command-line options

| Option | Values | Default |
|---|---|---|
| `--batch` | run to completion and print the final state (never waits for input) | – |
| `--single-cycle` | use the functional single-cycle executor | off |
| `--hazard` | `none` \| `stall` \| `forward` | `forward` |
| `--predictor` | `not-taken` \| `btfn` \| `1bit` \| `2bit` | `2bit` |
| `--max-cycles N` | stop after N cycles | – |
| `--trace` | print per-cycle stage occupancy | off |

> `--hazard none` gives wrong results by design whenever a RAW hazard exists, including every `li`/`la`
> (each expands to a dependent `lui`+`addi`). It is kept for comparison only.

Test programs need a `main:` label. Without it, execution starts at address 0, which is the data section
when the program has `.data`.

## Repository layout

```
.
├── riscv_sim_base_updated/     The simulator (this is what gets submitted)
│   ├── main.c, Makefile        CLI and build (the Makefile also has a `test` target)
│   ├── core/                   CPU state (x0–x31, f0–f31, fcsr), memory, decoder, ISA registry,
│   │                           single-cycle executor, SimEngine interface
│   ├── assembler/              Two-pass assembler (li/la, .float, FP/CSR pseudo-ops)
│   ├── isa/                    rv32i_base.c, rv32m.c, rv32f.c, zicsr.c
│   ├── pipeline/               pipeline.c, control.c, hazard_*.c, bp_*.c
│   ├── ui/                     Output formatting (replaced by the reference ui/ at evaluation time)
│   └── tests/                  M-extension unit test and assembly programs
├── student_package_updated/    Official sample.s / sample.txt and evaluate.py self-check
├── sample_tests/               Reference goldens per lab (lab2–lab5)
├── eval_tests/                 Our test harness: run_all.py, custom hand-checked tests, goldens
├── Lab Sheets/                 Lab sheets 2–5
├── RISC-V_Simulator_Codebase_Manual.md   Codebase manual
├── Remarks.md                  Design notes, spec rules and verification record
├── log.md                      Latest test results
└── evaluation.txt, *.pdf       Course grading rules and reference material
```

## How the simulator works

### One clock cycle (`pipeline_step()` in `pipeline/pipeline.c`)

1. **Snapshot** the pipeline latches so hazard decisions use start-of-cycle values.
2. **Hazard unit** inspects ID/EX/MEM/WB control signals and returns `stall` and forwarding selects.
3. **Stages run in reverse order** (WB, MEM, EX, ID, IF) so each stage reads its input latch before the
   stage behind it overwrites it.

| Stage | Responsibility |
|---|---|
| **IF** | Fetch at `fetch_pc`; `predict_next()` picks the next PC (predictor for branches, direct jump for `jal`, `pc+4` otherwise) |
| **ID** | Decode, `control_from_instr()`, register read. WB writes before ID reads, so a producer 3 instructions ahead needs no forwarding |
| **EX** | Forwarding muxes, ALU/FPU via the ISA handler (run on a scratch `CPUState`), address calculation, branch/`jalr` resolution and redirect (2-cycle penalty) |
| **MEM** | Loads and stores; out-of-bounds accesses become carried faults |
| **WB** | Register write-back, `retired` count, fault raising, `ecall` exit |

Integer and FP registers share one numbering (`x1–x31` → 1–31, `f0–f31` → 32–63, `x0` → none), so the hazard
and forwarding logic handles FP with no extra code.

### Hazard units

| Unit | Behaviour |
|---|---|
| `none` | No stalls, no forwarding |
| `stall` | Stalls ID while an instruction in EX or MEM writes one of ID's sources |
| `forward` | EX/MEM→EX has priority over MEM/WB→EX; 1-cycle stall for a load in EX feeding ID |

### Branch predictors

| Predictor | Rule |
|---|---|
| `not-taken` | Always not taken |
| `btfn` | Taken when the target is backward (`target < pc`) |
| `1bit` | Last outcome; table starts not-taken |
| `2bit` | Saturating counter 0–3, taken at ≥ 2, starts at 1 |

### Counter identities (hold on every run)

```
cycles    = retired + 4 + 2 × redirects + stalls
redirects = mispredicts + jalr          (jal is free: redirected in IF)
squashed  = 2 × redirects               (one fewer when the slot in ID was already a bubble)
```

### M and F extensions

- **M** (`isa/rv32m.c`): no undefined behaviour. Division by zero and `INT_MIN / -1` return the spec values.
- **F** (`isa/rv32f.c`): canonical NaN (`0x7fc00000`), flags raised via `<fenv.h>`, `rm=7` uses `frm`, saturating
  `fcvt.w[u].s`, `fmin/fmax` NaN rules, single-rounding `fmaf`, one-hot `fclass`. **RMM** has no host rounding
  mode, so it is handled by an error-free-transformation check that fixes the exact-tie case.
- **Zicsr** (`isa/zicsr.c`): `csrrw/s/c` and the immediate forms on `fflags` (0x001), `frm` (0x002), `fcsr` (0x003).

See [`Remarks.md`](Remarks.md) for the full spec-rule checklist and the RMM fix write-up.

## Testing

### Official self-check

```bash
python3 student_package_updated/evaluate.py riscv_sim_base_updated \
        student_package_updated/sample.s student_package_updated/sample.txt --verbose
# register state match= 100% ,counter match = 100%
```

`evaluate.py` copies the source, swaps in the reference `ui/`, runs `make`, runs `./rvsim` with the options from
the test's `# RUN:` line, then compares registers (`# GRADE: int` = 32 values, `int+fp` = 65) and the nine
graded counters.

### Full local suite

```bash
python3 eval_tests/run_all.py                      # 5 custom + 41 reference goldens
python3 eval_tests/run_all.py Final_<IDs>.zip      # same, on a submission zip
```

### Results (23 Nov 2026)

| Suite | Result |
|---|---|
| Official `sample.s` | 100% registers, 100% counters |
| Custom hand-derived tests (5) | 5/5 |
| Goldens: Lab 4 branch + Lab 5 FP (23) | 23/23 |
| Goldens: Lab 2 + Lab 3 (18) | 13/18 exact; 5 match on every register |
| 1,000,000 cycles | 0.32 s (limit 10 s) |

**Why 5 goldens show counter differences (expected, not a bug).** `p1_broad`, `p2_edge`, `p3_kernel`
(pipeline) and `h3_kernel` (forward, stall) were recorded on the early Lab 2/3 pipeline, which resolved `jal`
in EX with a 2-cycle penalty. The final reference resolves `jal` in IF for free, so each `jal` saves 2 cycles,
1 redirect and 2 squashed (and, under `stall`, exposes 1 extra stall). All registers match. Changing this
would break `sample.txt` and every Lab 4/5 golden.

### Other verification

- M handlers vs. an independent `__int128` model: 8 ops × 25,800 operand pairs, 0 mismatches.
- F handlers vs. an exact `Fraction` model (all rounding modes, all flags): 210,055 cases, 0 mismatches.
- Random integer, M, FP and CSR programs vs. `--single-cycle` across hazard units and predictors: thousands of
  runs, 0 failures; the cycle identity held every time.
- Fault paths (out-of-bounds, illegal instruction, wrong-path faults, `--max-cycles`) match the oracle.

## Building a submission

```bash
cd riscv_sim_base_updated && make clean && rm -f rvsim_asan.d && cd ..
zip -r Final_<ID1_ID2>.zip riscv_sim_base_updated -x "*.o" "*.d" "*/rvsim" "*/tests/test_rv32m"
python3 eval_tests/run_all.py Final_<ID1_ID2>.zip
```

## Known notes

- A taken branch whose target is `pc + 4` is recorded as not taken (timing is unaffected).
- Lab sheets 2–4 describe an older design (own `pipeline_state.h`, `--pipeline` flag, `flushes` counter);
  the skeleton's interfaces and the identities above are what the evaluator uses.
- Code `labN` tags are one lower than the lab sheet numbers (M is `lab4`).
- Not checkable locally: MOSS similarity and the five hidden tests.

## Documentation

- [`RISC-V_Simulator_Codebase_Manual.md`](RISC-V_Simulator_Codebase_Manual.md): codebase manual
- [`Remarks.md`](Remarks.md): design notes and verification record
- [`log.md`](log.md): latest test run
- [`riscv_sim_base_updated/README.md`](riscv_sim_base_updated/README.md): skeleton README
