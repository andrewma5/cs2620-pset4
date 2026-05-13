#!/usr/bin/env bash
# scripts/full-test.sh — run the complete free regression suite in one shot.
#
# Runs every $0-cost test layer:
#   1. SM unit tests        (tm-tests binary, in docker)
#   2. tm CLI unit tests    (test_tm_cli.py, on host)
#   3. Paxos sim — single   (tm-paxos-tests one run, in docker)
#   4. Smoke (single-tm)    (scripts/smoke.sh against ephemeral tm-server)
#   5. Smoke (lease)        (scripts/smoke-lease.sh, tests lease-window path)
#   6. Docker failure sweep (3 iters × 4 scenarios, on host)
#
# Optional layers (env-gated, skipped by default):
#   SIM_SWEEP=1     run the 34000-run paxos sim sweep (~5-10 min)
#   LOCAL_TCP=1     run the local-tcp.sh sweep (needs jq in container)
#
# Redundant — already exercised inside other phases:
#   - scripts/synthetic-agent.sh   (each docker-sweep iter drives it)
#   - tm-replay + 3-way verify     (docker-sweep's check_logs runs both per iter)
#
# Skips by design:
#   - Real-Claude runs (cost real money — those live in docs/REAL_CLAUDE_RUN.md)
#
# Usage:
#   scripts/full-test.sh                      # default: ~5-10 min
#   ITERS=5 scripts/full-test.sh              # heavier docker sweep
#   SIM_SWEEP=1 scripts/full-test.sh          # also run 34k sim sweep
#   LOCAL_TCP=1 scripts/full-test.sh          # also run local-tcp
#
# Exit 0 iff every phase passed.
#
# Notes:
#   - Compatible with macOS bash 3.2 (no associative arrays).
#   - Runs the *pre-built* binaries directly (./build/tm-tests, etc) so we
#     don't trigger cmake re-checks that fail on the bind-mount path mismatch
#     between this host's `docker compose` and the user's dev-shell mount.
#     Build manually first via `make` in your dev shell.

set -u
cd "$(dirname "$0")/.."

# Output log per phase so we can show the tail on failure.
LOG_DIR="$(mktemp -d -t full-test-XXXXXX)"

# Parallel arrays — bash 3.2 compatible (no associative arrays).
PHASE_NAMES=()
PHASE_RESULTS=()
PHASE_DURATIONS=()

run_phase() {
    local name="$1"
    shift
    local log="$LOG_DIR/$name.log"

    echo
    echo "===================================================================="
    echo "[$name]"
    echo "  cmd: $*"
    echo "  log: $log"
    echo "===================================================================="

    local t0
    t0=$(date +%s)
    PHASE_NAMES+=("$name")

    local result
    if "$@" > "$log" 2>&1; then
        result=PASS
    else
        result=FAIL
        echo "  --- last 25 lines of log ---"
        tail -25 "$log" | sed 's/^/    /'
    fi

    local dt=$(($(date +%s) - t0))
    echo "[$name] $result (${dt}s)"
    PHASE_RESULTS+=("$result")
    PHASE_DURATIONS+=("$dt")
}

# Helper: run a command inside cs2620:latest via compose. Bind-mounted to
# /work/pset4-active. No-op container — destroyed on exit.
in_docker() {
    docker compose run --rm --entrypoint='' replica-0 "$@"
}

# === Layer 1: Unit tests ($0, seconds) ===
# Run the pre-built binary directly to skip cmake's bind-mount path check.
run_phase "tm-tests" \
    in_docker /work/pset4-active/build/tm-tests

run_phase "tm-cli-tests" \
    python3 tests/test_tm_cli.py

# === Layer 2: Sim tests ($0, deterministic, no real sockets) ===
run_phase "tm-paxos-tests-single" \
    in_docker /work/pset4-active/build/tm-paxos-tests

if [ "${SIM_SWEEP:-0}" = "1" ]; then
    run_phase "tm-paxos-sweep" \
        in_docker bash -c 'cd /work/pset4-active && scripts/run-paxos-tests.sh'
fi

# === Layer 3a: Smoke against an ephemeral single-instance tm-server ===
# Tests the non-paxos POC code path: SM correctness via real HTTP.
run_smoke_single() {
    in_docker bash -c '
        set -e
        cd /work/pset4-active
        rm -f /tmp/full-test-tm.log
        ./build/tm-server -V -p 8080 -L /tmp/full-test-tm.log >/tmp/full-test-tm.stderr 2>&1 &
        TM_PID=$!
        trap "kill $TM_PID 2>/dev/null || true" EXIT
        for i in $(seq 1 20); do
            curl -fsS http://localhost:8080/dump >/dev/null 2>&1 && break
            sleep 0.2
        done
        TM_URL=http://localhost:8080 scripts/smoke.sh
    '
}
run_phase "smoke-single" run_smoke_single

run_smoke_lease() {
    in_docker bash -c '
        set -e
        cd /work/pset4-active
        rm -f /tmp/full-test-tm.log
        ./build/tm-server -V -p 8080 -L /tmp/full-test-tm.log >/tmp/full-test-tm.stderr 2>&1 &
        TM_PID=$!
        trap "kill $TM_PID 2>/dev/null || true" EXIT
        for i in $(seq 1 20); do
            curl -fsS http://localhost:8080/dump >/dev/null 2>&1 && break
            sleep 0.2
        done
        TM_URL=http://localhost:8080 scripts/smoke-lease.sh
    '
}
run_phase "smoke-lease" run_smoke_lease

# === Layer 3b: Real-TCP host processes (3 tm-servers on localhost) ===
if [ "${LOCAL_TCP:-0}" = "1" ]; then
    run_phase "local-tcp-sweep" \
        in_docker bash -c 'cd /work/pset4-active && scripts/local-tcp.sh sweep'
fi

# === Layer 4: Docker / full-stack failure sweep ($0, the strongest check) ===
ITERS_DEFAULT="${ITERS:-3}"
run_phase "docker-failure-sweep" \
    env ITERS="$ITERS_DEFAULT" scripts/docker-failure-sweep.sh sweep

# === Summary ===
echo
echo "===================================================================="
echo "Summary"
echo "===================================================================="
overall_fail=0
total_dt=0
i=0
while [ "$i" -lt "${#PHASE_NAMES[@]}" ]; do
    name="${PHASE_NAMES[$i]}"
    result="${PHASE_RESULTS[$i]}"
    dt="${PHASE_DURATIONS[$i]}"
    printf "  %-28s %-4s  %4ss\n" "$name" "$result" "$dt"
    [ "$result" = "FAIL" ] && overall_fail=1
    total_dt=$((total_dt + dt))
    i=$((i + 1))
done
echo "  ----------------------------"
printf "  %-28s        %4ss\n" "total" "$total_dt"
echo

if [ "$overall_fail" -eq 0 ]; then
    echo "All phases passed. Free regression clean."
    echo "Per-phase logs in: $LOG_DIR"
    exit 0
else
    echo "Some phases failed."
    echo "Per-phase logs in: $LOG_DIR"
    exit 1
fi
