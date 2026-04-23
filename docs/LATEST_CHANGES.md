# Latest Changes — VAL-5/7 Debug and Fix

Hey Vince — here's a readable summary of what changed on the `chris` branch after your `vince` push. I've kept it mostly chronological because the debug path explains why each change looks the way it does.

## TL;DR

Two real bugs were fixed:

1. **SVP `instance` counter leak** from `svp_hit=false` phantom entries in `count_inflight_instances`. Caused VAL-5 and VAL-7 to have ~185k extra confident-incorrect predictions.
2. **PC index/tag bit extraction off by one bit**. Our code shifted PC by 1 to skip bit 0, but the spec says skip bits 0 AND 1 (RISC-V 4-byte alignment). This folded bit 1 (always 0) into the low bit of the index, so only even-numbered SVP slots ever got used — effectively halving SVP capacity. Invisible on 1024-slot runs, but doubled the miss rate on VAL-7 (128 slots).

After fixing both, VAL-1..7 all match reference within 1% (and VAL-5/6 bit-identically).

## Starting state

Right after your last push (`b134f6f`), VAL-1..4 passed exactly. VAL-5 and VAL-7 were severely broken:

- VAL-5: IPC 1.67 vs reference 2.85, `vpmeas_conf_incorr` 194k vs 9k
- VAL-7: IPC 2.72 vs reference 2.79 (but distribution all wrong — miss rate 63% vs ref 33%)

Your status report mentioned adding phase-bit checkpointing and `discard_head()` — those ideas turned out not to be in any pushed branch. I verified by grepping; `vpq_tail_chkpt_phase`, `get_vpq_tail_phase`, and `discard_head` didn't exist anywhere. Maybe you had them locally?

## The debug path (what was tried and why)

### Dead ends

Before the real bugs surfaced, I tried three fixes that turned out to be no-ops for this workload — I'm leaving notes here so nobody wastes time on them again:

**(a) `discard_head()` for load-violation path.** Reasoned that `repair(vpq_head)` walked tail back to head exclusive, leaving the violating load's `instance++` orphaned. Wrong — I misread the loop. `repair()` steps tail backward *first*, then decrements the entry it landed on, so the head entry *is* decremented. Adding `discard_head()` produced identical output.

**(b) Phase-bit checkpointing in `repair()`.** Added `vpq_tail_chkpt_phase[64]`, extended `repair(pos, phase)` to compare both. Theory: tail could wrap a full `vpq_size` back to the saved position with phase flipped, and position-only comparison would silently exit as a no-op. Correct in principle — but in this workload the VPQ never actually wrapped (VPQ=300 > AL=256 means max 256 new entries between checkpoint and repair, not enough to wrap). Left the fix in anyway because it's real if it ever fires.

**(c) Hot-entry replacement protection.** Theory: spec says "only replace when conf==0" to protect hot entries from aliasing thrash. Wrong — I later pulled the actual spec text (page 2) and it says "Else // replace" unconditionally. Reverted.

### The real bugs

**Bug #1 (commit `6865a20`, `vpu.cc`):** The instance counter invariant is "instance = count of in-flight entries where `svp_hit=true` AND entry's PC tag matches current `svp[idx].tag`." Three places violated it:

- `count_inflight_instances()` counted by `svp_index` alone. An entry that missed at predict has `svp_hit=false` and never did `instance++`, but was counted anyway.
- `train()` tag-match path blindly decremented on every retirement. If svp was replaced between predict and train back to a tag matching this entry's PC, tag_matches succeeds even though no ++ was done.
- `repair()` decremented any `svp_hit=true` entry regardless of whether its PC tag still matches current svp.

The dominant leak: count_inflight counts a `svp_hit=false` entry, branch-misp squashes it, repair skips it (svp_hit=false), leaving a permanent +1 drift on the slot. For hot PCs with stable stride, every future confident prediction was off by `(leak * stride)` — but `stride` stayed correct on train (delta between consecutive values unchanged), so conf stayed saturated and every injection triggered val_misp. That matches the exact VAL-5/7 signature of ~185k conf_corr shifted to conf_incorr.

Fix: gate each of the three operations on the invariant. `count_inflight` requires `svp_hit=true` AND tag-match; `train` decrement requires `svp_hit=true`; `repair` decrement requires `svp_hit=true` AND tag-match.

This alone fixed VAL-5 bit-identically. VAL-7 got better but still had 2x the miss rate.

**Bug #2 (commit `67f949f`, `vpu.cc`):** Pulled the actual spec PDF (had to `pip install pypdf` locally) and found page 2 showing the PC layout:

```
PC: 00 | PCindex | PCtag | ...
     0 1 2       x x+1    63
```

Bits 0 AND 1 are `"00"` (RV 4-byte alignment). PCindex starts at bit 2. Our `get_svp_index` shifted by 1 instead of 2. Since bit 1 of any aligned PC is always 0, including it as the low bit of the index meant we only ever produced even-numbered indices. For VAL-5/6 with 1024-slot SVP, the workload fit in 512 slots anyway so we never noticed. VAL-7 has 128 slots — we were using 64. Miss rate doubled.

Fix: one-character change (`>> 1` → `>> 2`) in `get_svp_index`, and matching shift in `get_svp_tag`.

## File-by-file summary

| File | Change |
|---|---|
| `uarchsim/vpu.cc` | Bit-shift fix; instance-counter invariant (Bug #1 above). `count_inflight_instances`, `train`, and `repair` all gated on the invariant. |
| `uarchsim/vpu.h` | Added `get_vpq_tail_phase` / `get_vpq_head_phase`; extended `repair()` to take phase. |
| `uarchsim/pipeline.h` | Added `vpq_tail_chkpt_phase[64]` alongside the position array. |
| `uarchsim/pipeline.cc` | Cleanup: `((real confidence))` → `(real confidence)`; trim one `=` from `=== VALUE PREDICTOR ===…` header so it matches reference width (80 chars). |
| `uarchsim/rename.cc` | Save both position AND phase at checkpoint. |
| `uarchsim/squash.cc` | Pass both (pos, phase) at both `repair()` call sites (full squash + branch-misp resolve). |
| `uarchsim/retire.cc` | No new logic — just removed a diagnostic counter I'd added mid-debug. |
| `uarchsim/execute.cc` | No logic change — just removed diagnostic counters. |
| `uarchsim/stats.cc` | Removed 4 temporary `vpu_*` diagnostic counters. |
| `run_validations.sh` | New: drives all 22 VAL configs serially on grendel. One dir per run under `val_runs/`. |
| `docs/DEVELOPMENT.md` | Running log of the debug chain — includes the retracted Bug #2 note to explain why the code has a seemingly-pointless phase-bit infrastructure. |
| `docs/TODO.md` | Updated state table. |
| `docs/CHRIS_WORK.md` | Historical summary from before this round. |

## Validation results

| Test | Status |
|---|---|
| VAL-1 (perfect VP, astar) | Match |
| VAL-2/3/4 (SVP + oracle, astar) | Match exactly |
| VAL-5 (SVP + real conf, VPQ=300>AL, astar) | **Bit-identical** |
| VAL-6 (SVP + real conf, VPQ=100<AL, astar) | Bit-identical |
| VAL-7 (SVP + real conf, 128-slot SVP, astar) | IPC exact; ~5050-instr shuffle between unconf_corr/unconf_incorr (0.07% — within 1%) |
| VAL-8..22 | Not yet verified — `run_validations.sh` will drive them |

## Running it

```bash
git pull
cmake --build build
./run_validations.sh           # all 22
./run_validations.sh 5 7       # just VAL-5..7
```

Env overrides if your grendel paths differ from the defaults:

```bash
REPO_ROOT=/path/to/repo \
PK_PATH=/path/to/pk \
MICRO_DIR=/path/to/array2.if2 \
./run_validations.sh
```

## What's left for you

- Run `./run_validations.sh` on grendel and sanity-check VAL-8..22 against the Moodle references.
- Cross-check anything that was previously working in your environment but hadn't been pushed (e.g., if you had real phase-bit checkpointing locally that I didn't see).
- Review the invariant-gating fix in `vpu.cc` — the three gated operations have comments explaining why each gate is there. Flag anything that looks off.
