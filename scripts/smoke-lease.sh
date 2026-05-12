#!/usr/bin/env bash
# pset4/scripts/smoke-lease.sh - integration test for lazy lease expiry.
# Assumes tm-server is running.
set -euo pipefail

TM_URL="${TM_URL:-http://localhost:8080}"

post() { curl -fsS -X POST "$TM_URL$1" -H 'Content-Type: application/json' -d "$2"; }

echo "==> create + claim by agent-a"
PLAN_ID=$(post /task_create '{
  "agent_id":"human","serial":100,
  "spec":{"type":"plan","title":"lease","prompt":"x"}
}' | jq -r .task_id)
CLAIM=$(post /task_claim '{"agent_id":"agent-a","serial":101}')
test "$(echo "$CLAIM" | jq -r .task_id)" = "$PLAN_ID"

echo "==> agent-b tries to claim immediately: expect none"
RES=$(post /task_claim '{"agent_id":"agent-b","serial":102}')
test "$(echo "$RES" | jq -r .none)" = "true"

echo "==> sleep 50s to let agent-a's lease expire"
sleep 50

echo "==> agent-b tries again: should now take over"
RES=$(post /task_claim '{"agent_id":"agent-b","serial":103}')
test "$(echo "$RES" | jq -r .task_id)" = "$PLAN_ID"
echo "    new owner is agent-b"

echo "==> verify dump"
curl -fsS "$TM_URL/dump" | jq '.tasks[] | select(.id=="'$PLAN_ID'") | {owner_agent,status}'

echo "OK: lease-expiry smoke passed"
