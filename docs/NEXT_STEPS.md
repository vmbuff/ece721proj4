# Next Steps for Chris

## 1. Review the code

Read through each changed file and make sure you understand the logic before testing or sending to Vincent.

- `uarchsim/vpu.cc`: predict, train, repair, print_storage. Follow the spec diagram in the PDF to confirm the SVP indexing, prediction formula, training paths, and instance counter management match.
- `uarchsim/rename.cc`: VPQ stall condition, SVP prediction block, oracle confidence override, VPQ tail checkpoint save. Confirm `good_instruction` only appears inside the `PERFECT_VALUE_PRED` and `SVP_ORACLE_CONF` blocks.
- `uarchsim/payload.h`: new fields (`vp_eligible`, `vp_svp_hit`, `vp_confident`, `vpq_index`). Make sure `vp_val` is set on any SVP hit (not just confident ones) so retire can check correctness for all stat categories.
- `uarchsim/pipeline.h`: VPU pointer and `vpq_tail_chkpt[64]` array.
- `uarchsim/pipeline.cc`: VPU instantiation gated by `SVP_ENABLED`.
- `uarchsim/parameters.h/cc`: SVP parameter declarations and defaults.
- `docs/CHRIS_WORK.md`: full summary of every change and why.

## 2. Test on grendel (before Vincent's code)

SSH into `grendel.ece.ncsu.edu`, build, and run these tests. You can test your rename/dispatch/VPQ path without Vincent's execute/retire/squash changes.

### 2a. Perfect VP mode (regression check)

This mode was already working before your changes. Confirm it still works.

```
--vp-perf=1 --vp-eligible=1,1,1
```

Expected: completes without crashes/asserts, recovery_count = 0, IPC improves over baseline. If this breaks, you introduced a regression in rename.cc or payload.h.

### 2b. SVP + oracle confidence

First real test of the SVP path. Oracle confidence means only correct predictions are injected, so there should be no mispredictions. Chris added stub train() and repair() calls so the VPQ actually drains, making this test viable before Vince's full V2/V4.

```
--vp-svp=256,1,10,0,7 --vp-eligible=1,1,1
```

Expected: completes without crashes/asserts, recovery_count = 0. If it crashes, the bug is likely in predict(), train(), or repair(). If recovery_count > 0, the oracle confidence override in rename.cc is not working correctly.

Note: vpmeas counters aren't printed yet (Vincent's V3). No crashes means the VPU + train + repair loop is closed and predict/allocate/inject works.

### 2c. SVP + real confidence (no misprediction detection yet)

Test that real predictions inject without crashing. Since Vincent hasn't added misprediction detection in execute.cc, wrong predictions will silently produce incorrect values. The IPC will be wrong (checker will flag mismatches), but the pipeline should not crash or assert.

```
--vp-svp=256,0,10,0,7 --vp-eligible=1,1,1
```

Expected: runs without crashes. The checker WILL fire for wrong committed values (that's expected). If assertions fire in issue_queue.cc (lines 112/121/130), there's a double-wakeup bug, check the `!vp_pred` guards in execute.cc (those were Vince's earlier changes and should be fine).

If the checker fires, that's expected since misprediction detection isn't in yet. The key thing is no asserts/segfaults from the VPU path.

### 2d. print_storage sanity check

With `--vp-svp=256,0,10,0,7`: SVP has 2^10 = 1024 entries, 0 tag bits, conf_max = 7.

Manually compute:
- instance_bits = ceil(log2(257)) = 9
- conf_bits = ceil(log2(8)) = 3
- bits/entry = 0(tag) + 64(stride) + 64(retired_value) + 9(instance) + 3(conf) + 1(valid) = 141
- total bits = 1024 * 141 = 144384
- total bytes = ceil(144384 / 8) = 18048

Confirm the `stats.*` output file shows `Total SVP storage: 18048 bytes`. If the number is different, check the formula in print_storage().

## 3. Tell Vincent what he needs to know

- Branch `chris` is where all the work is. He should pull it.
- Chris stubbed in V2 (training), V4 (repair), and V5 (CLI) so testing can proceed without waiting. He should review the stubs in `retire.cc`, `squash.cc`, and `main.cc` and replace them if he would have written them differently.
- Remaining for Vincent:
  - **V1**: execute.cc misprediction detection (3 `REN->write()` sites)
  - **V3a**: retire.cc vpmeas counter increments (uses payload fields `vp_eligible`, `vp_svp_hit`, `vp_confident`, `vp_val`)
  - **V3b**: stats.cc vpmeas counter declarations
  - **V6b**: end-of-sim `VPU->print_storage(stats_log)` call

## 4. After Vincent's code is merged

Once his execute/retire/squash/CLI changes are in:

- Run SVP + real confidence again. Now recovery_count should be > 0 (mispredictions detected and recovered).
- Check all vpmeas counters are non-trivial.
- Run Gradescope validation runs. Target: IPC within 1%, vpmeas within 1%, storage cost exact match.
- Fix any mismatches by comparing stats output against validation reference.

## 5. Competition prep (Phase 2)

Only after Phase 1 passes all Gradescope validation runs:

- Experiment with SVP parameters within the storage budget (index_bits, tag_bits, conf_max).
- Consider which instruction classes to predict (some may hurt more than help).
- Look into CVP-1 predictors or other algorithms.
- Consider VR-5 recovery for lower misprediction penalty.
