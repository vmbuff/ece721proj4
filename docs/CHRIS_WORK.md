# Chris's Work Summary

Everything I implemented for the SVP/VPQ module and pipeline wiring.

## Files Changed

| File | What |
|---|---|
| `uarchsim/vpu.h` | SVP + VPQ class definition |
| `uarchsim/vpu.cc` | Full SVP + VPQ implementation |
| `uarchsim/payload.h` | Added VP stat/tracking fields to payload_t |
| `uarchsim/pipeline.h` | Added VPU include, VPU pointer, VPQ tail checkpoint array |
| `uarchsim/pipeline.cc` | VPU instantiation in constructor |
| `uarchsim/rename.cc` | VPQ stall, SVP prediction, oracle conf, VPQ tail checkpoint save |
| `uarchsim/parameters.h` | SVP parameter declarations |
| `uarchsim/parameters.cc` | SVP parameter defaults |

## vpu.h

Class interface for the VPU. Two internal structures:

**svp_entry_t**: tag, stride (int64_t), retired_value, instance, conf, valid. The prediction
formula is `retired_value + instance * stride`. Instance is speculatively incremented at
rename so multiple in-flight copies of the same PC each predict successive stride steps.

**vpq_entry_t**: pc, svp_index, predicted_value, svp_hit, confident. Stores the predicted
value even for unconfident predictions so retire can classify correctness for all stat
categories.

VPQ uses head/tail phase bits (same pattern as renamer FL/AL) instead of a count variable.

## vpu.cc

Full implementation of all methods:

**get_svp_index()**: Extracts index bits from PC. Bit 0 discarded (always 0), next
`index_bits` bits are the index.

**get_svp_tag()**: Extracts tag bits from PC, sitting above the index bits.

**tag_matches()**: Returns true if tag_bits == 0 (no tags) or if the stored tag matches.

**count_inflight_instances()**: Walks VPQ head to tail counting entries that map to a given
SVP index. Only used during SVP entry replacement to initialize the instance counter.

**vpq_free_entries()**: Phase-bit arithmetic. Same phase + same position = empty (all free),
different phase + same position = full (none free), otherwise computed from pointer
difference.

**predict()**: Called per VP-eligible instr in rename2. Looks up SVP by PC, computes
predicted value on hit, speculatively increments instance. Always allocates a VPQ entry
(even on miss) because misses need training at retire and repair on squash. Returns hit/miss.

**train()**: Called per VP-eligible retired instr. Two paths:
- Tag match: compute `new_stride = committed_val - retired_value`. If stride matches,
  increment conf (saturate at conf_max). If stride changed, replace stride and reset conf.
  Update retired_value. Decrement instance.
- Tag miss: replace the entry entirely. Set instance by walking VPQ to count in-flight peers.
  Temporarily advances head past the entry being freed before counting.
Frees VPQ head after training.

**repair()**: Called on any squash. Walks VPQ backward from tail to the restored position,
decrementing SVP instance counters for each discarded hit entry.

**print_storage()**: Computes SVP bits per entry = tag + 64(stride) + 64(retired_value) +
ceil(log2(vpq_size+1)) for instance + ceil(log2(conf_max+1)) for conf + 1(valid). Prints
total bytes.

## payload.h Changes

Added these fields to `payload_t` (in the Rename Stage section):

- `vp_val`: now set on any SVP hit (even unconfident), not just when injected. This lets
  retire check correctness for the unconf_corr and unconf_incorr stat categories.
- `vp_pred`: unchanged, true only when confident and injected.
- `vp_eligible`: true if the instruction passed `is_eligible()`. Used by retire for the
  ineligible vs eligible stat split.
- `vp_svp_hit`: true if SVP had a valid tag-matching entry. Used for the miss stat.
- `vp_confident`: true if the prediction was confident (or oracle said correct). Used for
  the confident/unconfident stat split.
- `vpq_index`: VPQ entry allocated at rename, passed to `train()` at retire.

## pipeline.h Changes

- Added `#include "vpu.h"` in the includes section.
- Added `vpu_t *VPU` member to `pipeline_t` (next to LSU).
- Added `unsigned int vpq_tail_chkpt[64]` array for saving VPQ tail positions alongside
  branch checkpoints. Indexed by branch_ID (0-63). Saved at checkpoint creation, read
  by squash.cc during branch misprediction recovery.

## pipeline.cc Changes

Added VPU instantiation after the renamer setup:
- If `SVP_ENABLED`: creates `new vpu_t(VPQ_SIZE, SVP_INDEX_BITS, SVP_TAG_BITS, SVP_CONF_MAX)`.
- Otherwise: sets `VPU = NULL`.

## parameters.h/cc Changes

Added declarations and defaults for: `SVP_ENABLED`, `SVP_ORACLE_CONF`, `VPQ_SIZE`,
`SVP_INDEX_BITS`, `SVP_TAG_BITS`, `SVP_CONF_MAX`. These are the config variables
that Vincent's CLI parsing (Task V5) will set from `--vp-svp=...`.

## rename.cc Changes

Three additions to `rename2()`:

**1. VPQ stall condition** (after checkpoint/reg stalls, before rename loop):
Counts VP-eligible instrs in the bundle, stalls rename if VPQ doesn't have enough free
entries. Only runs when VPU is non-null.

**2. VP prediction block** (replaced the old oracle-only block):
For each instruction in the rename loop, initializes all VP payload fields to defaults, then:
- If `PERFECT_VALUE_PRED`: original oracle path (unchanged from Vince's code). Uses
  `good_instruction` to gate the functional sim peek.
- Else if `VPU` exists: calls `VPU->predict()`, stores vpq_index and stat flags in payload.
  If oracle conf mode (`SVP_ORACLE_CONF`), overrides SVP's confidence by comparing the
  prediction against the functional sim value (uses `good_instruction`, allowed in oracle mode).
  If real conf mode, uses SVP's own confidence directly. `good_instruction` never appears
  in the real conf path.

**3. VPQ tail checkpoint** (inside checkpoint creation):
When a branch checkpoint is created, saves `VPU->get_vpq_tail()` into
`vpq_tail_chkpt[branch_ID]`. This is what squash.cc reads when calling `VPU->repair()`
on a branch misprediction.

## Build

Builds clean at 100%. `vpu.cc` is auto-picked up by the glob in CMakeLists. No manual
CMakeLists edit needed.

## Added Stubs for Vincent's V2, V4, V5 (so Chris can test)

These were added to unblock testing. Vince should review and replace if he has
something different in mind.

- **V2 (retire.cc training)**: added `VPU->train()` call after `REN->commit()` in the
  main commit path. Reads committed value from PRF.
- **V4 (squash.cc repair)**:
  - `squash_complete()`: `VPU->repair(VPU->get_vpq_head())` (empties VPQ)
  - `resolve()` misp path: `VPU->repair(vpq_tail_chkpt[branch_ID])`
- **V5 (CLI parsing)**: added `--vp-svp=<VPQsize>,<oracleconf>,<indexbits>,<tagbits>,<confmax>`
  parsing in main.cc. Mutex with `--vp-perf`.

## What Still Needs Vincent

- V1: execute.cc misprediction detection (3 REN->write sites)
- V3: retire.cc vpmeas stats counter increments + stats.cc counter declarations
- V6b: end-of-sim `VPU->print_storage(stats_log)` call

## squash_complete repair note

For `squash_complete()`, Vincent needs to call `VPU->repair(VPU->get_vpq_head())`
to discard all in-flight VPQ entries. Added `get_vpq_head()` public method to vpu.h
for this purpose.

For branch mispredictions, Vincent calls `VPU->repair(vpq_tail_chkpt[branch_ID])`
using the checkpoint array saved in rename.

## Build Status

Builds clean at 100%. All changes compile with no warnings or errors.
