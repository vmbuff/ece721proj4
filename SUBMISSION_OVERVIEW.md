# ECE 721 Project 4 -- Submission Overview

## What We Built

We implemented a **Stride Value Predictor (SVP)** with a **Value Prediction Queue (VPQ)** in the 721sim microarchitecture simulator. The system predicts the destination register values of eligible instructions at the Rename stage, injects confident predictions into the physical register file to break data dependencies, detects mispredictions at Execute, trains the predictor at Retire, and repairs speculative state on squash.

---

## Files Changed (from initial commit)

| File | What changed |
|------|-------------|
| `uarchsim/vpu.h` | **New.** SVP table + VPQ circular queue class definition. |
| `uarchsim/vpu.cc` | **New.** Full SVP/VPQ implementation: `predict()`, `train()`, `repair()`, `print_storage()`. |
| `uarchsim/payload.h` | Added 6 VP fields to `payload_t`: `vp_val`, `vp_pred`, `vp_eligible`, `vp_svp_hit`, `vp_confident`, `vpq_index`. |
| `uarchsim/pipeline.h` | Added VPU pointer, `vpq_tail_checkpoint`/phase arrays, `is_eligible()` declaration, VP-related macros. |
| `uarchsim/pipeline.cc` | VPU instantiation (gated on `SVP_ENABLED`), VP config logging, `print_storage()` call. |
| `uarchsim/parameters.h` | Declared VP parameters: `PERFECT_VALUE_PRED`, `predINTALU/FPALU/LOAD`, `SVP_ENABLED`, `SVP_ORACLE_CONF`, `VPQ_SIZE`, `SVP_INDEX_BITS`, `SVP_TAG_BITS`, `SVP_CONF_MAX`. |
| `uarchsim/parameters.cc` | Default values for all VP parameters. |
| `uarchsim/main.cc` | CLI parsing for `--vp-perf`, `--vp-svp`, `--vp-eligible`; mutual exclusion check between perf and SVP modes. |
| `uarchsim/rename.cc` | Eligibility check, VPQ stall logic, SVP prediction path, oracle confidence override, VPQ tail checkpoint saving at branches. |
| `uarchsim/dispatch.cc` | Confident predictions: write predicted value to PRF, mark dest register ready (breaks data dependency). |
| `uarchsim/register_read.cc` | Wakeup suppression for VP-predicted instructions (already woken at dispatch). |
| `uarchsim/execute.cc` | Misprediction detection at all three write sites (ALU, load hit, load replay); wakeup suppression. |
| `uarchsim/retire.cc` | SVP training at retirement, 7-category vpmeas classification, value misprediction recovery via `squash_complete()`. |
| `uarchsim/squash.cc` | VPQ repair on both complete squash and selective (branch) squash using checkpointed tail+phase. |
| `uarchsim/stats.cc` | 7 `vpmeas_*` counter declarations. |
| `uarchsim/writeback.cc` | Minor (no VP-specific logic). |
| `run_validations.sh` | **New.** Script to run all 22 VAL configs serially on grendel. |

---

## How It Works

### Instruction Flow Through the VP Pipeline

```
RENAME --> DISPATCH --> REG_READ --> EXECUTE --> RETIRE
predict    inject      suppress     detect      train
           into PRF    wakeup       mispred     + classify
                                                + recover
                                    SQUASH
                                    repair VPQ
```

**1. Rename -- Predict**
- `is_eligible()` checks: instruction has a dest register, is INTALU/FPALU/LOAD (not atomic), and the corresponding `predINTALU`/`predFPALU`/`predLOAD` flag is on.
- Stall the rename bundle if VPQ doesn't have enough free entries.
- Call `VPU->predict(pc, ...)`:
  - Direct-mapped SVP lookup by PC (index = `(PC >> 2) & mask`, tag = remaining bits).
  - On tag hit: increment the speculative instance counter, compute `predicted_value = retired_value + (instance * stride)`, return confidence.
  - Always allocate a VPQ entry (even on miss) so retirement can train.
- **Perfect mode**: uses functional simulator's actual value for on-path instructions.
- **Oracle confidence mode**: overrides SVP confidence with functional simulator correctness check.
- If confident (or perfect mode): set `vp_pred = true` in the payload.
- Save VPQ tail position + phase as a branch checkpoint.

**2. Dispatch -- Inject Prediction**
- If `vp_pred`: write predicted value into the PRF and mark the dest register ready. Dependents can now schedule immediately without waiting for the actual computation.
- If not predicted: clear ready bit (normal path).

**3. Register Read -- Suppress Wakeup**
- For VP-predicted instructions, skip the normal 1-cycle wakeup call (it already happened at dispatch). Prevents double-wakeup.

**4. Execute -- Detect Misprediction**
- At all three `REN->write()` sites (ALU result, load hit, load replay): if `vp_pred` is true and the actual value differs from the predicted value, set `value_misprediction` on the AL entry.
- Wakeup suppression (same reason as register read).

**5. Retire -- Train and Recover**
- For every VP-eligible instruction, call `VPU->train(vpq_index, committed_value)`:
  - Tag match: update stride = `committed_value - retired_value`, update confidence (increment if stride matches, reset to 0 otherwise), update `retired_value`, decrement instance.
  - Tag miss: replace the SVP entry, initialize instance by counting in-flight peers in the VPQ.
  - Free the VPQ head entry.
- Classify the instruction into one of 7 `vpmeas_*` stat buckets.
- If a value misprediction is flagged: commit the instruction, then `squash_complete(pc + 4)` -- full pipeline flush, resume at next PC (VR-1 recovery).

**6. Squash -- Repair VPQ**
- **Branch misprediction (selective)**: restore VPQ tail to the saved checkpoint (position + phase). Walk backwards, decrementing instance counters for discarded entries.
- **Complete squash** (exception, load violation): restore VPQ to head position.

### Key Data Structures

**SVP Entry**: `{tag, stride, retired_value, instance, conf, valid}` -- direct-mapped, indexed by PC bits.

**VPQ Entry**: `{pc, svp_index, predicted_value, svp_hit, confident}` -- circular queue with phase bits for wrap disambiguation. Allocated at rename, freed at retire.

### Prediction Formula

```
predicted_value = retired_value + (instance * stride)
```

The instance counter tracks how many in-flight instructions share this SVP entry, allowing correct stride extrapolation for multiple outstanding copies of the same PC.

---

## CLI Arguments

| Flag | Description |
|------|-------------|
| `--vp-perf=<0\|1>` | Perfect VP mode (oracle values, no SVP). |
| `--vp-svp=<VPQ>,<oconf>,<idx>,<tag>,<cmax>` | Enable SVP. VPQ size, oracle confidence (0/1), index bits, tag bits, conf_max. |
| `--vp-eligible=<int>,<fp>,<load>` | Which instruction types are VP-eligible (1=yes, 0=no). |

`--vp-perf` and `--vp-svp` are mutually exclusive.

---

## Statistics Tracked

At retirement, every instruction is classified:

| Counter | Meaning |
|---------|---------|
| `vpmeas_ineligible` | Not VP-eligible |
| `vpmeas_eligible` | VP-eligible (sum of the 4 below + miss) |
| `vpmeas_miss` | Eligible but no SVP hit |
| `vpmeas_conf_corr` | Confident + correct (IPC gain) |
| `vpmeas_conf_incorr` | Confident + incorrect (triggers recovery) |
| `vpmeas_unconf_corr` | Unconfident + correct (lost opportunity) |
| `vpmeas_unconf_incorr` | Unconfident + incorrect (harmless) |

Storage cost is printed via `print_storage()` (SVP table only; VPQ excluded per spec).

---

## Validation Configs (22 total)

| VAL | Benchmark | Mode | Notes |
|-----|-----------|------|-------|
| 1 | astar | Perfect VP | Baseline IPC upper bound |
| 2-4 | astar | SVP + oracle conf | Rollback-free; vary eligible types |
| 5-6 | astar | SVP + real conf | Vary VPQ size (300 vs 100) |
| 7-11 | astar | SVP + real conf | Vary index bits, tag bits, conf_max |
| 12 | perl | Perfect VP | |
| 13-18 | perl | SVP + real conf | Vary geometry and conf_max |
| 19-21 | array2 (micro) | SVP + real conf | Vary conf_max (3, 1, 0) |
| 22 | if2 (micro) | SVP + real conf | Different benchmark |

Run all with: `./run_validations.sh` (on grendel).

---

## What to Check Before Submitting to Gradescope

### 1. Build cleanly
```bash
cmake -B build && cmake --build build
```
Make sure there are no warnings or errors.

### 2. Run all 22 validations on grendel
```bash
./run_validations.sh
```
Check `val_runs/SUMMARY.txt` for pass/fail. Compare `ipc_rate` and all `vpmeas_*` counters against the reference values provided on Moodle.

### 3. Known issues to investigate

- **VAL-5 / VAL-7 (VPQ > AL)**: These had a regression where IPC collapsed when VPQ size (300) > AL size (256). Phase-bit checkpointing was applied as a fix but needs **re-validation on grendel** against the reference. This is the highest-priority item.

- **VAL-6 unconfident misclassification**: There's a ~1% divergence between `vpmeas_unconf_corr` and `vpmeas_unconf_incorr` vs. the reference. The confident counters and IPC match exactly. This is borderline and may or may not be within the grading threshold -- verify the latest numbers.

### 4. Stat output format
The stats must match the reference format exactly. Check that:
- `ipc_rate` is printed
- All 7 `vpmeas_*` counters appear in the stat log
- VP config and storage cost are printed
- No extra debug/diagnostic print statements remain

### 5. No leftover debug code
Grep for any diagnostic counters, debug prints, or `#if 0` blocks that should have been cleaned up:
```bash
grep -rn "printf\|cout\|cerr\|DEBUG\|DIAG\|TEMP\|HACK\|TODO" uarchsim/*.cc uarchsim/*.h | grep -v '//'
```

### 6. Edge cases to sanity-check
- **Baseline (no VP flags)**: simulator should produce identical results to the original -- no regressions.
- **Perfect VP**: `vpmeas_conf_incorr` should be 0, recovery count should be 0.
- **SVP + oracle conf (VAL-2)**: IPC should match reference exactly (e.g., 3.72 for astar).
- **Microbenchmarks (VAL-19-22)**: these run differently (direct `./721sim` invocation, not `make cleanrun`) -- make sure they work.

### 7. Code quality
- Descriptive comments have been added (PR #6).
- Make sure `perf-vp confidence` is set correctly in `rename.cc` (was fixed in da31ba6).

### 8. Submission contents
Verify Gradescope expects just the source files or the full repo. Typically:
- All `.cc` and `.h` files in `uarchsim/`
- Possibly `CMakeLists.txt`
- Check the project handout for exact submission instructions.
