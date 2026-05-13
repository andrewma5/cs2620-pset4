# Real-Claude demo runs

End-to-end instructions for the 4 demo scenarios used in the writeup.
**These cost real money** (Sonnet: ~$1-3/run, Opus: ~$5-15/run). Default
to Sonnet unless the scenario specifically needs deeper reasoning.

Each run captures into `demo/runs/<scenario>/` (e.g. `demo/runs/clean/`,
`demo/runs/pause-resume/`, `demo/runs/kill-leader/`) so the directory
name tells you which scenario it was without cross-referencing.

## Pre-flight (do once before any run)

Open three host terminals + one docker dev shell. Tabs/panes work fine.

| Terminal | Where | Purpose |
|---|---|---|
| **dev-shell** | `cs61-user@…` docker | Building the Linux ELF binary |
| **host-claude** | host (macOS) | Slash commands + `docker compose` orchestration |
| **agent-1/2/3** | host (macOS), 3 terminals | One real-Claude per replica |
| **(optional) watch** | host (macOS) | Live `docker compose logs` |

### Build the Linux binary

In the **dev-shell** (only needed when source has changed):
```bash
make
file build/tm-server     # must say: ELF 64-bit LSB ... Linux
```

If it already says ELF Linux from earlier today, you can skip the build.

---

## Run #1 — Clean (no failure injection)

Goal: prove the swarm completes the calculator decomposition end-to-end
with all 3 replicas healthy. The baseline run.

### Step 1 — Deploy swarm (host-claude terminal)

```bash
cd /Users/annieliupoo/Documents/harvard/code/cs2620-s26-psets-aliu104/pset4-active
claude
```

In that Claude session:
```
/swarm-deploy ../pset4-testing/test1
```

Wait for completion. Creates `../pset4-testing/test1/repo.git` (bare)
plus `agent-{1,2,3}/` with templated `CLAUDE.md` files.

### Step 2 — Bring up 3 replicas + seed (same Claude session)

Tell host-Claude:
```
now bring up the 3 replicas and seed the calculator task:
  make -C demo demo-up
  make -C demo demo-seed
verify curl localhost:8081/dump returns 200 before reporting done.
```

After this, you should see all 3 replicas healthy:
```bash
docker compose ps      # 3 services, all 'healthy'
curl -s localhost:8081/dump | jq '.tasks | length'   # should be 1
```

### Step 3 — Launch 3 real-Claude agents

In **agent-1 terminal**:
```bash
cd /Users/annieliupoo/Documents/harvard/code/cs2620-s26-psets-aliu104/pset4-testing/test1/agent-1
claude --dangerously-skip-permissions
```
In that Claude: `/task-loop`

**Repeat for agent-2 and agent-3** in their respective terminals.

### Step 4 — Watch (optional, in watch terminal)

```bash
cd /Users/annieliupoo/Documents/harvard/code/cs2620-s26-psets-aliu104/pset4-active
docker compose logs -f | grep -iE 'leader|prepare|propose|decision'
```

### Step 5 — Observe

**Signs it's working:**
- agent-1 claims root planning task within ~5s
- A `task_plan` appears (decomposes calculator into 2-3 implements + 1 merge)
- agents 2/3 claim implement tasks
- Total runtime ~5-15 min with Sonnet

**Signs it's wedged (abort):**
- No task transitions for >2 min
- Agents looping on the same task
- One agent repeatedly hitting redirect errors

Abort with `Ctrl-C` in each agent terminal, then `make -C demo demo-down`.

### Step 6 — Capture (before tear-down!)

```bash
make -C demo demo-capture DIR=runs/clean
make -C demo demo-status     # should show all tasks → done
make -C demo demo-replay-verify   # 3-way log replay invariant
```

Also save the 3 agent terminal scrollback (Ctrl-S / shell save) — that's
the qualitative narrative for the writeup that logs don't capture.

### Step 7 — Tear down

```bash
make -C demo demo-down
```

---

## Run #2 — Replica failure (pause + resume)

**Setup identical to Run #1** through Step 4. Then mid-execution:

```bash
# Wait until at least one implement task is in progress, then:
make -C demo demo-pause-1     # SIGSTOP replica-1
sleep 10
make -C demo demo-resume-1    # SIGCONT replica-1
```

**What to expect:** agent-2 (pinned to replica-1) sees 307 redirects or
transport errors, falls back via `TM_URL_LIST` to replicas 0/2. Swarm
continues. After resume, replica-1 catches up via the paxos log.

**Capture:**
```bash
make -C demo demo-capture DIR=runs/pause-resume
make -C demo demo-replay-verify
```

---

## Run #3 — Leader failure (SIGKILL the leader)

**Setup identical to Run #1** through Step 4. Then mid-execution:

```bash
# First identify the current leader by inspecting any replica:
curl -s localhost:8081/dump | jq '.leader_index'
# Say it returns 0. Then:
make -C demo demo-kill-0      # SIGKILL replica-0
```

**What to expect:** ~3s gap while the remaining 2 replicas elect a new
leader, then swarm continues with quorum on {replica-1, replica-2}.
Per CLAUDE.md hard-rule #5, the killed replica does NOT recover state
— this is the **survival** story, not the recovery story.

**Important:** with replica-0 dead, `demo-status` (which curls :8081)
will fail. Use `:8082` or `:8083` instead.

**Capture:**
```bash
make -C demo demo-capture DIR=runs/kill-leader
# replay-verify only works on surviving replicas
```

---

## Run #4 — Claude agent failure (/task-shutdown)

**Setup identical to Run #1** through Step 4. Then mid-execution, in
**agent-2 terminal**:

```
/task-shutdown
```

This exercises partner's new `task-kill` + `task-shutdown` skills. The
agent abandons its in-flight work, kills its tm-hb/tm-wait stragglers,
and exits cleanly. After `LEASE_WINDOW=45s` the dropped task gets
re-claimed by agent-1 or agent-3.

**What to expect:** ~45s gap while the lease expires, then surviving
agents pick up the dropped task and continue.

**Capture:**
```bash
make -C demo demo-capture DIR=runs/agent-shutdown
make -C demo demo-replay-verify
```

---

## Tear-down + reset between runs

```bash
make -C demo demo-down
rm -rf demo/work               # wipe bare repo + agent dirs
docker compose down -v         # also clear any cached volumes
```

For runs #2-4, you also want a fresh agent workspace:
```bash
rm -rf ../pset4-testing/test1
# then re-run /swarm-deploy ../pset4-testing/test1 in host-claude
```

---

## Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| `curl localhost:8081/dump` 5xx or hangs | replicas haven't formed quorum yet | `sleep 5`, retry |
| Agent stuck on "no such replica" | `TM_URL_LIST` not set in CLAUDE.md | `grep TM_URL_LIST ../pset4-testing/test1/agent-1/CLAUDE.md` — should list all 3 ports |
| `demo-seed` POST hangs | quorum not ready post-restart | `demo-seed` has retry loop; if it gives up, `make -C demo demo-down && demo-up && demo-seed` |
| `build/tm-server: cannot execute` | binary is Mach-O, not ELF | rebuild inside docker dev shell |
| After partition/reconnect: peer NXDOMAIN | docker DNS quirk | use `make -C demo demo-reconnect-N` (has `--alias`) |
| Run cost ballooning | left agents running too long | hard 20-min time-box; abort if wedged |
