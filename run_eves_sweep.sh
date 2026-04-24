#!/bin/bash
# ECE 721 Project 4 — competition branch EVES FPC denom sweep.
#
# Runs the 22 validation configs × 5 denominator configurations serially
# on grendel, producing per-cell run trees plus a combined CSV suitable
# for pasting into the presentation.
#
# Usage:
#   ./run_eves_sweep.sh                # all 5 configs × all 22 VALs (110 runs)
#   ./run_eves_sweep.sh 5              # all configs × VAL-5..22
#   ./run_eves_sweep.sh 5 18           # all configs × VAL-5..18
#   CONFIG_ONLY=3 ./run_eves_sweep.sh         # only config #3
#   CONFIG_ONLY=default ./run_eves_sweep.sh   # only the 'default' config (label match)
#   CONFIG_ONLY=3 ./run_eves_sweep.sh 5 7     # one slice
#
# Build on grendel first:
#   (cd "$REPO_ROOT" && cmake -B build && cmake --build build -j)
#
# Environment (override as needed):
#   REPO_ROOT      repo checkout             [default $HOME/ece721/project4]
#   BIN            721sim binary             [default $REPO_ROOT/build/uarchsim/721sim]
#   PK_PATH        SPEC proxy kernel
#   SPEC_ACTIVATE  SPEC env activate script
#   MICRO_DIR      dir with array2.riscv / if2.riscv
#   RUN_DIR        output tree               [default $REPO_ROOT/eves_sweep_runs]
#
# Output tree:
#   $RUN_DIR/CONFIG-<N>_<label>/VAL-<M>/...   # per-cell stats + make artifacts
#   $RUN_DIR/SUMMARY.csv                      # config,denoms,val,benchmark,ipc,status
#   $RUN_DIR/SUMMARY.txt                      # human-readable log

# Not using `set -u` — SPEC activate.bash references unset vars.

# --- paths -----------------------------------------------------------------
REPO_ROOT="${REPO_ROOT:-$HOME/ece721/project4}"
BIN="${BIN:-$REPO_ROOT/build/uarchsim/721sim}"
PK_PATH="${PK_PATH:-/mnt/designkits/spec_2006_2017/O2_fno_bbreorder/app_storage/pk}"
SPEC_ACTIVATE="${SPEC_ACTIVATE:-/mnt/designkits/spec_2006_2017/O2_fno_bbreorder/activate.bash}"
RUN_DIR="${RUN_DIR:-$REPO_ROOT/eves_sweep_runs}"
MICRO_DIR="${MICRO_DIR:-$REPO_ROOT/microbenchmarks}"

# --- five denom configurations ---------------------------------------------
# Ordered by strictness of single-cycle-ALU demotion, loose → strict, then the
# uniform-1/32 baseline from the EVES paper (Seznec CVP-1 §3.2 ablation).
#
#   fpc_off    — p=1/1  for all types  = FPC disabled (always increment).
#                Isolates the cooldown + SafeStride contribution.
#   balanced   — half of defaults; more permissive globally.
#   default    — our Week-1 recommended setting.
#   strict_alu — double ALU demotion (p=1/256); more filtering on noisy ops.
#   uniform_32 — Seznec's ablation; drops ~12% IPC on the paper, included as
#                a cautionary counterexample for the talk.
CONFIG_LABELS=("fpc_off"  "balanced"  "default"   "strict_alu" "uniform_32")
CONFIG_DENOMS=("1,1,1"    "64,16,4"   "128,32,8"  "256,32,8"   "32,32,32")

# --- args ------------------------------------------------------------------
START="${1:-1}"
END="${2:-22}"
CONFIG_ONLY="${CONFIG_ONLY:-}"

# --- sanity ----------------------------------------------------------------
if [ ! -x "$BIN" ]; then
    echo "ERROR: 721sim not found at $BIN" >&2
    echo "Build on grendel: (cd \"$REPO_ROOT\" && cmake -B build && cmake --build build -j)" >&2
    exit 1
fi
if [ ! -e "$PK_PATH" ]; then
    echo "ERROR: pk not found at $PK_PATH" >&2
    exit 1
fi
if [ ! -f "$SPEC_ACTIVATE" ]; then
    echo "ERROR: SPEC activate script not found at $SPEC_ACTIVATE" >&2
    exit 1
fi

# SPEC env (atool-simenv, benchmark checkpoints, etc.)
# shellcheck disable=SC1090
source "$SPEC_ACTIVATE"

mkdir -p "$RUN_DIR"
GLOBAL_TXT="$RUN_DIR/SUMMARY.txt"
GLOBAL_CSV="$RUN_DIR/SUMMARY.csv"
: > "$GLOBAL_TXT"
echo "config,denoms,val,benchmark,ipc,status" > "$GLOBAL_CSV"

# --- shared pipeline flags (same as run_validations.sh) --------------------
COMMON="-t --cbpALG=0 --fq=64 --cp=32 --al=256 --lsq=128 --iq=64 --iqnp=4 --fw=8 --dw=8 --iw=16 --rw=8 -e10000000"
MICRO_COMMON="--mdp=5,0 -t --cbpALG=0 --fq=64 --cp=32 --al=256 --lsq=128 --iq=64 --iqnp=4 --fw=8 --dw=8 --iw=16 --rw=8"

ASTAR_BENCH="473.astar_biglakes_ref"
ASTAR_CKPT="473.astar_biglakes_ref.1844.0.50.gz"
PERL_BENCH="400.perlbench_splitmail_ref"
PERL_CKPT="400.perlbench_splitmail_ref.2724.0.44.gz"

# --- helpers ---------------------------------------------------------------
run_spec() {
    # run_spec <cfg_dir> <val_id> <benchmark> <checkpoint.gz> <flags>
    local cfg_dir="$1" val_id="$2" bench="$3" ckpt="$4" flags="$5"
    local dir="$cfg_dir/VAL-${val_id}"

    rm -rf "$dir"
    mkdir -p "$dir"
    (
        cd "$dir" || exit 1
        ln -sf "$PK_PATH" pk
        ln -sf "$BIN" 721sim
        atool-simenv mkgen "$bench" --checkpoint "$ckpt"
        make cleanrun SIM_FLAGS_EXTRA="$flags"
    )
    return $?
}

run_micro() {
    # run_micro <cfg_dir> <val_id> <riscv_binary> <flags>
    local cfg_dir="$1" val_id="$2" riscv="$3" flags="$4"
    local dir="$cfg_dir/VAL-${val_id}"

    if [ ! -e "$MICRO_DIR/$riscv" ]; then
        return 2
    fi

    rm -rf "$dir"
    mkdir -p "$dir"
    (
        cd "$dir" || exit 1
        ln -sf "$PK_PATH" pk
        ln -sf "$BIN" 721sim
        ln -sf "$MICRO_DIR/$riscv" "$riscv"
        # shellcheck disable=SC2086
        ./721sim $flags pk "$riscv" 2>&1 | tee stats.micro
    )
    return ${PIPESTATUS[0]}
}

# Pull the last "IPC = N.N..." line from any stats.* file in the run dir.
# Returns empty string if not found.
extract_ipc() {
    local dir="$1"
    grep -Eho "IPC[[:space:]]*=[[:space:]]*[0-9]+\.[0-9]+" "$dir"/stats.* 2>/dev/null \
        | tail -1 \
        | grep -oE "[0-9]+\.[0-9]+"
}

record() {
    local label="$1" denoms="$2" val_id="$3" bench="$4" status="$5" dir="$6"
    local ipc
    ipc=$(extract_ipc "$dir")
    [ -z "$ipc" ] && ipc="NA"
    echo "$label,$denoms,$val_id,$bench,$ipc,$status" >> "$GLOBAL_CSV"
    echo "[cfg=$label val=$val_id bench=$bench ipc=$ipc status=$status]" | tee -a "$GLOBAL_TXT"
}

# --- the 22 VAL commands, {DENOMS} substituted per config ------------------
# Each entry: "kind|bench_or_riscv|ckpt_or_empty|flag_template"
#   kind  = spec|micro
#   {DENOMS} placeholder is replaced with --vp-eves-denoms=<intalu>,<fpalu>,<load>
# Translation from run_validations.sh: --vp-svp=<size,oc,idx,tag,cmax>
#   →  --vp-eves=<size,idx,tag,cmax>   (drop the oracleconf slot)
# VAL-1 and VAL-12 use --vp-perf=1 and ignore denoms (kept for a uniform grid).
VAL_DEFS=(
  # VAL-1:  perfect VP on astar (denoms ignored)
  "spec|$ASTAR_BENCH|$ASTAR_CKPT|--vp-eligible=1,0,1 --vp-perf=1 --mdp=5,0 --perf=0,0,0,1 $COMMON"
  # VAL-2..4: SVP + oracle-conf → EVES + real-conf, rollback-free
  "spec|$ASTAR_BENCH|$ASTAR_CKPT|--vp-eligible=1,0,1 --vp-eves=300,10,10,31 {DENOMS} --mdp=4,0 --perf=1,0,0,1 $COMMON"
  "spec|$ASTAR_BENCH|$ASTAR_CKPT|--vp-eligible=1,0,0 --vp-eves=300,10,10,31 {DENOMS} --mdp=4,0 --perf=1,0,0,1 $COMMON"
  "spec|$ASTAR_BENCH|$ASTAR_CKPT|--vp-eligible=0,0,1 --vp-eves=300,10,10,31 {DENOMS} --mdp=4,0 --perf=1,0,0,1 $COMMON"
  # VAL-5,6: varied VPQ
  "spec|$ASTAR_BENCH|$ASTAR_CKPT|--vp-eligible=1,0,1 --vp-eves=300,10,10,31 {DENOMS} --mdp=5,0 --perf=0,0,0,1 $COMMON"
  "spec|$ASTAR_BENCH|$ASTAR_CKPT|--vp-eligible=1,0,1 --vp-eves=100,10,10,31 {DENOMS} --mdp=5,0 --perf=0,0,0,1 $COMMON"
  # VAL-7..11: varied geometry/confmax
  "spec|$ASTAR_BENCH|$ASTAR_CKPT|--vp-eligible=1,0,1 --vp-eves=300,7,10,31  {DENOMS} --mdp=5,0 --perf=0,0,0,1 $COMMON"
  "spec|$ASTAR_BENCH|$ASTAR_CKPT|--vp-eligible=1,0,1 --vp-eves=300,7,0,31   {DENOMS} --mdp=5,0 --perf=0,0,0,1 $COMMON"
  "spec|$ASTAR_BENCH|$ASTAR_CKPT|--vp-eligible=1,0,1 --vp-eves=300,8,0,31   {DENOMS} --mdp=5,0 --perf=0,0,0,1 $COMMON"
  "spec|$ASTAR_BENCH|$ASTAR_CKPT|--vp-eligible=1,0,1 --vp-eves=300,8,0,15   {DENOMS} --mdp=5,0 --perf=0,0,0,1 $COMMON"
  "spec|$ASTAR_BENCH|$ASTAR_CKPT|--vp-eligible=1,0,1 --vp-eves=300,8,0,7    {DENOMS} --mdp=5,0 --perf=0,0,0,1 $COMMON"
  # VAL-12: perfect VP on perl (denoms ignored)
  "spec|$PERL_BENCH|$PERL_CKPT|--vp-eligible=1,0,1 --vp-perf=1 --mdp=5,0 --perf=0,0,0,1 $COMMON"
  # VAL-13..18: perl EVES sweeps
  "spec|$PERL_BENCH|$PERL_CKPT|--vp-eligible=1,0,1 --vp-eves=300,10,10,31  {DENOMS} --mdp=5,0 --perf=0,0,0,1 $COMMON"
  "spec|$PERL_BENCH|$PERL_CKPT|--vp-eligible=1,0,1 --vp-eves=80,10,10,31   {DENOMS} --mdp=5,0 --perf=0,0,0,1 $COMMON"
  "spec|$PERL_BENCH|$PERL_CKPT|--vp-eligible=1,0,1 --vp-eves=300,10,0,31   {DENOMS} --mdp=5,0 --perf=0,0,0,1 $COMMON"
  "spec|$PERL_BENCH|$PERL_CKPT|--vp-eligible=1,0,1 --vp-eves=300,14,0,31   {DENOMS} --mdp=5,0 --perf=0,0,0,1 $COMMON"
  "spec|$PERL_BENCH|$PERL_CKPT|--vp-eligible=1,0,1 --vp-eves=300,14,0,15   {DENOMS} --mdp=5,0 --perf=0,0,0,1 $COMMON"
  "spec|$PERL_BENCH|$PERL_CKPT|--vp-eligible=1,0,1 --vp-eves=300,14,0,63   {DENOMS} --mdp=5,0 --perf=0,0,0,1 $COMMON"
  # VAL-19..21: array2 microbenchmark
  "micro|array2.riscv||--vp-eligible=1,0,1 --vp-eves=256,4,0,3 {DENOMS} --perf=0,0,0,1 $MICRO_COMMON -s4481409 -e4000000"
  "micro|array2.riscv||--vp-eligible=1,0,1 --vp-eves=256,4,0,1 {DENOMS} --perf=0,0,0,1 $MICRO_COMMON -s4481409 -e4000000"
  "micro|array2.riscv||--vp-eligible=1,0,1 --vp-eves=256,4,0,0 {DENOMS} --perf=0,0,0,1 $MICRO_COMMON -s4481409 -e4000000"
  # VAL-22: if2 microbenchmark (different --perf and no trace cache)
  "micro|if2.riscv||--vp-eligible=1,0,1 --vp-eves=256,4,0,3 {DENOMS} --mdp=5,0 --perf=0,0,0,0 --cbpALG=0 --fq=64 --cp=32 --al=256 --lsq=128 --iq=64 --iqnp=4 --fw=8 --dw=8 --iw=16 --rw=8 -s869325 -e45036"
)

# --- resolve CONFIG_ONLY ---------------------------------------------------
num_cfgs=${#CONFIG_LABELS[@]}
resolve_cfg() {
    local needle="$1"
    if [[ "$needle" =~ ^[0-9]+$ ]]; then
        echo $((needle - 1))
        return
    fi
    for i in $(seq 0 $((num_cfgs - 1))); do
        if [[ "${CONFIG_LABELS[$i]}" == *"$needle"* ]]; then
            echo "$i"; return
        fi
    done
    echo -1
}

cfg_lo=0
cfg_hi=$((num_cfgs - 1))
if [ -n "$CONFIG_ONLY" ]; then
    idx=$(resolve_cfg "$CONFIG_ONLY")
    if [ "$idx" -lt 0 ]; then
        echo "ERROR: CONFIG_ONLY='$CONFIG_ONLY' not recognized." >&2
        echo "Valid: ${CONFIG_LABELS[*]}" >&2
        exit 1
    fi
    cfg_lo=$idx
    cfg_hi=$idx
fi

# --- driver ----------------------------------------------------------------
total=$(( (cfg_hi - cfg_lo + 1) * (END - START + 1) ))
cell=0
echo
echo "###################################################################"
echo "### EVES FPC denom sweep:  $((cfg_hi - cfg_lo + 1)) configs × $((END - START + 1)) VALs = $total runs"
echo "###   RUN_DIR    = $RUN_DIR"
echo "###   BIN        = $BIN"
echo "###   PK_PATH    = $PK_PATH"
echo "###   MICRO_DIR  = $MICRO_DIR"
echo "###################################################################"

for cfg_i in $(seq $cfg_lo $cfg_hi); do
    label="${CONFIG_LABELS[$cfg_i]}"
    denoms="${CONFIG_DENOMS[$cfg_i]}"
    dflag="--vp-eves-denoms=${denoms}"
    cfg_dir="$RUN_DIR/CONFIG-$((cfg_i + 1))_${label}"
    mkdir -p "$cfg_dir"

    echo
    echo "==================================================================="
    echo "=== CONFIG $((cfg_i+1))/$num_cfgs  label=$label  denoms=$denoms"
    echo "==================================================================="

    for val_i in $(seq "$START" "$END"); do
        if [ "$val_i" -lt 1 ] || [ "$val_i" -gt "${#VAL_DEFS[@]}" ]; then continue; fi

        cell=$((cell + 1))
        entry="${VAL_DEFS[$((val_i - 1))]}"
        IFS='|' read -r kind bench_or_riscv ckpt flag_template <<< "$entry"
        flags="${flag_template//\{DENOMS\}/$dflag}"

        echo
        echo "---- [$cell/$total]  CFG-$((cfg_i+1))/$label  VAL-$val_i  $bench_or_riscv ----"
        echo "flags: $flags"

        if [ "$kind" = "spec" ]; then
            run_spec "$cfg_dir" "$val_i" "$bench_or_riscv" "$ckpt" "$flags"
            rc=$?
        else
            run_micro "$cfg_dir" "$val_i" "$bench_or_riscv" "$flags"
            rc=$?
        fi

        case $rc in
            0) st=OK ;;
            2) st=SKIPPED ;;
            *) st="FAIL($rc)" ;;
        esac
        record "$label" "$denoms" "$val_i" "$bench_or_riscv" "$st" "$cfg_dir/VAL-$val_i"
    done
done

echo
echo "###################################################################"
echo "### Sweep complete."
echo "### CSV: $GLOBAL_CSV"
echo "### TXT: $GLOBAL_TXT"
echo "###################################################################"
echo
echo "Per-config IPC view (paste into presentation):"
column -t -s, "$GLOBAL_CSV" 2>/dev/null || cat "$GLOBAL_CSV"
