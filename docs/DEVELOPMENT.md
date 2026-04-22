# Project 4 Development Log

A running record of bugs, investigations, and roadblocks encountered during SVP/VPQ implementation. The intent is that if we (or anyone continuing this) hit a new failure, we can check here first to see what has already been ruled out or attempted.

---

## Validation status (as of 2026-04-22)

| Test | Config | Status |
|---|---|---|
| VAL-1 | Perfect VP | Pass — IPC + all vpmeas counters match |
| VAL-2 | SVP + oracle conf, VPQ=300, no rollback | Pass — IPC 3.72 exact |
| VAL-3 | SVP + oracle conf, `--vp-eligible=1,0,0` | Pass |
| VAL-4 | SVP + oracle conf, `--vp-eligible=0,0,1` | Pass |
| VAL-5 | SVP + real conf, **VPQ=300 > AL=256** | **Fail — IPC 1.67 vs 2.85, conf_incorr ~194k vs ~9k** |
| VAL-6 | SVP + real conf, **VPQ=100 < AL=256** | ~Pass — IPC + conf_corr/incorr match, but unconf_corr ↔ unconf_incorr misclassified by ~11766 instrs (~1% over threshold) |
| VAL-7 | SVP + real conf, **VPQ=300 > AL=256** | Fail (same signature as VAL-5) |

Common thread: tests that do not exercise rollback (perfect BP + oracle MDP + oracle conf) all pass. Tests that trigger branch misprediction recovery or load-violation recovery expose bugs in the VPQ repair path.

---

## Bug #1: VPQ position-only repair (FIXED 2026-04-22)

**Symptom:** When `VPQ_SIZE > AL_SIZE`, IPC drops from 2.85 to 1.67 and `vpmeas_conf_incorr` jumps from ~9k to ~194k. `ld_vio_count` also grows (75 → 103). Does not reproduce when VPQ ≤ AL.

**Root cause:** The original `repair()` in `vpu.cc` compared VPQ tail positions without phase bits:

```cpp
while (vpq_tail != restored_vpq_tail) { ... }
```

In long branch-resolution windows the VPQ can advance a full `vpq_size` between checkpoint save and repair (instructions keep flowing; old entries retire and make room at the tail). When that happens, `vpq_tail` wraps back to the saved position with `vpq_tail_phase` flipped. The position-only loop then exits immediately as a no-op and the entire window of speculative `svp[idx].instance++` increments is orphaned. Inflated instance counters produce wildly wrong `retired_value + instance*stride` predictions on future hits, which retire as confidently-incorrect.

Why this hits VPQ > AL harder: when VPQ < AL, the VPQ stall condition throttles rename and the VPQ rarely fills. When VPQ > AL, AL stalls rename first, VPQ never stalls, and speculative tail can walk much farther — including past the same physical position.

### Fix applied

Position + phase comparison throughout the repair path:

1. **`uarchsim/vpu.h`** — `repair()` signature extended to `repair(unsigned int pos, bool phase)`. Added `get_vpq_tail_phase()` and `get_vpq_head_phase()`.
2. **`uarchsim/vpu.cc`** — `repair()` loop condition is now `while (vpq_tail != restored_vpq_tail || vpq_tail_phase != restored_vpq_tail_phase)`. New phase getters.
3. **`uarchsim/pipeline.h`** — added `bool vpq_tail_chkpt_phase[64]` alongside the existing position array.
4. **`uarchsim/rename.cc`** — checkpoint save stores both position and phase.
5. **`uarchsim/squash.cc`** — both call sites (`squash_complete` full squash and `resolve` branch-misp) pass both values.

### What has been ruled out

- **Cache latency config difference (red herring).** An AI-generated diff summary of VAL-5 output vs reference flagged `L2_HIT_LATENCY=10` and `L3_HIT_LATENCY=30` as changes, but these are the defaults in `uarchsim/parameters.cc:111` and `:122`. No VAL config passes `--L2` / `--L3` flags, so both runs use identical cache parameters. Not the cause.

### Note on prior claim

Vincent's earlier status report described implementing phase-bit checkpointing, but no such code was present on any branch as of `b134f6f` on 2026-04-22. The fix above is the first time it has been committed.

---

## Bug #2 (retracted): Load violation head-decrement — **not a real bug**

**Initial claim:** That `repair()` walked `[head+1, tail)` and skipped the head entry itself, orphaning the violating load's `instance++`. Fix proposed: a `discard_head()` helper called from the load-violation retire path.

**Reality:** `repair()` actually decrements the head entry. The loop body steps tail backward *first*, then decrements the entry it lands on:

```cpp
while (vpq_tail != restored_vpq_tail) {
   if (vpq_tail == 0) { vpq_tail = vpq_size - 1; vpq_tail_phase = !vpq_tail_phase; }
   else                 vpq_tail--;
   if (vpq[vpq_tail].svp_hit) { ... instance--; }
}
```

Trace with `head=5, tail=10, repair(5)`: walks tail 10→9→8→7→6→5, decrementing entries 9, 8, 7, 6, 5. The loop exits *after* decrementing entry 5 (the head). So the original `squash_complete() → repair(get_vpq_head())` path was correct.

**Outcome of the fix attempt:** committed `discard_head()` (commit `da8e73d`), pushed, rebuilt on grendel. Output was bit-identical to the pre-fix run. That identical-output result is what forced re-reading the loop — `discard_head()` + `repair(new_head)` decrements the exact same set of entries as `repair(old_head)`, just in a different order. No-op.

**Rolled back** in the same commit as the Bug #1 fix.

**Lesson:** trace the loop on paper before confidently asserting what it touches. "Loop exits when pos==target" doesn't mean the target position isn't processed — depends on whether the step comes before or after the body.

---

## Bug #3: VAL-6 unconfident misclassification (~1% over threshold, unresolved)

**Symptom:** VAL-6 passes IPC, `conf_corr`, and `conf_incorr` exactly. But `vpmeas_unconf_corr` and `vpmeas_unconf_incorr` are swapped for ~11,766 instructions. Score impact: 43/50 instead of 50/50.

**Hypothesis (Vincent):** SVP state differences after branch misprediction recovery affect the unconfident-prediction correctness comparison at retirement. An unconfident entry's `vp_val` is computed at rename time using the speculative SVP state; if the SVP state is different at retire (post-recovery), the comparison against the committed value flips.

**Not yet investigated.** Possibly related to Bug #1 — if `repair()` is partial, SVP state diverges from the oracle path after every branch misp.

---

## Dead ends and design notes

### Instance counter on tag miss replacement (resolved before it became a bug)

`vpu.cc:191-197`: when `train()` replaces an SVP entry (tag mismatch or invalid), it initializes the new entry's instance counter by walking the VPQ *past* the entry being freed. The head is temporarily advanced, `count_inflight_instances()` runs, then head is restored. If this walk included the freed entry, it would over-count by one. Pattern verified correct.

### `instance++` ordering in `predict()` (resolved)

`vpu.cc:121`: instance is incremented *before* computing the prediction, so the first in-flight instance predicts `retired_value + 1*stride` (the next expected value), not `retired_value + 0*stride` (the already-committed value). This was a bug in an earlier version — fixed by commit `20e1e56` on main.

### Oracle conf off-path handling (resolved)

Off-path instructions (branches not yet resolved, predicted wrong direction) have no functional-sim `good_instruction` reference. Original oracle-conf path assumed all renamed instructions were on-path. Fixed by commit `c719e5e`: off-path oracle conf falls back to the SVP's own confidence counter.

---

## Debugging aids that have been removed

- `vp_svp_hit`, `vp_svp_miss`, `vp_svp_inject` diagnostic counters were added during VAL-2 validation then removed after IPC matched reference (commits `caf8912`, `633e3cd`).
- `vpq_stall_count` rename-stall counter was added to measure VPQ backpressure (commit `96e6f6c`, not removed).

If debugging Bug #1 again, consider re-adding:
- a counter for `repair()` entries walked per call (Vincent tried this, saw 30–200 per call — ruling out infinite loop, but did not disambiguate the 0-entry wrap-around case)
- a counter for `repair()` no-op invocations (tail already at restored position)
- per-SVP-index instance counter sanity histogram at end of sim

---

## Build and test recipe

```bash
cmake -B build && cmake --build build
# on grendel.ece.ncsu.edu
make cleanrun SIM_FLAGS_EXTRA='-e10000000 [pipeline flags] [vp flags]'
```

Known VAL configs (from Moodle):

```
VAL-2: --vp-eligible=1,0,1 --vp-svp=300,1,10,10,31 --mdp=4,0 --perf=1,0,0,1 -t --cbpALG=0 --fq=64 --cp=32 --al=256 --lsq=128 --iq=64 --iqnp=4 --fw=8 --dw=8 --iw=16 --rw=8
VAL-5/7: --vp-svp=300,0,10,0,7 (real conf, VPQ=300 > AL=256)  [exact full command TBD from Moodle]
VAL-6:   --vp-svp=100,0,10,0,7 (real conf, VPQ=100 < AL=256)  [exact full command TBD from Moodle]
```

Checkpoint: `473.astar_biglakes_ref.1844.0.50.gz`.
