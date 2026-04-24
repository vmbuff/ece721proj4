# EVES Competition Testing Summary

Handoff doc for whoever is building the presentation/script.
You also have access to the raw CSVs in the repo root (`SVP-baseline.csv`, `EVES-*.csv`).

---

## TL;DR

- **Final entry:** EVES with FPC denominators `(intalu, fpalu, load) = (4, 2, 2)`
- **Flag:** `--vp-eves=300,10,10,31 --vp-eves-denoms=4,2,2`
- **Performance metric:** harmonic mean of IPC across 15 SPEC checkpoints
- **Result:** **2.0294 H-mean IPC, +0.49% over SVP-baseline (2.0195)**, on the same binary, same micro-architecture

EVES wins because aggressive integer/FP confidence saturation recovers most of SVP's xalancbmk performance while LOAD denom = 2 prevents an mcf collapse. The wins on hmmer/sphinx3/povray/sjeng (where SVP mispredicts at non-trivial rates) more than offset the residual xalancbmk gap.

---

## Test setup

- **Binary:** `721sim` (built locally, scp'd to NCSU HPC as `721sim-eves`)
- **Cluster:** HPC LSF (`login.hpc.ncsu.edu`), submitted via `sim-launch` from `/share/ece721/benchmarks/app_storage/activate.source` toolchain
- **Benchmarks:** 15 SPEC checkpoints (mcf, xalancbmk, fotonik3d, zeusmp, sphinx3, h264ref, astar, omnetpp, bwaves, perlbench, leela, hmmer, povray, sjeng, bzip2). 10M committed instructions each.
- **Common micro-arch flags** (held constant across all configs):
  ```
  --vp-eligible=1,0,1 --mdp=5,0 --perf=0,0,0,1 -t --cbpALG=0
  --fq=64 --cp=32 --al=256 --lsq=128 --iq=64 --iqnp=4
  --fw=8 --dw=8 --iw=16 --rw=8 -e10000000
  ```
  (16-wide issue, 256-entry AL, 64-entry IQ, 32 branch checkpoints, gshare branch predictor.)
- **Sweep harness:** `run_hpc_eves_sweep.sh` in repo root. Each config submits 15 LSF jobs (one per benchmark); `sim-collect <name>` aggregates results into a CSV.

The `--vp-eligible=1,0,1` mask predicts INTALU and LOAD ops but not FPALU. Even though FPALU isn't predicted in this run, the FP denominator still appears in the EVES knob (it's used by EVES's per-type FPC if/when FPALU is enabled — kept in the sweep for completeness).

### CSV columns

```
checkpoint, ipc, commit_count,
vpmeas_ineligible, vpmeas_eligible, vpmeas_miss,
vpmeas_conf_corr, vpmeas_conf_incorr,
vpmeas_unconf_corr, vpmeas_unconf_incorr
```

`vpmeas_conf_*` count predictions actually injected into the pipeline. `vpmeas_unconf_*` count predictions that EVES filtered out (training the SVP but not affecting execution). The competition metric uses only `ipc`, but the breakdown is useful for explaining *why* a config wins or loses.

---

## The story: three rounds of sweeps

### Round 1 — Initial sweep across the knob space

5 configs spanning slow-confidence to fast-confidence, with one "flat" config (all denoms equal):

| Config | denoms (int, fp, load) | H-mean IPC | vs SVP |
|---|---|---|---|
| SVP-baseline | (n/a — saturating counter) | 2.0195 | — |
| EVES-d128-32-8 | 128, 32, 8 | 1.9892 | -1.50% |
| EVES-aggressive | 32, 8, 2 | 2.0131 | -0.32% |
| EVES-conservative | 512, 128, 32 | 1.9334 | -4.27% |
| EVES-flat | 64, 64, 64 | 1.9124 | -5.30% |
| EVES-load-heavy | 256, 256, 8 | 1.9889 | -1.51% |

**Headline finding: no EVES config beat SVP.** Closest was `EVES-aggressive` at -0.32%.

**Diagnostic — where the regression lives:**

xalancbmk dominates the H-mean gap. SVP gets it at 1.91 IPC; even EVES-aggressive drops it to 1.69 (-11.5%). Looking at vpmeas: SVP has 3.39M confident-correct vs only 1,329 confident-incorrect predictions on xalancbmk. SVP is **99.96% accurate** on its confident predictions. EVES's FPC layer can only *gate* good predictions away here — there's nothing wrong to filter out. With slow denoms, half the correct predictions never reach confidence and IPC tanks.

**Where EVES wins:** hmmer (+8%), sphinx3 (+6%), povray (+2.3%), sjeng (+1.3%), bzip2 (+1.5%). These are workloads where SVP has meaningful confident-mispredict rates (hmmer: 1.4%, sphinx3: 1.4%) — exactly where the EVES gating story is supposed to pay off.

**Why H-mean punished us:** these wins are at the *high* end of the IPC distribution (2.3-3.2 IPC), where harmonic mean is least sensitive. Reciprocal contribution of the xalancbmk loss (`1/1.69 - 1/1.91 = +0.068`) is ~3× the hmmer gain (`1/2.26 - 1/2.37 = -0.021`).

### Round 2 — Recover xalancbmk by saturating FPC faster

Hypothesis: if FPC denoms approach 1, EVES's confidence ramps as fast as SVP's saturating counter, and we keep most of SVP's xalancbmk predictions while still gating the bad ones on hmmer/sphinx3.

3 configs sweeping toward denom=1:

| Config | denoms | H-mean IPC | vs SVP |
|---|---|---|---|
| EVES-d16-4-1 | 16, 4, 1 | 1.8682 | **-7.49%** |
| EVES-d8-2-1 | 8, 2, 1 | 1.8739 | **-7.21%** |
| EVES-d4-2-1 | 4, 2, 1 | 1.8760 | **-7.10%** |

**These were our worst configs.** What happened:

The xalancbmk recovery worked exactly as predicted: 1.91 → 1.86 (-2.6%) on d4-2-1, beating round-1's best.

But all three configs collapsed mcf from 1.68 IPC to **0.82 IPC** (-51%). All three configs landed at *exactly* 0.82, suggesting they hit the same degenerate equilibrium.

**Root cause:** with `LOAD_DENOM=1`, FPC saturates after a single successful training. mcf is pointer-chasing — chaotic value patterns and high cache miss rate. EVES becomes confident on a load PC after one correct prediction, the next prediction is wrong (causing a pipeline flush), the cooldown gates predictions for 128 retires after each mispredict, the SafeStride counter ratchets up but doesn't cross threshold fast enough, and the system enters a flush-and-cooldown feedback loop where IPC sits below the no-VP baseline.

**Lesson:** EVES's safety nets (cooldown + SafeStride) work for slow-burn drift, not single-load-PC chaos. LOAD denom = 1 is a cliff for chaotic-value workloads.

### Round 3 — Aggressive INT/FP, conservative LOAD

Hypothesis from Round 2: keep the xalancbmk recovery (which is INT-driven) but hold `LOAD_DENOM ≥ 2` to avoid the mcf cliff. Scan INT denom across 16, 8, 4 with FP=2, LOAD=2:

| Config | denoms | H-mean IPC | vs SVP |
|---|---|---|---|
| EVES-d16-4-2 | 16, 4, 2 | 2.0181 | -0.07% |
| EVES-d8-2-2 | 8, 2, 2 | **2.0234** | **+0.20%** |
| **EVES-d4-2-2** | **4, 2, 2** | **2.0294** | **+0.49%** |

**Two configs beat SVP. d4-2-2 is the competition entry.**

The hypothesis was right end-to-end:
- xalancbmk on d4-2-2: 1.80 (SVP: 1.91, only -5.8% — vs round-1's -11.5%)
- mcf on d4-2-2: 1.67 (SVP: 1.68, -0.6% — basically tied)
- All round-1 wins preserved: hmmer +4.4%, sphinx3 +5.9%, astar +1.8%, sjeng +1.3%, povray +2.3%, bzip2 +1.1%

The xalancbmk gap is the floor: even with denom=4, FPC needs ~4 successful trainings to saturate, vs SVP's saturating counter that takes just 31. (SVP "warmup" is faster in absolute trains because conf_max=31 means it counts up by 1 per correct training, not down by ~30. EVES with denom=4 increments by 1 with probability 1/4 each correct training, so on average it takes 4× as many trainings to add 1 to conf — meaning roughly 4×31 = 124 successful trainings to saturate.) Some xalancbmk PCs never reach saturation in the 10M-instruction window, so a few correct predictions stay gated. But that residual loss is more than offset by the wins elsewhere.

---

## Final config: per-benchmark breakdown

| Benchmark | SVP IPC | d4-2-2 IPC | Δ % | Notes |
|---|---|---|---|---|
| 429.mcf | 1.68 | 1.67 | -0.6% | LOAD=2 just barely safe |
| 623.xalancbmk | 1.91 | 1.80 | **-5.8%** | The unavoidable floor |
| 649.fotonik3d | 1.07 | 1.07 | 0.0% | VP-ineligible-dominated |
| 434.zeusmp | 2.30 | 2.29 | -0.4% | Tied |
| 482.sphinx3 | 1.70 | 1.80 | **+5.9%** | High SVP mispred rate, EVES gates |
| 464.h264ref | 5.78 | 5.79 | +0.2% | Tied |
| 473.astar | 2.85 | 2.90 | +1.8% | Mild win |
| 471.omnetpp | 0.83 | 0.83 | 0.0% | VP doesn't help |
| 603.bwaves | 1.81 | 1.81 | 0.0% | VP doesn't help |
| 400.perlbench | 5.28 | 5.29 | +0.2% | Tied |
| 641.leela | 2.40 | 2.40 | 0.0% | Tied |
| 456.hmmer | 2.26 | 2.36 | **+4.4%** | High SVP mispred rate, EVES gates |
| 453.povray | 3.05 | 3.12 | +2.3% | Win |
| 458.sjeng | 3.12 | 3.16 | +1.3% | Win |
| 401.bzip2 | 2.68 | 2.71 | +1.1% | Win |

Score: **8 wins, 1 significant loss (xalancbmk), 6 tied** vs SVP-baseline.

---

## Suggested visuals

Recipient has the CSVs — these are all directly plottable.

### Plot 1 — Headline H-mean ranking (hero slide)
- Bar chart, all 9 configs sorted by H-mean IPC
- Horizontal reference line at `SVP-baseline = 2.0195`
- Bars colored green if > SVP, red if < SVP
- Annotate `EVES-d4-2-2` as the winner with the +0.49% delta

### Plot 2 — The xalancbmk vs mcf tradeoff (the round-2 lesson)
- X-axis: INT_DENOM (log scale, range 4–128)
- Two lines plotted as IPC on Y:
  - xalancbmk IPC (one series for LOAD=1, one for LOAD=2)
  - mcf IPC (one series for LOAD=1, one for LOAD=2)
- The LOAD=1 mcf line has the dramatic 0.82 cliff regardless of INT_DENOM
- The LOAD=2 mcf line stays flat near 1.68
- Both xalancbmk lines slope from ~1.4–1.5 (high INT_DENOM) up to ~1.8 (low INT_DENOM)
- This is the visual punch line for "we found the sweet spot"

### Plot 3 — Per-benchmark Δ IPC for d4-2-2 vs SVP
- Horizontal bar chart, 15 bars sorted by signed Δ
- Green bars above zero (sphinx3, hmmer, povray, sjeng, astar, bzip2)
- Tied bars near zero (8 of them)
- Single red bar below zero (xalancbmk)
- Caption: "EVES wins on 6 benchmarks, ties 8, loses 1 — net H-mean win"

### Plot 4 — vpmeas breakdown showing what EVES is *doing*
- Stacked horizontal bar per benchmark for two configs (SVP vs d4-2-2)
- Pick 4 benchmarks to show: xalancbmk (the loss), hmmer (the win), sphinx3 (the win), mcf (the recovery)
- Stack: `conf_corr | conf_incorr | unconf_corr | unconf_incorr | miss | ineligible`
- Visualizes the gating: for hmmer/sphinx3 you see EVES converted high `conf_incorr` SVP bars into `unconf_*`. For xalancbmk you see EVES took some `conf_corr` away (the unavoidable cost). For mcf you see basically identical breakdowns (LOAD=2 preserves SVP's behavior).

### Plot 5 — IPC heatmap over the (LOAD, INT) denom space (optional, depends on time)
- 2D grid: rows = LOAD denoms tested {1, 2, 8, 32}, cols = INT denoms tested {4, 8, 16, 32, 64, 128, 256, 512}
- Color = H-mean IPC
- Sparse — only colored cells exist where we ran a config
- Highlight the cliff at LOAD=1 and the sweet spot at (LOAD=2, INT=4)
- Good for a "design space exploration" slide

---

## Talking points / story arc for the script

1. **Setup the metric (1 slide):** Harmonic mean of IPC across 15 benchmarks — emphasize that this metric punishes low-IPC regressions disproportionately.

2. **Setup EVES (1-2 slides):** EVES = SVP + filtering layer. Forward Probabilistic Counter (FPC), per-type denominators, 128-cycle cooldown after mispredict, SafeStride kill-switch if global mispredict rate exceeds 1/1024. Cite Perais & Seznec HPCA 2014 + Seznec CVP-1 2018.

3. **First sweep — the surprise (2 slides):** "We swept obvious configs and SVP beat all of them. The culprit: xalancbmk." Show H-mean ranking + xalancbmk vpmeas (99.96% SVP accuracy → nothing for the filter to gain there).

4. **Second sweep — the cliff (2 slides):** "We tried saturating FPC faster to recover xalancbmk. xalancbmk recovered, but mcf fell off a cliff." Show the LOAD=1 mcf collapse (0.82 IPC) — the punchline diagnostic that drove the next iteration.

5. **Third sweep — the sweet spot (1 slide):** "Hold LOAD ≥ 2, push INT denom down. Found two configs that beat SVP." Headline ranking with d4-2-2 highlighted.

6. **Per-benchmark breakdown (1 slide):** d4-2-2 vs SVP for all 15 benchmarks. The wins/losses table.

7. **Why it works (1 slide):** vpmeas breakdown for hmmer + xalancbmk. EVES gates SVP's mispredictions on bursty workloads while paying a small price on already-accurate ones.

8. **Submission (1 slide):** `--vp-eves=300,10,10,31 --vp-eves-denoms=4,2,2`, **2.0294 H-mean IPC, +0.49% over SVP-baseline.**

---

## File reference (in repo root)

| File | Config |
|---|---|
| `SVP-baseline.csv` | `--vp-svp=300,0,10,10,31` |
| `EVES-d128-32-8.csv` | round 1, denoms 128/32/8 |
| `EVES-aggressive.csv` | round 1, denoms 32/8/2 |
| `EVES-conservative.csv` | round 1, denoms 512/128/32 |
| `EVES-flat.csv` | round 1, denoms 64/64/64 |
| `EVES-load-heavy.csv` | round 1, denoms 256/256/8 |
| `EVES-d16-4-1.csv` | round 2, denoms 16/4/1 |
| `EVES-d8-2-1.csv` | round 2, denoms 8/2/1 |
| `EVES-d4-2-1.csv` | round 2, denoms 4/2/1 |
| `EVES-d16-4-2.csv` | round 3, denoms 16/4/2 |
| `EVES-d8-2-2.csv` | round 3, denoms 8/2/2 |
| **`EVES-d4-2-2.csv`** | **round 3, denoms 4/2/2 — final entry** |

H-mean computation:
```python
import csv
def hmean(rows): return len(rows)/sum(1/float(r["ipc"]) for r in rows)
with open("EVES-d4-2-2.csv") as f:
    print(hmean(list(csv.DictReader(f))))
# 2.0294...
```
