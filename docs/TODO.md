# Project 4 TODO: Work Split

**Chris**: SVP/VPQ module + rename.cc wiring
**Vincent**: execute.cc misprediction detection + retire.cc training/stats + squash.cc repair + CLI/params

---

## Status

- [x] `vpu.h` interface defined (see `uarchsim/vpu.h`)
- [x] `vpu.cc` full implementation (predict, train, repair, print_storage)
- [x] Payload field additions (vp_eligible, vp_svp_hit, vp_confident, vpq_index)
- [x] Rename wiring (VPQ stall, SVP prediction, oracle conf, VPQ tail checkpoint)
- [x] Pipeline integration (VPU pointer, include, instantiation, vpq_tail_chkpt array)
- [x] SVP parameters in parameters.h/cc (defaults, ready for CLI parsing)
- [ ] execute.cc misprediction detection (Vincent)
- [ ] retire.cc training + stats (Vincent)
- [ ] squash.cc repair (Vincent)
- [ ] CLI args `--vp-svp` in main.cc (Vincent)
- [ ] stats.cc counter declarations (Vincent)

---

## Chris's Tasks

**Files**: `vpu.h`, `vpu.cc`, `rename.cc`, `payload.h`

### C1: Implement `vpu.cc` (SVP + VPQ logic) -- DONE

- [x] Define `vpu.h` with SVP entry, VPQ entry, and public interface
- [x] Constructor: allocate SVP table (2^index_bits entries, all invalid) and VPQ (circular buffer, empty)
- [x] `get_svp_index()`: `(pc >> 1) & ((1 << index_bits) - 1)`
- [x] `get_svp_tag()`: `(pc >> (1 + index_bits)) & ((1 << tag_bits) - 1)`
- [x] `tag_matches()`: return true if tag_bits == 0 or tags equal
- [x] `count_inflight_instances()`: walk VPQ head to tail, count entries with matching svp_index
- [x] `predict()`: lookup SVP, compute prediction on hit, increment instance, always allocate VPQ entry
- [x] `train()`: tag match (update stride/conf/retired_value, decrement instance) or tag miss (replace entry, init instance via walk)
- [x] `repair()`: walk VPQ backward, decrement instance for each discarded hit entry
- [x] `vpq_free_entries()`: phase-bit arithmetic
- [x] `get_vpq_tail()` and `get_vpq_head()`
- [x] `print_storage()`: SVP bits per entry with valid bit, prints total bytes

### C2: Payload field additions (`payload.h`) -- DONE

- [x] `unsigned int vpq_index`
- [x] `bool vp_eligible`
- [x] `bool vp_svp_hit`
- [x] `bool vp_confident`
- [x] `vp_val` now set on any SVP hit (even unconfident)

### C3: Wire VPU into `rename.cc` -- DONE

- [x] VPQ stall condition (counts VP-eligible instrs, stalls if VPQ doesn't have enough free entries)
- [x] SVP prediction path with `VPU->predict()`, stores all stat flags in payload
- [x] Perfect VP path kept intact
- [x] Oracle confidence mode: overrides SVP confidence using functional sim check
- [x] `good_instruction` only in perfect and oracle conf paths, never in real conf path

### C4: VPQ tail checkpointing (`rename.cc`) -- DONE

- [x] At checkpoint creation, saves `VPU->get_vpq_tail()` into `vpq_tail_chkpt[branch_ID]`
- [x] Stored in `pipeline_t` as `unsigned int vpq_tail_chkpt[64]` (parallel array, avoids touching renamer.h)
- [x] For squash_complete, Vincent calls `VPU->repair(VPU->get_vpq_head())`

### C5: Pipeline integration -- DONE

- [x] `#include "vpu.h"` in `pipeline.h`
- [x] `vpu_t *VPU` member in `pipeline_t`
- [x] `vpq_tail_chkpt[64]` array in `pipeline_t`
- [x] VPU instantiation in `pipeline.cc` constructor (SVP_ENABLED check)
- [x] SVP parameters declared in `parameters.h/cc` with defaults

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

**Chris** -- ALL DONE, build passes:
1. ~~Write `vpu.h`~~ Done
2. ~~Add payload fields to `payload.h`~~ Done
3. ~~Implement `vpu.cc`~~ Done
4. ~~Wire into `rename.cc`~~ Done
5. ~~VPQ tail checkpoint save~~ Done
6. Test independently (before Vincent's code is merged):
   - [x] Build succeeds with `vpu.cc` included (glob picks it up)
   - [ ] Perfect VP mode still works (no regressions from payload/rename changes)
   - [ ] SVP + oracle confidence: predictions happen, VPQ allocates/frees, no crashes. recovery_count = 0 since only correct predictions injected
   - [ ] SVP + real confidence with misprediction detection stubbed (no `set_value_misprediction` calls yet): predictions inject, dependents schedule early, but mispredictions silently produce wrong values. IPC will be wrong but no crashes/asserts means the rename/dispatch/VPQ path is solid
   - [ ] VPQ stall counter: confirm VPQ is not frequently stalling rename (if it is, increase VPQ size)
   - [ ] print_storage output matches expected byte count for given SVP config

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
