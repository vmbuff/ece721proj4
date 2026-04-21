# Project 4 TODO

Current state of the `chris` branch.

---

## Done

- [x] `vpu.h` interface (SVP + VPQ class, predict/train/repair/print_storage)
- [x] `vpu.cc` full implementation
- [x] `payload.h` VP fields (vp_eligible, vp_svp_hit, vp_confident, vpq_index)
- [x] `rename.cc` VPQ stall condition + SVP prediction path + oracle conf + VPQ tail checkpoint save
- [x] `pipeline.h` VPU include, pointer, vpq_tail_chkpt[64] array
- [x] `pipeline.cc` VPU instantiation gated on SVP_ENABLED
- [x] `parameters.h/cc` SVP_ENABLED, SVP_ORACLE_CONF, VPQ_SIZE, SVP_INDEX_BITS, SVP_TAG_BITS, SVP_CONF_MAX
- [x] `main.cc` `--vp-svp=<VPQsize>,<oracleconf>,<indexbits>,<tagbits>,<confmax>` CLI + mutex check with --vp-perf
- [x] `retire.cc` VPU->train() stub (Chris did V2 so testing could proceed)
- [x] `squash.cc` VPU->repair() stubs in squash_complete and resolve misp (Chris did V4)
- [x] Build passes at 100%
- [x] Ran baseline + perfect VP mode without regressions on grendel
- [x] **VAL-2 validation: IPC 3.72 and cycle_count 2687129 exactly match reference**
- [x] Fixed instance++ ordering bug in predict() (increment before computing prediction)

---

## Still Needed (Vincent)

### V1: execute.cc misprediction detection

Three `REN->write()` call sites need a VP check. If `vp_pred` and computed value differs from `vp_val`: call `set_value_misprediction(AL_index)` and write correct value. If match: skip write (Slide 45 optimization). If not predicted: write normally.

- [ ] ALU path (FIX_ME #14, line ~140)
- [ ] Load hit path (FIX_ME #13, line ~82)
- [ ] Load replay path (FIX_ME #18a in `load_replay()`, line ~264)

**Do NOT use `actual->a_rdst[0].value` to check correctness.** Spec deducts for this.

### V3: vpmeas statistics

**stats.cc**: declare 7 counters via DECLARE_COUNTER:
- [ ] vpmeas_ineligible
- [ ] vpmeas_eligible
- [ ] vpmeas_miss
- [ ] vpmeas_conf_corr
- [ ] vpmeas_conf_incorr
- [ ] vpmeas_unconf_corr
- [ ] vpmeas_unconf_incorr

**retire.cc**: at commit (before `num_insn++`), increment the appropriate counter based on payload fields `vp_eligible`, `vp_svp_hit`, `vp_confident`, and a correctness check (predicted `vp_val` vs actual committed value from PRF).

- [ ] Implement the classification logic
- [ ] Output format must match Gradescope validation runs exactly

### V6b: print_storage call

- [ ] At end of simulation (wherever stats are dumped), call `VPU->print_storage(stats_log)` if `VPU` is non-null

### Optional: review Chris's stubs

Chris wrote temporary stubs for V2 and V4 to unblock testing. They're functionally correct but Vince should review and keep/replace as he prefers:
- [ ] retire.cc training call after `REN->commit()`
- [ ] squash.cc repair calls in `squash_complete()` and `resolve()` branch misp path

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
