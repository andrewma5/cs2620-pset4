# Partner onboarding — paxos-ified pset4

Read this first to ramp on what changed since the single-instance POC, how
to build/test/run the demo, and where invariants live. Then `PLAN.md` for
the full design rationale.

## What's new since the POC

Single-instance task manager → 3-replica **Multi-Paxos** replicated state
machine.

The splice point is unchanged: `task_manager_db::process_req(req, now_unix)`
is still a pure function. It's now driven through a paxos engine that:

- **Leader stamps `now_unix`** at propose time and carries it inside the
  decided value
- **All replicas replay the same `now_unix`** at apply time → identical
  SM state across the cluster
- **Every replica logs on apply** via a callback registered with
  `paxos_replica::set_decision_logger` — so all 3 replicas produce
  byte-identical decision logs (real-paxos "3 replicas, 1 timeline"
  invariant)
- Client requests hit any replica; non-leaders **307-redirect** to the
  current leader (followed transparently by `curl -L` in agent skills)

## New / changed files at a glance

| File | Role |
|---|---|
| `tm-paxos.hh`, `tm-paxos.cc` | `paxos_replica` facade (public surface) |
| `tm-paxos-internal.hh` | `pt_paxos_replica` (full multi-paxos impl, exposed for tests) |
| `transport.hh` | abstract `transport<T>` interface |
| `tcp_transport.{hh,cc}` | real-socket transport (length-prefixed JSON frames) |
| `netsim.hh`, `random_source.hh` | in-process simulator + RNG (sim tests) |
| `tm-paxos-tests.cc` | sim test harness, synthetic agents, failure schedules |
| `tm-server.cc` | now opens log + registers `paxos.set_decision_logger`; 307-redirect on busy errcode |
| `docker-compose.yml`, `.env` | 3-replica demo deployment |
| `demo/Makefile` | `demo-up/down/init/seed/status/pause-N/kill-N/partition-N/reconnect-N/replay-verify` |
| `scripts/run-paxos-tests.sh` | sim sweep — 34,000 runs |
| `scripts/local-tcp.sh` | host-TCP sweep — 150 runs across 3 scenarios |
| `scripts/docker-failure-sweep.sh` | docker sweep — 20 runs across 4 scenarios |
| `scripts/synthetic-agent.sh` | free bash agents (no Claude $) for demo dry-run |

## Build

Run inside `cs2620:latest` (the bind-mounted `build/` outputs Linux ELF):

```bash
make            # tm-server, tm-tests, tm-paxos-tests, tm-replay
make check      # 31 SM unit tests — must stay green
make SAN=1      # general sanitizers; ASAN=1 / UBSAN=1 / TSAN=1 also available
```

## Test stack — three layers, three orders of magnitude

| Layer | Command | Runs | Cost | What it catches |
|---|---|---|---|---|
| **Sim** (`tm-paxos-tests`) | `scripts/run-paxos-tests.sh` | 34,000 deterministic | $0 | paxos protocol bugs, lost messages, split-brain |
| **Local-TCP** (3 host processes) | `scripts/local-tcp.sh sweep` | 150 (50 × 3 scenarios) | $0 | TCP framing, socket lifecycle, signal handling |
| **Docker** (3 containers) | `scripts/docker-failure-sweep.sh sweep` | 20 (5 × 4 scenarios) | $0 | network plumbing, embedded DNS, port mapping, mid-flight failure injection |
| **Real Claude** (the demo) | `make demo-init` + 3 `claude` terminals | 1-2 | 💰 $1-15 | qualitative — for writeup |

Every layer checks two invariants per iteration:

1. **SM-state agreement** — `{id, status, owner_token, owner_agent}`
   matches across surviving replicas via `/dump`.
2. **Log-replay agreement** — surviving replicas' decision logs replay
   through `tm-replay` to byte-identical snapshots.

The sim layer additionally checks the in-engine paxos log
(`pt_paxos_replica::log_`) with a lag-tolerant disagreement budget,
pset3-style.

## Running the demo

```bash
make -C demo demo-init              # docker-compose up + bare repo + seed
scripts/synthetic-agent.sh          # FREE dry-run with 3 bash agents
make -C demo demo-status            # pretty-print task table
make -C demo demo-replay-verify     # 3-way log replay invariant check
make -C demo demo-down              # tear down
```

Failure injection (cluster must be up):

```bash
make -C demo demo-pause-1       # SIGSTOP replica-1 (recovery story)
make -C demo demo-resume-1      # SIGCONT replica-1
make -C demo demo-kill-1        # SIGKILL replica-1 (survival story — no recovery)
make -C demo demo-partition-1   # docker network disconnect
make -C demo demo-reconnect-1   # docker network connect --alias  (DNS quirk fix)
```

For the **💰 real-Claude run**: 3 separate terminals, each in
`demo/work/agent-{1,2,3}/`. Run `claude`, then `/task-loop`. Agents are
pinned to different replicas (agent-N → `localhost:808(N+1)`), so 307
redirects exercise the full distributed path.

## Hard-rules (carry these in your head while editing)

1. **Don't break the POC.** `make && make check` must stay green at every
   commit. The SM unit tests are the safety net.
2. **Don't change `task_manager_db` or `tm.hh` types** unless strictly
   necessary. They are the replicated state machine.
3. **Wall-clock determinism.** `process_req(req, now_unix)` is a pure
   function. `now_unix` MUST be stamped by the leader at propose time
   and carried inside the decided value. If a replica calls `std::time()`
   at apply time, the SMs diverge silently and paxos is broken.
4. **Decision log format stays compatible** with `tm-replay.cc` and the
   visualizer. If you change it, update both in the same patch.
5. **State transfer is out of scope.** A killed replica losing state is
   a known PoC limitation. Demo uses `pause`/`partition` for recovery,
   `kill` for survival.
6. **No new external dependencies.** The build pulls in `xxhash`,
   `llhttp`, `nlohmann_json`, cotamer. Don't add more.
7. **Skills are the agent contract.** If you change anything skill-side
   (`skills/*.md`), update both the canonical copy AND the swarm-deploy
   bundle (`.claude/skills/swarm-deploy/assets/agent-skills/*`).

## Architectural decisions (decided — don't relitigate)

- **Typed state machine**, not pancydb-on-top. Every op is one atomic
  SM transition.
- **`curl` from skills**, no MCP shim.
- **Children created during a task survive that task's failure** (so a
  re-claimed task picks up where the failed agent left off).
- **Lazy wall-clock leases**, no sweeper. `LEASE_WINDOW = 45s`.
- **Inter-replica transport is raw framed TCP**; client↔leader is
  HTTP/JSON.
- **Single log per replica** (each replica writes its own decision log
  on apply — driven by the `set_decision_logger` callback in
  `apply_decided`).

## Known limitations

- **No state-transfer.** `demo-kill-N` is the survival story (the 2
  survivors agree). `demo-pause-N` and `demo-partition-N` are the
  recovery stories (all 3 replicas agree after resume/reconnect).
- **Docker DNS quirk on reconnect.** `docker network connect` alone
  leaves the service name NXDOMAIN on peers, so paxos can't reach the
  recovering replica. Fix: `--alias replica-N` (already in
  `demo-reconnect-%`). Discovered by the docker partition sweep.
- **Real-Claude runs cost real money.** Budget 1–3 runs total for the
  demo. Sonnet ~$1–3/run, Opus ~$5–15/run.

## Where to look next

- `PLAN.md` — original design, full test plan, writeup outline
- `tm-paxos-internal.hh:107` — `apply_decided` (the on-apply callback site)
- `tm-server.cc` — paxos splice point + log callback registration
- `demo/Makefile` — every failure-injection target
- `scripts/docker-failure-sweep.sh` — automated reproducer for the
  failure-injection demo (one command, runs the whole story)
