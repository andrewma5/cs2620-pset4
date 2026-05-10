# pset4 — Distributed Task Manager for Coding Agents (Plan)

**Tagline:** A Paxos-replicated coordinator, exposed over MCP, that hands out
tasks and locks to multiple Claude Code agents. Agents create their own tasks
(plan tasks decompose into more tasks, recursively). Survives replica crashes
and reassigns work when an agent dies or stalls. Demo: 3 replicas in
docker-compose + 3 Claude CLIs cooperatively building a CLI calculator.

---

## 1. Goals and non-goals

**Goals.**
1. Correct task ownership: never two live agents working the same task; never
   a permanently-lost task while a quorum survives.
2. **Recursive planning.** A `plan` task can produce more `plan` and
   `implement` tasks. No depth limit (PoC).
3. **Dependency respect.** A task with `requires=[X,Y]` is only claimable when
   X and Y are `done`. No data pass-off through the coordinator — handoff goes
   through `.md` files committed to the predecessor's branch.
   **Git is the agent's responsibility, not the coordinator's.** The
   coordinator never touches a worktree, never invokes `git`, and the main
   lock is purely advisory — an agreement enforced by skill instructions, not
   by anything the coordinator does to the filesystem.
4. **Two failure axes survive.** Replica crash/partition/recovery (pset3
   territory). Agent crash/stall/zombie (new — wall-clock heartbeats +
   lazy lease expiry + per-task fencing).
5. **MCP-driven end-to-end.** Real Claude CLIs use 4 skills + 1 MCP server to
   loop on claim/work/complete autonomously.
6. **Two-tier testing.** Deterministic many-seed simulation for correctness;
   docker-compose + real Claudes for the qualitative integration story.

**Non-goals.**
- Not BFT.
- No on-disk state. A killed replica doesn't recover state in this PoC.
  Stretch: state transfer.
- No automatic data pass-off between tasks (filed under handoff `.md` files).
- No DAG scheduler optimization — claim is "first eligible task."
- No nested Claude processes — one Claude per agent, the agent runs everything
  in its own session.

---

## 2. Architecture

```
   Claude CLI #1                Claude CLI #2                Claude CLI #3
   (running task-loop skill)    (task-loop skill)            (task-loop skill)
   + heartbeat bg shell         + heartbeat bg shell         + heartbeat bg shell
        │ stdio (MCP)                │ stdio (MCP)                │ stdio (MCP)
        ▼                            ▼                            ▼
   tm-mcp shim (per-Claude)     tm-mcp shim                  tm-mcp shim
        │ HTTP/JSON                  │ HTTP/JSON                  │ HTTP/JSON
        ▼                            ▼                            ▼
   ┌── docker-compose network ──────────────────────────────────────┐
   │  ┌──── replica-1 ────┐  ┌──── replica-2 ────┐  ┌── replica-3 ──┐│
   │  │ HTTP front :8001  │  │ HTTP front :8002  │  │ HTTP :8003    ││
   │  │ Coordinator app   │  │ Coordinator app   │  │ Coordinator   ││
   │  │ Multi-Paxos+SM    │◄─┤ Multi-Paxos+SM    │◄─┤ Multi-Paxos   ││
   │  └───────────────────┘  └───────────────────┘  └───────────────┘│
   │       inter-replica raw TCP (length-prefixed paxos_message)     │
   └─────────────────────────────────────────────────────────────────┘
                                                            │
                                  bind-mount (host filesystem)
                                                            ▼
                                                    /work/repo.git
                                                    (bare repo, source of truth)
                                                    /work/worktrees/<task_id>/
                                                    (per-task worktrees, used
                                                     by tm-mcp shim only)

   Lease expiry is lazy — checked by the next task_claim or
   main_lock_acquire that touches the key. No periodic actor;
   no sweeper coroutine. Heartbeats write Unix-second timestamps
   (cot::steady_now() on the coordinator-leader); claimants
   compare to their own steady_now and CAS-clear stale owners.
```

The coordinator is implemented as a **client model on top of unmodified
pset3 Paxos**, in the same shape as `lockseq_model` in pset3: a coroutine
that translates higher-level RPCs (claim/heartbeat/complete/etc.) into
sequences of `pancy::request`s. The Paxos layer carries only `pancy::request`
payloads, exactly as it does today — no new command types, no SM extensions.
Multi-key operations like `task_complete` are best-effort multi-CAS sequences;
correctness comes from replay-safe ordering and lazy GC, not from atomicity
of the sequence itself (§11).

---

## 3. Data model

All state in pancydb. No multi-key atomicity assumed — every operation
touches one key. Buffered task creation gives us replay-safe two-phase
semantics for recursive decomposition without needing multi-key transactions
(§5, §11).

| Key                              | Value                                         | Notes |
| -------------------------------- | --------------------------------------------- | --- |
| `tasks/{id}/spec`                | JSON `{type:"plan"/"implement"/"merge", title, prompt, requires:[ids], parent_id?, branch_base?}` | written once |
| `tasks/{id}/status`              | `pending/in_progress/done/failed`             | only owner writes; CAS-fenced |
| `tasks/{id}/owner`               | `<agent_id> <random_token>` or empty          | the lock; CAS for safe takeover/release; absent ⇒ unclaimed |
| `tasks/{id}/heartbeat`           | int (Unix seconds at last heartbeat, leader-stamped) | refreshed by owner; next claimant compares to its own `cot::steady_now()` |
| `tasks/{id}/result_branch`       | git SHA (for completed implement tasks)       | written on `task_complete` |
| `tasks/{id}/result_summary`      | string                                        | what the agent says they did |
| `tasks/{id}/buffered_children`   | JSON `[{tmp_id, spec, ...}, ...]`             | accumulator; flushed on `task_complete` |
| `tasks/{id}/fail_reason`         | string                                        | only on `failed` |
| `meta/task_index`                | JSON list of all task ids                     | rarely-mutated index |
| `meta/main_lock`                 | `<agent_id> <task_id> <random_token>` or empty | held briefly during merge; lease piggybacks the holder's merge task lease (no separate heartbeat) |
| `agents/{aid}/label`             | human-readable name                           | for logs only |

**Lock pattern (lifted from `lockseq_model`).** The owner value is
`agent_id` + a random token. The CAS that claims a task returns a version;
that version is the **fencing token**, included in every subsequent
status-mutating call from the owner. Mismatch ⇒ `fenced` error.

**Why buffered children.** When a `plan` task spawns 5 children, we don't
write them to `meta/task_index` immediately. We accumulate them in
`tasks/{parent}/buffered_children` and flush them as a multi-CAS sequence on
`task_complete` (write each child's `tasks/{id}/spec` first, then update
`meta/task_index` last — see §11 for the partial-flush story). If the
planner crashes mid-decomposition, the lease lapses, the next claimer finds
`buffered_children` cleared and starts fresh; any orphan `tasks/{id}/spec`
keys from a partial flush get GC'd lazily on the next claim attempt that
notices them. Same rule for any task creating any task — recursion-friendly.

---

## 4. Task types and lifecycle

Three types. All share the same state machine; they differ in what the
agent's skill tells them to do while owning the task.

**`plan` task.** Read the prompt. Produce a plan. Decompose into children
(further `plan`s or `implement`s) via `task_create` (buffered). Write a brief
plan summary as the result. Complete.

**`implement` task.** The agent itself does the git work — `git fetch`,
`git worktree add` against `spec.branch_base`, merge in any predecessor
branches listed in `spec.requires`. The skill walks them through it. Agent
edits, commits to the work branch, calls `task_complete` with the work
branch's tip SHA as a string. The completion **automatically enqueues a
synthetic `merge` task** with `requires=[this]`. The coordinator only
stores the SHA string; it never invokes git.

**`merge` task.** Created by the coordinator on completion of any
`implement` task. The agent claims it, calls `main_lock_acquire` (polls
until granted), checks out main + the implement task's branch, merges,
runs configured sanity tests (e.g. `pytest -q`), pushes, calls
`main_lock_release`, completes. Conflicts: the merging agent resolves them
in-place; if it gives up, it `task_fail`s and a downstream re-decomposition
is the human's call (we don't auto-retry).

```
         pending ──claim──> in_progress ──complete──> done
            ▲                    │
            │                    └─lease lapse─┐
            │             (lazy: next claimant │
            │              CAS-clears stale    │
            │              owner)              │
            └──────────────────────────────────┘
```

A reclaimed-because-stalled task goes back to `pending` with empty owner;
its `buffered_children` is also cleared (they belonged to the failed
attempt). The next claimer starts fresh.

---

## 5. Coordinator RPCs (the MCP surface)

All exposed as MCP tools through the `tm-mcp` shim. Each maps to one HTTP
call to a replica; non-leader replicas reply 307 with the leader's URL.

- `task_create(spec)` → `{tmp_id}` — buffered if called from inside a task
  (uses MCP-call-context to know whose buffer); immediate if called by the
  human via `tm-cli`. **Depth-capped:** the coordinator computes
  `spec.depth = parent.depth + 1` (root tasks created via `tm-cli` are
  depth 0) and rejects with `{depth_exceeded: true}` if depth > 5. Prevents
  a buggy planner from livelocking the system with infinite recursion.
- `task_list({status?, mine?})` → `[{id, type, title, status, owner?}]`
- `task_claim({prefer_type?})` → `{id, spec, fencing_token}` or
  `{none: true}`. Coordinator picks the first task whose `requires` are all
  `done` and `owner` is empty *or expired*. "Expired" means
  `now() - tasks/{id}/heartbeat > LEASE_WINDOW` (15s). When claim sees an
  expired non-empty owner, it CAS-clears the owner first (using the stale
  string as expected value), then CAS-installs the new owner. Same pass
  also lazy-GCs orphan child specs whose tmp_id isn't in `meta/task_index`
  and whose creator is `done`/`failed`. The agent (guided by the skill)
  does any git/worktree setup itself after claiming.
- `task_heartbeat({id, fencing_token})` → `{ok}` — coordinator validates
  the fencing token against `tasks/{id}/owner`'s current version, then
  writes `tasks/{id}/heartbeat = now()` (the leader stamps the wall-clock
  timestamp; the value is just data flowing through pancydb). (The
  background shell calls this; see §7.)
- `task_complete({id, fencing_token, result_branch?, result_summary})`
  → `{ok}`. Multi-CAS sequence executed by the coordinator coroutine on
  the leader, in this order: (1) for each buffered child, CAS-write
  `tasks/{tmp_id}/spec`; (2) CAS-append the new tmp_ids to
  `meta/task_index`; (3) for `implement` tasks, CAS-write the synthetic
  `merge` task's spec and append to index; (4) CAS-write `result_*`;
  (5) CAS `status=done`; (6) CAS-clear `owner` (fenced by `fencing_token`).
  Each step is individually atomic; the *sequence* is not. A crash mid-way
  leaves recoverable state — see §11 for which intermediate states are
  possible and how the next claim heals them.
- `task_fail({id, fencing_token, reason})` → `{ok}`. Discards
  `buffered_children`, records `fail_reason`, sets `status=failed`.
- `main_lock_acquire({task_id, fencing_token})` → `{ok, lock_token}` or
  `{busy: true}`. Client-side poll. **Advisory only:** the coordinator
  serializes who *holds* the token via Paxos/CAS, but it does nothing to
  prevent a misbehaving agent from pushing to main without it. Skills
  enforce the discipline.
- `main_lock_release({lock_token})` → `{ok}`. CAS-checked.

All RPCs from a non-owner (wrong agent_id or wrong fencing token) get
`{fenced: true}`. The skill instructs the agent to drop the task and loop.

---

## 6. Leases and fencing

**Lazy wall-clock leases — no sweeper, no logical tick.** Heartbeats write
`tasks/{id}/heartbeat = now()` (Unix seconds, stamped by the
coordinator-leader's `cot::steady_now()`). Lease expiry is checked
*lazily*, by the next client RPC that cares about ownership — primarily
`task_claim`, secondarily `main_lock_acquire`. There is no periodic
sweeper coroutine, no `meta/sweep_tick` key, and no leader-handoff
machinery for cleanup. Same-host docker-compose deployment + simulated
single-event-loop tier-1 means all clocks are aligned to within
sub-millisecond, so a generous lease window (`LEASE_WINDOW = 15s`)
absorbs any realistic jitter.

**The lazy-expiry pattern, applied to `task_claim`:**
1. Read `tasks/{id}/owner`. If empty, proceed to claim.
2. If non-empty, read `tasks/{id}/heartbeat`. If
   `now() - heartbeat > LEASE_WINDOW`, the lease is stale.
3. CAS-clear `tasks/{id}/owner` using the stale owner string as expected
   value. If this CAS fails (someone else got there first, or a fresh
   heartbeat just landed), back off and re-attempt the whole claim.
4. CAS-install the new owner. The version returned is the new fencing
   token.

The CAS in step 3 is the only race-sensitive write. It's safe because
pancydb CAS is atomic w.r.t. the replicated log: if a fresh heartbeat
landed concurrently, the owner key's version moved forward and our
"clear" CAS fails cleanly.

**`main_lock_acquire` uses the same pattern.** Read `meta/main_lock`; if
held, look up the holder's merge-task heartbeat; if stale, CAS-clear the
lock and proceed. The main lock lease piggybacks the holder's merge task
lease — there's no separate `meta/main_lock/heartbeat` key (§3).

**Fencing rules.**
- Every status-mutating RPC carries `fencing_token = version of
  tasks/{id}/owner at claim time`. The coordinator CAS's the status write
  with `version_match = fencing_token`. Wrong version ⇒ `fenced`.
- `main_lock_release` carries `lock_token = version of meta/main_lock at
  acquire time`. Same CAS pattern.
- `task_heartbeat` validates the fencing token against
  `tasks/{id}/owner` before writing the new timestamp. Worst-case stale
  timestamp from a TOCTOU race extends a stolen lease by at most one
  heartbeat interval (5s), well inside `LEASE_WINDOW`.
- **Per-task fencing only.** A fenced agent is *not* permanently banned.
  It loops back, calls `task_claim` again, gets fresh work. (Permanent
  agent fencing was considered and rejected — flaky networks would
  falsely permaban.)

**Why two-leader races don't break things.** Briefly two replicas
believe they're leader (split-brain). Both run coordinator coroutines
that emit pancy CAS requests on behalf of clients. Each CAS goes through
Paxos like any other write; only one wins per slot, and the loser's
stale Paxos round number gets rejected at the protocol layer. The
ownership invariant is preserved because pancydb CAS is atomic w.r.t.
the replicated log.

---

## 7. Agent side: heartbeats, fencing, the loop

**Heartbeats via background shell.** Claude can spawn background shell
commands. The `task-loop` skill instructs Claude, on successful claim, to
launch a background shell:

```bash
while true; do
  curl -s -X POST $LEADER/task_heartbeat \
    -d "{\"id\":\"$TASK_ID\", \"fencing_token\":$FENCING_TOKEN}" \
    || true
  sleep 5
done
```

When Claude exits, the shell exits with it (this is the Claude Code default
behavior we're relying on; if it turns out unreliable we'll revisit). When
Claude calls `task_complete` or `task_fail`, the skill instructs it to kill
the heartbeat shell first.

**The loop.**
```
while true:
  resp = task_claim(prefer_type="implement" if I just merged else None)
  if resp.none:
    sleep 10s; continue
  start_heartbeat_shell(resp.id, resp.fencing_token)
  if resp.spec.type == "plan":      invoke task-planning skill
  if resp.spec.type == "implement": invoke task-implementing skill (sets up worktree itself)
  if resp.spec.type == "merge":     invoke task-merging skill (does git itself)
  on success: task_complete(...)
  on fenced:  drop everything, continue loop
  on failure: task_fail(reason)
  kill heartbeat shell
```

**Dealing with `fenced`.** Any MCP call returning `fenced` ⇒ the
`task-loop` skill instructs the agent to immediately abandon the current
worktree (don't commit, don't push), kill the heartbeat shell, and `continue`
the outer loop. Fenced means another agent has been assigned this task; our
work is wasted but causes no harm.

---

## 8. Skills inventory (4 skills)

Each skill is a markdown file the agent reads when it enters that phase. I'll
write them in `pset4/skills/` and ship them alongside the MCP server config.

### 8.1 `task-loop`
- **When:** the user runs `/task-loop` (or just says "join the swarm"). Always
  on for the lifetime of this Claude session.
- **Contents:** the loop pseudocode in §7, fully spelled out. Concrete
  commands for launching the heartbeat shell. Concrete handling of the
  three task types (delegate to the other three skills). Explicit
  instructions for `fenced` responses. How to print progress so the human
  watching can follow along.

### 8.2 `task-planning`
- **When:** invoked by `task-loop` after claiming a `plan` task.
- **Contents:** read the spec's prompt. Think about decomposition. Rules
  for what makes a good child task:
  - A child task should be ~30 min of work for a competent agent.
  - Each child must have an unambiguous acceptance criterion.
  - Children that share state must be ordered via `requires`.
  - Implementation children must specify `branch_base` (usually `main`,
    or the predecessor's branch).
  - Plan children are fine — recursion is OK.
  - Pass-off between siblings happens through `.md` files in the
    predecessor's branch (e.g., `HANDOFF.md` describing the API).
- Call `task_create` for each child (these are buffered). Write a short
  plan summary. Call `task_complete`.

### 8.3 `task-implementing`
- **When:** invoked by `task-loop` after claiming an `implement` task.
- **Contents:** the skill walks Claude through the git setup explicitly:
  `git fetch`, `git worktree add ../wt-<task_id> -b task/<task_id>
  <branch_base>`, `cd ../wt-<task_id>`, then for each `dep` in
  `spec.requires` look up `tasks/{dep}/result_branch` and `git merge` it
  in. Read any `HANDOFF.md` files from predecessors. Implement. Commit.
  Run local tests if any exist. Write a `HANDOFF.md` describing what you
  did for downstream tasks. If during implementation you discover follow-up
  work, call `task_create` — it's buffered. Call `task_complete` with your
  branch tip SHA. On `fenced` or failure, abandon the worktree (don't
  bother cleaning it up — the human can `git worktree prune` later).

### 8.4 `task-merging`
- **When:** invoked by `task-loop` after claiming a synthetic `merge` task.
- **Contents:**
  - Call `main_lock_acquire`. Poll with backoff while busy (concrete:
    sleep 2s, 4s, 8s, capped at 30s).
  - Once acquired: `git fetch`, `git checkout main`, `git merge
    <implement-branch-sha>`. Resolve conflicts if any (the skill includes
    a short conflict-resolution playbook).
  - Run sanity tests (`pytest -q` or whatever the repo configures).
  - `git push`.
  - Call `main_lock_release`.
  - Call `task_complete`.
  - If conflicts can't be resolved cleanly: `main_lock_release`, then
    `task_fail` with the reason. Do **not** push partial state.
- The skill branches on local-vs-remote based on a config value. Same
  workflow either way; only the `git remote` URL changes.

---

## 9. Git is the agent's job

The coordinator and shim do **no git work at all**. Git is just what
agents happen to use to do their work; the coordinator is task-management
infrastructure that would work equally well for non-code tasks. Keeping
git out of the coordinator means:

- The shim is tiny — just an HTTP client wrapping MCP tool calls.
- Coordinator failures can't corrupt a worktree.
- Git failures can't take down the coordinator.
- The system is reusable for any kind of task, not just code.

**What the agent (via skills) is responsible for:**
- `git fetch` to refresh refs.
- `git worktree add` for the per-task worktree (any layout it likes; we
  suggest `../wt-<task_id>`).
- Merging predecessor branches before starting (skill walks through it).
- Committing work to a per-task branch.
- Recording the branch tip SHA in `task_complete`.
- For merge tasks: acquiring the (advisory) main lock, fetching, merging
  the implement branch into main, running tests, pushing, releasing.

**Configuration: just a `GIT_REMOTE` env var the human sets** when
launching Claude. Local path or remote URL — skills don't care. Demo
uses a local bare repo at `/work/repo.git`. Stretch demo points at a
real GitHub repo with no code changes.

**Credentials:** whoever ran the Claude has the necessary git
credentials. The coordinator never sees them.

**Worktree leaks** (agent crashes, worktree never cleaned): acceptable
PoC behavior. The human runs `git worktree prune` between demo runs;
`make demo-clean` does this for the demo.

---

## 10. Transport: bridging pset3 sim → real network

**The split.** Pset3 Paxos uses `netsim::channel`/`netsim::port`. We need
both: simulation transport for tier-1 tests (deterministic many-seed),
real TCP for the docker demo. Introduce a `transport` interface:

```c++
template <typename T>
struct transport {
  virtual cot::task<>     send(size_t target, T msg) = 0;
  virtual cot::task<T>    receive() = 0;
  virtual void            set_loss(size_t target, double rate) = 0;
};
```

Two impls. `sim_transport` wraps `netsim`. `tcp_transport` uses Cotamer's
HTTP/socket support (or hand-rolled length-prefixed framing on top of
`cotamer/io.hh` if the new HTTP support is rough on Windows). The
`pt_paxos_replica` becomes generic over `transport<paxos_message>`.

**Inter-replica protocol** is raw framed binary — we don't need HTTP
between replicas. **Client→leader is HTTP/JSON** because that's what the
MCP shim speaks.

**Fallback if Cotamer HTTP is rough.** Hand-roll a minimal HTTP/1.1
subset on top of `cotamer/io.hh`. ~200 lines. Drop HTTPS entirely; demo
is localhost so it doesn't matter.

---

## 11. Failure handling — what we test

**Replica/Paxos-layer failures (re-exercised from pset3).**
- Permanent leader crash (`docker compose kill` — sim equivalent: 100% loss).
  Survivors elect new leader, continue.
- Temporary replica unavailability + recovery (`docker compose pause`/
  `unpause`). The replica's process state survives, so Paxos resumes
  cleanly.
- Symmetric partition / split-brain (`docker network disconnect`/`connect`).
  Old leader loses majority and stops committing; majority elects new leader;
  old leader rejoins, learns it's stale.

**Important caveat: replicas that are `kill`'d don't recover state in this
PoC.** Pset3 punts on state-loss recovery, and we're not implementing
state transfer. So `kill`-then-`start` produces a *fresh* replica that
violates Paxos's no-state-loss assumption. The demo uses `kill` only for
"survival" stories (don't restart) and `pause` for "recovery" stories.
Listed in §15 as known limitation.

**Agent-layer failures (new for pset4).**
- Agent killed mid-task (`SIGKILL` the Claude). Heartbeats stop.
  `tasks/{id}/heartbeat` ages past `LEASE_WINDOW`. The next agent that
  calls `task_claim` notices and CAS-clears the owner before claiming.
  Buffered children from the dead attempt are discarded by that same
  claim path. End state: task `done` exactly once.
- Agent stalled mid-task (`SIGSTOP`). Heartbeats stop. Same recovery as
  kill.
- **Zombie agent** (stalled, lease expired, then unstalls and tries to
  call `task_complete`). Fenced. Drops to outer loop, claims a fresh
  task. The recently-completed task (by the new owner) stays done.
- Agent crashes during merge (while holding `meta/main_lock`). The lock
  lease piggybacks the merge task's heartbeat — when the next
  `main_lock_acquire` call sees the holder's merge task heartbeat is
  stale, it CAS-clears the lock as part of acquiring it.
- Network partition between agent and leader. Heartbeats fail to land
  ⇒ lease ages out ⇒ next claimant takes over. Original agent eventually
  reconnects, sees `fenced`, drops.

**Cross-product cases tested at least once.**
- Leader fails *while* an agent is mid-claim (CAS may or may not have
  committed). Idempotent claim under retry must not double-assign.
- Agent crashes during `task_complete`. The multi-CAS sequence in §5 is
  not atomic, so a partial flush is possible. The flush order is chosen
  so partial states are recoverable: child `tasks/{tmp_id}/spec` keys are
  written before `meta/task_index` is updated, so a partial flush leaves
  spec keys *with no index entry* (orphans) but never the reverse (an
  index pointing at a missing spec). Healing: the next `task_claim` pass
  detects orphan specs whose creator parent is `done`/`failed` and
  CAS-removes them. If the parent is still in `in_progress`, its lease
  ages out, the next claimant CAS-clears the owner and the buffered
  children, and the orphan specs get GC'd as above. Either way, end
  state has no dangling references.

---

## 12. Testing — two tiers

### Tier 1: deterministic simulation (most of the correctness signal)

Mirrors pset3's `try_one_seed` structure. Synthetic agents are coroutines
implementing the loop in §7 against `sim_transport`. The "agent's
implementation work" is mocked — they don't actually edit files; they
just sleep for some sim-time, optionally fail, optionally create
buffered children, then `task_complete`.

**End-of-run invariants:**
1. Every task in `meta/task_index` reaches a terminal state.
2. No two distinct fencing tokens were ever simultaneously held against
   the same task (audit-log check).
3. Every replica's Pancydb agrees (pset3-style `diff` with version skew).
4. For every `done` task: `requires` were all `done` at claim time
   (audit-log check).
5. `meta/main_lock` was never held by two distinct tokens simultaneously.
6. No `buffered_children` orphan: every entry in some flushed children
   list either appears in `meta/task_index` or was discarded with the
   parent's failure.

**Failure schedules** (bring forward pset3's three; add three new):
- `fail_leader_permanent`, `fail_leader_temporary`, `fail_split_brain`
  from pset3.
- `kill_agent` — randomly stops one synthetic agent's heartbeats.
- `stall_agent` — delays one agent's next call by > lease.
- `agent_during_leader_change` — combines the two.

Run each at 1000+ seeds with loss rates 0, 0.01, 0.05.

### Tier 2: docker-compose + real Claude integration (the demo)

`make demo-up` starts 3 replicas in compose. `make demo-init` wipes and
seeds the bare repo and the coordinator (creates the root `plan` task
"build a CLI calculator with +, -, *, / and a REPL"). Three real Claude
CLIs in three terminals, each running `/task-loop`.

**Demo failure injections (Makefile targets).**
- `make demo-pause-1` — `docker compose pause replica-1`. Replica
  freezes; agents observe it as unreachable. **Recovery story.**
- `make demo-resume-1` — `docker compose unpause replica-1`. Replica
  resumes with full state, catches up.
- `make demo-kill-1` — `docker compose kill replica-1`. Replica gone
  for the rest of this run. **Survival story.** No restart in this
  demo — survivors keep serving.
- `make demo-partition-1` — `docker network disconnect demo_default
  replica-1`. Replica alive but isolated. **Split-brain story.**
- `make demo-reconnect-1` — `docker network connect demo_default
  replica-1`.
- `make demo-stall-claude` — instructions to manually `SIGSTOP` one
  Claude and observe its task get reassigned.

The integration tier is **qualitative** — we capture logs and one or two
screenshots for the writeup, not a perf number. Tier 1 carries the
correctness story.

**Stretch demo: chess engine or URL shortener** as a more impressive
recursive-decomposition target. CLI calculator is the baseline.

---

## 13. Build order (~15 hours, risk front-loaded)

Each step ends in something runnable. Decision points marked.

1. **(2h) Coordinator as client model on top of unmodified pset3 Paxos.**
   New `pset4/coordinator.cc`. Implements §5 RPCs as functions that emit
   `pancy::request`s on existing channels. Tier-1 sim tests work
   immediately.
2. **(2h) Lease/heartbeat/fencing semantics.** `task_heartbeat` writes
   wall-clock timestamp; `task_claim` does the lazy-expiry CAS-clear
   pattern (§6). Fencing-token CAS on every status-mutating RPC. First
   few invariant checks. Synthetic agent harness (no failures yet).
3. **(2h) Buffered task creation + recursive planning + dependencies.**
   The `task_complete` multi-CAS flush (specs first, index last) and
   `requires` check in `task_claim`. Depth cap (≤5) on `task_create`.
   Orphan-spec lazy GC piggybacked on `task_claim`. Synthetic planner
   agents that create buffered children.
4. **(2h) Failure schedules + 1000-seed clean run.** Port pset3's three;
   add `kill_agent`, `stall_agent`, `agent_during_leader_change`. Drive
   to clean.
5. **(2h) Transport abstraction + raw TCP impl.** Coordinator runs as a
   real process bound to a port. Length-prefixed framing on
   `cotamer/io.hh` if Cotamer HTTP isn't ready.
6. **(1h) HTTP front + MCP shim.** Hand-rolled HTTP/1.1 minimal subset
   if needed. The shim is now tiny — no git work, just an HTTP client
   wrapping MCP tool calls (~150 lines Python/Node).
7. **(1h) Skills.** Write the four markdown skill files. Iterate on
   prompts until a single Claude can run the loop end-to-end against one
   replica.
8. **(1h) docker-compose + Makefile.** Three replicas + bind-mounted bare
   repo. Make targets for each failure injection.
9. **(1h) Integration demo run + writeup capture.** Run the demo, record
   a session, capture screenshots/logs.
10. **(2h) Writeup + lab notebook polish.** §16 outline, including the
    inevitable bug story.

Slack: ~2h built into the budget. If we need the May-13 extension,
priority order for things we'd add: state transfer (real recovery),
GitHub-remote demo, performance metrics.

---

## 14. Resolved design decisions

- **`task_complete` flush order.** Child `tasks/{tmp_id}/spec` keys are
  written first (each its own CAS), then `meta/task_index` is updated
  last. A partial flush leaves orphan spec keys with no index entry.
  **Healing:** lazy GC piggybacked on `task_claim` — when a claim runs,
  it scans for orphan spec keys whose creator parent is `done`/`failed`
  and CAS-removes them. Cheap (constant overhead per claim) and avoids
  a periodic GC actor.
- **Worktree cleanup.** Agents own worktrees; on agent death they leak
  until `git worktree prune`. Acceptable PoC behavior; `make demo-clean`
  prunes between demo runs.
- **`task_create` from a fenced agent.** From the human via `tm-cli`,
  immediate write is fine. From an agent that's already been fenced,
  the buffered-children write goes into a buffer that will never be
  flushed (parent task is reclaimed). Acceptable — the children leak,
  the next-claim orphan GC cleans them up.

---

## 15. Risks and mitigations

| Risk | Likelihood | Mitigation |
| --- | --- | --- |
| Cotamer HTTP support late/buggy on Windows | Med | Hand-roll HTTP/1.1 subset on `cotamer/io.hh` |
| pset3 Paxos has latent bug exposed by new traffic patterns | Med | Run 10k+ seeds tier-1 early; keep pset3 regression seeds |
| Real Claude CLIs are non-deterministic; integration test flaky | High | Tier 2 is qualitative; correctness rests on tier 1 |
| Background-shell heartbeat doesn't survive Claude session lifecycle as expected | Med | We assume the shell exits with Claude. If wrong, fall back to a separate sidecar process tied to a PID Claude reports |
| Zombie agent's stale `task_complete` writes data after fencing | Low | Every status write is CAS-fenced — verify in tier-1 |
| `kill`-restart of a replica violates Paxos no-state-loss | High (by design) | Document as known PoC limitation; demo only `pause`/`partition` for recovery; document state transfer as future work |
| 15h budget overrun | Med | Step 10 slack; pset extension to May 13 with check-in |

---

## 16. Writeup outline (4–6 pages)

1. **Problem & motivation.** Multiple coding agents, why centralized
   coordination matters even in an agent-per-task world, why fault
   tolerance matters when the agents themselves are flaky.
2. **Design.** Architecture (§2), data model (§3), task lifecycle (§4),
   leases/fencing (§6), the agent loop and skills (§7–8). Diagrams.
3. **Implementation.** The coordinator-as-client-model trick (Paxos
   stayed untouched). Transport split (§10). Buffered children for
   recursion safety (§3, §11).
4. **Testing.** Invariants (§12), failure schedules, results table
   (seeds run, failures found, what we fixed). At least one war-story
   bug à la pset3 TURNIN.md.
5. **Limitations & future work.** No state-transfer (so killed replicas
   don't recover); no MVCC; no real cross-cell transactions; no
   adaptive scheduling; brief gesture at what BFT would change.
6. **References.** Multi-Paxos lecture notes, Chubby (closest prior art
   for lease-based locking), MCP spec, the pset3 writeup.
