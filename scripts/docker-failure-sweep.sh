#!/usr/bin/env bash
# scripts/docker-failure-sweep.sh — automated failure-injection sweep at
# the docker-compose layer. Mirrors scripts/local-tcp.sh but exercises the
# full real-world stack: 3 containers, compose networking, host port maps,
# embedded DNS. Catches plumbing bugs that bare-metal local-tcp can't
# (e.g. the `--alias` DNS quirk on partition/reconnect).
#
# Each iteration:
#   1. tear down + fresh demo-init (clean state, seed task posted)
#   2. start synthetic-agent.sh in background with WORK_SLEEP=3 so the
#      run lasts ~15-20s — long enough to inject failures mid-flight
#   3. sleep, inject failure on replica-1 (always replica-1, replica-0
#      stays alive so we can `docker compose exec` it to run tm-replay)
#   4. for recovery scenarios: sleep, then recover, sleep for catch-up
#   5. wait for synth to finish
#   6. verify (a) /dump SM-state agreement on relevant ports and
#             (b) decision-log replay agreement on relevant indices
#
# Scenarios:
#   baseline             no injection — control
#   pause_resume         SIGSTOP replica-1 mid-run, SIGCONT, verify 3-way convergence
#   kill                 SIGKILL replica-1 (survival story; survivors agree)
#   partition_reconnect  docker network disconnect, reconnect with --alias,
#                        verify 3-way convergence
#
# Usage:
#   scripts/docker-failure-sweep.sh sweep              # 5 iters × 4 scenarios
#   ITERS=10 scripts/docker-failure-sweep.sh sweep     # heavier
#   scripts/docker-failure-sweep.sh <scenario>         # one shot
#
# Cost: ~25-30s per iteration. Default sweep = ~10 min.

set -u
cd "$(dirname "$0")/.."

# Always exec tm-replay in replica-0 (it survives all our scenarios since
# we only ever inject on replica-1). The bind-mount means it sees the
# same logs/ directory the failing replicas write to.
REPLAY_HOST=pset4demo-replica-0-1

# Per-task projection of a replica's /dump for strict comparison.
task_state() {
    local port="$1"
    curl -s --max-time 3 "http://localhost:$port/dump" 2>/dev/null \
        | jq -cS '.tasks | sort_by(.id) | map({id, status, owner_agent, owner_token})'
}

# Strict SM-state agreement across listed ports. 0 = agreed.
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

# Real-paxos log-agreement: replay each listed replica index's log and
# diff the resulting snapshots inside REPLAY_HOST (Linux container).
# 0 = byte-identical.
check_logs() {
    local first_idx="$1"
    local first_snap="/tmp/sweep-snap-${first_idx}.json"
    docker exec "$REPLAY_HOST" \
        ./build/tm-replay -L "logs/tm-${first_idx}.log" -o "$first_snap" \
        >/dev/null 2>&1 || return 1
    shift
    for idx in "$@"; do
        local snap="/tmp/sweep-snap-${idx}.json"
        docker exec "$REPLAY_HOST" \
            ./build/tm-replay -L "logs/tm-${idx}.log" -o "$snap" \
            >/dev/null 2>&1 || return 1
        docker exec "$REPLAY_HOST" \
            diff -q "$first_snap" "$snap" >/dev/null 2>&1 || return 1
    done
    return 0
}

# Wait until replica-1's log catches up to replica-0 (or timeout).
# Default raised from 15s → 30s after Fix D (tcp_transport reconnect on
# write-timeout): under recovery scenarios, the close-reconnect churn
# sometimes pushes the last-slot catch-up past 15s. See docs/BUGS.md.
wait_for_catchup() {
    local timeout="${1:-30}"
    for _ in $(seq 1 "$timeout"); do
        local n0 n1
        n0=$(wc -l < logs/tm-0.log 2>/dev/null || echo 0)
        n1=$(wc -l < logs/tm-1.log 2>/dev/null || echo 0)
        [ "$n1" -ge "$n0" ] && [ "$n1" -gt 1 ] && return 0
        sleep 1
    done
    return 1
}

reset_demo() {
    make -C demo demo-down >/dev/null 2>&1 || true
    rm -rf demo/work logs
    make -C demo demo-init >/dev/null 2>&1
}

# === Scenario: baseline ===
# No injection — proves docker plumbing isn't flaky on its own.
iter_baseline() {
    reset_demo || return 1
    WORK_SLEEP=3 scripts/synthetic-agent.sh >/tmp/sweep-synth.out 2>&1 &
    local synth=$!
    wait "$synth"
    # Settle: synth exits as soon as /dump shows all done on its pinned
    # replica, but the leader's final decided_slot may still be propagating
    # to followers via the 100ms retransmit. ~1s margin avoids that race.
    sleep 2
    check_strict 8081 8082 8083 || return 1
    check_logs 0 1 2 || return 1
    return 0
}

# === Scenario: pause_resume ===
# Mid-flight SIGSTOP on replica-1, SIGCONT after ~5s, verify 3-way convergence.
iter_pause_resume() {
    reset_demo || return 1
    WORK_SLEEP=3 scripts/synthetic-agent.sh >/tmp/sweep-synth.out 2>&1 &
    local synth=$!
    sleep 2
    make -C demo demo-pause-1 >/dev/null 2>&1 || { kill "$synth" 2>/dev/null; return 1; }
    sleep 5
    make -C demo demo-resume-1 >/dev/null 2>&1 || { kill "$synth" 2>/dev/null; return 1; }
    wait "$synth"
    wait_for_catchup 30 || return 1
    check_strict 8081 8082 8083 || return 1
    check_logs 0 1 2 || return 1
    return 0
}

# === Scenario: kill ===
# Mid-flight SIGKILL replica-1. Replica-1's log freezes (hard-rule #5: no
# state transfer). Survivors must still finish work and agree.
iter_kill() {
    reset_demo || return 1
    WORK_SLEEP=3 scripts/synthetic-agent.sh >/tmp/sweep-synth.out 2>&1 &
    local synth=$!
    sleep 2
    make -C demo demo-kill-1 >/dev/null 2>&1 || { kill "$synth" 2>/dev/null; return 1; }
    wait "$synth"
    # Settle: synth exits as soon as /dump shows all done on its pinned
    # replica, but the leader's final decided_slot may still be propagating
    # to followers via the 100ms retransmit. ~1s margin avoids that race.
    sleep 2
    check_strict 8081 8083 || return 1
    check_logs 0 2 || return 1
    return 0
}

# === Scenario: partition_reconnect ===
# Mid-flight `docker network disconnect`, then `docker network connect
# --alias replica-1`. Tests the DNS-alias quirk and the TCP reconnect path.
iter_partition_reconnect() {
    reset_demo || return 1
    WORK_SLEEP=3 scripts/synthetic-agent.sh >/tmp/sweep-synth.out 2>&1 &
    local synth=$!
    sleep 2
    make -C demo demo-partition-1 >/dev/null 2>&1 || { kill "$synth" 2>/dev/null; return 1; }
    sleep 5
    make -C demo demo-reconnect-1 >/dev/null 2>&1 || { kill "$synth" 2>/dev/null; return 1; }
    wait "$synth"
    wait_for_catchup 30 || return 1
    check_strict 8081 8082 8083 || return 1
    check_logs 0 1 2 || return 1
    return 0
}

cmd_sweep() {
    local iters="${ITERS:-5}"
    echo "Docker failure-injection sweep: $iters iterations per scenario"
    echo "  invariants: /dump agreement (SM state) + tm-replay byte-identity"
    echo

    local overall_fail=0
    for scenario in baseline pause_resume kill partition_reconnect; do
        local pass=0
        local fail=0
        for i in $(seq 1 "$iters"); do
            if iter_${scenario}; then
                pass=$((pass + 1))
            else
                fail=$((fail + 1))
                echo "  [$scenario] iter $i: FAIL"
                if [ "$fail" -le 2 ]; then
                    echo "    --- synth tail ---"
                    tail -10 /tmp/sweep-synth.out 2>/dev/null | sed 's/^/      /'
                    echo "    --- log line counts ---"
                    wc -l logs/tm-*.log 2>/dev/null | sed 's/^/      /'

                    # Forensic capture: save compose stderr (paxos events) +
                    # live /dump from each replica + last few lines of each
                    # decision log. Lets us see post-failure WHY replica-1 is
                    # behind (election rounds, applied_slot, etc).
                    local fdir
                    fdir="demo/sweep-failures/${scenario}-iter${i}-$(date +%s)"
                    mkdir -p "$fdir"
                    docker compose logs --no-color > "$fdir/compose-stderr.log" 2>&1 || true
                    for p in 8081 8082 8083; do
                        curl -s --max-time 3 "http://localhost:$p/dump" \
                            > "$fdir/dump-port-$p.json" 2>/dev/null || true
                    done
                    cp logs/tm-0.log logs/tm-1.log logs/tm-2.log "$fdir/" 2>/dev/null || true
                    echo "    --- forensics captured to $fdir ---"
                fi
            fi
            printf "  [%s] %d/%d (pass=%d fail=%d)\r" "$scenario" "$i" "$iters" "$pass" "$fail"
        done
        echo
        if [ "$fail" -ne 0 ]; then overall_fail=1; fi
    done

    # Final cleanup so we don't leave containers around.
    make -C demo demo-down >/dev/null 2>&1 || true

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
    baseline)             iter_baseline             && echo PASS || echo FAIL ;;
    pause_resume)         iter_pause_resume         && echo PASS || echo FAIL ;;
    kill)                 iter_kill                 && echo PASS || echo FAIL ;;
    partition_reconnect)  iter_partition_reconnect  && echo PASS || echo FAIL ;;
    sweep) cmd_sweep ;;
    *)
        echo "Usage: $0 {baseline|pause_resume|kill|partition_reconnect|sweep}"
        echo "  sweep — run all 4 scenarios, ITERS=5 per scenario (default)"
        exit 1
        ;;
esac
