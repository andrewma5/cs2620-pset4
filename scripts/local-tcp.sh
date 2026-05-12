#!/usr/bin/env bash
# scripts/local-tcp.sh — local 3-replica TCP test harness for pset4 paxos.
#
# All sweep iterations test real tm-server processes communicating over real
# TCP sockets — the production code path, not the in-process sim. Two
# invariants checked per iteration:
#   (1) {id, status, owner_agent, owner_token} agreement across surviving
#       replicas via /dump (SM-state invariant).
#   (2) Surviving replicas' decision logs replay to byte-identical
#       snapshots via tm-replay (real-paxos log-agreement invariant —
#       catches divergence in the path through paxos even when (1) holds).
#
# Subcommands:
#   start          launch 3 tm-server replicas, leave running for inspection
#   stop           pkill all tm-server processes
#   sweep          run automated failure-injection sweep (ITERS=50 default)
#
# Scenarios (run in `sweep`):
#   baseline       no injection — control, proves no flakiness on its own
#   kill_leader    SIGKILL replica 0, drive smoke against a survivor (with
#                  HTTP redirect to the newly-elected leader)
#   pause_resume   SIGSTOP replica 2 (frozen node), drive smoke through
#                  quorum 0+1, SIGCONT, verify replica 2 catches up
#
# Usage:
#   scripts/local-tcp.sh start
#   scripts/local-tcp.sh sweep              # 50 iters/scenario, ~10 min
#   ITERS=100 scripts/local-tcp.sh sweep    # heavier
#   scripts/local-tcp.sh stop

set -u
PEERS=localhost:9000,localhost:9001,localhost:9002
HTTP_PEERS=http://localhost:8080,http://localhost:8081,http://localhost:8082

start_replicas() {
    pkill -f tm-server 2>/dev/null || true
    mkdir -p logs
    rm -f logs/tm-*.log logs/tm-*.stderr
    sleep 1  # release TIME_WAIT'd sockets before rebind
    ./build/tm-server --replica-index=0 --peers=$PEERS --http-peers=$HTTP_PEERS -p 8080 -L logs/tm-0.log >logs/tm-0.stderr 2>&1 &
    ./build/tm-server --replica-index=1 --peers=$PEERS --http-peers=$HTTP_PEERS -p 8081 -L logs/tm-1.log >logs/tm-1.stderr 2>&1 &
    ./build/tm-server --replica-index=2 --peers=$PEERS --http-peers=$HTTP_PEERS -p 8082 -L logs/tm-2.log >logs/tm-2.stderr 2>&1 &
    sleep 2  # connect-loops + initial election
}

stop_replicas() {
    pkill -f tm-server 2>/dev/null || true
}

# Run smoke against URL; 0 = pass.
run_smoke() {
    local url="$1"
    TM_URL="$url" scripts/smoke.sh > /tmp/smoke-out.txt 2>&1
    grep -q "OK: smoke test passed" /tmp/smoke-out.txt
}

# Per-task projection of a replica's /dump for strict comparison.
task_state() {
    local port="$1"
    curl -s --max-time 2 "http://localhost:$port/dump" 2>/dev/null \
        | jq -cS '.tasks | sort_by(.id) | map({id, status, owner_agent, owner_token})'
}

# All listed ports agree on task_state. 0 = agreed.
check_strict() {
    local first
    first=$(task_state "$1")
    [ -n "$first" ] && [ "$first" != "null" ] || return 1
    shift
    for port in "$@"; do
        local other
        other=$(task_state "$port")
        [ "$first" = "$other" ] || return 1
    done
    return 0
}

# Real-paxos log-replay invariant: all listed replica indices replay to
# byte-identical snapshots. Catches divergence in the decision log even
# when /dump agrees (i.e. SM state matches but the path through paxos
# differed). 0 = agreed.
check_logs() {
    local first_idx="$1"
    local first_snap="/tmp/local-tcp-snap-${first_idx}.json"
    ./build/tm-replay -L "logs/tm-${first_idx}.log" -o "$first_snap" >/dev/null 2>&1 || return 1
    shift
    for idx in "$@"; do
        local snap="/tmp/local-tcp-snap-${idx}.json"
        ./build/tm-replay -L "logs/tm-${idx}.log" -o "$snap" >/dev/null 2>&1 || return 1
        diff -q "$first_snap" "$snap" >/dev/null 2>&1 || return 1
    done
    return 0
}

# === Scenario: baseline ===
# No failure injection. Two smoke runs back-to-back, then strict invariant on
# all 3 replicas. Catches flakes that happen without any injection.
iter_baseline() {
    start_replicas
    run_smoke http://localhost:8080 || { stop_replicas; return 1; }
    run_smoke http://localhost:8080 || { stop_replicas; return 1; }
    sleep 1
    check_strict 8080 8081 8082 || { stop_replicas; return 1; }
    check_logs 0 1 2 || { stop_replicas; return 1; }
    stop_replicas
    return 0
}

# === Scenario: kill_leader ===
# SIGKILL replica 0 → election → smoke against replica 1 (HTTP 307-redirects
# to new leader) → strict invariant on surviving replicas {1, 2}.
iter_kill_leader() {
    start_replicas
    run_smoke http://localhost:8080 || { stop_replicas; return 1; }
    pkill -9 -f "replica-index=0" 2>/dev/null
    sleep 2  # election
    run_smoke http://localhost:8081 || { stop_replicas; return 1; }
    sleep 1  # let final decided_slot propagate via 100ms retransmit
    check_strict 8081 8082 || { stop_replicas; return 1; }
    # Only survivors' logs are expected to agree — replica 0 is dead and its
    # log is frozen at whatever it wrote before SIGKILL (hard-rule #5: no
    # state transfer).
    check_logs 1 2 || { stop_replicas; return 1; }
    stop_replicas
    return 0
}

# === Scenario: pause_resume ===
# SIGSTOP replica 2 → smoke succeeds via quorum 0+1 → SIGCONT → replica 2
# catches up → strict invariant on all {0, 1, 2}.
iter_pause_resume() {
    start_replicas
    run_smoke http://localhost:8080 || { stop_replicas; return 1; }
    local pid
    pid=$(pgrep -f "replica-index=2" | head -1)
    kill -STOP "$pid" 2>/dev/null
    sleep 0.3
    run_smoke http://localhost:8080 || {
        kill -CONT "$pid" 2>/dev/null
        stop_replicas
        return 1
    }
    kill -CONT "$pid" 2>/dev/null
    sleep 3  # catch-up
    check_strict 8080 8081 8082 || { stop_replicas; return 1; }
    check_logs 0 1 2 || { stop_replicas; return 1; }
    stop_replicas
    return 0
}

# === Subcommands ===

cmd_start() {
    start_replicas
    echo "3 replicas up:"
    echo "  replica 0: HTTP :8080, paxos :9000, logs/tm-0.{log,stderr}"
    echo "  replica 1: HTTP :8081, paxos :9001, logs/tm-1.{log,stderr}"
    echo "  replica 2: HTTP :8082, paxos :9002, logs/tm-2.{log,stderr}"
    echo
    echo "drive with: TM_URL=http://localhost:8080 scripts/smoke.sh"
    echo "stop with:  scripts/local-tcp.sh stop"
}

cmd_stop() {
    stop_replicas
    echo "stopped"
}

cmd_sweep() {
    local iters="${ITERS:-50}"
    echo "TCP multi-replica failure sweep: $iters iterations per scenario"
    echo "  strict invariant: {id, status, owner_agent, owner_token}"
    echo

    local overall_fail=0
    for scenario in baseline kill_leader pause_resume; do
        local pass=0
        local fail=0
        for i in $(seq 1 "$iters"); do
            if iter_${scenario}; then
                pass=$((pass + 1))
            else
                fail=$((fail + 1))
                echo "  [$scenario] iter $i: FAIL"
                if [ "$fail" -le 2 ]; then
                    echo "    --- last smoke output ---"
                    tail -10 /tmp/smoke-out.txt 2>/dev/null | sed 's/^/      /'
                    for stderr in logs/tm-0.stderr logs/tm-1.stderr logs/tm-2.stderr; do
                        if [ -s "$stderr" ]; then
                            echo "    --- $stderr ---"
                            tail -8 "$stderr" | sed 's/^/      /'
                        fi
                    done
                fi
            fi
            printf "  [%s] %d/%d (pass=%d fail=%d)\r" "$scenario" "$i" "$iters" "$pass" "$fail"
        done
        echo
        if [ "$fail" -ne 0 ]; then overall_fail=1; fi
    done

    echo
    if [ "$overall_fail" -eq 0 ]; then
        echo "All scenarios passed at $iters iterations each."
        exit 0
    else
        echo "Some iterations failed."
        exit 1
    fi
}

case "${1:-}" in
    start) cmd_start ;;
    stop)  cmd_stop ;;
    sweep) cmd_sweep ;;
    *)
        echo "Usage: $0 {start|stop|sweep}"
        echo "  start  — launch 3 tm-server replicas for manual inspection"
        echo "  stop   — kill all tm-server processes"
        echo "  sweep  — run automated failure-injection sweep (ITERS=50)"
        exit 1
        ;;
esac
