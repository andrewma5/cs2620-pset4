#!/bin/bash
# Polls /task_claim every 10s via the unified `tm` CLI; exits the
# moment a task is claimed or the swarm halts. Stashes the claim
# response JSON to $CLAIM_OUT.
#
# Required env:
#   TM_URL_LIST or TM_URL   (read by tm CLI)
#   AGENT_ID                (read by tm CLI)
#   CLAIM_OUT               path to write the claim response JSON
#   SKILL_DIR               directory containing the tm CLI
#
# Output line on exit: "GOT_TASK" or "HALTED".
set -u
: "${AGENT_ID:?missing}" "${CLAIM_OUT:?missing}" "${SKILL_DIR:?missing}"

# Pick a working python launcher (see tm-hb.sh for rationale).
if command -v py >/dev/null 2>&1 && py -3 --version >/dev/null 2>&1; then
    PY="py -3"
elif command -v python3 >/dev/null 2>&1 && python3 --version >/dev/null 2>&1; then
    PY="python3"
else
    echo "tm-wait: no python launcher (tried 'py -3' and 'python3')" >&2
    exit 1
fi

while true; do
    RESP=$($PY "$SKILL_DIR/tm" claim 2>/dev/null) || { sleep 10; continue; }
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
