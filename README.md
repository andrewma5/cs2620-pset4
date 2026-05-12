# pset4 — Distributed Task Manager

A 3-replica Multi-Paxos task manager that hands work to a swarm of
Claude CLI agents over HTTP/JSON.

> **New contributor?** Read [`docs/PARTNER_ONBOARDING.md`](docs/PARTNER_ONBOARDING.md) first.
> It covers what changed since the single-instance POC, the three-layer
> test stack (sim / local-TCP / docker), how to build and run the demo,
> the hard-rules, and known limitations.

This file describes the POC layer of the codebase (the state machine,
the lifecycle, the lease + fencing story) — all of which is unchanged
by paxos and still accurate. The replicated layer (paxos engine,
transport split, multi-replica deployment, failure injection, replay
invariant) is documented in `docs/PARTNER_ONBOARDING.md` and
`docs/PLAN.md`.

---

## What's in the repo

### C++ source (the task manager itself)

| File | Role |
| --- | --- |
| `tm.hh` / `tm.cc` | Request and response variant types + JSON (de)serialization. Every wire payload is a `tmgr::request` or `tmgr::response`. |
| `task_manager_db.hh` / `.cc` | The typed state machine. One entry point: `process_req(request, now_unix) → response`. Atomic, deterministic, no I/O. **This is exactly what Paxos will replicate.** |
| `tm-server.cc` | HTTP/JSON front. One coroutine per connection (Cotamer). Dispatches each request through `db.process_req()` and writes a JSONL decision log to disk. |
| `tm-replay.cc` | Offline tool. Reads the decision log, replays it through a fresh `task_manager_db`, emits `snapshots.json` (one post-state per non-no-op decision) for the visualizer. |
| `tm-tests.cc` | Unit tests for the SM. `make check`. |
| `tm_dump.hh` | Pretty-printer used by `/dump` and the snapshot writer. |
| `CMakeLists.txt`, `GNUmakefile` | Build. `make` builds `tm-server`, `tm-tests`, `tm-replay`. |

### Skills (canonical copies — what the agents follow)

`skills/`:

- `task-loop.md` — the outer loop the agent runs forever: claim →
  background-heartbeat → dispatch a fresh subagent for the work →
  KillShell heartbeat → loop.
- `task-planning.md` — inlined into the subagent prompt for `plan`
  tasks. Decomposes a prompt into child tasks (more plans or
  implements) and posts them via `task_complete`'s `new_children`.
- `task-implementing.md` — inlined for `implement` tasks. Does the git
  setup (`git fetch`, `git worktree add`, merge predecessors), writes
  code, commits, reports the branch tip SHA. Completion synthesizes a
  matching `merge` task automatically.
- `task-merging.md` — inlined for synthetic `merge` tasks. Acquires the
  main lock, merges into `main`, runs sanity tests, pushes, releases.

These four files are the agent's contract. The server enforces fencing
+ atomicity; the skills enforce the workflow on the agent side.

### The `swarm-deploy` skill (`.claude/skills/swarm-deploy/`)

A **deploy skill** that scaffolds a fresh agent-swarm test ground in a
target directory. Use it whenever you want to spin up a new test run —
including the demo. See "Deploying a swarm" below for the workflow.

### Demo + scripts + visualizer

- `demo/Makefile` — one-shot orchestration for the localhost demo: init
  bare repo + agent dirs, seed the root plan task, dump status, replay
  + start the visualizer.
- `demo/seed-task.json` — the example root plan ("build a CLI
  calculator") used by `make seed`.
- `scripts/smoke.sh` — end-to-end HTTP smoke against a running
  `tm-server` (creates a plan, claims, completes, asserts).
- `scripts/smoke-lease.sh` — the same but exercising lease expiry.
- `monitor_tm.py` — polls `/dump` and prints a line only when state
  changes. Useful while watching a run.
- `visualize_tm.py` — Flask UI that scrubs through `snapshots.json`
  with a slider + arrow keys. Trello-style columns (pending /
  in_progress / done / failed); the affected task pulses on each step,
  and the merge task currently holding the main lock gets a 🔒 badge.

### Docs

- `docs/PLAN.md` — the **full replicated target** (what the next person is
  building toward). Covers data model, RPC surface, leases, fencing,
  failure handling, the two-tier test plan.
- `docs/PLAN.md` — older copy of the same plan kept for reference.

---

## Build

Prereqs: a Linux/macOS box (or WSL/Git Bash on Windows) with `cmake`,
`xxhash`, `llhttp`, `nlohmann_json`. The course's docker image already
has these.

```bash
make            # builds tm-server, tm-tests, tm-replay
make check      # runs the SM unit tests
```

Sanitizer builds: `make SAN=1`, `make ASAN=1`, `make UBSAN=1`,
`make TSAN=1`.

---

## Deploying a swarm (the `swarm-deploy` skill)

The fastest way to get a working test setup is the `swarm-deploy`
skill. From a Claude Code session in this repo, say:

> deploy a swarm at `<target-dir>` with 3 agents

(or "spin up a test swarm", "set up agent folders for tm-server",
etc. — anything matching the skill description triggers it.)

The skill is a thin wrapper around
`.claude/skills/swarm-deploy/scripts/deploy.py`. Equivalent CLI:

```bash
py -3 .claude/skills/swarm-deploy/scripts/deploy.py <target-dir> --agents 3
# Pass --force to wipe an existing repo.git/agent-N and redeploy.
```

What deploy actually produces in `<target-dir>`:

```
<target-dir>/
├── repo.git/                              # bare git repo, the shared "remote"
│                                          # seeded with one empty commit on `main`
├── agent-1/
│   ├── CLAUDE.md                          # env vars + instructions for this agent
│   └── .claude/
│       ├── settings.json                  # {"model": "sonnet"} — pins default model
│       └── skills/
│           ├── task-loop/SKILL.md
│           ├── task-planning/SKILL.md
│           ├── task-implementing/SKILL.md
│           └── task-merging/SKILL.md
├── agent-2/   (same structure as agent-1, AGENT_ID=agent-2)
└── agent-3/   (same structure, AGENT_ID=agent-3)
```

A few details worth knowing:

- **`repo.git` is the agents' git remote.** It's a bare repo; the
  script seeds it with a single empty commit on `main` so agents can
  branch off `main` immediately. Each agent's `CLAUDE.md` points
  `TM_REPO` at this directory. There is no GitHub remote — the
  "remote" is just a directory on the same disk.
- **Each agent's `CLAUDE.md` is templated** with `AGENT_ID`, `TM_REPO`
  (the bare repo path), and `TM_WORK` (the agent's own directory),
  written as POSIX paths because the agents shell out to bash. This
  is the file Claude reads on startup, so the env vars are always in
  scope.
- **Default model is pinned to Sonnet** via
  `.claude/settings.json = {"model":"sonnet"}` in each agent dir. The
  rationale: agents are doing well-scoped, skill-driven work — Sonnet
  is fast and cheap enough to be the right default. Override per-run
  with `claude --model opus` if you want a stronger agent.
- **The bundled skills under
  `.claude/skills/swarm-deploy/assets/agent-skills/` are snapshots.**
  When you edit the canonical files in `pset4/skills/*.md`, refresh
  the snapshots before redeploying:

  ```bash
  PSET4=$(pwd)/skills
  BUNDLE=.claude/skills/swarm-deploy/assets/agent-skills
  cp "$PSET4/task-loop.md"         "$BUNDLE/task-loop/SKILL.md"
  cp "$PSET4/task-implementing.md" "$BUNDLE/task-implementing/SKILL.md"
  cp "$PSET4/task-planning.md"     "$BUNDLE/task-planning/SKILL.md"
  cp "$PSET4/task-merging.md"      "$BUNDLE/task-merging/SKILL.md"
  ```

  The helper scripts (`tm-wait.sh`, `tm-hb.sh`) live only in the
  bundle's `task-loop/` folder.
- **Run agents with `--dangerously-skip-permissions`** when you want
  them to grind autonomously without permission prompts: each agent
  runs many `curl`/`git`/`Bash` calls per task and prompting on every
  one defeats the swarm. Example below.

---

## Running a swarm end-to-end

This is the full demo path: build → deploy → start server → seed →
launch agents → replay → visualize.

### 1. Build the binaries

```bash
make
```

### 2. Deploy a target directory

From this repo (so the `swarm-deploy` skill is in scope), tell Claude
Code: *"deploy a swarm at `~/swarm-test` with 3 agents"*. Or run the
script directly:

```bash
py -3 .claude/skills/swarm-deploy/scripts/deploy.py ~/swarm-test --agents 3
```

You now have `~/swarm-test/repo.git` and `~/swarm-test/agent-{1,2,3}/`.

### 3. Start `tm-server`

In its own terminal, from this directory:

```bash
./build/tm-server -V -p 8080
# -V              verbose
# -p 8080         HTTP port
# -L tm-server.log  decision log path (default tm-server.log; -L - disables)
```

Each request lands as one line of JSONL in `tm-server.log`:
`{seq, now_unix, path, request}`. That log is the input to `tm-replay`.

### 4. Seed the root plan task

In another terminal:

```bash
cd demo
make seed       # POSTs demo/seed-task.json to /task_create
make status     # pretty-prints the current task table
```

Or hand-roll it with `curl -X POST localhost:8080/task_create -d @demo/seed-task.json`.
The seed creates one root `plan` task; the agents will decompose it.

### 5. Launch the agents

One terminal per agent. From the deployed swarm directory:

```bash
cd ~/swarm-test/agent-1
claude --dangerously-skip-permissions
# inside Claude:
/task-loop
```

Repeat for `agent-2`, `agent-3`. The `--dangerously-skip-permissions`
flag is what makes the agent grind without prompting on every
`curl`/`git`/`Bash` call. Each agent's `CLAUDE.md` already exports the
right `TM_URL`, `AGENT_ID`, `TM_REPO`, `TM_WORK`; `/task-loop` runs
the canonical loop.

While they run, `watch -n 2 'cd demo && make status'` (or `python
monitor_tm.py`) is a nice live view.

### 6. Replay the log into snapshots

After (or during) a run, turn the decision log into a sequence of
snapshots:

```bash
./build/tm-replay -L tm-server.log -o snapshots.json
# or:  cd demo && make replay
```

`tm-replay` reuses `task_manager_db` directly — same code the live
server runs. Each log line is replayed through `process_req`, the
post-state is hashed, and a snapshot is emitted only if the hash
differs from the previous one (collapses no-op heartbeats, redundant
list calls, etc., so the timeline scrubs smoothly).

### 7. Visualize the run

```bash
py -3 -m pip install --user flask    # one-time
py -3 visualize_tm.py                # http://127.0.0.1:5050
# or:  cd demo && make viz           # replay + start the UI
```

The page is a Trello-style task board (pending / in_progress / done /
failed). Use the slider for wall-clock scrubbing or arrow keys to step
event-by-event; the affected task pulses on each step, and the merge
task currently holding the main lock gets a 🔒 badge.

---

## The C++/Python split (and why)

The replay/visualize pipeline is deliberately split:

- **C++ (`tm-replay`) holds all the SM logic.** It links against
  `task_manager_db.cc` directly — the same code path the live server
  uses. The visualizer's timeline is *exactly* what the server
  computed; there is no risk of the UI's "what does this op mean"
  drifting from the real state machine. If the SM changes, replay
  changes for free.
- **Python (`visualize_tm.py`) is just presentation.** It serves a
  single static HTML page and exposes `snapshots.json`. It does not
  understand task semantics — it renders whatever C++ wrote. Iterating
  on the UI (CSS, layout, badges, key bindings) is fast and doesn't
  touch C++.

The contract between them is `snapshots.json`: an array of
`{op_summary, affected_task_id, post_state}` records. C++ owns truth;
Python owns ergonomics.

---

## Architecture (very brief)

```
   Claude CLI #1            Claude CLI #2            Claude CLI #3
   /task-loop               /task-loop               /task-loop
   + bg heartbeat shell     + bg heartbeat shell     + bg heartbeat shell
        │ HTTP/JSON (curl)         │ HTTP/JSON               │ HTTP/JSON
        ▼                         ▼                          ▼
                       ┌──────── tm-server ────────┐
                       │  HTTP front (Cotamer)     │
                       │  task_manager_db          │
                       │  → JSONL decision log     │  ──► tm-replay ──► snapshots.json ──► visualize_tm.py
                       └───────────────────────────┘

   Agents share a bare git repo at $TM_REPO. Git is the agent's
   responsibility — tm-server never touches a worktree. The "main lock"
   is purely advisory: tm-server serializes who holds the token, the
   skills agree to only push under it.
```

The HTTP surface (full list and semantics in `docs/PLAN.md` §5):
`task_create`, `task_list`, `task_claim`, `task_heartbeat`,
`task_complete`, `task_fail`, `swarm_resume`, `main_lock_acquire`,
`main_lock_release`, plus `GET /dump` for debugging.

Leases are lazy wall-clock (`LEASE_WINDOW = 15s`): no sweeper, no
periodic tick. The next `task_claim` or `main_lock_acquire` that
notices a stale heartbeat reclaims the task atomically inside the same
`process_req` call. Fencing tokens are minted on every claim;
mismatch ⇒ `{fenced: true}` and the agent drops the work and loops.

---

## Testing

- `make check` — SM unit tests (`tm-tests`).
- `scripts/smoke.sh` — end-to-end HTTP smoke (server must be running).
- `scripts/smoke-lease.sh` — lease-expiry integration smoke.
- The 3-Claude calculator demo above is the qualitative integration
  story.

---

## Known limitations

- **No state transfer.** `kill`'d replicas stay dead — the survivors
  keep going (`docker compose pause` / `partition` are the recovery
  paths). See `docs/PARTNER_ONBOARDING.md` and hard-rule #5 in
  `docs/PLAN.md`.
- **No persistence past full-cluster crash.** Each replica's decision
  log + SM state is in-process; killing all 3 replicas loses state.
  The decision log is for replay/visualization, not crash recovery.
- **No HTTP-layer retransmit dedup.** Idempotency rests on the fencing
  token. `(agent_id, serial)` dedup at the HTTP layer is deferred
  (see `docs/PLAN.md` §2.5).
- **No `tm-cli`.** Humans use `curl` directly (or `make status`).
- **Worktree leaks.** Agent crashes can leave worktrees behind; clean
  with `git worktree prune` between runs.
- **Background heartbeat scripts can outlive Claude on Windows / Git
  Bash.** If you Ctrl-C an agent mid-task, sweep for stragglers — see
  the swarm-deploy skill's troubleshooting section for the PowerShell
  one-liner (`Get-CimInstance Win32_Process | Where-Object {
  $_.CommandLine -match 'tm-hb|tm-wait' } | ...`).

---

## Where the paxos layer lives

The replicated state machine, transport split, multi-replica
deployment, failure-injection commands, automated test sweeps, and
the on-apply log-agreement invariant are all documented in
[`docs/PARTNER_ONBOARDING.md`](docs/PARTNER_ONBOARDING.md). Read that
for the current architecture and how to build / test / run the demo.
[`docs/PLAN.md`](docs/PLAN.md) has the original target spec, RPC
table, full failure model, and build order.
