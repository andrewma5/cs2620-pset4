#!/usr/bin/env bash
# scripts/run-paxos-tests.sh - tier-1 sim test sweep for pset4 paxos.
#
# Matrix: 4 N values × loss/failure configs = 34 configurations.
#   - N=1:   baseline only (degenerate path, no peers)
#   - N=3,5,7 each get 11 configs:
#       1 baseline
#       4 loss levels: 0.01, 0.05, 0.10, 0.20
#       3 failure schedules: permanent, temporary, split-brain
#       3 failure × loss=0.10 combinations
#
# Usage:
#   scripts/run-paxos-tests.sh                # SEEDS=100 (default, ~4 min)
#   SEEDS=1000 scripts/run-paxos-tests.sh     # full rigor (~30 min)

set -u
BIN="${BIN:-./build/tm-paxos-tests}"
SEEDS="${SEEDS:-100}"

if [ ! -x "$BIN" ]; then
    echo "binary not found: $BIN — run 'make' first" >&2
    exit 1
fi

failed_any=0
run() {
    local name="$1"; shift
    local flags="$*"
    printf "  %-42s " "$name"
    local start_ts; start_ts=$(date +%s)
    local out
    out=$("$BIN" $flags 1 "$SEEDS" 2>&1)
    local end_ts; end_ts=$(date +%s)
    local secs=$((end_ts - start_ts))
    local result
    result=$(echo "$out" | tail -1)
    if [[ "$result" == *"0 fail"* ]]; then
        printf "PASS   %-18s  %3ds\n" "($result)" "$secs"
    else
        printf "FAIL   %-18s  %3ds\n" "($result)" "$secs"
        echo "$out" | tail -10 | sed 's/^/      /' >&2
        failed_any=1
    fi
}

run_for_n() {
    local n="$1"
    echo
    echo "  --- N=$n (quorum=$(( n / 2 + 1 ))) ---"
    if [ "$n" -eq 1 ]; then
        run "N=$n  baseline"                       --nreplicas=$n
        return
    fi
    run "N=$n  baseline"                           --nreplicas=$n
    run "N=$n  loss=0.01"                          --nreplicas=$n --loss=0.01
    run "N=$n  loss=0.05"                          --nreplicas=$n --loss=0.05
    run "N=$n  loss=0.10"                          --nreplicas=$n --loss=0.10
    run "N=$n  loss=0.20"                          --nreplicas=$n --loss=0.20
    run "N=$n  fail_permanent"                     --nreplicas=$n --failure=1
    run "N=$n  fail_temporary"                     --nreplicas=$n --failure=2
    run "N=$n  fail_split_brain"                   --nreplicas=$n --failure=3
    run "N=$n  fail_permanent  + loss=0.10"        --nreplicas=$n --failure=1 --loss=0.10
    run "N=$n  fail_temporary  + loss=0.10"        --nreplicas=$n --failure=2 --loss=0.10
    run "N=$n  fail_split_brain + loss=0.10"       --nreplicas=$n --failure=3 --loss=0.10
}

echo "tier-1 sim test sweep: $SEEDS seeds per configuration"
echo "  binary: $BIN"
echo "  matrix: N ∈ {1, 3, 5, 7} × {baseline, 4 loss levels, 3 failures, 3 failure×loss-0.10}"
printf "\n  %-42s %-6s %-18s %s\n" "configuration" "result" "(detail)" "time"
echo "  ------------------------------------------------------------------------"

for n in 1 3 5 7; do
    run_for_n $n
done

echo
if [ "$failed_any" -eq 0 ]; then
    echo "All configurations passed."
    exit 0
else
    echo "Some configurations failed. See output above."
    exit 1
fi
