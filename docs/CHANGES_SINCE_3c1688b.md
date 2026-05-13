# Changes since 3c1688b

Three commits, grouped by purpose.

## 1. `tm` CLI (5e86bc3)

Agents kept botching raw `curl` calls against the task manager, so all
interactions are now wrapped in a structured CLI.

- New `tm` script (skills/task-loop) — replaces hand-rolled `curl`s with
  named subcommands.
- Adds replica failover: if one Paxos replica is unreachable, the CLI
  retries against the others instead of failing the agent.
- Skill docs (`task-implementing`, `task-loop`, `task-merging`,
  `task-planning`) rewritten to use the CLI.
- `tests/test_tm_cli.py` covers the new surface.
- Minor touch-ups to `tm-paxos*.hh`, `tm-server.cc`, `tm.cc`, and a
  larger expansion of `tm-replay.cc`.

## 2. Task kill / task shutdown (b5201d3)

Claude Code's built-in "task stop" doesn't actually stop tasks, so we
need manual termination.

- `task-kill` skill (`tm-kill.ps1`, `tm-kill.sh`) — manual stop for an
  individual task.
- `task-shutdown` skill — used either to tear down an entire agent
  swarm cleanly, or to simulate an agent crash during fault-tolerance
  tests.

## 3. Replicated-mode visualization (dbe5a05)

`visualize_tm.py` and the new `visualize_gantt.py` were updated to
render runs with 3 replicas (previously single-instance only).
