# Real-Claude demo runs (2026-05-13)

Captured artifacts from the real-Claude swarm demo. Each subdirectory
is one swarm run against the 3-replica docker-compose cluster, driving
the calculator task decomposition end-to-end. Procedure for repeating
the runs lives in [../../docs/REAL_CLAUDE_RUN.md](../../docs/REAL_CLAUDE_RUN.md).

All runs that completed end-to-end **passed** `make demo-replay-verify`
(byte-identical decision logs across surviving replicas — the
real-paxos "3 replicas, 1 timeline" invariant).

## Index

| Dir | Scenario | Instrumented¹ | Outcome | Notes |
|---|---|---|---|---|
| [`clean/`](clean/) | Baseline (no failure) | no | ✅ all done, replay-verify PASS | First successful swarm run |
| [`clean-1/`](clean-1/) | Baseline (no failure) | yes | ✅ all done, replay-verify PASS | Re-run with `apply_at_local` instrumentation |
| [`pause-resume/`](pause-resume/) | 10s SIGSTOP replica-1 (follower) | no | ✅ all done, replay-verify PASS | Pause hit during plan→implement transition |
| [`pause-resume-1/`](pause-resume-1/) | 10s SIGSTOP replica-1 (follower) | yes | ✅ all done, replay-verify PASS | Pause hit mid-implement (agent-2 actively heartbeating) |
| [`pause-resume-2/`](pause-resume-2/) | **60s** SIGSTOP replica-1 (follower) | yes | ✅ all done, replay-verify PASS | Long pause exceeding lease window — failover held, no task re-claim |
| [`kill-leader-WEDGED/`](kill-leader-WEDGED/) | SIGKILL leader (replica-2) | yes | ❌ cluster wedged for writes | **Found a real bug** — see [`docs/BUGS.md`](../../docs/BUGS.md) |

¹ "Instrumented" = `tm-server.cc` carries the `apply_at_local` and
`replica_index` fields added 2026-05-13 so the `paxos_viz.py` chart
can show per-replica decided_slot progression. Pre-instrumentation
logs only have leader-stamped `now_unix` (identical across replicas
by design), so per-replica timelines look like a single line.

## What each run shows

### `clean/` and `clean-1/`
Healthy 3-replica cluster, 3 Claude agents (Sonnet) decompose and
build the CLI calculator. ~6.5 min end-to-end. Gantt shows both
implement tasks running in parallel across different agents (the
whole point of having 3 replicas + 3 agents). `clean-1`'s
`paxos-timeline.png` is the visual baseline — three overlapping
staircases proving the "1 timeline" invariant.

### `pause-resume/` and `pause-resume-1/`
SIGSTOP replica-1 for 10s mid-run. The paused replica is a follower
(leader stays as replica-2), so paxos keeps making progress on
quorum {replica-0, replica-2}. Agent-2 is pinned to replica-1 and
must fail over via `TM_URL_LIST` to land its heartbeats / claims on a
live peer. After SIGCONT, replica-1 catches up via the paxos log →
replay-verify PASS confirms byte-identical state across all 3.

### `pause-resume-2/` — the dramatic recovery figure
Same as above but 60s pause (exceeds `LEASE_WINDOW=45s`). The expected
risk was that agent-2's heartbeats lapse → its task gets re-claimed by
a surviving agent. **Did not happen** — `tm` CLI failover via
`TM_URL_LIST` was fast enough to keep heartbeats landing on live
replicas. Task completed cleanly during the pause.

`paxos-timeline.png` is the clearest visualization in the bundle:
replica-1's line **flatlines at slot 24 from +107s to +167s** while
the other two keep climbing, then **shoots vertically upward** at
resume as it catches up the missed slots in sub-second time. This is
the recovery story rendered as one image.

### `kill-leader-WEDGED/` — bug evidence
SIGKILL of the elected leader (replica-2 at round 5). Election fires
correctly (replica-1 becomes leader at round 7, slot 37). Reads (`/dump`)
keep working. **But writes to the new leader hang indefinitely** and
do not recover even after restarting the dead replica. After 4+
minutes the cluster is permanently wedged for writes.

Root cause is in `tcp_transport.cc:send` — no socket-health check, no
reconnect logic on peer churn. The dead peer's TCP socket stays in
CLOSE-WAIT with kilobytes queued in the kernel send buffer; eventually
`co_await cot::write(...)` suspends forever on a full buffer to a
peer that will never ACK.

Full diagnosis (forensics, why automated tests missed it, fix options)
is in [`docs/BUGS.md`](../../docs/BUGS.md). Captured artifacts in
this dir include a `compose-stderr-full.log` snapshot taken during
the wedge.

## File layout per run

```
gantt.png           task-level gantt: 5 tasks × 3 agents on a wall-clock axis
gantt.svg           same, vector
paxos-timeline.png  per-replica decided_slot over time (instrumented runs only)
paxos-timeline.svg  same, vector
snapshots-0.json    tm-replay output from replica-0's decision log
snapshots-1.json    tm-replay output from replica-1's decision log
snapshots-2.json    tm-replay output from replica-2's decision log
tm-0.log            replica-0's raw decision log (newline-delimited JSON)
tm-1.log            replica-1's raw decision log
tm-2.log            replica-2's raw decision log
compose-stderr.log  docker compose log scrape (paxos election events, etc)
dump-replica-N.json /dump response from each replica at capture time
```

## Cost (Anthropic credits)

All six runs used Sonnet (configured per-agent via swarm-deploy's
`settings.json`). Per-run cost was roughly $1-3. Total spend across
this batch: ~$10-15.

## Regenerating these figures

Both viz scripts live at the repo root:

```bash
# task gantt (works on any run)
python3 gantt_tm.py --snapshots demo/runs/<scenario>/snapshots-0.json \
    --out demo/runs/<scenario>/gantt.png \
    [--annotate-at <sec> --annotate-label <label>]

# per-replica paxos timeline (instrumented runs only)
python3 paxos_viz.py --logs-dir demo/runs/<scenario> \
    --out demo/runs/<scenario>/paxos-timeline.png \
    [--annotate-at <sec> --annotate-label <label>]
```

For interactive task-state scrubbing, the Flask-based viewer:
```bash
python3 visualize_tm.py --snapshots demo/runs/<scenario>/snapshots-0.json
# then open http://localhost:5050
```
