---
name: task-shutdown
description: Stop a running swarm agent. Use ONLY when (1) the user wants to cleanly exit a finished or in-progress swarm, or (2) the user wants to simulate a Claude failure for failover testing. Kills every tm-hb / tm-wait shell for this agent AND abandons the in-flight subagent if any. Trigger phrases - "stop the swarm", "kill this agent", "simulate failure", "pause the loop", "/task-shutdown". For per-task heartbeat cleanup during normal loop operation, do NOT use this skill - the loop handles that internally via task-kill.
---

# task-shutdown

Orchestrator skill. Kills every `tm-hb.sh` / `tm-wait.sh` background
shell belonging to this agent, abandons the in-flight subagent if any,
and verifies. Delegates the actual killing to the `task-kill` library
skill — this skill is only orchestration.

## When this skill applies

Exactly two cases:

1. **Clean exit.** The swarm is done (or the user is walking away
   from it). They want all `tm-*` background shells for this agent
   stopped, no orphans left in Task Manager pinging tm-server.
2. **Simulate Claude failure.** The user wants to test how the swarm
   reacts when one agent dies mid-task — does another agent reclaim
   the fenced task? Same kill sequence; the in-flight subagent is
   also abandoned so the lease lapses and a peer reclaims.

**Do NOT use this skill for anything else.** The loop running normally
already calls `task-kill by-task` between tasks for per-task heartbeat
cleanup. If you find yourself reaching for `task-shutdown` mid-loop
without one of the two reasons above, stop and reconsider.

## Why TaskStop alone is wrong here

See `task-kill/SKILL.md` for the full story. Short version:
`TaskStop` / `/bashes → X` on Windows fire `TerminateProcess` on the
outermost bash.exe PID only; grandchildren survive. Empirically (the
2026-05-13 trace) every `TaskStop` in this skill's old shape reported
success while the actual shell kept running. This skill goes straight
to the proven PowerShell recipe via `task-kill`.

## Step 1 — Abandon the in-flight subagent (if any)

If an `Agent()` subagent is currently dispatched on a task, do NOT
wait for it to return.

You cannot truly interrupt a running `Agent()` from inside the parent
context — the parent simply moves on. The subagent will eventually
finish whatever it was doing and try to call `tm-complete.sh` into a
context that has already torn down its heartbeat. By then the SM will
have expired the lease and re-pended the task; the subagent's
`tm-complete.sh` will be rejected as `fenced`. That is the correct
simulated failure: the lease lapses, a peer reclaims.

So: just proceed to Step 2. The subagent stays "running" in the
harness sense, but it's effectively dead for swarm-coordination
purposes.

## Step 2 — `task-kill all-for-agent`

**Windows agent** — use the PowerShell tool:

```powershell
& "$env:TM_WORK\.claude\skills\task-kill\tm-kill.ps1" `
    -Mode all-for-agent -AgentId $env:AGENT_ID
```

**macOS / Linux agent** — Bash tool is fine:

```bash
# === STANDARD PREAMBLE ===
"$TM_WORK/.claude/skills/task-kill/tm-kill.sh" all-for-agent \
    --agent-id "$AGENT_ID"
```

(`$AGENT_ID` and `$TM_WORK` come from the standard preamble — same
values shown in CLAUDE.md.)

## Step 3 — Verify

Re-invoke `task-kill` in `verify` mode:

```powershell
& "$env:TM_WORK\.claude\skills\task-kill\tm-kill.ps1" `
    -Mode verify -AgentId $env:AGENT_ID
```

```bash
"$TM_WORK/.claude/skills/task-kill/tm-kill.sh" verify \
    --agent-id "$AGENT_ID"
```

Exit code 0 (`verified clean: no tm-* shells for $AGENT_ID`) = success.

Non-zero exit means `task-kill` found residual PIDs. **Do NOT loop.**
Paste the residual PID + command line to the user, ask them to kill
manually in their own PowerShell window. One pass, one verify, one
escalate-or-success — that's it.

## Step 4 — Clean up the claim file

If `$TMPDIR/tm-claim-$AGENT_ID.json` exists, remove it so the next
`/task-loop` invocation can't read a stale claim by accident:

```bash
rm -f "$TMPDIR/tm-claim-$AGENT_ID.json"
```

(Reading stale claims is already forbidden by `task-loop` invariant
#7, but belt-and-suspenders.)

## Step 5 — Tell the user how to resume

One sentence:

> `/task-loop` re-enters the swarm with the same `AGENT_ID`. If any
> task was fenced by the shutdown, another agent already reclaimed
> it — no action needed.

That's it. The skill is done.

## What this skill does NOT do

- Does NOT stop `tm-server`. That's the human's job (or another
  skill's).
- Does NOT delete worktrees or clean up any git state.
- Does NOT call `/task_fail`. That would halt the entire swarm; this
  skill is per-agent only.
- Does NOT touch other agents' shells. The `--agent-id` filter is
  strict — only this Claude session's `tm-*` shells die. Peer agents
  keep running.
- Does NOT modify any tm-server state. The SM will notice the
  heartbeat stopped and expire the lease on its own.
