# Known bugs and failure modes

Bugs discovered during pset4 development, kept here so they're not lost
between sessions. Some are fixed, some are deferred with documented
workarounds, some are open.

## OPEN — Killing the elected leader wedges write quorum

**Discovered:** 2026-05-13 during real-Claude run #3 (kill-leader scenario)
**Layer:** `tcp_transport.cc` (pset4-new code), NOT the inherited
pset3 paxos protocol logic
**Severity:** correctness — the writeup's "survival" story is broken on
this specific failure mode
**Forensic artifacts:** `demo/runs/kill-leader-WEDGED/`

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

### Root cause (suspected)

`tcp_transport::send` at `tcp_transport.cc:100-116`:

```cpp
cot::task<> tcp_transport::send(size_t peer, paxos_message msg) {
    if (peer >= peer_addrs_.size() || peer == my_index_) co_return;
    if (!outbound_fds_[peer].valid()) co_return;  // not connected yet

    std::string body = to_json(msg).dump();
    uint32_t len_be = htonl(static_cast<uint32_t>(body.size()));

    auto w1 = co_await cot::write(outbound_fds_[peer], &len_be, sizeof(len_be));
    if (!w1) {
        outbound_fds_[peer].close();
        co_return;
    }
    auto w2 = co_await cot::write(outbound_fds_[peer], body.data(), body.size());
    if (!w2) {
        outbound_fds_[peer].close();
    }
}
```

Failure path:

1. Each replica holds one persistent outbound socket per peer in
   `outbound_fds_[peer]`.
2. When peer-2 dies, peer-2's kernel sends FIN as the container tears
   down. Our side enters CLOSE-WAIT (half-closed: we can still write
   from this end, but the peer can never read it).
3. Successive `co_await cot::write(...)` calls keep queueing data into
   the kernel send buffer. There is no peer to ACK and drain it.
4. Once the buffer fills (~32-43KB observed), `write()` suspends the
   coroutine indefinitely waiting for buffer space.
5. There is no socket-health check, no reconnect-on-error path, no
   reconnect-when-peer-comes-back path. Once the socket is in
   CLOSE-WAIT with full Send-Q, it stays that way.

The propose path on the new leader presumably waits for ack count >=
majority before deciding the slot. Acks would have come back from
replica-0 (healthy peer), but the leader's outbound to replica-2 is
permanently stuck. If the per-peer send is awaited serially (rather
than detached fire-and-forget), even sends to the healthy peer-0 stall
behind the wedged peer-2 send.

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

### Hypothesized fix

Three increasingly aggressive changes, in order of preference:

**1. Detach sends, fire-and-forget per peer.** Make `send()` return
immediately and run the actual write as a detached coroutine. This way
a stuck send to one peer can never block sends to others. If we already
do this elsewhere in the caller, the bug is one layer up; if not, this
is a small refactor at the `send()` call sites.

**2. Detect dead socket + reconnect on send failure.** In `send()`,
when `co_await cot::write(...)` fails OR returns 0-byte write more
than once, call `outbound_fds_[peer].close()` and trigger a reconnect
attempt asynchronously. The connect loop in `tcp_transport` likely
already exists for initial connection — wire it to re-fire on close.

**3. Socket-level timeout on `cot::write`.** If the underlying I/O
primitive supports a deadline, wrap each `co_await cot::write(...)`
with a 1-2 second timeout. On timeout, treat as a write failure (close
socket, fall through to reconnect).

**Minimum fix:** (1) + (2). That should resolve the wedge without
introducing new races.

**Belt-and-suspenders fix:** (1) + (2) + (3). Slightly more code but
the timeout makes the failure mode self-healing even if reconnect
logic has bugs.

### Workaround for the demo writeup (if not fixing tonight)

The `pause-resume` scenario (`demo-pause-1` / `demo-resume-1`) is
unaffected — paused replicas don't close their TCP sockets, so the
buffer never fills and the new leader never wedges. Document this bug
as a known limitation and use `pause-resume-2` (60s pause) as the
demonstrated recovery story.

### Investigation still open

- Does this also occur when killing a *follower* on docker but the
  leader's TCP keepalive is too slow to notice? Our docker sweep says
  no, but the sweep's synthetic agents finish in ~10s — too quick to
  fill the buffer.
- Could the local-tcp `kill_leader` scenario be modified to
  (a) actually identify and kill the leader, and (b) drive sustained
  smoke load for 30+ seconds post-kill? That would have caught this.
- What is the failure mode if we kill the leader and the leader's
  outbound socket to the *other* follower is healthy? The bug requires
  the leader to be stuck waiting for an ack from a dead peer — but
  with only 3 replicas and one dead, the leader has 2 surviving peers
  (itself + one). Only one outbound socket can wedge. So we'd expect
  paxos to still get majority via the live peer's ack alone… unless
  our send loop is sequential rather than parallel. Worth checking
  the call site in `paxos_replica::propose`.
