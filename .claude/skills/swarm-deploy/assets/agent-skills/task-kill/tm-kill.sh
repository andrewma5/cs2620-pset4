#!/bin/bash
# tm-kill.sh — canonical macOS / Linux kill recipe for tm-hb.sh /
# tm-wait.sh background shells launched by task-loop.
#
# On POSIX, Claude Code's KillShell propagates SIGTERM correctly via
# process groups, so the issue this script addresses is largely a
# Windows-specific problem. This POSIX variant exists for consistency
# (one call site in task-loop / task-shutdown regardless of platform)
# and to give a clean exit code from `verify` mode that matches the .ps1.
#
# Usage:
#   tm-kill.sh <mode> --agent-id <id> [--task-id <tid>]
#
#   Modes:
#     by-task         kill heartbeat(s) for a single task (needs --task-id)
#     all-for-agent   kill every tm-hb/tm-wait for this agent
#     verify          enumerate; exit 0 if clean, 1 if residual PIDs
set -u

MODE="${1:-}"; shift || true
AGENT_ID=""
TASK_ID=""
while [ $# -gt 0 ]; do
    case "$1" in
        --agent-id) AGENT_ID="$2"; shift 2 ;;
        --task-id)  TASK_ID="$2";  shift 2 ;;
        *) shift ;;
    esac
done

if [ -z "$AGENT_ID" ]; then
    echo "tm-kill.sh: --agent-id required" >&2
    exit 2
fi

# Escape regex metacharacters in user input for grep -E. Only `/` and
# `.` realistically appear in our IDs; escaping them is enough.
escape_re() { printf '%s' "$1" | sed -e 's/[.[\*^$()+?{|]/\\&/g' -e 's,/,\\/,g'; }
AGENT_RE="$(escape_re "$AGENT_ID")"
TASK_RE="$(escape_re "$TASK_ID")"

case "$MODE" in
    by-task)
        if [ -z "$TASK_ID" ]; then
            echo "tm-kill.sh: by-task mode requires --task-id" >&2
            exit 2
        fi
        # pkill -f matches against the full argv string.
        pkill -f "tm-hb\\.sh.*${AGENT_RE}.*TASK_ID=\"${TASK_RE}\"" || true
        echo "killed (by-task $TASK_ID / $AGENT_ID)"
        ;;
    all-for-agent)
        pkill -f "(tm-hb|tm-wait)\\.sh.*${AGENT_RE}" || true
        echo "killed (all-for-agent $AGENT_ID)"
        ;;
    verify)
        if pgrep -fa "(tm-hb|tm-wait)\\.sh.*${AGENT_RE}" >/dev/null 2>&1; then
            echo "RESIDUAL: tm-* shells for $AGENT_ID still alive:"
            pgrep -fa "(tm-hb|tm-wait)\\.sh.*${AGENT_RE}"
            exit 1
        else
            echo "verified clean: no tm-* shells for $AGENT_ID"
            exit 0
        fi
        ;;
    *)
        echo "tm-kill.sh: unknown mode: $MODE (use by-task|all-for-agent|verify)" >&2
        exit 2
        ;;
esac
