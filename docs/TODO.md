# Project 4 TODO: Work Split

**Chris**: SVP/VPQ module + rename.cc wiring
**Vincent**: execute.cc misprediction detection + retire.cc training/stats + squash.cc repair + CLI/params

---

## Status

- [x] `vpu.h` interface defined (see `uarchsim/vpu.h`)
- [ ] `vpu.cc` implementation
- [ ] Payload field additions
- [ ] All pipeline wiring (rename, execute, retire, squash)
- [ ] CLI args, stats, storage accounting

---

## Chris's Tasks

**Files**: `vpu.h`, `vpu.cc`, `rename.cc`, `payload.h`

### C1: Implement `vpu.cc` (SVP + VPQ logic)

- [x] Define `vpu.h` with SVP entry, VPQ entry, and public interface
- [ ] Constructor: allocate SVP table (2^index_bits entries, all invalid) and VPQ (circular buffer, empty)
- [ ] `get_svp_index()`: `(pc >> 1) & ((1 << index_bits) - 1)`
- [ ] `get_svp_tag()`: `(pc >> (1 + index_bits)) & ((1 << tag_bits) - 1)`
- [ ] `tag_matches()`: return true if tag_bits == 0 or tags equal
- [ ] `count_inflight_instances()`: walk VPQ head to tail, count entries with matching svp_index
- [ ] `predict()`:
  - [ ] Look up SVP entry, check valid + tag match
  - [ ] On hit: compute `retired_value + instance * stride`, check `conf == conf_max`, increment instance
  - [ ] Always allocate VPQ entry at tail (even on miss, needed for training + repair)
  - [ ] Return hit/miss
- [ ] `train()`:
  - [ ] Tag match path: compute `new_stride = committed_val - retired_value` (int64_t), update stride/conf/retired_value, decrement instance
  - [ ] Tag miss path: replace entry, init instance via `count_inflight_instances()`
  - [ ] Free VPQ head entry
- [ ] `repair()`: walk VPQ backward from tail to restored position, decrement instance for each hit entry, restore tail
- [ ] `vpq_free_entries()`: derived from head/tail/phase bits (same pattern as renamer FL)
- [ ] `get_vpq_tail()`: return current tail (saved at checkpoint time for squash repair)
- [ ] `print_storage()`: `bits/entry = tag + 64 + 64 + ceil(log2(vpq_size+1)) + ceil(log2(conf_max+1))`, print total bytes

### C2: Payload field additions (`payload.h`)

- [ ] `unsigned int vpq_index` (VPQ entry allocated at rename, used at retire for training)
- [ ] `bool vp_eligible` (was this instr VP-eligible, for stats)
- [ ] `bool vp_svp_hit` (did SVP have a prediction, for miss stat)
- [ ] `bool vp_confident` (was prediction confident, for conf/unconf stat split)
- [ ] Make `vp_val` always hold the predicted value on SVP hit (even if unconfident), so retire can check correctness for all stat categories

### C3: Wire VPU into `rename.cc`

- [ ] VPQ stall condition at line ~151 (after checkpoint/reg stalls, before rename loop): count VP-eligible instrs in bundle, stall if VPQ doesn't have enough free entries
- [ ] Replace oracle VP block (lines ~224-243) with SVP path: call `VPU->predict()`, store vpq_index + stat flags in payload, set `vp_pred`/`vp_val` if confident
- [ ] Keep existing `PERFECT_VALUE_PRED` path intact
- [ ] Add oracle confidence mode: if `SVP_ORACLE_CONF`, compare SVP prediction against functional sim value, override confidence. `good_instruction` only allowed here and in perfect mode
- [ ] **CRITICAL**: `good_instruction` must NOT appear in real confidence path. Spec deducts points for this.

### C4: VPQ tail checkpointing (`rename.cc`)

- [ ] At branch checkpoint creation (line ~218-220), call `VPU->get_vpq_tail()` and save it alongside the checkpoint
- [ ] Coordinate with Vincent on where to store it (renamer checkpoint struct or parallel array indexed by branch_ID)
- [ ] For `squash_complete`, repair target is just vpq_head (discard everything in flight)

### C5: Pipeline integration (`pipeline.h`)

- [ ] Add `#include "vpu.h"` to `pipeline.h`
- [ ] Add `vpu_t *VPU` member to `pipeline_t` class

---

## Vincent's Tasks

**Files**: `execute.cc`, `retire.cc`, `squash.cc`, `main.cc`, `parameters.h`, `parameters.cc`, `stats.cc`, `pipeline.cc`

### V1: Misprediction detection (`execute.cc`) [no dependencies, start now]

Three `REN->write()` call sites need a VP check. Same pattern at each:
- If `vp_pred`: compare `C_value.dw` vs `vp_val`. If different, call `set_value_misprediction(AL_index)` and write correct value to PRF. If same, skip the write (Slide 45 optimization).
- If not `vp_pred`: write PRF as normal.

Sites:
- [ ] ALU path (line ~140, FIX_ME #14)
- [ ] Load hit path (line ~82-83, FIX_ME #13)
- [ ] Load replay path (line ~264-265, FIX_ME #18a in `load_replay()`)

**Do NOT use `actual->a_rdst[0].value`**. Only compare `C_value.dw` vs `vp_val`. Spec deducts for this.

### V2: SVP training (`retire.cc`) [needs `vpu.h`]

- [ ] After `REN->commit()` and before `checker()`: if instr was VP-eligible, read committed value from PRF, call `VPU->train(vpq_index, committed_val)`
- [ ] Using PAY to decide *when* to call train is OK per spec. Actual training data comes from VPQ inside `train()`.

### V3: VP statistics (`retire.cc` + `stats.cc`) [needs payload fields from Chris]

- [ ] Register 7 counters in `stats.cc`: `vpmeas_ineligible`, `vpmeas_eligible`, `vpmeas_miss`, `vpmeas_conf_corr`, `vpmeas_conf_incorr`, `vpmeas_unconf_corr`, `vpmeas_unconf_incorr`
- [ ] At retirement (after training, before `num_insn++`):
  - [ ] Not eligible: `inc_counter(vpmeas_ineligible)`
  - [ ] Eligible + SVP miss: `inc_counter(vpmeas_eligible)` + `inc_counter(vpmeas_miss)`
  - [ ] Eligible + SVP hit: `inc_counter(vpmeas_eligible)` + compare predicted vs committed value + check confidence flag, increment the right sub-counter
- [ ] Output format must match Gradescope validation runs

### V4: VPQ repair (`squash.cc`) [needs `vpu.h` + checkpoint agreement with Chris]

- [ ] In `squash_complete()` (after `REN->squash()`): call `VPU->repair(vpq_head)` to discard all in-flight VPQ entries
- [ ] In `resolve()` mispredicted branch path: call `VPU->repair(saved_vpq_tail)` using the tail saved at checkpoint time by Chris (Task C4)

### V5: CLI + parameters [no dependencies]

- [ ] `parameters.h/cc`: add `SVP_ENABLED`, `SVP_ORACLE_CONF`, `VPQ_SIZE`, `SVP_INDEX_BITS`, `SVP_TAG_BITS`, `SVP_CONF_MAX` with defaults
- [ ] `main.cc`: parse `--vp-svp=<VPQsize>,<oracleconf>,<indexbits>,<tagbits>,<confmax>`, set `SVP_ENABLED = true`
- [ ] Ensure `--vp-perf` and `--vp-svp` are mutually exclusive

### V6: VPU instantiation (`pipeline.cc`) [needs `vpu.h`]

- [ ] In `pipeline_t` constructor: if `SVP_ENABLED`, create `VPU = new vpu_t(...)`, else `VPU = nullptr`
- [ ] At end of simulation (stats dump): call `VPU->print_storage(stats_log)` if VPU exists

---

## Coordination Points

| # | What | Chris | Vincent |
|---|---|---|---|
| 1 | `vpu.h` interface | Defines | Calls train/repair/print_storage |
| 2 | `payload.h` new fields | Adds | Reads in retire.cc |
| 3 | VPQ tail checkpoint storage | Saves in rename.cc | Reads in squash.cc |
| 4 | Where to store VPQ tail snapshot | Both agree on location | |
| 5 | `vpmeas_*` output format | | Vincent matches Gradescope |
| 6 | `pipeline.h` VPU pointer | Adds include + member | Uses `VPU->` everywhere |

---

## Implementation Order

**Chris**:
1. ~~Write `vpu.h`~~ Done
2. Add payload fields to `payload.h`
3. Implement `vpu.cc` (predict + train first, then repair)
4. Wire into `rename.cc` (VPQ stall + predict call)
5. VPQ tail checkpoint save
6. Test with oracle confidence (no mispredictions expected)

**Vincent** (start V1 now, needs `vpu.h` for V2+):
1. V1: execute.cc misprediction detection (no dependencies)
2. V5: CLI + parameters (no dependencies)
3. V3b: stats.cc counter declarations (no dependencies)
4. Wait for Chris to share `payload.h` changes
5. V6: pipeline.cc VPU instantiation
6. V2: retire.cc training
7. V3a: retire.cc statistics
8. V4: squash.cc repair

---

## Build

Executable is `721sim`. CMakeLists uses `file(GLOB *.cc)`, so `vpu.cc` is auto-picked up.

```bash
cmake -B build && cmake --build build
# build/uarchsim/721sim
```

---

## Testing

Run on `grendel.ece.ncsu.edu`. See `docs/PROJECT_OVERVIEW.md` for full run-directory workflow.

```bash
make cleanrun SIM_FLAGS_EXTRA='-e100000000 [pipeline flags] [vp flags]'
```

1. **Perfect VP** (already works): `--vp-perf=1 --vp-eligible=1,1,1`. recovery_count should be 0.
2. **SVP + oracle conf**: `--vp-svp=256,1,10,0,7 --vp-eligible=1,1,1`. recovery_count = 0, vpmeas_conf_corr > 0.
3. **SVP + real conf**: `--vp-svp=256,0,10,0,7 --vp-eligible=1,1,1`. recovery_count > 0 expected.
4. **Gradescope**: IPC within 1%, vpmeas within 1%, storage cost exact.

Output in `stats.[timestamp]`. Check `ipc_rate` and `vpmeas_*`.
