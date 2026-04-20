# Partner's Completed Work (Vincent Marcus Buff)

All commits from the `vmbuff/vince` branch, merged into `main` via two pull requests.

---

## Git History (All VP Commits, Chronological)

```
52c4629  VPU Support: instr. flag macros in pipeline.h & eligibility code in rename.cc
123ad8e  Updated spacing pipeline.h
a00ef6a  rename.cc oracle vp code, added new payload fields (val, pred)
73e6a13  perfect vp dispatch.cc logic
d2ea4c1  wakeup call guardrail logic execute.cc: addressing Comment 2 in Appendix
b6e993f  pipeline.h: moved macros to more appropriate location
04f995f  vp parameters added, perf vp mode, 3 vp eligible types in is_eligible defined/initialized
ad260f8  main.cc: perf vp command line argument support
d2a2839  rename.cc: perfect vp guardrail added
6ada40d  fixed build errors: main.cc vp function scope, pipeline.h eligibility function declaration
a371b4a  register_read.cc: additional wakeup call guardrail logic: addressing comment 2 in appendix
b21f062  Merge pull request #2 from vmbuff/vince  ← merged by you (Chris Clark)
```

"Addressing Comment 2 in Appendix" refers to **Appendix A, Comment 2** of the project spec, which explains the wakeup assertion problem and recommends the `!vp_pred` guard fix.

---

## What Each Commit Did

### `52c4629` — Flag macros + eligibility stub
- Added `IS_INTALU(flags)` and `IS_FPALU(flags)` macros to `pipeline.h`.
- Added initial eligibility check structure in `rename.cc`.

### `123ad8e` — Whitespace cleanup

### `a00ef6a` — Oracle VP + payload fields
- Added `vp_val` (uint64_t) and `vp_pred` (bool) to `payload_t` in `payload.h`.
- In `rename2()`: initialization block (`vp_pred = false`, `vp_val = 0`).
- Oracle VP block: queries `get_pipe()->peek(db_index)`, writes `a_rdst[0].value` into `vp_val`, sets `vp_pred = true`.

### `73e6a13` — Dispatch VP value injection
- In `dispatch.cc` FIX_ME #9: if `vp_pred == true` → `set_ready()` + `write()` with predicted value; else → `clear_ready()`.

### `d2ea4c1` — Wakeup suppression in execute.cc
- Added `if (!PAY.buf[index].vp_pred)` guards in three places:
  - Load hit path (FIX_ME #13)
  - Multi-cycle ALU second-to-last sub-stage (FIX_ME #11b)
  - Load replay path (FIX_ME #18a)

### `b6e993f` — Macro reorganization in pipeline.h

### `04f995f` — VP parameters + is_eligible implementation
- `parameters.h/cc`: `PERFECT_VALUE_PRED`, `predINTALU`, `predFPALU`, `predLOAD`.
- `rename.cc`: full `is_eligible()` implementation with all three instruction class checks.

### `ad260f8` — CLI args for `--vp-perf` and `--vp-eligible`

### `d2a2839` — `good_instruction` guard in rename.cc
- Added `PAY.buf[index].good_instruction` check before querying functional sim in oracle VP block. (Correct: `good_instruction` is required for oracle/perfect modes per spec.)

### `6ada40d` — Build error fixes
- Fixed `is_eligible` scope in `main.cc`, added declaration to `pipeline.h`.

### `a371b4a` — Wakeup suppression in register_read.cc
- Added `if (!PAY.buf[index].vp_pred)` guard around `IQ.wakeup()` in FIX_ME #11a. Matches pattern from `d2ea4c1`.

---

## Summary: What Is Complete

| Feature | Status | File(s) |
|---|---|---|
| `IS_INTALU` / `IS_FPALU` macros | **Done** | `pipeline.h` |
| `vp_val`, `vp_pred` payload fields | **Done** | `payload.h` |
| `is_eligible()` function | **Done** | `rename.cc` |
| VP parameters (`PERFECT_VALUE_PRED`, `predINTALU`, etc.) | **Done** | `parameters.h/cc` |
| CLI: `--vp-perf`, `--vp-eligible` | **Done** | `main.cc` |
| Oracle VP value fetch in `rename2()` | **Done** | `rename.cc` |
| `good_instruction` guard in oracle mode | **Done** | `rename.cc` |
| Dispatch-time VP value write + ready bit | **Done** | `dispatch.cc` |
| Wakeup suppression in `register_read.cc` | **Done** | `register_read.cc` |
| Wakeup suppression in `execute.cc` (3 places) | **Done** | `execute.cc` |
| `set_value_misprediction()` infrastructure | **Done** | `pipeline.cc`, `renamer.cc` |
| `val_misp` bit in Active List | **Done** | `renamer.h/cc` |
| VR-1 recovery at retire (`val_misp` → `squash_complete`) | **Done** | `retire.cc` |

---

## What Remains For You To Implement

### Phase 1 Remaining (from spec Section 2.1)

**Task 5 (partial) — Misprediction detection in `execute.cc`**  
After ALU or load execution computes `C_value.dw`, compare it against `vp_val` when `vp_pred == true`. Call `set_value_misprediction(AL_index)` if different. Must be done at all call sites where `REN->write()` is called:
- `pipeline_t::execute()` — ALU path (FIX_ME #14 area)
- `pipeline_t::execute()` — load hit path (FIX_ME #13 area)
- `pipeline_t::load_replay()` — load replay path (FIX_ME #18a area)

Per spec: **do NOT use `actual->a_rdst[0].value`** — only compare `C_value.dw` vs. `vp_val`.

Spec also recommends implementing the Slide 45 optimization: skip the redundant PRF write for correctly-predicted instructions (only write PRF if `!vp_pred || mispredicted`).

**Task 3 — SVP + VPQ**  
Create `uarchsim/vpu.h` and `uarchsim/vpu.cc` implementing:
- SVP table: entries with `tag`, `stride`, `retired_value`, `instance`, `conf`
- VPQ: circular FIFO queue, structural hazard in Rename stage
- `predict()`: query SVP at rename time, speculatively increment `instance`, allocate VPQ entry
- `train()`: called at retire time with the VPQ entry, update SVP in-order
- `repair()`: called on any squash to walk VPQ and repair `instance` counters

**Task 4 — Oracle confidence mode**  
Add to `rename2()` SVP prediction path: if `SVP_ORACLE_CONF` is true, instead of using `conf == confmax`, compare SVP's prediction against `actual->a_rdst[0].value` to determine confidence.

**Task 6 — CLI: `--vp-svp=...`**  
Parse `--vp-svp=<VPQsize>,<oracleconf>,<indexbits>,<tagbits>,<confmax>` in `main.cc`.

**Task 7 — Storage cost accounting**  
Output total SVP bytes at end of simulation. Formula:  
`(tag_bits + 64 + 64 + instance_bits + conf_bits) * num_entries / 8` bytes.  
VPQ is excluded from the budget.

**Task 8 — Retired instruction statistics**  
Track per-instruction at retirement:
- `vpmeas_ineligible`: not eligible per `is_eligible()`
- `vpmeas_miss`: eligible but no SVP entry (or low confidence)
- `vpmeas_conf_corr`: confident + correct
- `vpmeas_conf_incorr`: confident + wrong (caused a recovery)
- `vpmeas_unconf_corr`: unconfident + correct (lost opportunity)
- `vpmeas_unconf_incorr`: unconfident + wrong

**Rename stall conditions**  
In `rename2()`, add VPQ stall: count VP-eligible instructions, check VPQ has enough free entries, return early if not.

---

### Recommended Implementation Order

1. **Misprediction detection** (Task 5 completion) — enables you to run with real VP and see recoveries.
2. **SVP + VPQ skeleton** (Task 3) — create the data structure, wire `predict()` into rename, `train()` into retire.
3. **VPQ stall in rename** — once VPQ exists.
4. **CLI args for `--vp-svp`** (Task 6) — needed to submit to Gradescope.
5. **Statistics** (Task 8) — needed to match Gradescope output.
6. **Storage accounting** (Task 7) — needed for exact match on Gradescope.
7. **Oracle confidence** (Task 4) — useful for testing SVP without recovery.
