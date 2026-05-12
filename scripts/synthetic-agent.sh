#!/usr/bin/env bash
# scripts/synthetic-agent.sh — non-Claude bash agent for the docker-compose
# demo dry run. Three of these run in parallel against 3 replica URLs and
# drive plan → implement → merge through the paxos quorum, all over HTTP.
#
# Free; deterministic-ish (depends on race timing). Catches integration
# bugs in the compose deployment before any $ is spent on real-Claude runs.
#
# Usage:
#   scripts/synthetic-agent.sh                       # 3 agents, default URLs
#   scripts/synthetic-agent.sh URL1 URL2 URL3        # custom URLs
#
# Each agent loops claim→work→complete until /dump reports all tasks done
# (or 60s timeout). On exit, the script prints final task state.

set -u

URL1="${1:-http://localhost:8081}"
URL2="${2:-http://localhost:8082}"
URL3="${3:-http://localhost:8083}"
URLS=("$URL1" "$URL2" "$URL3")

# One synthetic agent. Args: $1 = agent_id, $2 = tm_url.
agent() {
    local AID="$1"
    local URL="$2"
    local serial=0

    while true; do
        # Stop when all tasks in /dump are done.
        local statuses
        statuses=$(curl -fsSL --max-time 3 "$URL/dump" 2>/dev/null \
            | jq -r '.tasks | map(.status) | unique | join(",")' 2>/dev/null)
        if [ "$statuses" = "done" ]; then break; fi

        # Claim a task.
        serial=$((serial + 1))
        local claim
        claim=$(curl -fsSL --max-time 3 -X POST "$URL/task_claim" \
            -H 'Content-Type: application/json' \
            -d "{\"agent_id\":\"$AID\",\"serial\":$serial}" 2>/dev/null)
        if [ -z "$claim" ]; then sleep 1; continue; fi
        if [ "$(echo "$claim" | jq -r '.none')" = "true" ]; then
            sleep 1; continue
        fi

        local task_id token type
        task_id=$(echo "$claim" | jq -r '.task_id')
        token=$(echo "$claim" | jq -r '.fencing_token')
        type=$(echo "$claim" | jq -r '.spec.type')

        echo "[$AID @ $URL] claimed $task_id ($type), token=$token"

        # Simulate work. WORK_SLEEP lets the docker-failure-sweep slow agents
        # down so it has time to inject failures mid-run.
        sleep "${WORK_SLEEP:-1}"

        # For merge tasks, acquire + release the main_lock.
        if [ "$type" = "merge" ]; then
            serial=$((serial + 1))
            local lock
            lock=$(curl -fsSL --max-time 3 -X POST "$URL/main_lock_acquire" \
                -H 'Content-Type: application/json' \
                -d "{\"agent_id\":\"$AID\",\"serial\":$serial,\"merge_task\":\"$task_id\",\"merge_token\":$token}" 2>/dev/null)
            local lock_token
            lock_token=$(echo "$lock" | jq -r '.lock_token // 0')
            if [ "$lock_token" != "0" ]; then
                serial=$((serial + 1))
                curl -fsSL --max-time 3 -X POST "$URL/main_lock_release" \
                    -H 'Content-Type: application/json' \
                    -d "{\"agent_id\":\"$AID\",\"serial\":$serial,\"lock_token\":$lock_token}" >/dev/null 2>&1
            fi
        fi

        # Complete.
        serial=$((serial + 1))
        local complete_body
        if [ "$type" = "plan" ]; then
            # Plans synthesize 1 implement child (smaller than seed-task.json
            # suggests, but enough to exercise the lifecycle).
            complete_body=$(jq -n \
                --arg id "$task_id" --argjson tok "$token" --arg aid "$AID" --argjson ser "$serial" \
                '{agent_id:$aid,serial:$ser,id:$id,token:$tok,result_summary:"plan done",
                  new_children:[{type:"implement",title:"impl",prompt:"do thing",branch_base:"main"}]}')
        elif [ "$type" = "implement" ]; then
            # Implement provides result_branch → SM synthesizes a merge task.
            complete_body=$(jq -n \
                --arg id "$task_id" --argjson tok "$token" --arg aid "$AID" --argjson ser "$serial" \
                '{agent_id:$aid,serial:$ser,id:$id,token:$tok,result_summary:"impl done",
                  result_branch:"abc1234"}')
        else
            complete_body=$(jq -n \
                --arg id "$task_id" --argjson tok "$token" --arg aid "$AID" --argjson ser "$serial" \
                '{agent_id:$aid,serial:$ser,id:$id,token:$tok,result_summary:"merge done"}')
        fi
        curl -fsSL --max-time 3 -X POST "$URL/task_complete" \
            -H 'Content-Type: application/json' \
            -d "$complete_body" >/dev/null 2>&1
        echo "[$AID @ $URL] completed $task_id"
    done
    echo "[$AID @ $URL] all tasks done — exiting"
}

# Run 3 agents in parallel, each pinned to a different replica.
agent agent-1 "${URLS[0]}" &
A1_PID=$!
agent agent-2 "${URLS[1]}" &
A2_PID=$!
agent agent-3 "${URLS[2]}" &
A3_PID=$!

# Timeout in case something hangs.
(sleep 60 && kill $A1_PID $A2_PID $A3_PID 2>/dev/null) &
TIMEOUT_PID=$!

wait $A1_PID $A2_PID $A3_PID 2>/dev/null
kill $TIMEOUT_PID 2>/dev/null || true

echo
echo "=== Final task state on each replica ==="
for url in "${URLS[@]}"; do
    echo "--- $url ---"
    curl -fsSL --max-time 3 "$url/dump" 2>/dev/null | jq '.tasks | map({id, status, owner_agent})' || echo "  (unreachable)"
done
