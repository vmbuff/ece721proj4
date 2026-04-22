#!/bin/bash
# Run all Project 4 validation configs serially on grendel.
# Each VAL gets its own run directory under $RUN_DIR.
#
# Usage:
#   ./run_validations.sh               # run all 22
#   ./run_validations.sh 5             # run VAL-5 and onward
#   ./run_validations.sh 5 7           # run VAL-5 through VAL-7
#
# Before running: build the simulator (cmake --build build) and make sure
# $REPO_ROOT points to this repo and $PK_PATH points to the proxy kernel.
# Adjust MICRO_DIR if the array2.riscv / if2.riscv microbenchmarks live
# somewhere other than the default.

set -u

# --- paths (override via env if needed) -------------------------------------
REPO_ROOT="${REPO_ROOT:-$HOME/ece721/project4}"
BIN="$REPO_ROOT/build/uarchsim/721sim"
PK_PATH="${PK_PATH:-/mnt/designkits/spec_2006_2017/O2_fno_bbreorder/app_storage/pk}"
SPEC_ACTIVATE="${SPEC_ACTIVATE:-/mnt/designkits/spec_2006_2017/O2_fno_bbreorder/activate.bash}"
RUN_DIR="${RUN_DIR:-$REPO_ROOT/val_runs}"
MICRO_DIR="${MICRO_DIR:-$REPO_ROOT/microbenchmarks}"  # where array2.riscv / if2.riscv live

# --- range ------------------------------------------------------------------
START="${1:-1}"
END="${2:-22}"

# --- sanity -----------------------------------------------------------------
if [ ! -x "$BIN" ]; then
    echo "ERROR: 721sim not found at $BIN" >&2
    echo "Run: (cd $REPO_ROOT && cmake --build build)" >&2
    exit 1
fi
if [ ! -e "$PK_PATH" ]; then
    echo "ERROR: pk not found at $PK_PATH" >&2
    exit 1
fi

# Source spec env (sets up atool-simenv, benchmark checkpoints, etc.)
# shellcheck disable=SC1090
source "$SPEC_ACTIVATE"

mkdir -p "$RUN_DIR"

SUMMARY="$RUN_DIR/SUMMARY.txt"
: > "$SUMMARY"

# --- helpers ----------------------------------------------------------------
run_spec() {
    # run_spec <val_id> <benchmark> <checkpoint.gz> <flags...>
    local val_id="$1"; local bench="$2"; local checkpoint="$3"; local flags="$4"
    local dir="$RUN_DIR/VAL-${val_id}"

    echo
    echo "================================================================"
    echo "=== VAL-${val_id}  [${bench}]"
    echo "================================================================"
    echo "flags: $flags"

    rm -rf "$dir"
    mkdir -p "$dir"
    (
        cd "$dir" || exit 1
        ln -sf "$PK_PATH" pk
        ln -sf "$BIN" 721sim
        atool-simenv mkgen "$bench" --checkpoint "$checkpoint"
        make cleanrun SIM_FLAGS_EXTRA="$flags"
    )
    local rc=$?
    if [ $rc -eq 0 ]; then
        echo "VAL-${val_id}: OK" | tee -a "$SUMMARY"
    else
        echo "VAL-${val_id}: FAILED (exit $rc)" | tee -a "$SUMMARY"
    fi
}

run_micro() {
    # run_micro <val_id> <riscv_binary> <flags_before_pk>
    # The micro configs invoke ./721sim directly with "pk <binary>" as args.
    local val_id="$1"; local riscv="$2"; local flags="$3"
    local dir="$RUN_DIR/VAL-${val_id}"

    echo
    echo "================================================================"
    echo "=== VAL-${val_id}  [${riscv}]"
    echo "================================================================"
    echo "flags: $flags"

    if [ ! -e "$MICRO_DIR/$riscv" ]; then
        echo "VAL-${val_id}: SKIPPED ($MICRO_DIR/$riscv not found)" | tee -a "$SUMMARY"
        return
    fi

    rm -rf "$dir"
    mkdir -p "$dir"
    (
        cd "$dir" || exit 1
        ln -sf "$PK_PATH" pk
        ln -sf "$BIN" 721sim
        ln -sf "$MICRO_DIR/$riscv" "$riscv"
        # Micro configs run 721sim directly; tee stats so user can grep them.
        # shellcheck disable=SC2086
        ./721sim $flags pk "$riscv" 2>&1 | tee stats.micro
    )
    local rc=${PIPESTATUS[0]}
    if [ $rc -eq 0 ]; then
        echo "VAL-${val_id}: OK" | tee -a "$SUMMARY"
    else
        echo "VAL-${val_id}: FAILED (exit $rc)" | tee -a "$SUMMARY"
    fi
}

in_range() { [ "$1" -ge "$START" ] && [ "$1" -le "$END" ]; }

# --- config tables ----------------------------------------------------------
# Common pipeline flags shared by VAL-1..18 (SPEC runs).
COMMON="-t --cbpALG=0 --fq=64 --cp=32 --al=256 --lsq=128 --iq=64 --iqnp=4 --fw=8 --dw=8 --iw=16 --rw=8 -e10000000"

ASTAR_BENCH="473.astar_biglakes_ref"
ASTAR_CKPT="473.astar_biglakes_ref.1844.0.50.gz"

PERL_BENCH="400.perlbench_splitmail_ref"
PERL_CKPT="400.perlbench_splitmail_ref.2724.0.44.gz"

# --- VAL-1: perfect VP on astar --------------------------------------------
if in_range 1; then
    run_spec 1 "$ASTAR_BENCH" "$ASTAR_CKPT" \
        "--vp-eligible=1,0,1 --vp-perf=1 --mdp=5,0 --perf=0,0,0,1 $COMMON"
fi

# --- VAL-2..4: SVP + oracle conf, rollback-free (perfect BP, oracle MDP) ----
if in_range 2; then
    run_spec 2 "$ASTAR_BENCH" "$ASTAR_CKPT" \
        "--vp-eligible=1,0,1 --vp-svp=300,1,10,10,31 --mdp=4,0 --perf=1,0,0,1 $COMMON"
fi
if in_range 3; then
    run_spec 3 "$ASTAR_BENCH" "$ASTAR_CKPT" \
        "--vp-eligible=1,0,0 --vp-svp=300,1,10,10,31 --mdp=4,0 --perf=1,0,0,1 $COMMON"
fi
if in_range 4; then
    run_spec 4 "$ASTAR_BENCH" "$ASTAR_CKPT" \
        "--vp-eligible=0,0,1 --vp-svp=300,1,10,10,31 --mdp=4,0 --perf=1,0,0,1 $COMMON"
fi

# --- VAL-5,6: SVP + real conf, varied VPQ size ------------------------------
if in_range 5; then
    run_spec 5 "$ASTAR_BENCH" "$ASTAR_CKPT" \
        "--vp-eligible=1,0,1 --vp-svp=300,0,10,10,31 --mdp=5,0 --perf=0,0,0,1 $COMMON"
fi
if in_range 6; then
    run_spec 6 "$ASTAR_BENCH" "$ASTAR_CKPT" \
        "--vp-eligible=1,0,1 --vp-svp=100,0,10,10,31 --mdp=5,0 --perf=0,0,0,1 $COMMON"
fi

# --- VAL-7..11: varied SVP geometry/confmax --------------------------------
if in_range 7; then
    run_spec 7 "$ASTAR_BENCH" "$ASTAR_CKPT" \
        "--vp-eligible=1,0,1 --vp-svp=300,0,7,10,31 --mdp=5,0 --perf=0,0,0,1 $COMMON"
fi
if in_range 8; then
    run_spec 8 "$ASTAR_BENCH" "$ASTAR_CKPT" \
        "--vp-eligible=1,0,1 --vp-svp=300,0,7,0,31 --mdp=5,0 --perf=0,0,0,1 $COMMON"
fi
if in_range 9; then
    run_spec 9 "$ASTAR_BENCH" "$ASTAR_CKPT" \
        "--vp-eligible=1,0,1 --vp-svp=300,0,8,0,31 --mdp=5,0 --perf=0,0,0,1 $COMMON"
fi
if in_range 10; then
    run_spec 10 "$ASTAR_BENCH" "$ASTAR_CKPT" \
        "--vp-eligible=1,0,1 --vp-svp=300,0,8,0,15 --mdp=5,0 --perf=0,0,0,1 $COMMON"
fi
if in_range 11; then
    run_spec 11 "$ASTAR_BENCH" "$ASTAR_CKPT" \
        "--vp-eligible=1,0,1 --vp-svp=300,0,8,0,7 --mdp=5,0 --perf=0,0,0,1 $COMMON"
fi

# --- VAL-12..18: perl ------------------------------------------------------
if in_range 12; then
    run_spec 12 "$PERL_BENCH" "$PERL_CKPT" \
        "--vp-eligible=1,0,1 --vp-perf=1 --mdp=5,0 --perf=0,0,0,1 $COMMON"
fi
if in_range 13; then
    run_spec 13 "$PERL_BENCH" "$PERL_CKPT" \
        "--vp-eligible=1,0,1 --vp-svp=300,0,10,10,31 --mdp=5,0 --perf=0,0,0,1 $COMMON"
fi
if in_range 14; then
    run_spec 14 "$PERL_BENCH" "$PERL_CKPT" \
        "--vp-eligible=1,0,1 --vp-svp=80,0,10,10,31 --mdp=5,0 --perf=0,0,0,1 $COMMON"
fi
if in_range 15; then
    run_spec 15 "$PERL_BENCH" "$PERL_CKPT" \
        "--vp-eligible=1,0,1 --vp-svp=300,0,10,0,31 --mdp=5,0 --perf=0,0,0,1 $COMMON"
fi
if in_range 16; then
    run_spec 16 "$PERL_BENCH" "$PERL_CKPT" \
        "--vp-eligible=1,0,1 --vp-svp=300,0,14,0,31 --mdp=5,0 --perf=0,0,0,1 $COMMON"
fi
if in_range 17; then
    run_spec 17 "$PERL_BENCH" "$PERL_CKPT" \
        "--vp-eligible=1,0,1 --vp-svp=300,0,14,0,15 --mdp=5,0 --perf=0,0,0,1 $COMMON"
fi
if in_range 18; then
    run_spec 18 "$PERL_BENCH" "$PERL_CKPT" \
        "--vp-eligible=1,0,1 --vp-svp=300,0,14,0,63 --mdp=5,0 --perf=0,0,0,1 $COMMON"
fi

# --- VAL-19..22: microbenchmarks --------------------------------------------
# These invoke ./721sim directly; no make cleanrun / atool-simenv.
# Adjust $MICRO_DIR to point at the directory containing array2.riscv / if2.riscv.
MICRO_COMMON="--mdp=5,0 -t --cbpALG=0 --fq=64 --cp=32 --al=256 --lsq=128 --iq=64 --iqnp=4 --fw=8 --dw=8 --iw=16 --rw=8"

if in_range 19; then
    run_micro 19 array2.riscv \
        "--vp-eligible=1,0,1 --vp-svp=256,0,4,0,3 --perf=0,0,0,1 $MICRO_COMMON -s4481409 -e4000000"
fi
if in_range 20; then
    run_micro 20 array2.riscv \
        "--vp-eligible=1,0,1 --vp-svp=256,0,4,0,1 --perf=0,0,0,1 $MICRO_COMMON -s4481409 -e4000000"
fi
if in_range 21; then
    run_micro 21 array2.riscv \
        "--vp-eligible=1,0,1 --vp-svp=256,0,4,0,0 --perf=0,0,0,1 $MICRO_COMMON -s4481409 -e4000000"
fi
if in_range 22; then
    # VAL-22 uses if2, no trace cache flag in its command, and --perf=0,0,0,0.
    run_micro 22 if2.riscv \
        "--vp-eligible=1,0,1 --vp-svp=256,0,4,0,3 --mdp=5,0 --perf=0,0,0,0 --cbpALG=0 --fq=64 --cp=32 --al=256 --lsq=128 --iq=64 --iqnp=4 --fw=8 --dw=8 --iw=16 --rw=8 -s869325 -e45036"
fi

echo
echo "================================================================"
echo "All requested validations complete."
echo "Summary: $SUMMARY"
echo "Outputs: $RUN_DIR/VAL-<N>/stats.*"
echo "================================================================"
cat "$SUMMARY"
