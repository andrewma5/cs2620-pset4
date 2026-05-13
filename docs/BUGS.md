# Known bugs and failure modes

Bugs discovered during pset4 development, kept here so they're not lost
between sessions. Some are fixed, some are deferred with documented
workarounds, some are open.

## FIXED (2026-05-13) — `/task-shutdown` reported success but didn't kill `tm-hb` shells

**Discovered:** 2026-05-13 during real-Claude agent-shutdown scenario
**Fixed:** same day — regex in `tm-kill.sh` (canonical bundle at
`.claude/skills/swarm-deploy/assets/agent-skills/task-kill/tm-kill.sh`)
**Forensic artifacts (bug):** `demo/runs/kill-agent-1/` — `/task-shutdown`
self-reported "verified clean" but orphan `tm-hb.sh` PID survived;
external `pkill` was required to make the lease-expiry re-claim story
unfold.
**Validation (fix):** `demo/runs/kill-agent-2/` — `/task-shutdown` on
agent-3 mid-t/0003 cleanly killed shells (verified with host `pgrep`
showing 0 matches immediately after, vs other agents unaffected);
lease expired naturally at +45s; agent-2 re-claimed; all 5 tasks done.
**No external intervention required.**

### Symptom

Run `/task-shutdown` in one agent's Claude terminal. The skill walks
through its checklist:

> All clean. All tm-* background shells for agent-1 have been killed and
> verified gone.

But from the host:

```
$ pgrep -fa "agent-1.*tm-"
59254 /bin/zsh -c ... eval 'source ".../agent-1/tm.env"\012export
       TASK_ID="t/0003"\012export TOK=3\012"$SKILL_DIR/tm-hb.sh"' ...
```

PID 59254 is agent-1's `tm-hb.sh` for `t/0003`, still alive and still
heartbeating. The skill's `task-kill verify` step did not detect it.

Consequence: the dropped task's `heartbeat_unix` stays current. The
`LEASE_WINDOW = 45s` clock never starts. No other agent can re-claim.
The "agent failure → re-claim" recovery story is broken unless someone
externally `pkill`s the orphan shells.

### Forensic data point (from 2026-05-13 session)

- `t_shutdown ≈ 18:11:00`. Agent-1's Claude reported "all clean".
- 40s later, `t/0003` still owned by agent-1, hb_age=4-10s (live).
- External `pkill -f "agent-1.*tm-"` killed PID 59254.
- 45s after pkill, t/0003 lease expired, agent-2 re-claimed cleanly.
- Captured at `demo/runs/agent-shutdown/` (when scenario completes).

### Root cause

The original `tm-kill.sh` regexes anchored on the `tm-hb.sh` /
`tm-wait.sh` substring **first**, then required the `agent-id` to
follow:

```bash
pkill -f "(tm-hb|tm-wait)\\.sh.*${AGENT_RE}"
```

But the actual argv of a `tm-hb.sh` process is one of two forms,
both with `agent-N` appearing **before** `tm-hb.sh`:

1. **Eval-wrapped** (Claude Code Bash tool, original format):
   ```
   /bin/zsh -c source <snapshot>... && eval 'source ".../agent-N/tm.env"\012
     export TASK_ID="..."\012... "$SKILL_DIR/tm-hb.sh"'
   ```
2. **Direct bash invocation** (observed in our test runs):
   ```
   /bin/bash /Users/.../pset4-testing/test1/agent-N/.claude/skills/task-loop/tm-hb.sh
   ```

In both forms, the substring order is `agent-N` → `tm-hb.sh`. The
original regex required the opposite order → never matched → `pkill`
found nothing → `verify` reported "clean" (false negative).

### Process: how we got to the fix

Two iterations:

1. **First attempt: anchor on `agent-N/tm.env`** — `pkill -f "${AGENT_RE}/tm\\.env.*tm-hb\\.sh"`. Reasoning: agent's tm.env path is unique per agent, and the eval-wrapped form has it. Tested in real-Claude (kill-agent-1 cluster, manually patching deployed shells): **failed**. The actual argv of live tm-hb.sh shells was form (2) — `/bin/bash <path>/agent-N/.../tm-hb.sh` — which has **no `tm.env` substring at all**. The fix anchored on a string that wasn't always there.

2. **Second attempt (shipped): just put agent-N before tm-hb/tm-wait** — `pkill -f "${AGENT_RE}.*(tm-hb|tm-wait)\\.sh"`. Works for both forms (1) and (2) because both have agent-N before tm-hb.sh. Validated in kill-agent-2: agent-3's `/task-shutdown` killed only agent-3's shells (agent-1 and agent-2 each kept their 2 shells alive — proper isolation).

Lesson: don't anchor on a substring you can't prove appears in every
process variant. The agent-id-then-tm-hb pattern is the minimum that
captures both wrapper forms.

### `by-task` mode caveat

`by-task` was supposed to kill only the specific task's hb shell using
the `TASK_ID="..."` substring. Form (2) doesn't include TASK_ID in
argv. The new `by-task` regex falls back to "kill all tm-hb for this
agent" — equivalent in practice since each agent has at most one
active task at a time.



## FIXED (Fix D, 2026-05-13) — Killing the elected leader wedged write quorum

**Discovered:** 2026-05-13 during real-Claude run #3 (kill-leader scenario)
**Fixed:** same day in `tcp_transport.cc` (write timeout + persistent
`connect_loop_`)
**Layer:** `tcp_transport.cc` (pset4-new code), NOT the inherited pset3
paxos protocol logic
**Forensic artifacts (bug):** `demo/runs/kill-leader-WEDGED/`
**Validation (fix):** `demo/runs/kill-leader-1/` — real-Claude kill-leader
run completed all 5 tasks end-to-end; survivors byte-identical on
`tm-replay`; election event captured (`replica 2 round 5`)

### Symptom

After `make demo-kill-N` where replica-N is the current paxos leader:

- The remaining 2 replicas elect a new leader (visible in compose
  stderr: `become_leader: replica X round Y`).
- Both surviving replicas continue to answer GET `/dump` in <2ms.
- 307 redirects from the new follower correctly point to the new
  leader's HTTP port.
- **BUT** POST writes to the new leader hang indefinitely. `curl
  --max-time 5 -X POST localhost:<new-leader>/task_list ...` times out.
- Client agents observe the cluster as "leader unresponsive" and retry
  forever.

After 4+ minutes of waiting, writes never recover. The cluster is
permanently wedged for writes; reads still work.

Even restarting the killed replica via `docker compose start replica-N`
does NOT unblock writes — the old TCP sockets to the dead peer stay in
CLOSE-WAIT with kilobytes queued in Send-Q, and our impl never closes
or reconnects them.

### Reproduction (from 2026-05-13 session)

1. `make demo-init`
2. Launch 3 Claude agents, wait for one implement task `in_progress`
3. Identify leader: `docker compose logs 2>&1 | grep become_leader | tail -1`
4. `make demo-kill-<leader-index>`
5. Wait. The new leader's POST endpoints will hang.

Concrete numbers:
- `t_kill = 1778699592` (replica-2 killed, was leader at round 5)
- New leader elected: `replica-1 round 7 slot 37`
- 265s later: agent-3's heartbeat 254s stale, writes still timing out

### Forensic evidence

Captured live from the wedged cluster:

```
==== replica-0 (follower) sockets ====
CLOSE-WAIT 1 32990  172.18.0.4:38562  172.18.0.2:9000  tm-server  <-- dead peer-2, 33KB queued

==== replica-1 (new leader) sockets ====
CLOSE-WAIT 1 43194  172.18.0.3:52192  172.18.0.2:9000  tm-server  <-- dead peer-2, 43KB queued
CLOSE-WAIT 1 0      172.18.0.3:8080   192.168.65.1:*   tm-server  x19  <-- hung client POSTs
ESTAB      0 0      172.18.0.3:9000   172.18.0.4:46712 tm-server      <-- healthy ↔ replica-0
ESTAB      0 0      172.18.0.3:60618  172.18.0.4:9000  tm-server      <-- healthy ↔ replica-0
```

### Root cause (confirmed)

Three independent code-level facts that compose into the wedge:

**1. `cot::write` suspends forever on a dead peer's full send buffer.**
At `cotamer/io.cc:734-735`, when `writev()` returns EAGAIN (kernel send
buffer full), cotamer does `co_await writable(f)` — suspending the
coroutine until the fd becomes writable. For a peer in CLOSE-WAIT
(dead container's FIN received but our close not called yet), the
kernel never signals the fd writable, so the coroutine suspends
indefinitely. Default Linux TCP keepalive is 2 hours, so the kernel
won't proactively close the socket either.

**2. `send_propose` and `start_election` await sends serially across
peers.** `tm-paxos-internal.hh:142-156` (`send_propose`) and
`tm-paxos.cc:33-39` (`start_election`) iterate over peers with
`co_await transport_->send(i, msg)` in sequence. One stuck send blocks
the rest of the loop. The entire `run()` event loop (one coroutine)
is blocked too — so the leader cannot even *process* ACKs that arrive
from healthy peers. This is the immediate cause of the wedge.

There are 8 `transport_->send` call sites in total across `tm-paxos.cc`
and `tm-paxos-internal.hh`. All are awaited serially.

**3. `tcp_transport::connect_loop_` does not reconnect.** At
`tcp_transport.cc:128-143`, the connect loop calls `co_return` after
the first successful connect. Once a socket is established, nothing
ever re-establishes it. So even if Fix #1 + #2 closed the dead fd,
nothing would bring a peer back when its container restarts.

The 19 client-side CLOSE-WAIT sockets visible on the leader (see
`forensic evidence` block above) are a downstream symptom: every hung
POST handler was a coroutine suspended inside `propose_and_apply`,
waiting for the wedged `send_propose` to return.

### Why our existing tests didn't catch it

The kill-replica path is tested at three layers, none of which
exercises the bug's preconditions:

| Layer | Script | Killed which replica? | Sustained load? |
|---|---|---|---|
| Sim | `scripts/run-paxos-tests.sh` (34k runs) | leader | yes — but no real sockets |
| Local TCP | `scripts/local-tcp.sh kill_leader` | always replica-0 (may not be leader) | no — one smoke run |
| Docker | `scripts/docker-failure-sweep.sh kill` | always replica-1 (not the leader) | no — synthetic-agent finishes in ~10s |

Three independent reasons the local-tcp `kill_leader` scenario doesn't
catch this:

1. **Wrong target**: kills replica-0 unconditionally, but the leader is
   non-deterministic at startup. Could easily kill a follower.
2. **Insufficient load**: ~33-43KB of buffered data is needed to wedge
   the kernel send buffer. One smoke pass doesn't generate that volume.
3. **Localhost timing differs from docker bridge**: connection-refused
   on localhost fires instantly via kernel-internal signaling; on the
   docker bridge network, peer-down detection lags by enough that the
   send-buffer fills before the socket gets marked broken.

The sim tests don't have real sockets, so they're protocol-only — they
verify that paxos *would* make progress on majority if messages
delivered, but they don't exercise `tcp_transport.cc` at all.

This is the canonical "your tests pass but production breaks" failure:
each test layer covered a real concern, but the intersection
"leader-killed + sustained-load + real-sockets" was untested.

### Process: how we got to Fix D

The fix was non-obvious; multiple candidates were considered and one
was rejected with material help from independent review. Recorded here
because the rejected path is a real trap.

**Fix A (initial instinct — rejected): "detach sends".** Change
`co_await transport_->send(i, msg)` to
`transport_->send(i, msg).detach()` at all 8 call sites in `tm-paxos.cc`
/ `tm-paxos-internal.hh`. Sends would run in parallel; one stuck peer
couldn't block others. **First independent agent review rejected this:**
two concurrent detached sends to the same peer interleave `len_be`
(4-byte length prefix) and `body` on the wire, because
`tcp_transport::send` issues them via *two separate* `cot::write` calls
(`tcp_transport.cc:107, :112`). The peer's `read_loop_` reads `len`
bytes after the prefix, but the second send's prefix might land inside
the first send's body. Frame corruption → `read_loop_` exits → silent
partitioning. Detach would also break paxos per-peer message ordering.
Doing parallel sends correctly requires per-peer outbound queue +
dedicated writer coroutine — a redesign, not a patch.

**Fix B (write timeout only).** Wrap `cot::write` with
`cot::first(write, after(2s))`. On timeout, close fd. **Reviewed: works
for the demo's exact bug but leaves no reconnect path** — a restarted
peer would never rejoin (the old `connect_loop_` had already
`co_return`ed). Incomplete on its own.

**Fix C → D (converged).** Second independent agent review pointed out
that cotamer already ships `cot::closed(const fd&)` (`cotamer.hh:367`);
no need to invent an "fd-closed" signal. Final design: write timeout
+ persistent `connect_loop_` parked on `cot::closed(...).arm()`. Two
small functions in one file.

**Timeout tuning (250ms → 500ms).** Initial implementation used 250ms
(picked to fit between the 100ms retransmit cadence and 500ms election
timeout). docker-failure-sweep showed occasional false-positive
timeouts on `pause_resume` iterations — replica-1 was slow-but-alive,
not dead. Bumped to 500ms; sweeps then stabilized.

### Fix D (implemented in `tcp_transport.cc`, converged after two independent reviews)

All changes localized to `tcp_transport.cc`. No protocol-layer changes.
Cot primitives used are already proven in the codebase: `cot::first`
(precedent at `tm-paxos.cc:123`) and `cot::closed(fd)` (declared at
`cotamer/cotamer.hh:367`).

**1. Bounded write timeout in `send()`** — wrap each `cot::write` with
`cot::first(write(...), cot::after(PEER_SEND_TIMEOUT))`. On timeout or
write error, close the fd. Next `send()` short-circuits on the
valid-fd guard, so we pay the timeout once per dead-peer detection.

**2. Persistent `connect_loop_`** — after successful connect,
`co_await cot::closed(outbound_fds_[peer]).arm()` then loop back to
retry. Cotamer's `closed(fd)` event fires when we close the fd (via
timeout) or the peer disconnects.

**3. `PEER_SEND_TIMEOUT = 500ms`** — chosen to fit between paxos
election timeout (500ms, `tm-paxos.cc:241`) and retransmit cadence
(100ms, `tm-paxos.cc:125`). Initially set to 250ms; raised after
docker-failure-sweep showed occasional false-positive timeouts during
pause-resume churn.

**4. Leave `send_propose` / `start_election` serial.** Avoids the
framing-corruption bug that detached parallel sends would introduce
(rejected "Fix A" above).

### Why Fix D preserves paxos correctness

- **Safety (≤1 value per slot):** unchanged. Quorum logic still needs
  majority ACK; Fix D doesn't change voting — just lets the leader
  *reach* majority by not blocking on the third.
- **Total order:** unchanged. Slot numbering and apply order are
  protocol-level; transport timeouts can lose messages but PROPOSEs
  are idempotent (`tm-paxos.cc:194-204`, overwrite-or-append) and
  retransmit at 100ms.
- **No partial-frame corruption:** if length prefix sent but body
  times out, peer's `read_loop_` does a short read on the next bytes
  and exits cleanly. Fresh reconnect → fresh `accept` → fresh
  `read_loop_` on the peer side. No inherited corrupted state.
- **Wall-clock determinism (CLAUDE.md hard-rule #3):** unchanged.
  `now_unix` is still leader-stamped at propose time.
- **Decision log format (hard-rule #4):** unchanged.

### Liveness trade-offs

| Scenario | Before Fix D | After Fix D |
|---|---|---|
| Healthy 3 replicas | ~100ms/propose | ~100ms/propose |
| 1 peer dead | **Wedged forever** | ~100ms/propose (one-time 250ms cost when fd first closes; subsequent rounds skip via valid-fd guard) |
| 1 peer paused | Works under low load; wedges under sustained load | ~250ms per round during pause (reconnect-then-timeout churn). Still live. |
| 1 peer dead → restarted | Stays dead from leader's view | Reconnects on next `cot::closed` cycle |

### Validation (post-Fix-D)

| Test | Result |
|---|---|
| `make check` (31 SM unit tests) | ✅ pass |
| `tests/test_tm_cli.py` (12 partner CLI tests) | ✅ 12/12 |
| `tm-paxos-tests` sim sweep (3,400 runs) | ✅ 100% |
| `docker-failure-sweep` (3 iters × 4 scenarios, post-Fix-D) | ✅ 12/12 |
| `docker-failure-sweep` heavier (5 iters × 4 scenarios) | ✅ 20/20 |
| 💰 Real-Claude kill-leader retest (`kill-leader-1`) | ✅ all 5 tasks done, survivors byte-identical |

Real-Claude run details: leader killed at +180s (replica-0, slot 45);
new leader elected at round 5 with slot 55; survivors continued to
slot 112; agent-transcripts show ONE 307 redirect (handled by `tm` CLI
transparently). Compared to pre-Fix-D `kill-leader-WEDGED` which never
made another decision after the kill.

### Post-fix work (not blocking the demo)

- **Harden `scripts/local-tcp.sh kill_leader`** to (a) actually identify
  and kill the elected leader (currently hardcoded to replica-0,
  which may not be leader), and (b) drive sustained smoke load for
  30+ seconds post-kill. Would have caught this bug. Add a similar
  long-load case to `scripts/docker-failure-sweep.sh`.
- **Add a kill-then-restart scenario** to the docker sweep — Fix D
  enables this story (peer rejoins via the new `cot::closed`-driven
  reconnect path), so we should test it explicitly.
- **Add `jq` to `cs2620:latest` image** — already in the Dockerfile
  (`docker/Dockerfile:50`) but the image needs `./build-docker` to
  rebuild. Without it, `scripts/local-tcp.sh` and `smoke.sh` fail in
  ephemeral compose containers.
