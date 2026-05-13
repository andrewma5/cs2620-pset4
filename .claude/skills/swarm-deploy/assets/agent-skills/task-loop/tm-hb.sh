#!/bin/bash
# Pings /task_heartbeat every 10s via the unified `tm` CLI. Runs
# forever; the parent (Claude task-loop) is responsible for KillShell
# after the task finishes.
#
# All replica-failover, JSON construction, and "token" field-name
# correctness live in the `tm` CLI. This wrapper exists only because
# Claude Code's Bash tool treats `run_in_background: true` as the
# natural way to manage a long-running poll loop.
#
# Required env:
#   TM_URL_LIST or TM_URL   (read by tm CLI)
#   AGENT_ID                (read by tm CLI)
#   TASK_ID                 task id to heartbeat
#   TOK                     fencing token from the original claim
#   SKILL_DIR               directory containing the tm CLI
set -u
: "${AGENT_ID:?missing}" "${TASK_ID:?missing}" "${TOK:?missing}" "${SKILL_DIR:?missing}"

# Pick a working python launcher. On Windows, "python3" usually resolves
# to a Microsoft Store stub that fails. `py -3` is the real launcher.
if command -v py >/dev/null 2>&1 && py -3 --version >/dev/null 2>&1; then
    PY="py -3"
elif command -v python3 >/dev/null 2>&1 && python3 --version >/dev/null 2>&1; then
    PY="python3"
else
    echo "tm-hb: no python launcher (tried 'py -3' and 'python3')" >&2
    exit 1
fi

while true; do
    $PY "$SKILL_DIR/tm" hb-once --id "$TASK_ID" --token "$TOK" \
        > /dev/null 2>&1 || true
    sleep 10
done
