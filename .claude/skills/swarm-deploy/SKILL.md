---
name: swarm-deploy
description: Deploy a tm-server agent-swarm test instance — creates a target directory containing a bare git repo and N agent workspaces, each preloaded with the task-loop / task-planning / task-implementing / task-merging / task-kill / task-shutdown skills and a CLAUDE.md wired to the right env vars. Use when the user asks to "deploy a swarm", "set up a test swarm", "spin up agent folders for tm-server", or otherwise prepares a new pset4 swarm test ground. Default agent count is 3.
---

# swarm-deploy

## Overview

This skill scaffolds a fresh swarm-test directory: one bare git repo
shared by all agents, plus one folder per agent populated with a
`CLAUDE.md`, the six `.claude/skills/*.md` files the agents need
(`task-loop`, `task-planning`, `task-implementing`, `task-merging`,
`task-kill`, `task-shutdown`), and a `.claude/settings.json` pinning
the agent's default model to Sonnet.

The actual `tm-server` binary, demo seed task, and `claude` CLI launches
are NOT this skill's job — see `pset4/README.md` and `pset4/demo/Makefile`
for those. This skill only produces the directory tree the agents run in.

## Step 1: Gather inputs

Ask the user (or proceed with sensible defaults if auto-mode and the
target is obvious from context):

1. **Target directory** — where the swarm should live. Example:
   `C:/Users/andre/Documents/Harvard/CS2620/pset4-testing-grounds/remote2`.
   Created if missing. Refuses to overwrite an existing `repo.git` or
   `agent-N/`.
2. **Number of agents** — integer >= 1. Default `3`.

## Step 2: Sync canonical skills into the bundle (if they changed)

Before deploying, refresh the bundle from the canonical pset4 skills so
the deployed agents pick up any edits the user has made:

```bash
PSET4=<repo>/pset4/skills
BUNDLE=<skill-dir>/assets/agent-skills
cp "$PSET4/task-loop.md"         "$BUNDLE/task-loop/SKILL.md"
cp "$PSET4/task-implementing.md" "$BUNDLE/task-implementing/SKILL.md"
cp "$PSET4/task-planning.md"     "$BUNDLE/task-planning/SKILL.md"
cp "$PSET4/task-merging.md"      "$BUNDLE/task-merging/SKILL.md"
cp "$PSET4/task-kill.md"         "$BUNDLE/task-kill/SKILL.md"
cp "$PSET4/task-shutdown.md"     "$BUNDLE/task-shutdown/SKILL.md"
```

Do **not** copy the helper scripts (`tm-wait.sh`, `tm-hb.sh`, `tm`,
`tm-kill.ps1`, `tm-kill.sh`); those live only in the bundle
(`assets/agent-skills/task-loop/` and `assets/agent-skills/task-kill/`).

## Step 3: Run the deploy script

```bash
py -3 <skill-dir>/scripts/deploy.py <TARGET> --agents <N> [--force] \
      [--tm-urls http://host1:port,http://host2:port,...]
```

`--tm-urls` defaults to the docker-compose 3-replica layout
(`http://localhost:8081,http://localhost:8082,http://localhost:8083`).
Pass a single URL for single-replica testing
(e.g. `--tm-urls http://localhost:8080`).

(`<skill-dir>` is the directory containing this `SKILL.md`. On Windows
the python launcher is `py -3`; on macOS/Linux use `python3`.)

Pass `--force` to wipe an existing `repo.git`/`agent-N` and redeploy
(useful when iterating on the skill files).

The script:

- creates `<TARGET>/repo.git` as a bare repo with one empty initial
  commit on `main` (so agent branches off `main` work immediately);
- creates `<TARGET>/agent-1`, `agent-2`, ..., `agent-N`;
- in each agent folder writes:
  - `CLAUDE.md` with `TM_URL_LIST` (and `TM_URL` for legacy paths),
    `AGENT_ID`, `TM_REPO`, `TM_WORK` filled in;
  - `.claude/skills/task-loop/SKILL.md`,
    `.claude/skills/task-planning/SKILL.md`,
    `.claude/skills/task-implementing/SKILL.md`,
    `.claude/skills/task-merging/SKILL.md`,
    `.claude/skills/task-kill/SKILL.md` (+ `tm-kill.ps1` and
    `tm-kill.sh` helper scripts in the same folder),
    `.claude/skills/task-shutdown/SKILL.md` (copies of the canonical
    skill folders bundled with this skill in `assets/agent-skills/`);
  - `.claude/settings.json` with `{"model": "sonnet"}`.
- prints a JSON summary and the next-step commands.

## Step 4: Tell the user how to actually run it

After deploy succeeds, the user still needs to:

1. Start the task manager in a separate terminal:
   `./build/tm-server -V -p 8080` (built from `pset4/`).
2. Seed a root plan task:
   `curl -X POST http://localhost:8080/task_create -d @pset4/demo/seed-task.json`.
3. In one terminal per agent:
   `cd <TARGET>/agent-N && claude`, then inside Claude run `/task-loop`.

## Notes

- The bundled skill folders in `assets/agent-skills/<name>/SKILL.md`
  are snapshots. If the canonical files in
  `cs2620-s26-psets-andrewma5/pset4/skills/<name>.md` change, refresh
  each snapshot by copying the new content into the matching
  `assets/agent-skills/<name>/SKILL.md`. The deploy script copies
  whatever is in `assets/agent-skills/` at deploy time.
- `repo.git` is intentionally bare. The agents `git clone` it from
  inside their work directories.
- `TM_WORK` in each `CLAUDE.md` is set to the agent's own folder;
  `TM_REPO` is the shared bare repo. Both are written as POSIX paths
  (forward slashes) since the agent shells out to bash.
- Setting `model: sonnet` in `.claude/settings.json` means each agent
  will use Sonnet by default when launched with `claude` from that
  folder. Override with `claude --model opus` if you want a stronger
  agent for a specific run.

## Troubleshooting

- **`git: command not found`** — install git, or run from a shell where
  git is on PATH.
- **`error: ... paths already exist`** — pass `--force` to wipe and
  redeploy (typical when iterating on skill files), or pick a fresh
  target. `--force` only removes `repo.git` and the `agent-N`
  subdirectories under `<TARGET>` — anything else under `<TARGET>` is
  left alone.
- **Agents can't reach `tm-server`** — check `TM_URL` in the agent's
  `CLAUDE.md` matches the port `tm-server` is bound to.
- **Orphaned `tm-hb.sh` / `tm-wait.sh` after closing an agent** — if
  Claude is still alive in the session, invoke `/task-shutdown` from
  inside that agent's Claude window. It calls the `task-kill` helper,
  which uses `Get-WmiObject Win32_Process` + `Stop-Process -Force` on
  Windows (or `pkill -f` on POSIX) to actually reap the OS process.

  **Do not rely on `KillShell` / `TaskStop` / `/bashes → X`** to kill
  these shells — on Windows they report success while the underlying
  bash subtree keeps running (anthropics/claude-code#8865). See
  `assets/agent-skills/task-kill/SKILL.md` for the full failure-mode
  write-up.

  If Claude is no longer attached (you closed it / the session
  crashed), run the `task-kill` helper directly from any PowerShell
  window — no Claude needed:

  ```powershell
  & "<TARGET>\agent-N\.claude\skills\task-kill\tm-kill.ps1" `
      -Mode all-for-agent -AgentId "agent-N"
  ```

  ```bash
  # macOS / Linux
  "<TARGET>/agent-N/.claude/skills/task-kill/tm-kill.sh" \
      all-for-agent --agent-id "agent-N"
  ```

  Add `-Mode verify` / `verify` afterwards to confirm zero residual
  matches.
