# CLAUDE.md — pset4 paxos-ification

Rules for any Claude Code session working in this repo. Read these
before you touch anything.

## Project context

- This is the pset4 distributed task manager. The single-instance POC
  is **done** (Annie's partner shipped it). The next step is dropping
  Multi-Paxos on top to make it replicated. The active student is Annie
  (annieliu@g.harvard.edu); the work below is hers.
- The state machine (`tmgr::task_manager_db`) is **already** shaped to
  be replicated. The splice point is exactly one line:
  [tm-server.cc:97](tm-server.cc#L97) — the `db.process_req(tmreq, now)`
  call. Paxos drops in there.
- The pset3 paxos lives at
  `/Users/annieliupoo/Documents/harvard/code/cs2620-s26-psets-aliu104/pset3/`
  (`pt-paxos.cc`, `pancydb.cc`, `pancy_msgs.hh`). It's hardcoded to
  `pancy` types. We're either templatizing it over `<Request, Response,
  SM>` or copying it and retyping; the decision is open until we audit
  it.
- The full target spec is in [PLAN.md](PLAN.md). The README has the
  POC handoff notes. Read both before suggesting design changes.

## How to work with Annie (her preferences)

- **Plan before editing.** Annie wants to see the plan first and
  approve before code changes. Default to writing/sketching, not
  patching. When she says "go", then edit.
- **Walk her through code as you go.** She has limited bandwidth to
  read the whole codebase. When introducing a file, summarize what it
  does in 2–3 sentences before diving into specifics.
- **Be transparent about credit cost.** Annie is budget-conscious.
  Before any action that costs real money (real-Claude swarm runs,
  `/ultrareview`, long Opus sessions), say so up front. Default to:
  - Deterministic C++ tests for correctness (~free).
  - Real-Claude swarm only for the qualitative demo, 1–3 runs total.
- **Don't apologize, don't preamble.** Answer the question, edit the
  file. Keep updates short.

## Hard rules

1. **Don't break the existing POC.** `make` and `make check` must keep
   passing at every commit. The SM unit tests are the safety net.
2. **Don't change `task_manager_db` or `tm.hh` types unless
   strictly necessary.** They are the replicated SM. If you have to
   touch them, flag it loudly and explain why.
3. **Wall-clock determinism trap.** `process_req(req, now_unix)` is a
   pure function. With Paxos, `now_unix` MUST be stamped by the leader
   when proposing and carried inside the decided value. Every replica
   replays the *same* `now_unix`. If replicas pull their own clock at
   apply time, the SM diverges and Paxos is silently broken. Watch for
   this on every patch that touches the splice point.
4. **The decision log format ([tm-server.cc:35](tm-server.cc#L35))
   should stay compatible** with the replay tool / visualizer. If you
   change it, update [tm-replay.cc](tm-replay.cc) in the same patch.
5. **State transfer is out of scope.** A `kill`'d replica losing state
   is a known PoC limitation ([PLAN.md:563](PLAN.md#L563)). Do not try
   to add disk persistence or state-transfer recovery. Demo uses
   `pause`/`partition` for recovery, `kill` for survival.
6. **No new external dependencies.** The build already pulls in
   `xxhash`, `llhttp`, `nlohmann_json`, Cotamer. Don't add more.
7. **Skills are the agent contract.** If you change anything skill-side
   (`skills/*.md` or `.claude/skills/swarm-deploy/assets/agent-skills/*`),
   update both the canonical copy AND the swarm-deploy bundle, per
   [README.md:153](README.md#L153).

## Build / test commands

```bash
make            # builds tm-server, tm-tests, tm-replay
make check      # runs SM unit tests — must stay green
make SAN=1      # sanitizer build
make ASAN=1     # ASan
make UBSAN=1    # UBSan
make TSAN=1     # TSan

# Free smoke tests (need tm-server running):
./build/tm-server -V -p 8080
scripts/smoke.sh
scripts/smoke-lease.sh

# Pretty status:
cd demo && make status

# Replay + visualizer:
cd demo && make viz
```

## Credit-cost ladder (cheapest → most expensive)

| Action | Cost |
|---|---|
| Me reading source files | pennies |
| Me writing code / patches | low-to-medium (depends on length) |
| `make`, `make check`, `tm-tests` | $0 |
| Tier-1 sim tests (synthetic agents in C++) | $0 |
| `scripts/smoke.sh` against tm-server | $0 |
| Real-Claude swarm via swarm-deploy (the calculator demo) | **real money — budget 1–3 runs total** |
| `/ultrareview` of the final branch | real money but bounded |

Tell Annie before you propose anything in the bottom two rows.

## Architectural decisions already made (don't relitigate)

- Typed state machine, not pancydb-on-top. Every op is one atomic
  transition. ([PLAN.md:731](PLAN.md#L731))
- `curl` from skills, no MCP shim. ([PLAN.md:739](PLAN.md#L739))
- Children created during a task survive that task's failure.
  ([PLAN.md:744](PLAN.md#L744))
- Lazy wall-clock leases, no sweeper, `LEASE_WINDOW = 45s`.
  ([tm.hh:190](tm.hh#L190))
- Inter-replica transport is raw framed TCP; client↔leader is HTTP/JSON.
  ([PLAN.md:538](PLAN.md#L538))

## When in doubt

Ask. The cost of a clarifying question is much lower than the cost of
a 3-hour wrong patch. But spend 60 seconds in the codebase first
(grep, read), so the question is specific.
