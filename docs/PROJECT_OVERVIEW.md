# ECE 721 Project 4 – Value Prediction Simulator: Full Overview

> Instructor: Eric Rotenberg. Spec version: March 13, 2026.

## What Is This Project?

This is a **cycle-accurate microarchitecture timing simulator** for a RISC-V out-of-order superscalar processor, built for ECE 721 (Advanced Microarchitecture) at NC State University (Spring 2026). Project 4 extends the Project 3 baseline with **value prediction (VP)** support.

There are two graded phases:
- **Phase 1 (80 pts)**: Implement a Stride Value Predictor (SVP) with a Value Prediction Queue (VPQ), plus all pipeline support for making predictions, breaking data dependencies, detecting mispredictions, and recovering.
- **Phase 2 (20 pts)**: Competition — maximize harmonic mean IPC across benchmarks within a fixed storage budget by improving the value predictor.

---

## Two Layers of Simulation

The codebase has two distinct simulation layers that work together:

### Layer 1: Functional Simulator (`riscv-base/`)
A fast, **ISA-level simulator** derived from RISC-V Spike. It executes instructions one at a time, computing correct values, addresses, and next PCs. The timing simulator queries this layer to:
- Get oracle (perfect) values for value prediction
- Detect branch mispredictions
- Verify committed results at retire (`checker()`)

Key access point: `get_pipe()->peek(db_index)` returns a `db_t*` for any in-flight instruction, containing the actual result (`a_rdst[0].value`), memory address (`a_addr`), etc.

### Layer 2: Timing Simulator (`uarchsim/`)
Models the **cycle-by-cycle microarchitecture** behavior. Every function call in the main loop advances one pipeline stage by one cycle. Almost all your work happens here.

---

## Directory Structure

```
ece721proj4/
├── CMakeLists.txt          Build system
├── README.md               Project spec summary
├── proj4-vp-v1.pdf         Official project specification
├── riscv-base/             RISC-V ISA functional simulator (Spike-derived)
│   ├── insns/              ~200 instruction implementation files
│   ├── processor.cc/h      Core ISA state machine
│   ├── mmu.cc/h            Memory management unit
│   └── softfloat/          IEEE 754 floating-point library
└── uarchsim/               Microarchitecture timing simulator
    ├── main.cc             Entry point, argument parsing, simulation loop
    ├── sim.h/cc            Simulation kernel
    ├── pipeline.h          Central header: all pipeline state, macros, declarations
    ├── pipeline.cc         Pipeline constructor, VP helper functions
    ├── payload.h/cc        Instruction payload buffer (central data struct)
    ├── parameters.h/cc     Configurable knobs (sizes, widths, VP flags, etc.)
    ├── [pipeline stages]
    │   ├── fetch.cc        Fetch1 and Fetch2 sub-stages
    │   ├── decode.cc       Instruction decode, flag/FU assignment
    │   ├── rename.cc       Register rename + VP eligibility + oracle VP + SVP predict
    │   ├── dispatch.cc     Dispatch to IQ/AL/LSU + VP value writing
    │   ├── schedule.cc     Out-of-order instruction selection from IQ
    │   ├── register_read.cc Register read + wakeup (suppressed for VP'd instructions)
    │   ├── execute.cc      Execution + misprediction detection
    │   ├── writeback.cc    Branch resolution, completed bits
    │   ├── retire.cc       Commit, exception handling, SVP training, VP recovery
    │   └── squash.cc       Pipeline flush and recovery logic
    ├── [hardware modules]
    │   ├── renamer.h/cc    RMT, AMT, Free List, PRF, Active List, checkpoints
    │   ├── issue_queue.h/cc Out-of-order scheduling queue
    │   ├── lsu.h/cc        Load/Store Unit with memory disambiguation
    │   ├── lane.h/cc       Pipelined execution lanes
    │   ├── alu.cc          ALU computation
    │   ├── fetchunit.h/cc  Fetch unit with branch prediction
    │   ├── btb.h/cc        Branch Target Buffer
    │   ├── bq.h/cc         Branch Queue
    │   ├── ras.h/cc        Return Address Stack
    │   ├── gshare.h/cc     Gshare conditional branch predictor
    │   ├── tage-sc-l.h/cc  TAGE-SC-L branch predictor
    │   ├── ic.h/cc         Instruction cache
    │   ├── tc.h/cc         Trace cache
    │   └── CacheClass.h/cc Generic set-associative cache (L1/L2/L3)
    ├── [TO CREATE – Phase 1]
    │   ├── vpu.h/cc        Stride Value Predictor (SVP) + Value Prediction Queue (VPQ)
    └── [support]
        ├── stats.h/cc      Performance counter collection
        ├── debug.h/cc      Per-stage logging infrastructure
        ├── histogram.h/cc  PC execution histogram
        ├── fu.h            Function unit type enum
        └── checker.cc      Committed-result correctness checker
```

---

## The Pipeline: Stage by Stage

```
FETCH1 → FETCH2 → DECODE → [Fetch Queue] → RENAME1 → RENAME2
       → DISPATCH → [Issue Queue] → SCHEDULE
       → REGISTER_READ → EXECUTE (1–3 cycles) → WRITEBACK → RETIRE
                                                           ↑
                                                    [Active List]
```

### Fetch (`fetch.cc`)
- Two sub-stages: Fetch1 reads BTB/TC; Fetch2 validates bundle.
- Fetch width: up to 8 instructions/cycle (default).
- Branch prediction: BTB, RAS (64 entries), conditional branch predictor.
- I-cache: 128 sets, 8-way, 64-byte lines, 1-cycle hit.

### Decode (`decode.cc`)
- Sets instruction type flags (`F_ICOMP`, `F_FCOMP`, `F_LOAD`, `F_STORE`, etc.).
- Assigns function unit type (`FU_BR`, `FU_LS`, `FU_ALU_S`, `FU_ALU_C`, `FU_ALU_FP`).
- Identifies source/destination logical registers; sets `C_valid = false` when dest is x0.

### Rename (`rename.cc`) ← PRIMARY VP WORK HAPPENS HERE
- **Rename1**: Pulls bundle from Fetch Queue.
- **Rename2**: Renames logical→physical registers; creates branch checkpoints.
- **Value Prediction**: Checks eligibility, queries SVP, handles stall conditions for insufficient VPQ entries, writes `vp_pred`/`vp_val` into payload.

### Dispatch (`dispatch.cc`)
- Allocates Active List, Issue Queue, and LQ/SQ entries.
- **If value-predicted**: writes predicted value into PRF immediately and marks dest register ready — dependents can schedule immediately.
- **If not value-predicted**: clears destination register ready bit (normal).

### Schedule (`schedule.cc`)
- Selects ready instructions from IQ (all source operands ready).
- Issues up to `ISSUE_WIDTH` instructions/cycle to execution lanes.

### Register Read (`register_read.cc`)
- Reads source operand values from PRF.
- **Wakeup suppressed for VP'd instructions** (`if (!vp_pred) IQ.wakeup(...)`) — wakeup was already done at dispatch.

### Execute (`execute.cc`) ← MISPREDICTION DETECTION HERE
- Runs the ALU or LSU.
- **Wakeup suppressed for VP'd instructions** in three places.
- **Value misprediction check**: compare `C_value.dw` vs. `vp_val`; call `set_value_misprediction()` if they differ.
- **Recommended** by spec to check here rather than in Writeback (see Appendix A, Comment 3).

### Writeback (`writeback.cc`)
- Sets completed bit in Active List.
- Resolves branches.

### Retire (`retire.cc`) ← SVP TRAINING + VP RECOVERY HERE
- Calls `REN->precommit()` to get Active List head status.
- **SVP training**: at retirement, use the VPQ entry to train the SVP in-order, non-speculatively.
- **If `val_misp`**: commits the mispredicted instruction, then calls `squash_complete(pc+4)` — full pipeline flush ("VR-1" recovery).

---

## Key Data Structures

### `payload_t` (payload.h) — The Instruction Packet
Every instruction has a slot in `PAY.buf[]`. Key VP fields:

```cpp
// Project 4 – VALUE PREDICTION (added by partner)
uint64_t vp_val;      // Predicted destination value
bool vp_pred;         // true = this instruction was value-predicted
```

Note: `PAY` should only be used in retire for checking, assertions, and measurements — not as "silicon" state. SVP training should use the VPQ.

### `renamer` (renamer.h/cc) — The Register File Manager
- **PRF** (Physical Register File): 320 physical registers
- **PRF_ready[]**: ready bit per physical register — set at dispatch for VP'd instructions
- **Active List (AL)**: per-entry `val_misp` bit set when misprediction detected in execute
- **Checkpoints**: for branch misprediction recovery (shared with VP if using VR-5)

### VPU — To Be Created
The Stride Value Predictor (SVP) + Value Prediction Queue (VPQ). See `VALUE_PREDICTION.md` for full details.

---

## Phase 1 Task List (from Spec Section 2.1)

| # | Task | Status |
|---|---|---|
| 1 | Eligibility check + break data dependencies in Dispatch | Done (partner) |
| 2 | Perfect value prediction mode | Done (partner) |
| 3 | SVP + VPQ implementation | **TODO** |
| 4 | Oracle confidence mode | **TODO** |
| 5 | Misprediction detection (Execute) + VR-1 recovery (Retire) | Partially done (recovery exists, detection TODO) |
| 6 | CLI args: `--vp-svp=...` | **TODO** |
| 7 | Storage cost accounting output | **TODO** |
| 8 | Retired instruction breakdown statistics | **TODO** |

---

## Required Statistics (from Spec Section 2.1, Task 8)

Every benchmark run must output a breakdown of retired instructions:

```
vpmeas_ineligible     # instructions not eligible for VP
vpmeas_eligible       # instructions eligible for VP, broken into:
  vpmeas_miss         #   no prediction available (e.g., SVP miss)
  vpmeas_conf_corr    #   confident + correct (IPC benefit)
  vpmeas_conf_incorr  #   confident + incorrect (value mispredictions → recovery)
  vpmeas_unconf_corr  #   unconfident + correct (lost opportunity)
  vpmeas_unconf_incorr#   unconfident + incorrect
```

"Correct/incorrect" = was the prediction accurate (regardless of confidence).
"Confident/unconfident" = was the prediction used (injected into the pipeline).

---

## Configuration Parameters (parameters.h)

| Parameter | Default | Meaning |
|---|---|---|
| `FETCH_WIDTH` | 8 | Instructions fetched/cycle |
| `DISPATCH_WIDTH` | 4 | Instructions renamed+dispatched/cycle |
| `ISSUE_WIDTH` | 4 | Instructions issued/cycle |
| `RETIRE_WIDTH` | 4 | Instructions retired/cycle |
| `ACTIVE_LIST_SIZE` | 256 | ROB entries |
| `PRF_SIZE` | 320 | Physical registers |
| `ISSUE_QUEUE_SIZE` | 32 | IQ slots |
| `NUM_CHECKPOINTS` | 64 | Max unresolved branches |
| `LQ_SIZE` / `SQ_SIZE` | 32 | Load/Store Queue sizes |
| `PERFECT_VALUE_PRED` | false | Use perfect (oracle) VP |
| `predINTALU` | false | Integer ALU instructions eligible for VP |
| `predFPALU` | false | FP ALU instructions eligible for VP |
| `predLOAD` | false | Load instructions eligible for VP |

---

## Building the Simulator

The executable is named `721sim`. The CMakeLists uses `file(GLOB *.cc)`, so any new
`.cc` file you add (e.g., `vpu.cc`) is automatically included in the build.

```bash
# Configure and build
cmake -B build && cmake --build build

# The executable is at:
#   build/uarchsim/721sim
```

---

## Running the Simulator

### On Grendel (NCSU Server)

Benchmarks use SPEC CPU 2006/2017 SimPoint checkpoints hosted on
`grendel.ece.ncsu.edu`. You must run from grendel to access them.

**One-time setup after each login:**
```bash
source /mnt/designkits/spec_2006_2017/O2_fno_bbreorder/activate.bash
```

**List available benchmarks and their checkpoints:**
```bash
atool-simenv list                                    # all benchmarks
atool-simenv list 473.astar_rivers_ref               # checkpoints for one benchmark
```

**Set up a run directory for a specific checkpoint:**
```bash
# 1. Create a dedicated run directory (naming convention: checkpoint name + instance number)
mkdir 473.astar_rivers_ref.252.0.28.gz-1
cd 473.astar_rivers_ref.252.0.28.gz-1

# 2. Symlink the proxy kernel
ln -s /mnt/designkits/spec_2006_2017/O2_fno_bbreorder/app_storage/pk .

# 3. Symlink your built 721sim executable
ln -s /path/to/build/uarchsim/721sim .

# 4. Generate the Makefile for this checkpoint
atool-simenv mkgen 473.astar_rivers_ref --checkpoint 473.astar_rivers_ref.252.0.28.gz

# 5. Launch a run with simulator flags
make cleanrun SIM_FLAGS_EXTRA='-e100000000 --mdp=1,0 --perf=0,0,0,0 --fq=64 --cp=32 --al=256 --lsq=128 --iq=64 --iqnp=4 --fw=4 --dw=4 --iw=8 --rw=4'
```

**Rules:**
- One run directory per checkpoint. One job at a time per run directory.
- To run the same checkpoint with different configs simultaneously, create multiple
  run directories (`*.gz-1/`, `*.gz-2/`, etc.).
- `make cleanrun` resets the benchmark's file state each time (clean simulation).

**Output:**
- Results go to `stats.[timestamp]` in the run directory.
- Contains processor config, IPC (`ipc_rate`), and all measurement counters.

### VP-Specific Flags

```bash
# Perfect VP mode (testing: confirms data dependency breaking works)
make cleanrun SIM_FLAGS_EXTRA='... --vp-perf=1 --vp-eligible=1,0,1'

# SVP + oracle confidence (testing: confirms SVP predictions without recovery)
make cleanrun SIM_FLAGS_EXTRA='... --vp-svp=256,1,10,0,7 --vp-eligible=1,0,1'

# SVP + real confidence (Phase 1 deliverable, what Gradescope tests)
make cleanrun SIM_FLAGS_EXTRA='... --vp-svp=256,0,10,0,7 --vp-eligible=1,0,1'

# Baseline (no VP)
make cleanrun SIM_FLAGS_EXTRA='...'
```

### `--vp-svp` Parameter Breakdown
```
--vp-svp=<VPQsize>,<oracleconf>,<indexbits>,<tagbits>,<confmax>

<VPQsize>    - Number of VPQ entries (structural hazard in rename)
<oracleconf> - 0: real confidence, 1: oracle confidence (uses functional sim)
<indexbits>  - SVP has 2^(indexbits) entries, indexed by PC bits
<tagbits>    - 0: no tag, 1–62: partial/full tag for aliasing prevention
<confmax>    - Confidence threshold; prediction injected only when conf == confmax
```

### Common Simulator Flags Reference
```
-c<file>         Start from a .gz checkpoint file
-e<n>            End after n committed instructions (e.g., -e100000000)
-d               Interactive debug mode
-l<n>            Enable logging after n commits
--perf=<bp>,<tc>,<ic>,<dc>   Perfect branch pred / trace cache / I$ / D$ (0 or 1)
--mdp=<type>,<oracle>        Memory dependence predictor type
--al=<n>         Active List size
--lsq=<n>        Load/Store Queue size (LQ and SQ each get n entries)
--iq=<n>         Issue Queue size
--fw=<n>         Fetch width
--dw=<n>         Dispatch width (also rename width)
--iw=<n>         Issue width
--rw=<n>         Retire width
```

### SimPoints and IPC Aggregation

Each benchmark has multiple SimPoints (representative 100M-instruction regions).
Each checkpoint file corresponds to one SimPoint. The filename encodes the SimPoint
number and its weight:
```
473.astar_rivers_ref.252.0.28.gz
                     ^^^  ^^^^
                     |    weight (0.28 = most important SimPoint)
                     SimPoint number (252 × 10^8 = 25.2B instructions in)
```

To get the benchmark's overall IPC, compute the **weighted harmonic mean** of
all SimPoint IPCs:
```
benchmark_IPC = sum(weights) / sum(weight_i / IPC_i)
```

For the competition, your score is the **harmonic mean of per-benchmark IPCs**
across all benchmarks.

---

## Grading (Phase 1)

Validation runs are scored per run (max 50 pts each):
- **50 pts**: Completes, IPC within 1%, all VP measurements within 1%, storage accounting exact.
- **43 pts**: Completes, IPC matches, but VP measurements or storage cost off.
- **36 pts**: Completes, IPC off by >1%.
- **29/22/15/8 pts**: Crashes at different instruction count thresholds.

**BASE = 30** if you pass at least one run with SVP + real confidence. Otherwise 0–30 based on manual review.

**Final score = BASE + sum(validation_run_score_i) / N * 50**

### Manual Inspection Checks (Deductions for Infractions)
- VPQ must be used to train SVP (not PAY)
- `good_instruction` must NOT be used in real confidence mode
- Must NOT use `actual->a_rdst[0].value` to check for mispredictions
- VP must actually break data dependencies and trigger recoveries
