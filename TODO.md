# TODO — pset4 paxos-ification

Live working doc for Annie. Check off as you go; expect to revise as
unknowns resolve (especially after the pset3 paxos audit).

Legend: 🆓 = no Claude credit cost · 💰 = real money

---

## Phase 0 — Orientation (1–2h, 🆓 walkthroughs)

- [x] **0.1** Walk through [tm.hh](tm.hh) — request/response variants,
      `errc`, `task_status`, `task_type`, `LEASE_WINDOW_SECONDS`.
- [x] **0.2** Walk through [task_manager_db.cc](task_manager_db.cc) —
      every handler, focus on atomicity, halt flag, lease expiry, main
      lock.
- [x] **0.3** Walk through [tm-server.cc](tm-server.cc) — pay
      special attention to line 97 (the splice point) and line 35–47
      (decision log).
- [x] **0.4** Skim [tm-tests.cc](tm-tests.cc) — know what coverage
      already exists so you don't duplicate it.
- [x] **0.5** Audit your pset3 paxos at
      `../cs2620-s26-psets-aliu104/pset3/pt-paxos.cc`. **Done.**
      Findings: Multi-Paxos protocol is solid + complete (~490 lines);
      `pancy::*` types appear at ~10 sites (mechanical s/pancy/tmgr/);
      SM interface mismatch — pset3's `process_req` takes no
      `now_unix`, must be added; transport is `netsim::*` (need
      abstraction for tier-2); 3 failure schedules from PLAN are
      already implemented. Recommendation: **copy-edit, not
      templatize** (per PLAN.md:766 fallback guidance).

## Phase 1 — Decisions before coding (~30 min)

- [x] **1.1** Decide: templatize vs. copy-edit. **Resolved:
      copy-edit.** Copy `pt-paxos.cc` → `tm-paxos.cc` in this repo,
      replace `pancy::*` with `tmgr::*`, wrap the value type to carry
      leader-stamped `now_unix`.
- [x] **1.2** Decide: keep `tm-server.log` JSONL decision log format
      unchanged? **Resolved: yes, unchanged.** Only the leader writes
      to it (post-decision). Replay + visualizer keep working for
      free.
- [x] **1.3** Decide: agent default model for demo runs. **Resolved:
      Sonnet** (current swarm-deploy default).
- [x] **1.4** Decide: are you on the May 13 extension? **Resolved:
      yes — due 2026-05-13.** Today is 2026-05-10, so ~3 days runway.
      See timing note below.

## Phase 2 — Paxos integration (the bulk, ~6–10h)

- [ ] **2.1** Bring `pt_paxos_replica` into this repo under the
      chosen approach (templatize or copy-edit).
- [ ] **2.2** Define a `tmgr::decided_value` carrying
      `{now_unix, request, agent_id, serial}` — the leader stamps
      `now_unix` here when proposing. **This is the wall-clock
      determinism step. Critical.**
- [ ] **2.3** Build the transport abstraction (per
      [PLAN.md:521](PLAN.md#L521)):
  - [ ] `transport<T>` interface
  - [ ] `sim_transport` impl (wraps netsim, for tier-1 tests)
  - [ ] `tcp_transport` impl (raw length-prefixed frames between
        replicas)
- [ ] **2.4** Splice into [tm-server.cc:97](tm-server.cc#L97). Replace
      direct `db.process_req()` with paxos propose-and-wait. Single
      replica still works (degenerate Paxos).
- [ ] **2.5** Add `(agent_id, serial)` dedup table at the HTTP layer.
      Same `(agent_id, serial)` ⇒ return cached response. (Currently
      POC relies on fencing for idempotency; this is required once
      retries can hit different replicas.)
- [ ] **2.6** Leader-redirect: non-leader replicas reply
      `{redirect, leader_url}`. Update agent skill `tm()` helpers to
      follow redirects.
- [ ] **2.7** Verify `make check` still passes. Verify
      [scripts/smoke.sh](scripts/smoke.sh) and
      [scripts/smoke-lease.sh](scripts/smoke-lease.sh) still pass
      against single-replica mode.

## Phase 3 — Tier-1 simulation tests (🆓, ~3–4h)

These are the **bulk of correctness signal** and cost zero credits.
Run them at 1000+ seeds before any real-Claude work.

- [ ] **3.1** Build a `tm-paxos-tests` binary (mirrors pset3's
      `try_one_seed` structure).
- [ ] **3.2** Synthetic agent coroutine — implements the §7 loop
      against `sim_transport`. Mocks "do work": sleeps, optionally
      requests children, then `task_complete`.
- [ ] **3.3** Invariants (end-of-run, per
      [PLAN.md:619](PLAN.md#L619)):
  - [ ] Every created task reaches a terminal state.
  - [ ] No two distinct fencing tokens ever simultaneously held against
        the same task.
  - [ ] Every replica's `task_manager_db` state agrees.
  - [ ] For every `done` task: `requires` were all `done` at claim
        time.
  - [ ] `main_lock_` never held by two distinct tokens
        simultaneously.
- [ ] **3.4** Port pset3's three failure schedules:
  - [ ] `fail_leader_permanent`
  - [ ] `fail_leader_temporary`
  - [ ] `fail_split_brain`
- [ ] **3.5** Add three new agent-layer failure schedules:
  - [ ] `kill_agent` (heartbeats stop, lease expires, reclaim)
  - [ ] `stall_agent` (delay past lease)
  - [ ] `agent_during_leader_change` (cross-product)
- [ ] **3.6** Drive to 1000-seed clean at loss rates 0, 0.01, 0.05.
- [ ] **3.7** Replay pset3 regression seeds to catch latent pset3 bugs
      ([PLAN.md:765](PLAN.md#L765)).

## Phase 4 — Tier-2 integration demo (💰, ~2–3h + budgeted credits)

Real Claude CLIs against 3 docker-compose replicas, building the
calculator. Qualitative — for the writeup.

- [ ] **4.1** `docker-compose.yml` with 3 `tm-server` replicas + bind-
      mounted bare repo.
- [ ] **4.2** Update [demo/Makefile](demo/Makefile) — add
      `demo-up`, `demo-init`, `demo-pause-N`, `demo-kill-N`,
      `demo-partition-N`, `demo-reconnect-N` targets per
      [PLAN.md:657](PLAN.md#L657).
- [ ] **4.3** 💰 Dry-run #1 — confirm 3-replica + 3-Claude calculator
      runs to completion. Capture logs.
- [ ] **4.4** 💰 Demo run #2 — with at least one failure injection
      (recommended: `demo-pause-1` mid-run to show recovery). Capture
      logs.
- [ ] **4.5** (optional) 💰 Demo run #3 — kill-survival or split-brain.

## Phase 5 — Polish + writeup (~2–3h)

- [ ] **5.1** Update [README.md](README.md) — replace POC-only notes
      with replicated reality.
- [ ] **5.2** Update [PLAN.md](PLAN.md) — mark "done" sections,
      remove POC-only caveats.
- [ ] **5.3** Writeup (4–6 pages, outline in
      [PLAN.md:776](PLAN.md#L776)):
  - [ ] Problem & motivation
  - [ ] Design (architecture, SM, lifecycle, leases, fencing)
  - [ ] Implementation (SM-as-replicated-SM, transport split, two-
        phase build)
  - [ ] Testing (invariants, schedules, results table, ≥1 war story)
  - [ ] Limitations & future work (no state-transfer, etc.)
  - [ ] References
- [ ] **5.4** (optional) 💰 `/ultrareview` of the final branch.

---

## Known traps (kept here so you don't forget)

- `process_req`'s `now_unix` must be carried *inside* the Paxos-decided
  value, not pulled at apply time. Otherwise replicas diverge silently.
- `kill`'d-then-restarted replicas don't recover state. Out of scope.
- Existing pset3 paxos may have latent bugs new traffic exposes. Run
  pset3 regression seeds.
- If you touch a skill, update *both* `skills/*.md` AND the swarm-
  deploy bundle copy.
- The agent demo costs real money. Budget 1–3 runs.

## Open questions for Annie

All resolved — see Phase 1.

## Timing — May 13 deadline

Today is 2026-05-10. Due 2026-05-13. ~3 days runway.

Remaining work estimate from the original plan:

| Phase | Estimate |
|---|---|
| Phase 2 (paxos integration) | 6–10h |
| Phase 3 (tier-1 sim tests) | 3–4h |
| Phase 4 (tier-2 demo) | 2–3h + credits |
| Phase 5 (polish + writeup) | 2–3h |
| **Total** | **13–20h over 3 days** |

Tight but feasible. **Triage priorities if time runs short:**

1. **Must ship:** Phase 2 (paxos works end-to-end), Phase 3.1–3.3
   (basic invariants, even at 100 seeds), Phase 5.3 (writeup).
2. **Strongly want:** Phase 3.4–3.6 (full failure schedules at 1000+
   seeds), Phase 4.3 (one successful demo run).
3. **Nice to have:** Phase 3.5 (new agent-layer schedules), Phase 4.4
   (demo with failure injection), Phase 4.5 (third demo run).
4. **Skip first if pressed:** Phase 5.4 (`/ultrareview`).
