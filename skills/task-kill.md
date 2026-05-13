---
name: task-kill
description: Canonical recipe for killing tm-hb.sh / tm-wait.sh background shells started by task-loop. Use whenever you need to stop a background tm-* shell, because Claude Code's TaskStop tool (and the /bashes → X UI) DO NOT actually kill these processes on Windows. Other skills invoke this one's helper scripts; humans read it to understand why TaskStop is unreliable. NOT a user-facing command — see task-shutdown for that.
---

# task-kill

This is a **library skill**. It is invoked by `task-loop` (per-task
heartbeat cleanup) and by `task-shutdown` (full teardown). Don't invoke
it directly from a user request — if a user wants to stop an agent,
they want `task-shutdown`, not this.

## TL;DR — what's broken

Claude Code's `TaskStop` tool (the `KillShell` rename, fetched via
`ToolSearch`) and the interactive `/bashes → X` panel both call the
same code path: Node's `child.kill()` → `TerminateProcess(bash.pid)` on
the outermost tracked bash. **On Windows this kills only that one PID
— grandchildren are NOT reaped** (Windows has no POSIX process
groups; TaskStop does not use `taskkill /T`).

The `task-loop` background shells launch through TWO bash wrappers
(Claude Code's snapshot-source wrapper, then an `eval` bash), so the
actual `tm-hb.sh` / `tm-wait.sh` is a grandchild. TaskStop returns
`"Successfully stopped task: ..."` but the script keeps running.

**Evidence** (2026-05-13 swarm run,
`pset4-testing-grounds/revised-replicas-real/`):

- Every inter-task `TaskStop` in `task-loop` Step 5 reported success
  while the heartbeat shell kept pinging tm-server. Orphans accumulated
  at ~1 per completed task per agent.
- agent-2 / agent-3 `tm-wait.sh` at end-of-swarm continued polling
  `/task_claim` every 10s long after TaskStop "stopped" them. Only
  agent-1's PowerShell `Stop-Process` sweep actually killed them.
- After ~2 minutes the harness loses the `bash_id` entirely
  (`TaskStop` returns `<tool_use_error>No task found with ID: ...`)
  but the OS process is still alive.

Related issues: anthropics/claude-code#8865 (open),
#43944, #32183.

On macOS / Linux `TaskStop` works correctly (POSIX signals propagate
via process groups). This skill exists primarily for Windows but the
POSIX helper exists so callers don't need to platform-branch.

## Helper scripts

Two scripts live alongside this SKILL.md:

- `tm-kill.ps1` — Windows recipe (`Get-WmiObject Win32_Process` →
  `Stop-Process -Force`).
- `tm-kill.sh` — POSIX recipe (`pkill -f` / `pgrep -fa`).

**Callers pick the right extension based on the agent's platform.**
There is intentionally no extensionless dispatcher — callers in
`task-loop` and `task-shutdown` already platform-branch (e.g. `py -3`
vs `python3`), one more branch is fine and avoids a fragile cmd shim.

### CLI

Both scripts share the same arg shape:

```
tm-kill.{ps1,sh} <mode> --agent-id <id> [--task-id <tid>]
```

PowerShell uses named params (`-Mode`, `-AgentId`, `-TaskId`); the
shell version uses `--agent-id` / `--task-id` long options. Both are
unambiguous.

Modes:

- **`by-task`** — kill the heartbeat for one completed task. Filters
  `tm-hb.sh` processes whose command line contains the literal
  `TASK_ID="<tid>"` assignment. Used by `task-loop` Step 5 after each
  task completes. Requires `--task-id`.
- **`all-for-agent`** — kill every `tm-hb.sh` and `tm-wait.sh` whose
  command line contains this agent's path. Used by `task-shutdown`
  for full teardown.
- **`verify`** — enumerate (don't kill); exit 0 if no `tm-*` shells
  remain for this agent, exit 1 with a list of residual PIDs if any.
  Used by `task-shutdown` after the `all-for-agent` pass.

## When to invoke from other skills

**From `task-loop` Step 5** (after each task completes):

```bash
# POSIX agent
"$TM_WORK/.claude/skills/task-kill/tm-kill.sh" by-task \
    --agent-id "$AGENT_ID" --task-id "$TASK_ID"
```

```powershell
# Windows agent — invoke via the PowerShell tool, NOT the Bash tool
& "$env:TM_WORK\.claude\skills\task-kill\tm-kill.ps1" `
    -Mode by-task -AgentId $env:AGENT_ID -TaskId $env:TASK_ID
```

**From `task-shutdown`** (full teardown):

```powershell
& "$env:TM_WORK\.claude\skills\task-kill\tm-kill.ps1" -Mode all-for-agent -AgentId $env:AGENT_ID
& "$env:TM_WORK\.claude\skills\task-kill\tm-kill.ps1" -Mode verify        -AgentId $env:AGENT_ID
```

## Hard NEVER list

- **NEVER call `TaskStop` / `KillShell` on `tm-*` background shells.**
  It lies on Windows ("Successfully stopped" + process keeps running).
  On Linux/macOS use this helper anyway for consistency.
- **NEVER use `Get-Process | Where CommandLine -match ...`** to find
  these shells — `.CommandLine` is null on Windows PowerShell 5.1
  `System.Diagnostics.Process` objects, so the filter silently matches
  nothing. Two of three agents in the 2026-05-13 trace fell into this
  trap and reported "process is gone" while it kept running.
- **NEVER use `Get-CimInstance Win32_Process | Where CommandLine
  -like ...`** — returned empty for cross-session processes in the
  trace (suspected visibility issue). Use `Get-WmiObject Win32_Process
  -Filter "Name='bash.exe'"` only.
- **NEVER use `taskkill /T`.** The bash wrappers don't form a clean
  parent-child tree on Windows (reparented children are common), so
  `/T` can reap unrelated children of the bash subtree.
  `Stop-Process -Force` per enumerated PID is what worked in the
  trace.
- **NEVER inline this kill logic in another skill.** The point of
  this skill is single-source-of-truth. Other skills call
  `tm-kill.{ps1,sh}`; they do not re-implement the WMI query.

## Why these specific cmdlets

Three PowerShell ways to get process command lines, only one works
reliably:

| Cmdlet | CommandLine populated? | Cross-session visibility |
|--------|------------------------|--------------------------|
| `Get-Process` | NO (null on Win PS 5.1) | n/a |
| `Get-CimInstance Win32_Process` + `Where CommandLine` | Inconsistent (empty in our trace) | partial |
| `Get-WmiObject Win32_Process -Filter "Name='bash.exe'"` | YES | YES |

agent-1 in the 2026-05-13 trace used `Get-WmiObject` and
successfully enumerated all three agents' bash subtrees. agent-2 and
agent-3 used `Get-Process` and `Get-CimInstance | Where CommandLine`
respectively, got empty output, and falsely concluded the processes
had exited.
