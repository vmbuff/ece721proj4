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

## Bug #1: VPQ > AL — IPC and conf_incorr blow up (unresolved)

**Symptom:** When `VPQ_SIZE > AL_SIZE`, IPC drops from 2.85 to 1.67 and `vpmeas_conf_incorr` jumps from ~9k to ~194k. `ld_vio_count` also grows (75 → 103). Does not reproduce when VPQ ≤ AL.

**Root hypothesis (not yet proven):** The repair path in `vpu.cc` compares VPQ positions without phase bits. If VPQ tail wraps a full `vpq_size` back to the same position between checkpoint save and repair, `repair(restored_vpq_tail)` becomes a no-op and the speculative `svp[idx].instance++` increments from `predict()` are never undone. Inflated instance counters then produce wildly wrong `retired_value + instance*stride` predictions on future hits, which retire as confidently-incorrect.

### What has been ruled out

- **Cache latency config difference (red herring).** An AI-generated diff summary of VAL-5 output vs reference flagged `L2_HIT_LATENCY=10` and `L3_HIT_LATENCY=30` as changes, but these are the defaults in `uarchsim/parameters.cc:111` and `:122`. No VAL config passes `--L2` / `--L3` flags, so both runs use identical cache parameters. Not the cause.

### What has NOT been tried (despite being claimed)

Vincent reported implementing phase-bit checkpointing as the Moodle-board-recommended fix. **None of that code is present on any branch** (verified 2026-04-22 on `chris` == `vince` == `b134f6f`):

- `vpq_tail_chkpt_phase[64]` — not in `pipeline.h`
- `get_vpq_tail_phase()` / `get_vpq_head_phase()` — not in `vpu.h`
- `repair(pos, phase)` two-argument signature — `repair()` in `vpu.cc:210` still takes position only
- `discard_head()` for load-violation handling — does not exist anywhere
- rename.cc saves only `VPU->get_vpq_tail()` at `rename.cc:235`, no phase

Either Vincent did the work on an un-pushed local branch, or his description was aspirational. Before doing anything else, confirm with him whether the phase-bit work exists somewhere (stash, local branch, WIP file).

### Specific code sites involved

- **Checkpoint save:** `uarchsim/rename.cc:235` — saves position, no phase
- **Checkpoint array:** `uarchsim/pipeline.h:370` — `vpq_tail_chkpt[64]`, no phase array
- **Branch-misp repair call:** `uarchsim/squash.cc:159` — `VPU->repair(vpq_tail_chkpt[branch_ID])`
- **Full-squash repair call:** `uarchsim/squash.cc:48` — `VPU->repair(VPU->get_vpq_head())`
- **Repair walk:** `uarchsim/vpu.cc:210-224` — `while (vpq_tail != restored_vpq_tail)` — loop exits on position match alone

---

## Bug #2: Load violation leaks one SVP instance increment (FIXED 2026-04-22)

**Symptom:** On a load violation, `retire.cc:233` calls `squash_complete(offending_PC)`, which in turn calls `VPU->repair(VPU->get_vpq_head())`. `repair()` walks tail backward until it equals head, undoing instance increments along the way — but it stops *at* head and never decrements the entry *at* head. The violating load itself was VP-eligible, had its VPQ entry at head, and got `svp[idx].instance++` in `predict()`. That increment is never undone because:

1. `train()` is skipped — the instruction is not committed (see `retire.cc:76` — `train()` only runs under `!exception && !load_viol`).
2. `repair()` decrements entries `[head+1, tail)`, not including head itself.

Each load violation on a VP-eligible PC therefore permanently leaks one instance count. On re-fetch after squash, the same PC gets `predict()` again and bumps instance a second time. Over many violations, prediction = `retired_value + instance*stride` diverges.

**Why this could explain VAL-5/7 failing but VAL-6 passing:** The failing configs have 103 load violations vs an unknown-but-likely-smaller number for VAL-6. Leaked instance counts compound with every violation; with VPQ > AL, more VP-eligible instructions are in flight at any time, so more of them are caught by each violation-triggered squash. Ratio effects could plausibly account for the 20× jump in `conf_incorr`.

**Fix shape:** Either extend `repair()` to walk *to and including* head for full-squash mode, or add a `discard_head()` method that decrements head's instance and advances head forward past the violating load (Vincent's design). The load-violation retire path at `retire.cc:212-239` would need to call it before `squash_complete()`.

### Fix applied (2026-04-22)

Went with the `discard_head()` approach. Three changes:

1. **`uarchsim/vpu.h`** — added public `void discard_head()` declaration next to `get_vpq_head()`. Comment explains the orphaned-instance invariant and why `repair()` alone can't cover it.

2. **`uarchsim/vpu.cc`** — new `discard_head()` body, mirrors the pattern inside `repair()`: if the head entry was a hit, `assert(instance > 0); instance--`. Then advance `vpq_head` and flip `vpq_head_phase` on wrap. No stats incremented (load violations don't retire, so no vpmeas change).

3. **`uarchsim/retire.cc`** — in the load-violation branch, added
   ```cpp
   if (VPU && PAY.buf[PAY.head].vp_eligible)
      VPU->discard_head();
   ```
   between the MDP squash and the `squash_complete(offending_PC)` call. Guarded by `vp_eligible` because a non-eligible violated load never allocated a VPQ entry. Ordering: `discard_head()` advances VPQ head past the violated load, then `squash_complete()` calls `repair(get_vpq_head())` which walks tail back to the new head — correctly undoing every speculative entry including the violated one.

**Ordering proof sketch:** When a VP-eligible load hits the load-violation retire path, it is at `AL.head`. AL commits in-order; VPQ advances only on VP-eligible retirements; all instructions between the last VP-eligible commit and this load were non-eligible (VPQ head did not move). Therefore the violated load's VPQ entry is at `vpq_head`, which is what `discard_head()` operates on.

**Does not cover:** the exception retire path (`retire.cc:240-281`) has the same pattern (no train, calls squash_complete) and would leak instance if the exception-raising instruction were VP-eligible. Benchmark exception_count is 0 for this workload so it doesn't affect scoring, but a defensive `discard_head()` call there would make the invariant airtight.

**Expected impact on VAL-5:** if this is the primary leak, `conf_incorr` should fall from 194,821 toward the reference 9,218. Load-violation count may also drop from 103 toward 75 if leaked-instance-induced wrong predictions were causing secondary violations. If numbers don't fully recover, Bug #1 (phase-bit wrap in branch-misp repair) is the remaining contributor.

**Build status:** `cmake --build build` clean, no warnings.

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
