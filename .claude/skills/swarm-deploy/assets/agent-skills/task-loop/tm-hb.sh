#!/bin/bash
# Pings /task_heartbeat every 10s. Runs forever; the parent (Claude
# task-loop) is responsible for KillShell after the task finishes.
#
# Required env:
#   TM_URL      task-manager base URL
#   AGENT_ID    this agent's id
#   TASK_ID     the claimed task's id
#   TOK         the fencing token from the claim
set -u
: "${TM_URL:?missing}" "${AGENT_ID:?missing}" "${TASK_ID:?missing}" "${TOK:?missing}"

while true; do
    curl -fsSL -X POST "$TM_URL/task_heartbeat" \
        -H 'Content-Type: application/json' \
        -d "$(jq -n --arg aid "$AGENT_ID" --argjson s $((RANDOM*RANDOM)) \
              --arg id "$TASK_ID" --argjson tok "$TOK" \
              '{agent_id:$aid,serial:$s,id:$id,token:$tok}')" \
        > /dev/null 2>&1 || true
    sleep 10
done
