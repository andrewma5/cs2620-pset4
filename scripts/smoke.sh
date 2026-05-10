#!/usr/bin/env bash
# pset4/scripts/smoke.sh - end-to-end HTTP smoke test of tm-server.
# Assumes tm-server is running on localhost:8080 (or set $TM_URL).
set -euo pipefail

TM_URL="${TM_URL:-http://localhost:8080}"

post() {
    curl -fsS -X POST "$TM_URL$1" -H 'Content-Type: application/json' -d "$2"
}

echo "==> creating plan task"
PLAN_ID=$(post /task_create '{
  "agent_id":"human","serial":1,
  "spec":{"type":"plan","title":"build calc","prompt":"build a CLI calculator"}
}' | jq -r .task_id)
echo "    PLAN_ID=$PLAN_ID"

echo "==> agent-a claims plan"
CLAIM=$(post /task_claim '{"agent_id":"agent-a","serial":10}')
TID=$(echo "$CLAIM" | jq -r .task_id)
TOK=$(echo "$CLAIM" | jq -r .fencing_token)
test "$TID" = "$PLAN_ID"
echo "    claimed $TID with token $TOK"

echo "==> heartbeat"
post /task_heartbeat "$(jq -n --arg id "$TID" --argjson tok "$TOK" \
  '{agent_id:"agent-a",serial:11,id:$id,token:$tok}')" | jq .

echo "==> complete plan with one implement child"
DONE=$(post /task_complete "$(jq -n --arg id "$TID" --argjson tok "$TOK" \
  '{agent_id:"agent-a",serial:12,id:$id,token:$tok,
    result_summary:"decomposed",
    new_children:[{type:"implement",title:"impl-1",prompt:"do thing",
                   branch_base:"main"}]}')")
CHILD=$(echo "$DONE" | jq -r '.child_ids[0]')
echo "    child=$CHILD"

echo "==> agent-b claims the implement task"
CLAIM2=$(post /task_claim '{"agent_id":"agent-b","serial":20,"prefer_type":"implement"}')
test "$(echo "$CLAIM2" | jq -r .task_id)" = "$CHILD"
TOK2=$(echo "$CLAIM2" | jq -r .fencing_token)

echo "==> complete implement; expect synthesized merge"
DONE2=$(post /task_complete "$(jq -n --arg id "$CHILD" --argjson tok "$TOK2" \
  '{agent_id:"agent-b",serial:21,id:$id,token:$tok,
    result_summary:"shipped",result_branch:"abc1234"}')")
MERGE_ID=$(echo "$DONE2" | jq -r .merge_task_id)
test -n "$MERGE_ID" && test "$MERGE_ID" != "null"
echo "    merge=$MERGE_ID"

echo "==> agent-c claims the merge task"
CLAIM3=$(post /task_claim '{"agent_id":"agent-c","serial":30,"prefer_type":"merge"}')
test "$(echo "$CLAIM3" | jq -r .task_id)" = "$MERGE_ID"
TOK3=$(echo "$CLAIM3" | jq -r .fencing_token)

echo "==> agent-c acquires + releases main lock"
LOCK_TOK=$(post /main_lock_acquire "$(jq -n --arg mt "$MERGE_ID" --argjson mtok "$TOK3" \
  '{agent_id:"agent-c",serial:31,merge_task:$mt,merge_token:$mtok}')" | jq -r .lock_token)
post /main_lock_release "$(jq -n --argjson lt "$LOCK_TOK" \
  '{agent_id:"agent-c",serial:32,lock_token:$lt}')" | jq .

echo "==> complete merge"
post /task_complete "$(jq -n --arg id "$MERGE_ID" --argjson tok "$TOK3" \
  '{agent_id:"agent-c",serial:33,id:$id,token:$tok,
    result_summary:"merged & pushed"}')" | jq .

echo "==> final state"
curl -fsS "$TM_URL/dump" | jq '.tasks | map({id,status,owner_agent})'

echo "OK: smoke test passed"
