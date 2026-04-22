# Project 4 TODO

Current state of the `chris` branch (now synced with `vince`).

---

## Done

### Chris's original work (merged to main via PR #4)
- [x] `vpu.h` interface (SVP + VPQ class, predict/train/repair/print_storage)
- [x] `vpu.cc` full implementation
- [x] `payload.h` VP fields (vp_eligible, vp_svp_hit, vp_confident, vpq_index)
- [x] `rename.cc` VPQ stall condition + SVP prediction path + oracle conf + VPQ tail checkpoint save
- [x] `pipeline.h` VPU include, pointer, vpq_tail_chkpt[64] array
- [x] `pipeline.cc` VPU instantiation gated on SVP_ENABLED
- [x] `parameters.h/cc` SVP_ENABLED, SVP_ORACLE_CONF, VPQ_SIZE, SVP_INDEX_BITS, SVP_TAG_BITS, SVP_CONF_MAX
- [x] `main.cc` `--vp-svp=<VPQsize>,<oracleconf>,<indexbits>,<tagbits>,<confmax>` CLI + mutex check with --vp-perf
- [x] Build passes at 100%
- [x] Fixed instance++ ordering bug in predict() (increment before computing prediction)

### Vincent's work (commits c1e8271..b134f6f on `vince`)
- [x] **V1**: `execute.cc` misprediction detection at the three `REN->write()` sites (ALU path, load hit, load replay) — commit c1e8271
- [x] **V2**: `retire.cc` real training-at-retirement logic (replaced Chris's stub) — commit 5baafba
- [x] **V3**: `stats.cc` 7 vpmeas counters + `retire.cc` classification logic — commit 29e9c5b
- [x] **V6b**: `VPU->print_storage()` + VP config logging in `pipeline.cc` — commit b134f6f
- [x] Phase bit checkpointing: `vpq_tail_chkpt_phase[64]`, `get_vpq_tail_phase()`/`get_vpq_head_phase()`, `repair(pos, phase)` signature, `discard_head()` for load-violation handling

### Validation status (per Vincent, as of 2026-04-21)
- [x] **VAL-1** (perfect VP): passing — IPC + all vpmeas counters match
- [x] **VAL-2, VAL-3, VAL-4** (SVP + oracle conf, rollback-free): passing — IPC + vpmeas + storage cost all match
- [x] **VAL-6** (SVP + real conf, VPQ=100 < AL=256): IPC, conf_corr, conf_incorr match exactly. ~11766-instr misclassification between `vpmeas_unconf_corr` ↔ `vpmeas_unconf_incorr` — just over 1% threshold (43/50 expected)

---

## Still Needed

### VAL-5 / VAL-7 regression (VPQ > AL)
Both fail when VPQ size (300) > AL size (256): IPC ~1.67 vs expected ~2.85, `conf_incorr` ~194k vs expected ~9k. Does not occur when VPQ ≤ AL. See `DEVELOPMENT.md` for full diff analysis.

- [x] **Fix #1 applied (2026-04-22)**: added `discard_head()` for load-violation path — was leaking one SVP instance count per violated VP-eligible load (see DEVELOPMENT.md §Bug #2).
- [ ] Rerun VAL-5 / VAL-7 on grendel and compare to reference
- [ ] If still failing: apply Fix #2 — phase-bit checkpointing in branch-misp repair path (DEVELOPMENT.md §Bug #1)

Note: the phase-bit checkpointing Vincent described in his status report is **not present on any branch**. Confirm with him whether it exists locally before duplicating the work.

### VAL-6 unconfident misclassification (~1% over threshold)
- [ ] Investigate SVP state divergence after branch misprediction recovery that causes correctness comparison drift for unconfident predictions at retire

### Ruled out: cache latency
A diff summary of VAL-5 output vs reference flagged `L2_HIT_LATENCY=10` and `L3_HIT_LATENCY=30` as "changes," but these are the defaults in `parameters.cc` (lines 111, 122) and no VAL config passes `--L2` / `--L3` flags — both runs use identical cache params. Not a factor in the VPQ > AL regression.

---

## Build

```bash
cmake -B build && cmake --build build
# build/uarchsim/721sim
```

CMakeLists uses `file(GLOB *.cc)`, so any new `.cc` file (e.g., vpu.cc) is auto-picked up after re-running `cmake -B build`.

---

## Testing

Run on `grendel.ece.ncsu.edu`. Full workflow in `docs/PROJECT_OVERVIEW.md`.

```bash
make cleanrun SIM_FLAGS_EXTRA='-e10000000 [pipeline flags] [vp flags]'
```

### Sanity tests
1. **Baseline** (no VP): no regressions -- passes
2. **Perfect VP**: `--vp-perf=1 --vp-eligible=1,1,1`. recovery_count = 0, IPC > baseline -- passes
3. **SVP + oracle conf**: VAL-2 config -- passes, IPC exactly matches reference (3.72)
4. **SVP + real conf**: `--vp-svp=256,0,10,0,7 --vp-eligible=1,1,1`. Will produce wrong values silently until Vincent adds V1 misp detection. Should not crash.

### Gradescope validation (needs Vincent's V1 + V3 + V6b)
Check Moodle for the actual validation runs. Known configs so far:
```
VAL-2: --vp-eligible=1,0,1 --vp-svp=300,1,10,10,31 --mdp=4,0 --perf=1,0,0,1 -t --cbpALG=0 --fq=64 --cp=32 --al=256 --lsq=128 --iq=64 --iqnp=4 --fw=8 --dw=8 --iw=16 --rw=8 -e10000000
VAL-3: --vp-eligible=1,0,0 ... (same otherwise)
VAL-4: --vp-eligible=0,0,1 ... (same otherwise)
```
All on checkpoint `473.astar_biglakes_ref.1844.0.50.gz`.

Output in `stats.[timestamp]`. Check `ipc_rate` and `vpmeas_*`.
