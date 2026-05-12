# pset4 — Distributed Task Manager for Coding Agents (Plan)

**Tagline:** A Paxos-replicated task manager, exposed over plain HTTP/JSON,
that hands out tasks and locks to multiple Claude Code agents. Agents create
their own tasks (plan tasks decompose into more tasks, recursively).
Survives replica crashes and reassigns work when an agent dies or stalls.
Demo: 3 replicas in docker-compose + 3 Claude CLIs cooperatively building
a CLI calculator.

> **Companion document:** [`docs/superpowers/specs/2026-05-06-pset4-poc-design.md`](../docs/superpowers/specs/2026-05-06-pset4-poc-design.md)
> spells out the **single-instance POC** (the first build step) in detail.
> This PLAN.md describes the full **replicated** target. The two are
> aligned: the POC's state machine is the same one Paxos will replicate, so
> the diff between POC and replicated is mechanical (swap `tm-server`'s
> direct-call dispatch for `pt_paxos_replica<tm::request, tm::response,
> tm::task_manager_db>`).

---

## 1. Goals and non-goals

**Goals.**
1. Correct task ownership: never two live agents working the same task; never
   a permanently-lost task while a quorum survives.
2. **Recursive planning.** A `plan` task can produce more `plan` and
   `implement` tasks. Depth-capped at 5 to prevent runaway recursion.
3. **Dependency respect.** A task with `requires=[X,Y]` is only claimable when
   X and Y are `done`. No data pass-off through the task manager — handoff
   goes through `.md` files committed to the predecessor's branch.
   **Git is the agent's responsibility, not the task manager's.** The task
   manager never touches a worktree, never invokes `git`, and the main
   lock is purely advisory — an agreement enforced by skill instructions,
   not by anything the task manager does to the filesystem.
4. **Two failure axes survive.** Replica crash/partition/recovery (pset3
   territory). Agent crash/stall/zombie (new — wall-clock heartbeats +
   lazy lease expiry + per-task fencing).
5. **HTTP/JSON-driven end-to-end.** Real Claude CLIs use 4 skills that drive
   the loop with `curl` directly, no separate shim process.
6. **Two-tier testing.** Deterministic many-seed simulation for correctness;
   docker-compose + real Claudes for the qualitative integration story.

**Non-goals.**
- Not BFT.
- No on-disk state. A killed replica doesn't recover state in this PoC.
  Stretch: state transfer.
- No automatic data pass-off between tasks (filed under handoff `.md` files).
- No DAG scheduler optimization — claim is "first eligible task" in
  creation order.
- No nested Claude processes — one Claude per agent, the agent runs
  everything in its own session.
- No MCP shim in the first build. Skills speak to the task manager via
  `curl`. An MCP shim can be added later if curl ergonomics get painful;
  the wire protocol is plain JSON and won't change.

---

## 2. Architecture

```
   Claude CLI #1                Claude CLI #2                Claude CLI #3
   (running task-loop skill)    (task-loop skill)            (task-loop skill)
   + heartbeat bg shell         + heartbeat bg shell         + heartbeat bg shell
        │ HTTP/JSON (curl)            │ HTTP/JSON (curl)           │ HTTP/JSON (curl)
        ▼                            ▼                            ▼
   ┌── docker-compose network ──────────────────────────────────────┐
   │  ┌──── replica-1 ────┐  ┌──── replica-2 ────┐  ┌── replica-3 ──┐│
   │  │ HTTP front :8001  │  │ HTTP front :8002  │  │ HTTP :8003    ││
   │  │ task_manager_db   │  │ task_manager_db   │  │ task_manager_db││
   │  │ + Multi-Paxos     │◄─┤ + Multi-Paxos     │◄─┤ + Multi-Paxos ││
   │  └───────────────────┘  └───────────────────┘  └───────────────┘│
   │       inter-replica raw TCP (length-prefixed paxos_message)     │
   └─────────────────────────────────────────────────────────────────┘
                                                            │
                                  bind-mount (host filesystem)
                                                            ▼
                                                    /work/repo.git
                                                    (bare repo, source of truth)
                                                    /work/wt-<task_id>/
                                                    (per-task worktrees, owned
                                                     and managed by agents only)

   Lease expiry is lazy — checked by the next task_claim or
   main_lock_acquire that touches the key. No periodic actor;
   no sweeper coroutine. Heartbeats write Unix-second timestamps
   stamped by the leader; claimants compare to the timestamp the
   leader stamps on their own request and update ownership atomically.
```

The task manager is implemented as a **typed state machine** (`tm::task_manager_db`)
with an atomic `process_req(request, now_unix) → response`. pset3's
Multi-Paxos implementation already treats client requests as opaque blobs —
the writeup's words: *"your Paxos implementation… need not understand
Pancydb messages or semantics… it will treat Pancydb requests as opaque
blobs."* So Paxos is parameterized over a state machine; we plug in
`task_manager_db` instead of `pancydb`. The Paxos protocol code from
`pset3/pt-paxos.cc` is unchanged; only the type carried in
`paxos_message::values` and the SM at `db_.process_req()` change.

This is a different and simpler architecture than an earlier draft of this
plan, which had a "coordinator coroutine" emit pancy CAS sequences against
a replicated pancydb. The pancydb-on-top approach forced multi-key CAS
sequences (e.g. `task_complete` touched six keys) that weren't atomic and
required a partial-flush recovery story. With a typed SM, every operation
is one atomic transition — that complexity is gone entirely. See the POC
spec doc §2 for the full reasoning.

---

## 3. Data model

All state is in-memory inside `tm::task_manager_db`. Every operation is
one atomic transition; there are no multi-key sequences and no recovery
state.

```c++
namespace tm {

enum class task_type    { plan, implement, merge };
enum class task_status  { pending, in_progress, done, failed };

using fencing_token = uint64_t;   // monotonic, 0 = unset
using task_id       = std::string; // SM-assigned, e.g. "t/0001"

struct task_spec {
    task_type type;
    std::string title;
    std::string prompt;
    std::vector<task_id> requires;
    std::optional<task_id> parent_id;
    std::optional<std::string> branch_base;            // for implement/merge
    std::optional<std::string> implement_branch_sha;   // for merge tasks
    int depth = 0;
};

struct task_record {
    task_id id;
    task_spec spec;
    task_status status = task_status::pending;
    std::string owner_agent;          // empty = unowned
    fencing_token owner_token = 0;    // 0 = unowned; nonzero monotonic
    int64_t heartbeat_unix = 0;       // last heartbeat wall-clock
    std::optional<std::string> result_branch;
    std::optional<std::string> result_summary;
    std::optional<std::string> fail_reason;
};

struct main_lock_state {
    std::string holder_agent;     // empty = unheld
    task_id holder_merge_task;    // task whose lease backs this lock
    fencing_token lock_token = 0; // monotonic
};

class task_manager_db {
    std::map<task_id, task_record> tasks_;
    std::vector<task_id> task_order_;     // creation order for "first eligible"
    main_lock_state main_lock_;
    fencing_token next_token_ = 1;
    uint64_t next_task_seq_ = 1;
public:
    response process_req(const request&, int64_t now_unix);
    // ... read-only inspectors for diff/dump
};

}
```

**Lock pattern.** When a task is claimed, the SM mints a fresh
`owner_token = next_token_++` and stores it on the task. That token is
returned to the agent and required on every subsequent status-mutating
RPC. Mismatch ⇒ `fenced` error.

**No buffered children.** Recursive `task_create` calls append the new
child to `tasks_` immediately as `pending`. If the parent later fails,
the children stay (other agents claim them and may decide they're
obsolete — `task_fail` with a reason). Different from a prior draft
that buffered children pending parent completion; the simpler model is
both easier to implement and preserves planning work across crashes.

**Determinism.** `process_req` is a pure function of `(state, request,
now_unix)`. The wall-clock is part of the input — the leader stamps
`now_unix` when proposing, and that value is replayed identically on
every replica. This is the standard SMR trick for incorporating real
time into a deterministic state machine.

---

## 4. Task types and lifecycle

Three types. All share the same state machine; they differ in what the
agent's skill tells them to do while owning the task.

**`plan` task.** Read the prompt. Produce a plan. Decompose into children
(further `plan`s or `implement`s) via `task_create`. Write a brief
plan summary as the result. Complete.

**`implement` task.** The agent itself does the git work — `git fetch`,
`git worktree add` against `spec.branch_base`, merge in any predecessor
branches listed in `spec.requires`. The skill walks them through it. Agent
edits, commits to the work branch, calls `task_complete` with the work
branch's tip SHA as a string. The completion **automatically enqueues a
synthetic `merge` task** with `requires=[this]`. The task manager only
stores the SHA string; it never invokes git.

**`merge` task.** Created by the task manager on completion of any
`implement` task. The agent claims it, calls `main_lock_acquire` (polls
until granted), checks out main + the implement task's branch, merges,
runs configured sanity tests (e.g. `pytest -q`), pushes, calls
`main_lock_release`, completes. Conflicts: the merging agent resolves them
in-place; if it gives up, it `task_fail`s and a downstream re-decomposition
is the human's call (we don't auto-retry).

```
         pending ──claim──> in_progress ──complete──> done
            ▲                    │            (may carry new_children
            │                    │             with or without
            │                    │             result_branch)
            │                    │
            │                    └─lease lapse─┐
            │             (lazy: next claimant │
            │              transitions stale   │
            │              owner back to       │
            │              pending atomically) │
            └──────────────────────────────────┘
                                 │
                                 └──fail (rare; halts swarm)──> failed
```

A reclaimed-because-stalled task goes back to `pending` with empty owner.
Any children it created earlier remain (they're useful to whoever picks up
the task next — they may be relevant or may be marked obsolete, agent's
call).

**`task_complete` is the dominant outcome — including for blocked
work.** An agent that hits a blocker (merge conflict, scope too large,
predecessor's API doesn't match what the prompt assumed, post-merge
test failure, transient lock contention) hands off the next unit of
work as `new_children` on `task_complete`. The `result_branch` is
**optional**; an implement that completes without one (mid-flight
pivot, scope decomposition, verify-only task) does not synthesize a
merge task. The swarm continues making progress.

**`task_fail` is genuinely terminal — and it halts the entire swarm.**
The first call to `task_fail` sets a swarm-halt flag in the SM. From
that point on, `task_claim` returns `none + halted=true` and surfaces
the halt cause to the agent loop, which exits. In-progress peers are
NOT disturbed — they keep heartbeating and can still complete or fail
their current task. Only NEW claims are gated. A human investigates,
calls `/swarm_resume`, and re-launches the agents. Use `task_fail`
only when the goal is truly unreachable (contradictory prompt,
missing environment, corrupt repo); reason field starts with
`ABANDON:`.

---

## 5. Task manager RPCs (the HTTP surface)

All exposed as `POST /<rpc_name>` with a JSON body, plus `GET /dump` for
debugging. Each request maps to one call to one replica; non-leader
replicas reply with a redirect envelope pointing at the leader (the POC
single-instance step skips redirects since there's only one replica).

Every request body carries `agent_id` and `serial` for retransmit
deduplication (the replicated version dedups by `(agent_id, serial)`;
the POC relies on fencing instead).

- `task_create({spec, creator_task?, creator_token?, ...})` →
  `{ok, task_id}` or `{err, depth_exceeded?}`. Root tasks (no
  `creator_task`) start at `depth=0`. Recursive calls compute
  `depth = creator.depth + 1` and reject if > 5. Recursive calls also
  validate `creator_token` matches the creator task's `owner_token`.
- `task_list({filter_status?, only_mine?, ...})` →
  `{tasks: [{id, type, title, status, owner?}, ...]}`.
- `task_claim({prefer_type?, ...})` → `{none}` or
  `{id, spec, fencing_token}` or `{none, halted, halted_by, halted_reason}`.
  Halt check first: if the swarm is halted (a previous `task_fail`),
  return `{none, halted=true, ...}` regardless of pending eligibility.
  Otherwise the SM scans `task_order_` and picks the first task where
  (a) `requires` are all `done` and (b) status is `pending` or status is
  `in_progress` with an expired lease (`now_unix - heartbeat_unix >
  LEASE_WINDOW`, 15s). Lease-expired tasks are transitioned back to
  `pending` first, then claimed — atomically, inside the same
  `process_req` call. The agent (guided by the skill) does any
  git/worktree setup itself after claiming.
- `task_heartbeat({id, token, ...})` → `{ok}` or `{fenced}`. SM checks
  `token == owner_token`; if so, sets `heartbeat_unix = now_unix`.
  **Unaffected by halt** — in-progress peers can still heartbeat.
- `task_complete({id, token, result_branch?, result_summary, new_children?, ...})`
  → `{ok, merge_task_id?, child_ids}` or `{fenced}`. Atomically:
  (1) for each child in `new_children`, validate depth and append;
  (2) for `implement` tasks **with a non-empty `result_branch`**,
  synthesize a `merge` task and append it (an implement that completes
  without a branch — mid-flight pivot, scope decomposition,
  verify-only — does NOT synthesize a merge);
  (3) set `status=done`, `result_branch`, `result_summary`;
  (4) clear owner. **All in one transition** — no multi-key flush, no
  partial-state recovery. **Unaffected by halt** — peers can still
  complete in-progress work.
- `task_fail({id, token, reason, ...})` → `{ok}` or `{fenced}`. Sets
  `status=failed`, `fail_reason`, clears owner, **and sets the
  swarm-halt flag** (recording `halted_by=this task` and
  `halted_reason=reason`). From this point on, new `task_claim`s return
  `{none, halted=true}` until `swarm_resume` clears it. Children
  created earlier remain (see §3). By contract, reason starts with
  `ABANDON:`.
- `swarm_resume({...})` → `{ok, was_halted}`. Human-triggered. Clears
  the halt flag and the recorded cause. No fencing — operator action.
  Idempotent: returns `was_halted=false` if there was nothing to clear.
- `main_lock_acquire({merge_task, merge_token, ...})` →
  `{ok, lock_token}` or `{busy}` or `{fenced}`. Validates the merge task
  is `in_progress` and `merge_token` matches. Then: if unheld, grant; if
  held but the holder's merge task heartbeat is stale, clear and grant;
  else busy. **Advisory only:** the SM serializes who *holds* the token
  but does nothing to prevent a misbehaving agent from pushing to main
  without it. Skills enforce the discipline.
- `main_lock_release({lock_token, ...})` → `{ok}` or `{fenced}`.

All RPCs from a non-owner (wrong agent_id or wrong fencing token) get
`{fenced: true}`. The skill instructs the agent to drop the task and loop.

**Why every operation is atomic.** Because the SM is in-process, every
`process_req` call is a single transition. There are no inter-key races
to design around. In the replicated version, Paxos ensures every replica
applies the same transitions in the same order — atomicity carries over
from "function call" to "decided log entry."

---

## 6. Leases and fencing

**Lazy wall-clock leases — no sweeper, no logical tick.** Heartbeats
write `tasks_[id].heartbeat_unix = now_unix`. Lease expiry is checked
*lazily*, by the next request that cares about ownership — primarily
`task_claim`, secondarily `main_lock_acquire`. There is no periodic
sweeper coroutine and no leader-handoff machinery for cleanup.
Same-host docker-compose deployment + simulated single-event-loop tier-1
means all clocks are aligned to within sub-millisecond, so a generous
lease window (`LEASE_WINDOW = 15s`) absorbs any realistic jitter.

**The lazy-expiry pattern, in `task_claim`:** the SM, in one transition,
finds an in-progress task with `now_unix - heartbeat_unix > 15s`,
transitions it back to `pending` (clearing owner fields), then claims it
on behalf of the requester (mints fresh `owner_token`, sets owner fields).
Because this is one `process_req`, there's no race — in the replicated
version, every replica replays the same transition deterministically.

**`main_lock_acquire` uses the same pattern.** Read `main_lock_`; if held,
look up the holder's merge-task heartbeat; if stale, clear the lock and
grant atomically. The main lock lease piggybacks the holder's merge task
lease — there's no separate `main_lock_heartbeat` field.

**Fencing rules.**
- Every status-mutating RPC carries `token = owner_token at claim time`.
  The SM compares `token == tasks_[id].owner_token`; mismatch ⇒ `fenced`.
- `main_lock_release` carries `lock_token`. Same check.
- `task_heartbeat` validates `token` before writing the new timestamp.
- **Per-task fencing only.** A fenced agent is *not* permanently banned.
  It loops back, calls `task_claim` again, gets fresh work. (Permanent
  agent fencing was considered and rejected — flaky networks would
  falsely permaban.)

**Why two-leader races don't break things.** Briefly two replicas believe
they're leader (split-brain). Both accept HTTP requests and submit them
to the Paxos log. Paxos serializes the log; only one ordering wins. The
loser's stale Paxos round number gets rejected at the protocol layer.
Once the log decides, every replica applies the same `process_req`
sequence and converges to the same SM state.

---

## 7. Agent side: heartbeats, fencing, the loop

**Heartbeats via background shell.** Claude can spawn background shell
commands. The `task-loop` skill instructs Claude, on successful claim, to
launch a background shell:

```bash
( while true; do
    curl -s -X POST $TM_URL/task_heartbeat \
      -d "{\"id\":\"$TID\",\"token\":$TOK,\"agent_id\":\"$AGENT_ID\",\"serial\":$((RANDOM*RANDOM))}" \
      > /dev/null || true
    sleep 5
  done ) &
HB_PID=$!
```

When Claude exits, the shell exits with it (default Claude Code behavior).
When Claude calls `task_complete` or `task_fail`, the skill instructs it to
`kill $HB_PID` first.

**The loop.**
```
while true:
  resp = POST /task_claim {agent_id, prefer_type=...}
  if resp.none:
    sleep 10s; continue
  start_heartbeat_shell(resp.id, resp.fencing_token)
  if resp.spec.type == "plan":      invoke task-planning skill
  if resp.spec.type == "implement": invoke task-implementing skill (sets up worktree itself)
  if resp.spec.type == "merge":     invoke task-merging skill (does git itself)
  on success: POST /task_complete  (with new_children if any)
  on fenced:  drop everything, continue loop
  on failure: POST /task_fail
  kill heartbeat shell
```

**Dealing with `fenced`.** Any HTTP response with `"fenced": true` ⇒ the
`task-loop` skill instructs the agent to immediately abandon the current
worktree (don't commit, don't push), kill the heartbeat shell, and `continue`
the outer loop. Fenced means another agent has been assigned this task; our
work is wasted but causes no harm.

---

## 8. Skills inventory (4 skills)

Each skill is a markdown file the agent reads when it enters that phase.
Lives in `pset4/skills/` and is loaded by Claude via the standard
`.claude/skills/` mechanism.

### 8.1 `task-loop`
- **When:** the user runs `/task-loop` (or just says "join the swarm").
  Always on for the lifetime of this Claude session.
- **Contents:** the loop pseudocode in §7, fully spelled out. Concrete
  `curl` commands, including JSON construction (skills use `printf` or
  `jq -n` to build payloads safely). Concrete commands for launching the
  heartbeat shell. Concrete handling of the three task types (delegate to
  the other three skills). Explicit instructions for `fenced` responses.
  How to print progress so the human watching can follow along.

### 8.2 `task-planning`
- **When:** invoked by `task-loop` after claiming a `plan` task.
- **Contents:** read the spec's prompt. Think about decomposition. Rules
  for what makes a good child task:
  - A child task should be ~30 min of work for a competent agent.
  - Each child must have an unambiguous acceptance criterion.
  - Children that share state must be ordered via `requires`.
  - Implementation children must specify `branch_base` (usually `main`,
    or the predecessor's branch).
  - Plan children are fine — recursion is OK (depth-capped at 5).
  - Pass-off between siblings happens through `.md` files in the
    predecessor's branch (e.g., `HANDOFF.md` describing the API).
- For each child, build a JSON spec and POST to `/task_create` with
  `creator_task` and `creator_token` set (or include them in `new_children`
  on the final `task_complete` — both work; the skill picks one for
  consistency). Write a short plan summary. Call `task_complete`.

### 8.3 `task-implementing`
- **When:** invoked by `task-loop` after claiming an `implement` task.
- **Contents:** the skill walks Claude through the git setup explicitly:
  `git fetch`, `git worktree add ../wt-<task_id> -b task/<task_id>
  <branch_base>`, `cd ../wt-<task_id>`, then for each `dep` in
  `spec.requires` look up the dep's `result_branch` from `/task_list`
  output and `git merge` it in. Read any `HANDOFF.md` files from
  predecessors. Implement. Commit. Run local tests if any exist. Write
  a `HANDOFF.md` describing what you did for downstream tasks. If during
  implementation you discover follow-up work, build a child spec and
  include it in the final `task_complete`'s `new_children`. Call
  `task_complete` with your branch tip SHA. On `fenced` or failure,
  abandon the worktree (don't bother cleaning it up — the human can
  `git worktree prune` later).

### 8.4 `task-merging`
- **When:** invoked by `task-loop` after claiming a synthetic `merge` task.
- **Contents:**
  - Call `/main_lock_acquire`. Poll with backoff while busy (concrete:
    sleep 2s, 4s, 8s, capped at 30s).
  - Once acquired: `git fetch`, `git checkout main`, `git merge
    <implement-branch-sha>`. Resolve conflicts if any (the skill includes
    a short conflict-resolution playbook).
  - Run sanity tests (`pytest -q` or whatever the repo configures).
  - `git push` (no-op for local-only demo; required when remote configured).
  - Call `/main_lock_release`.
  - Call `/task_complete`.
  - If conflicts can't be resolved cleanly: `/main_lock_release`, then
    `/task_fail` with the reason. Do **not** push partial state.
- The skill branches on local-vs-remote based on a config value. Same
  workflow either way; only the `git remote` URL changes.

---

## 9. Git is the agent's job

The task manager and skills do **no git work at all** on the server side.
Git is just what agents happen to use to do their work; the task manager
is task-management infrastructure that would work equally well for non-code
tasks. Keeping git out means:

- The server is small — task SM + HTTP front, ~800 lines.
- Server failures can't corrupt a worktree.
- Git failures can't take down the server.
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
credentials. The task manager never sees them.

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
`cotamer/io.hh` if HTTP-between-replicas is overkill). The
`pt_paxos_replica` becomes generic over both `transport<paxos_message>`
and the SM type (`tm::task_manager_db` here, `pancy::pancydb` in pset3).

**Inter-replica protocol** is raw framed binary — we don't need HTTP
between replicas. **Client→leader is HTTP/JSON** because that's what
agents speak via curl.

**Fallback if Cotamer HTTP support is rough on Windows.** Hand-roll a
minimal HTTP/1.1 subset on top of `cotamer/io.hh`. ~200 lines. Drop HTTPS
entirely; demo is localhost so it doesn't matter. (The POC step uses
Cotamer's existing `http_parser`, which is already proven by
`examples/jsond.cc` — so this risk only matters if the docker container
build environment differs from the dev box.)

---

## 11. Failure handling — what we test

**Replica/Paxos-layer failures (re-exercised from pset3).**
- Permanent leader crash (`docker compose kill` — sim equivalent: 100%
  loss). Survivors elect new leader, continue.
- Temporary replica unavailability + recovery (`docker compose pause`/
  `unpause`). The replica's process state survives, so Paxos resumes
  cleanly.
- Symmetric partition / split-brain (`docker network disconnect`/`connect`).
  Old leader loses majority and stops committing; majority elects new
  leader; old leader rejoins, learns it's stale.

**Important caveat: replicas that are `kill`'d don't recover state in this
PoC.** Pset3 punts on state-loss recovery, and we're not implementing
state transfer. So `kill`-then-`start` produces a *fresh* replica that
violates Paxos's no-state-loss assumption. The demo uses `kill` only for
"survival" stories (don't restart) and `pause` for "recovery" stories.
Listed in §15 as known limitation.

**Agent-layer failures (new for pset4).**

Note: an agent's `task_fail` is **not** a "failure" in the sense
below — it's a deliberate, by-contract abort that halts the swarm
(see §4 and §5). The cases below are the involuntary failure modes
(crash, stall, partition) where the lease-expiry path takes over and
the swarm continues.

- Agent killed mid-task (`SIGKILL` the Claude). Heartbeats stop.
  `tasks_[id].heartbeat_unix` ages past `LEASE_WINDOW`. The next agent
  that calls `task_claim` notices, transitions the task back to `pending`,
  and claims it — atomically. End state: task `done` exactly once.
- Agent stalled mid-task (`SIGSTOP`). Heartbeats stop. Same recovery as
  kill.
- **Zombie agent** (stalled, lease expired, then unstalls and tries to
  call `task_complete`). Fenced. Drops to outer loop, claims a fresh
  task. The recently-completed task (by the new owner) stays done.
- Agent crashes during merge (while holding `main_lock_`). The lock
  lease piggybacks the merge task's heartbeat — when the next
  `main_lock_acquire` call sees the holder's merge task heartbeat is
  stale, it clears the lock atomically as part of granting it.
- Network partition between agent and leader. Heartbeats fail to land
  ⇒ lease ages out ⇒ next claimant takes over. Original agent eventually
  reconnects, sees `fenced`, drops.

**Cross-product cases tested at least once.**
- Leader fails *while* an agent is mid-claim (Paxos may or may not have
  decided the request). Idempotent claim under retry must not double-assign.
  Because `process_req` is one transition, a successfully-decided claim
  produces one fencing token; a never-decided claim produces no state
  change. The retry path is handled by `(agent_id, serial)` dedup at the
  HTTP layer.
- Agent crashes during `task_complete`. **No partial-state to worry about**
  — `task_complete` is one atomic transition. Either the new children +
  `done` status all landed, or nothing did. (This is the big simplification
  vs. an earlier draft of this plan.)

---

## 12. Testing — two tiers

### Tier 1: deterministic simulation (most of the correctness signal)

Mirrors pset3's `try_one_seed` structure. Synthetic agents are coroutines
implementing the loop in §7 against `sim_transport`. The "agent's
implementation work" is mocked — they don't actually edit files; they
just sleep for some sim-time, optionally fail, optionally request child
tasks, then `task_complete`.

**End-of-run invariants:**
1. Every task created reaches a terminal state.
2. No two distinct fencing tokens were ever simultaneously held against
   the same task (audit-log check on owner_token transitions).
3. Every replica's `task_manager_db` agrees (state-equality check).
4. For every `done` task: `requires` were all `done` at claim time
   (audit-log check).
5. `main_lock_` was never held by two distinct tokens simultaneously.
6. Every task in `tasks_` is reachable via root tasks through
   `parent_id` (no orphans, modulo lazy GC of tasks created by failed
   parents — those are *expected* orphans, not bugs).

**Failure schedules** (bring forward pset3's three; add three new):
- `fail_leader_permanent`, `fail_leader_temporary`, `fail_split_brain`
  from pset3.
- `kill_agent` — randomly stops one synthetic agent's heartbeats.
- `stall_agent` — delays one agent's next call by > lease.
- `agent_during_leader_change` — combines the two.

Run each at 1000+ seeds with loss rates 0, 0.01, 0.05.

> **POC step note:** Tier 1 is not built in the POC step. With one
> replica there's nothing nondeterministic to shake out beyond what unit
> tests already cover. Tier 1 lands with the Paxos integration — and
> reuses the same `task_manager_db` SM that the POC's unit tests already
> exercise.

### Tier 2: docker-compose + real Claude integration (the demo)

`make demo-up` starts 3 replicas in compose. `make demo-init` wipes and
seeds the bare repo and the task manager (creates the root `plan` task
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

> **POC step note:** the POC demo is a single `tm-server` on localhost,
> not docker-compose. Same skills, same agent loop, same end-state
> (calculator built end-to-end by 3 Claudes) — just no replication, no
> failure injection. Docker-compose lands with the Paxos integration.

The integration tier is **qualitative** — we capture logs and one or two
screenshots for the writeup, not a perf number. Tier 1 carries the
correctness story.

**Stretch demo: chess engine or URL shortener** as a more impressive
recursive-decomposition target. CLI calculator is the baseline.

---

## 13. Build order

Two phases: **POC** (single instance) and **Replicated** (Paxos).

### Phase A — POC (single instance, ~6–8 hours)

Detailed in [`docs/superpowers/specs/2026-05-06-pset4-poc-design.md`](../docs/superpowers/specs/2026-05-06-pset4-poc-design.md).
Summary:

A1. **`tm.hh` + `task_manager_db` + unit tests.** The whole SM, exercised
    by `tm-tests`. No HTTP, no networking. Drives correctness.
A2. **JSON (de)serialization for `tm` types.** Round-trip tests.
A3. **HTTP front + `tm-server`.** Lifted from `examples/jsond.cc`.
    Smoke-test with `curl`.
A4. **`task-loop` + `task-planning` skills.** Run one Claude against
    `tm-server` with a small plan task; verify decomposition. Implement
    tasks are mocked at this point (skill just sleeps + completes).
A5. **`task-implementing` + `task-merging` skills.** Real git operations.
    Two Claudes against a tiny seeded plan; verify cooperation.
A6. **Demo Makefile + 3-Claude calculator run.** Capture logs.
A7. **Polish + POC writeup.**

### Phase B — Replicated (Paxos), ~8–10 hours

B1. **Templatize `pt_paxos_replica`.** Make it generic over
    `(request, response, SM)` instead of hard-coded to pancy types. Drop
    `tm::task_manager_db` in. Tier-1 sim tests (mocked agents + sim
    transport) start working.
B2. **Failure schedules.** Port pset3's three; add `kill_agent`,
    `stall_agent`, `agent_during_leader_change`. Drive to 1000-seed
    clean.
B3. **Transport abstraction + raw TCP impl.** Replicas as real processes
    bound to ports.
B4. **HTTP front gets redirect support.** Non-leader replicas reply
    `{redirect: leader_url}`. Skills already retry on transient errors;
    extend the `curl` wrapper to follow redirects.
B5. **docker-compose + Makefile.** Three replicas + bind-mounted bare
    repo. Failure injection targets.
B6. **Integration demo run + writeup capture.**
B7. **Final writeup polish.**

Slack ~2h built into Phase B. If we need the May-13 extension, priority
order for stretch work: state transfer (real recovery), GitHub-remote
demo, performance metrics.

---

## 14. Resolved design decisions

- **Typed SM, not pancydb-on-top.** Every operation is one atomic
  transition in `task_manager_db::process_req`. Eliminates the multi-key
  partial-flush recovery story that earlier drafts of this plan needed.
  Replication still works the same way — pset3 Paxos already replicates
  arbitrary opaque-blob SMs, so we plug ours in by templatizing
  `pt_paxos_replica`.

- **Curl from skills, no MCP shim.** First build is plain `curl` against
  HTTP/JSON. Avoids mixing languages, avoids a third process per agent
  that could fail independently. An MCP shim is a thin wrapper that can
  be added later if the curl-in-skills ergonomics become a real problem;
  the wire protocol is unchanged.

- **Children created during a task survive the task's failure.** Different
  from a buffered model that discards orphan children on parent failure.
  Simpler (no buffering); preserves planning work across crashes;
  occasionally produces obsolete tasks that the next claimant marks
  `failed`. Acceptable PoC trade-off.

- **Wall-clock from the request handler, not the SM.** `process_req`
  takes `now_unix` as a parameter. SM stays a pure function. In the
  replicated version, the leader stamps `now_unix` when proposing and
  every replica replays the same value.

- **Worktree cleanup.** Agents own worktrees; on agent death they leak
  until `git worktree prune`. Acceptable PoC behavior; `make demo-clean`
  prunes between demo runs.

---

## 15. Risks and mitigations

| Risk | Likelihood | Mitigation |
| --- | --- | --- |
| pset3 Paxos has latent bug exposed by new traffic patterns | Med | Run 10k+ seeds tier-1 early in Phase B; keep pset3 regression seeds. The SM swap is mechanical so any new bug is in Paxos, not the SM. |
| Templatizing `pt_paxos_replica` is harder than expected | Low-Med | If templating is awkward, copy `pt-paxos.cc` to `tm-paxos.cc` and edit the types. Loses code reuse but unblocks. |
| Real Claude CLIs are non-deterministic; integration test flaky | High | Tier 2 is qualitative; correctness rests on tier 1. |
| Background-shell heartbeat doesn't survive Claude session lifecycle as expected | Med | We assume the shell exits with Claude. If wrong, fall back to a separate sidecar process tied to a PID Claude reports. |
| Zombie agent's stale `task_complete` writes data after fencing | Low | Every status write is fenced — verify in tier-1. |
| Skills can't reliably build JSON in shell | Med | Skills require `jq` (standard on dev boxes). Document as prerequisite. |
| `kill`-restart of a replica violates Paxos no-state-loss | High (by design) | Document as known PoC limitation; demo only `pause`/`partition` for recovery; document state transfer as future work. |
| 15h budget overrun | Med | POC + Replicated split provides natural checkpoints; pset extension to May 13 with check-in. |

---

## 16. Writeup outline (4–6 pages)

1. **Problem & motivation.** Multiple coding agents, why centralized
   coordination matters even in an agent-per-task world, why fault
   tolerance matters when the agents themselves are flaky.
2. **Design.** Architecture (§2), data model and typed SM (§3), task
   lifecycle (§4), leases/fencing (§6), the agent loop and skills (§7–8).
   Diagrams. Discussion of why a typed SM beats pancydb-on-top for this
   problem (the partial-flush story you don't have to write).
3. **Implementation.** The SM-as-replicated-state-machine framing
   (Paxos stayed untouched in pset3, gets templatized for pset4).
   Transport split (§10). The two-phase build order: POC first, then
   replication.
4. **Testing.** Invariants (§12), failure schedules, results table (seeds
   run, failures found, what we fixed). At least one war-story bug à la
   pset3 TURNIN.md.
5. **Limitations & future work.** No state-transfer (so killed replicas
   don't recover); no MVCC; no real cross-cell transactions; no
   adaptive scheduling; brief gesture at what BFT would change. MCP shim
   as a follow-on.
6. **References.** Multi-Paxos lecture notes, Chubby (closest prior art
   for lease-based locking), the pset3 writeup.
