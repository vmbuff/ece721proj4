# ECE 721 Project 4 -- Implementation Overview

## What Was Implemented

A **Stride Value Predictor (SVP)** with a **Value Prediction Queue (VPQ)** was added to the 721sim microarchitecture simulator. The VP system spans the entire pipeline: it predicts destination register values at Rename, injects confident predictions into the physical register file at Dispatch to break data dependencies early, detects mispredictions at Execute, trains the predictor at Retire, and repairs speculative state on Squash.

The base simulator had no value prediction infrastructure. Every file change described below was added on top of the Project 3 base code (`b3ed1cd`).

---

## New Files

### `uarchsim/vpu.h` -- SVP + VPQ Class Definition

Defines the `vpu` class containing two core data structures:

**SVP Table** (Stride Value Predictor):
```cpp
struct svp_entry_t {
    uint64_t     tag;            // PC tag bits for disambiguation
    int64_t      stride;         // Signed stride between consecutive values
    uint64_t     retired_value;  // Last committed destination value for this PC
    uint64_t     instance;       // Number of in-flight copies (speculative counter)
    unsigned int conf;           // Saturating confidence counter
    bool         valid;          // Entry contains valid data
};
```
Direct-mapped table indexed by PC bits. The prediction formula is:
```
predicted_value = retired_value + (instance * stride)
```

**VPQ** (Value Prediction Queue):
```cpp
struct vpq_entry_t {
    uint64_t     pc;              // Instruction PC (for tag comparison at train time)
    unsigned int svp_index;       // Cached SVP index (avoids recomputation)
    uint64_t     predicted_value; // Value predicted at Rename
    bool         svp_hit;         // Whether SVP had a tag-matching entry
    bool         confident;       // Whether confidence threshold was met
};
```
Circular buffer with phase bits (same wrap-disambiguation pattern as the renamer's free list and active list). Entries are allocated at the tail (Rename) and freed at the head (Retire).

**Public interface:**
- `predict()` -- SVP lookup + VPQ allocation (called from Rename)
- `train()` -- SVP update + VPQ deallocation (called from Retire)
- `repair()` -- VPQ rollback with instance counter correction (called from Squash)
- `vpq_free_entries()` -- stall check (called from Rename)
- `get_vpq_tail()` / `get_vpq_tail_phase()` -- checkpoint saving (called from Rename)
- `get_vpq_head()` / `get_vpq_head_phase()` -- complete squash target (called from Squash)
- `print_storage()` -- storage cost accounting (called at simulation end)

Rule of Three: constructor allocates `svp` and `vpq` with `new[]`; destructor frees both; copy constructor and copy assignment are `= delete`.

### `uarchsim/vpu.cc` -- Full SVP/VPQ Implementation

**`predict(pc, &predicted_val, &confident, &vpq_index)`:**
1. Compute SVP index from PC: `(pc >> 2) & ((1 << index_bits) - 1)`
2. Check for tag hit: entry is valid AND tag matches
3. On hit: increment `instance` (before computing prediction so the first in-flight copy predicts `retired_value + 1*stride`, not `retired_value + 0`), compute predicted value, check confidence (`conf >= conf_max`)
4. Always allocate a VPQ entry (even on miss) -- misses become replacements at Retire, and `repair()` must walk all entries on Squash
5. Advance VPQ tail with phase-bit wrap

**`train(vpq_index, committed_val)`:**
1. Assert `vpq_index == vpq_head` (in-order retirement)
2. Tag match path: compute `new_stride = committed_val - retired_value`. If stride matches, increment conf (saturating). If not, replace stride and reset conf to 0. Update `retired_value`. Decrement `instance` if this was an SVP hit at predict time.
3. Tag miss path: replace the entire SVP entry (`tag`, `retired_value = committed_val`, `stride = committed_val`, `conf = 0`). Initialize `instance` by temporarily advancing the head pointer past the entry being freed, then calling `count_inflight_instances()` to count remaining in-flight peers in the VPQ.
4. Free VPQ head entry

**`repair(restored_tail, restored_tail_phase)`:**
1. Walk VPQ backwards from current tail to the restore point
2. For each discarded entry: if it was an SVP hit AND the tag still matches the current SVP entry, decrement `instance`. The tag check is critical -- if the SVP entry was replaced after this instruction's predict, decrementing would corrupt the new entry's counter.
3. Compare both position AND phase (a VPQ can wrap an entire `vpq_size` back to the same position with the phase flipped)

**`print_storage()`:**
Computes per-entry bit cost: `tag_bits + conf_bits + 64 (retired_value) + 64 (stride) + instance_bits`. Instance bits = `ceil(log2(VPQ_size + 1))`, conf bits = `ceil(log2(conf_max + 1))`. Multiplies by number of SVP entries for total cost. VPQ is excluded from the storage budget per spec.

---

## Modified Files

### `uarchsim/parameters.h` / `uarchsim/parameters.cc` -- VP Configuration Parameters

Added 10 new global parameters, all defaulting to `false`/`0`:

| Parameter | Type | Purpose |
|-----------|------|---------|
| `PERFECT_VALUE_PRED` | `bool` | Enable perfect (oracle) VP mode |
| `predINTALU` | `bool` | Integer ALU instructions eligible for VP |
| `predFPALU` | `bool` | FP ALU instructions eligible for VP |
| `predLOAD` | `bool` | Load instructions eligible for VP |
| `SVP_ENABLED` | `bool` | Enable the Stride Value Predictor |
| `SVP_ORACLE_CONF` | `bool` | Override SVP confidence with oracle check |
| `VPQ_SIZE` | `unsigned int` | Number of VPQ entries |
| `SVP_INDEX_BITS` | `unsigned int` | PC bits used to index SVP table |
| `SVP_TAG_BITS` | `unsigned int` | PC bits used for SVP tag (0 = no tag check) |
| `SVP_CONF_MAX` | `unsigned int` | Confidence saturation threshold |

### `uarchsim/main.cc` -- CLI Argument Parsing

Added three new command-line options:

**`--vp-perf=<0|1>`**: Enables perfect value prediction. Mutually exclusive with `--vp-svp`. Parsed by `set_vp_perf()` which sets `PERFECT_VALUE_PRED`.

**`--vp-svp=<VPQsize>,<oracleconf>,<indexbits>,<tagbits>,<confmax>`**: Configures and enables the SVP. Parsed by `set_vp_svp()` which sets `SVP_ENABLED=true` and all five sub-parameters. Mutually exclusive with `--vp-perf`.

**`--vp-eligible=<predINTALU>,<predFPALU>,<predLOAD>`**: Controls which instruction types are eligible. Each value is 0 or 1. Parsed by `set_vp_eligible()`.

### `uarchsim/payload.h` -- VP Fields in Payload

Added 6 fields to `payload_t` (the per-instruction data structure that flows through the pipeline):

```cpp
uint64_t vp_val;        // Predicted value (set on any SVP hit, even unconfident)
bool vp_pred;           // True if confident prediction was injected into PRF
bool vp_eligible;       // True if instruction type is eligible for VP
bool vp_svp_hit;        // True if SVP had a valid tag-matching entry
bool vp_confident;      // True if conf >= conf_max (or oracle confirmed)
unsigned int vpq_index; // VPQ entry allocated at Rename
```

### `uarchsim/pipeline.h` -- VP Declarations

- Added `#include "vpu.h"`
- Added macros: `IS_INTALU(flags)` (tests `F_ICOMP`) and `IS_FPALU(flags)` (tests `F_FCOMP`)
- Added private members: `vpu *VPU`, `vpq_tail_checkpoint[64]`, `vpq_tail_checkpoint_phase[64]`
- Added 7 public `uint64_t vpmeas_*` measurement counters (kept as plain members, not in `stats_t` counter_map, so they print in the VPU MEASUREMENTS block instead of the `[stats]` block)
- Added `set_value_misprediction()` and `is_eligible()` function declarations

### `uarchsim/pipeline.cc` -- VPU Instantiation and Stats Output

**Constructor additions:**
- Initialize all 7 `vpmeas_*` counters to 0
- If `SVP_ENABLED`: create `VPU = new vpu(VPQ_SIZE, SVP_INDEX_BITS, SVP_TAG_BITS, SVP_CONF_MAX)`. Otherwise: `VPU = NULL`
- Initialize `vpq_tail_checkpoint[64]` and phase arrays to 0/false

**Config logging (printed at simulation start):**
- VP-eligible configuration (`predINTALU`, `predFPALU`, `predLOAD`)
- VP mode (perfect or stride) with all SVP parameters
- Storage cost accounting via `VPU->print_storage()`

**Destructor additions (printed at simulation end):**
- VPU MEASUREMENTS block: all 7 `vpmeas_*` counters with counts and percentages of total committed instructions

**`set_value_misprediction()` wrapper:**
- Delegates to `REN->set_value_misprediction(al_index)` (renamer library function)

---

## Pipeline Stage Changes

### `uarchsim/rename.cc` -- Predict, Stall, Checkpoint

**New function `is_eligible(payload_t *pay)`:**
- Returns `false` if instruction has no destination register (`!C_valid`)
- Returns `predINTALU` if `IS_INTALU(flags)`
- Returns `predFPALU` if `IS_FPALU(flags)`
- Returns `predLOAD` if `IS_LOAD(flags) && !IS_AMO(flags)` (excludes atomics)
- Returns `false` for all other instruction types

**VPQ stall logic (added before rename processing):**
- If VPU exists: count VP-eligible instructions in the rename bundle, stall (return early) if `VPU->vpq_free_entries() < bundle_vp_eligible`

**VPQ checkpoint saving (added inside branch checkpoint creation):**
- When a branch takes a checkpoint (`REN->checkpoint()`), also save `VPU->get_vpq_tail()` and `VPU->get_vpq_tail_phase()` into `vpq_tail_checkpoint[branch_ID]` and `vpq_tail_checkpoint_phase[branch_ID]`

**VP prediction path (added after register renaming, per instruction):**
1. Initialize all 6 VP payload fields to defaults (`vp_pred=false`, etc.)
2. If `is_eligible()` returns true, set `vp_eligible=true` and enter prediction logic:
   - **Perfect mode** (`PERFECT_VALUE_PRED`): For on-path instructions (`good_instruction`), peek the functional simulator's destination value via `get_pipe()->peek()`. If valid, set `vp_pred=true`, `vp_confident=true`, `vp_val=actual_value`.
   - **Real SVP mode** (`VPU != NULL`): Call `VPU->predict(pc, predicted_val, confident, vpq_idx)`. Store `vpq_index` and `vp_svp_hit` in the payload. On hit: store `vp_val` (even if unconfident, for stat classification at retire). If **oracle confidence** is enabled, override `confident` by comparing the predicted value against the functional simulator's actual value (only for on-path instructions). If confident, set `vp_pred=true` to trigger injection at Dispatch.

### `uarchsim/dispatch.cc` -- Inject Prediction into PRF

Changed the existing FIX_ME #9 block (destination register handling):

**Before (base code):**
```cpp
if (PAY.buf[index].C_valid) {
    REN->clear_ready(PAY.buf[index].C_phys_reg);
}
```

**After:**
```cpp
if (PAY.buf[index].C_valid) {
    if (PAY.buf[index].vp_pred) {
        REN->set_ready(PAY.buf[index].C_phys_reg);
        REN->write(PAY.buf[index].C_phys_reg, PAY.buf[index].vp_val);
    } else {
        REN->clear_ready(PAY.buf[index].C_phys_reg);
    }
}
```
If the instruction has a confident prediction (`vp_pred`), mark its physical destination register ready immediately and write the predicted value. Dependents in the issue queue can now schedule without waiting for the actual computation. If not predicted, clear the ready bit as before.

### `uarchsim/register_read.cc` -- Suppress Wakeup

Changed the existing FIX_ME #11a block (1-cycle ALU wakeup):

**Before:**
```cpp
if (C_valid && lat==1 && !IS_LOAD && !IS_AMO) {
    IQ.wakeup(C_phys_reg, true);
    REN->set_ready(C_phys_reg);
}
```

**After:**
```cpp
if (C_valid && lat==1 && !IS_LOAD && !IS_AMO) {
    if (!PAY.buf[index].vp_pred) {
        IQ.wakeup(C_phys_reg, true);
    }
    REN->set_ready(C_phys_reg);
}
```
VP-predicted instructions already had their register marked ready and dependents woken at Dispatch. Calling `IQ.wakeup()` again would be a double-wakeup. The `set_ready()` call is harmless (idempotent) so it remains.

### `uarchsim/execute.cc` -- Detect Misprediction + Suppress Wakeup

Changes at **four** sites in execute:

**1. Load cache hit (FIX_ME #13)** -- added wakeup suppression and misprediction check:
```cpp
if (hit && C_valid) {
    if (!vp_pred) IQ.wakeup(C_phys_reg, true);  // suppress if VP
    REN->set_ready(C_phys_reg);
    REN->write(C_phys_reg, C_value.dw);
    if (vp_pred && C_value.dw != vp_val)
        REN->set_value_misprediction(AL_index);  // flag misprediction
}
```

**2. ALU result writeback (FIX_ME #14)** -- added misprediction check:
```cpp
if (C_valid) {
    REN->write(C_phys_reg, C_value.dw);
    if (vp_pred && C_value.dw != vp_val)
        REN->set_value_misprediction(AL_index);
}
```

**3. Multi-cycle ALU wakeup (FIX_ME #11b)** -- added wakeup suppression:
```cpp
if (C_valid && !IS_LOAD && !IS_AMO) {
    if (!vp_pred) IQ.wakeup(C_phys_reg, true);
    REN->set_ready(C_phys_reg);
}
```

**4. Load replay (FIX_ME #18a)** -- added wakeup suppression and misprediction check:
```cpp
if (!vp_pred) IQ.wakeup(C_phys_reg, true);
REN->set_ready(C_phys_reg);
REN->write(C_phys_reg, C_value.dw);
if (vp_pred && C_value.dw != vp_val)
    REN->set_value_misprediction(AL_index);
```

At every site where the actual value is written to the PRF, if the instruction was value-predicted and the actual value differs from the predicted value, a value misprediction flag is posted to the Active List via `REN->set_value_misprediction()`. The renamer library handles the flag; retire reads it.

### `uarchsim/retire.cc` -- Train SVP, Classify Stats, Recover

**SVP training (added after `REN->commit()`):**
```cpp
if (VPU && PAY.buf[PAY.head].vp_eligible) {
    uint64_t committed_val = 0;
    if (PAY.buf[PAY.head].C_valid)
        committed_val = REN->read(PAY.buf[PAY.head].C_phys_reg);
    VPU->train(PAY.buf[PAY.head].vpq_index, committed_val);
}
```
Reads the committed value from the physical register file and passes it to `VPU->train()`, which updates the SVP entry and frees the VPQ head.

**7-category stat classification (added after training):**
Every retired instruction is classified into exactly one of:
- `vpmeas_ineligible` -- not VP-eligible (no dest reg, or type not enabled)
- `vpmeas_eligible` -- VP-eligible (sum of the next 5 categories)
  - `vpmeas_miss` -- eligible but no SVP hit (and not in perfect mode)
  - `vpmeas_conf_corr` -- confident and correct prediction (IPC gain)
  - `vpmeas_conf_incorr` -- confident and incorrect (triggers recovery)
  - `vpmeas_unconf_corr` -- unconfident and correct (lost opportunity)
  - `vpmeas_unconf_incorr` -- unconfident and incorrect (harmless)

Correctness is determined by comparing `C_value.dw` (actual) against `vp_val` (predicted) at retirement.

**Value misprediction recovery (existing retire flow):**
The renamer library's Active List already checks for value misprediction flags (set by execute). When detected, retire commits the offending instruction, then calls `squash_complete(next_PC)` for a full pipeline flush and restart (VR-1 recovery model).

### `uarchsim/squash.cc` -- Repair VPQ

**Complete squash (`squash_complete`)** -- added VPQ repair to head:
```cpp
if (VPU) {
    VPU->repair(VPU->get_vpq_head(), VPU->get_vpq_head_phase());
}
```
On a full pipeline flush (load violation, exception, or value misprediction), all in-flight VPQ entries are discarded by walking the tail back to the head. Each discarded hit entry's SVP instance counter is decremented.

**Selective squash (`resolve`, branch misprediction)** -- added VPQ repair to checkpoint:
```cpp
if (VPU) {
    VPU->repair(vpq_tail_checkpoint[branch_ID],
                vpq_tail_checkpoint_phase[branch_ID]);
}
```
On a branch misprediction, VPQ entries allocated after the mispredicted branch are discarded by restoring the tail to the checkpointed position+phase. Instance counters are corrected for each discarded entry.

### `uarchsim/stats.cc` -- Minimal Change

The 7 `vpmeas_*` counters were originally declared here via `DECLARE_COUNTER` but were moved to plain `uint64_t` members on `pipeline_t` so they print in the VPU MEASUREMENTS block rather than the generic `[stats]` block. The `DECLARE_COUNTER` calls were removed; a comment notes the counters live in `pipeline.h`.

---

## CLI Arguments

| Flag | Description |
|------|-------------|
| `--vp-perf=<0\|1>` | Perfect VP mode (oracle values, no SVP). |
| `--vp-svp=<VPQ>,<oconf>,<idx>,<tag>,<cmax>` | Enable SVP. VPQ size, oracle confidence (0/1), index bits, tag bits, conf_max. |
| `--vp-eligible=<int>,<fp>,<load>` | Which instruction types are VP-eligible (1=yes, 0=no). |

`--vp-perf` and `--vp-svp` are mutually exclusive.

---

## Storage Cost Accounting

Per SVP entry:
```
tag_bits + ceil(log2(conf_max+1)) + 64 (retired_value) + 64 (stride) + ceil(log2(VPQ_size+1)) (instance)
```
Total = `(2^index_bits) * bits_per_entry`. VPQ is excluded from the budget per spec.

Example (VAL-5 config: 10 index bits, 10 tag bits, conf_max=31, VPQ=300):
- Per entry: 10 + 5 + 64 + 64 + 9 = 152 bits
- Total: 1024 * 152 = 155,648 bits = 19,456 bytes = 19.0 KB

---

## Statistics Output

At simulation end, the VPU MEASUREMENTS block reports:

| Counter | Meaning |
|---------|---------|
| `vpmeas_ineligible` | Not VP-eligible |
| `vpmeas_eligible` | VP-eligible (sum of the 4 below + miss) |
| `vpmeas_miss` | Eligible but no SVP hit |
| `vpmeas_conf_corr` | Confident + correct (IPC gain) |
| `vpmeas_conf_incorr` | Confident + incorrect (triggers recovery) |
| `vpmeas_unconf_corr` | Unconfident + correct (lost opportunity) |
| `vpmeas_unconf_incorr` | Unconfident + incorrect (harmless) |
