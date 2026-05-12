#!/bin/bash
# Polls /task_claim every 10s; exits the moment a task is claimed
# or the swarm halts. Stash the claim response JSON to $CLAIM_OUT.
#
# Required env:
#   TM_URL      task-manager base URL
#   AGENT_ID    this agent's id
#   CLAIM_OUT   path to write the claim response JSON
#
# Output line on exit: "GOT_TASK" or "HALTED".
set -u
: "${TM_URL:?missing}" "${AGENT_ID:?missing}" "${CLAIM_OUT:?missing}"

while true; do
    RESP=$(curl -fsSL -X POST "$TM_URL/task_claim" \
        -H 'Content-Type: application/json' \
        -d "$(jq -n --arg aid "$AGENT_ID" --argjson s $((RANDOM*RANDOM)) \
              '{agent_id:$aid,serial:$s}')" 2>/dev/null) || { sleep 10; continue; }
    HALTED=$(echo "$RESP" | jq -r '.halted // false' 2>/dev/null)
    if [ "$HALTED" = "true" ]; then
        echo "$RESP" > "$CLAIM_OUT"
        echo "HALTED"
        exit 0
    fi
    NONE=$(echo "$RESP" | jq -r '.none // false' 2>/dev/null)
    if [ "$NONE" != "true" ]; then
        echo "$RESP" > "$CLAIM_OUT"
        echo "GOT_TASK"
        exit 0
    fi
    sleep 10
done
