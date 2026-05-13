---
name: task-loop
description: Join the task manager swarm. Loops on claim → dispatch-subagent → complete with heartbeats. Always run when the user invokes /task-loop or says "join the swarm".
---

# task-loop

You are an agent in a task-manager swarm. Your job in this skill is
**thin dispatch**: claim a task, hand the actual work to a subagent,
manage the heartbeat lifecycle, and loop. The subagent does the coding /
git / merge work in its own context — your context stays clean.

Other Claude agents may be running this same skill concurrently against
the same task manager.

## How env vars work in this skill (read first, this matters)

Each Bash tool call spawns a fresh shell. **`export` inside one Bash
call does NOT propagate to the next Bash call.** Vars from CLAUDE.md or
your shell snapshot may or may not survive — treat them as untrusted.

The rule: **every Bash tool call in this skill MUST start by sourcing
the agent's `tm.env`.** That one line re-establishes `TM_URL_LIST`,
`AGENT_ID`, `TM_REPO`, `TM_WORK`, `TMPDIR`, `SKILL_DIR`, `CLAIM_OUT`,
`TM_ENV`, picks the right python launcher, and defines the `tm()` bash
function. No exceptions.

```bash
# === STANDARD PREAMBLE — paste at the top of every Bash tool call ===
source "<TM_WORK from CLAUDE.md>/tm.env"
# === END PREAMBLE ===
```

`<TM_WORK>` is the absolute path printed in your CLAUDE.md (e.g.
`C:/.../agent-1`). After the `source`, `$TM_ENV` itself holds the
absolute path to `tm.env`, so subsequent uses can write `source
"$TM_ENV"` once it's set.

`TASK_ID`, `TOK`, and `SPEC_JSON` are per-task — set them only inside
the Bash calls that need them, after a successful claim. They never
need to survive across calls (Step 4 hands them to the subagent via
the `Agent` prompt).

`AGENT_ID` must be unique among concurrent agents. `python3` (or
`py -3` on Windows), and `jq` are required and assumed installed.

## Why a single CLI for everything

All RPC against tm-server goes through the bundled `tm` Python CLI
(`$SKILL_DIR/tm`). Never hand-roll `curl`. Two reasons:

1. **Correctness.** The server reads the fencing token from the JSON
   key `token`. A hand-rolled payload that uses `tok` (the env-var name)
   silently sends `token=0` and gets fenced even when the agent is the
   rightful owner. The CLI builds payloads correctly.
2. **Replica failover.** `TM_URL_LIST` holds the three paxos replicas.
   If the first replica is down or partitioned, the CLI transparently
   retries the next on transport failure (connection refused, timeout,
   5xx). Application-level errors (4xx, `ok:false` bodies) pass through
   unchanged — retrying a 400 would mask payload bugs.

`tm --help` lists the subcommands. The ones you'll use in this skill
are `tm claim` and (implicitly, via `tm-hb.sh`) `tm hb-once`. Subagents
use `tm complete`, `tm fail`, `tm create`, `tm list`, `tm dump`, `tm
lock-acquire`, `tm lock-release`.

## The loop (high level)

You will repeat these steps forever:

1. **Try a foreground claim once.**
2. **If `none:true` → idle-wait** by launching `tm-wait.sh` in the
   background. You (Claude) sleep — no tokens spent — until the script
   completes (got a claim or swarm halted).
3. **Got a claim → launch the heartbeat** by running `tm-hb.sh` in the
   background. Save its `bash_id`.
4. **Dispatch the subskill to a fresh subagent** using the `Agent` tool.
   The subagent does the work and returns a one-line summary.
5. **Kill the heartbeat OS process via `task-kill by-task`** (NOT
   `KillShell` — see Step 5 / invariant #6 for why). Loop.

## Step 1: foreground claim attempt

```bash
source "<TM_WORK>/tm.env"   source "<TM_WORK>/tm.env"   # === STANDARD PREAMBLE ===
RESP=$(tm claim)
echo "$RESP"
NONE=$(echo "$RESP" | jq -r .none)
HALTED=$(echo "$RESP" | jq -r '.halted // false')
```

`$RESP` does NOT survive into the next Bash tool call. If `$NONE` is
`false` (you got a claim), parse the fields you need (TASK_ID, TOK,
TYPE, SPEC_JSON) **inside this same Bash call** and `echo` them so you
can read them back in your context window.

**Field names matter.** The `/task_claim` JSON response uses
`task_id` and `fencing_token` — NOT `id` and `token`. Parse with
`jq -r .task_id` and `jq -r .fencing_token`, exactly as the Step 2
parse block below shows. Getting this wrong forces a second Bash
call to re-parse, and every extra second between the claim and the
heartbeat eats into the lease window.

**If `$HALTED` is `true`, the swarm is halted** — some task called
`task_fail`, and the task manager will not hand out new work until a
human investigates and runs `/swarm_resume`. There's no point polling.
Log the halt cause and exit the loop.

If `$NONE` is `false`, jump to Step 3 with the parsed values. Otherwise
fall through to Step 2.

## Step 2: idle-wait via background Bash (token-efficient)

When there are no eligible tasks, you must **NOT** loop `sleep 10` in
your foreground Bash — that re-engages the model every 10 seconds and
burns tokens.

The wait script `tm-wait.sh` is bundled at
`$TM_WORK/.claude/skills/task-loop/tm-wait.sh`. It polls `/task_claim`
server-side (via the `tm` CLI, with replica failover) and exits the
moment it gets a claim or the swarm halts.

**Bash tool call with `run_in_background: true`** — paste the preamble
plus the script invocation:

```bash
source "<TM_WORK>/tm.env"   # === STANDARD PREAMBLE ===
"$SKILL_DIR/tm-wait.sh"
```

The script reads `TM_URL_LIST`, `AGENT_ID`, `CLAIM_OUT`, `SKILL_DIR`
from the env you just exported in the preamble. It writes the claim
JSON to `$CLAIM_OUT` and exits with `GOT_TASK` or `HALTED`.

Save the returned `bash_id` as `WAIT_BASH_ID`. Wait for the completion
notification (you spend zero tokens during this wait). When notified,
read the claim JSON in a NEW foreground Bash call:

```bash
source "<TM_WORK>/tm.env"   # === STANDARD PREAMBLE ===
RESP=$(cat "$CLAIM_OUT")
echo "$RESP"
HALTED=$(echo "$RESP" | jq -r '.halted // false')
TASK_ID=$(echo "$RESP" | jq -r .task_id)
TOK=$(echo "$RESP" | jq -r .fencing_token)
TYPE=$(echo "$RESP" | jq -r .spec.type)
SPEC_JSON=$(echo "$RESP" | jq -c .spec)
echo "TASK_ID=$TASK_ID TOK=$TOK TYPE=$TYPE"
echo "SPEC_JSON=$SPEC_JSON"
if [ "$HALTED" = "true" ]; then
    echo "[$AGENT_ID] SWARM HALTED"
fi
```

Note the values printed — those are your inputs to Step 3 / Step 4.

## Step 3: launch heartbeat (immediate)

**The very next tool call after extracting the claim MUST be the
heartbeat launch.** No file reads, no `task_list` poking, no "let me
first check ..." — every second between the claim landing and the
heartbeat starting eats into the lease, and if the lease expires the
SM will reclaim the task and the next claim will mint a fresh token
(see "Lease expiry & token bumps" below).

**Bash tool call with `run_in_background: true`** — preamble plus
inline TASK_ID/TOK substituted with the literal values you noted from
Step 1 or Step 2's echo:

```bash
source "<TM_WORK>/tm.env"   # === STANDARD PREAMBLE ===
export TASK_ID="t/0001"      # <-- substitute the actual claimed id
export TOK=3                 # <-- substitute the actual token
"$SKILL_DIR/tm-hb.sh"
```

The script reads `TM_URL_LIST`, `AGENT_ID`, `TASK_ID`, `TOK`,
`SKILL_DIR` from the env the preamble + these two lines just exported.
It pings `/task_heartbeat` every 10s (via `tm hb-once`, with replica
failover) and runs until you kill it in Step 5 via `task-kill`
(not `KillShell`).

Save the returned `bash_id` as `HB_BASH_ID`.

## Step 4: dispatch subskill to a fresh subagent

The whole point of this skill is that **you don't do the work yourself**.
Instead, use the `Agent` tool with `subagent_type: general-purpose` and
hand over a self-contained prompt. The subagent gets its own context and
its own tools. Your context stays light.

Pick the subskill body to inline based on `$TYPE`:

| TYPE        | Skill file to inline                             |
|-------------|--------------------------------------------------|
| `plan`      | `.claude/skills/task-planning/SKILL.md`          |
| `implement` | `.claude/skills/task-implementing/SKILL.md`      |
| `merge`     | `.claude/skills/task-merging/SKILL.md`           |

Read the appropriate file and inline its content into the dispatch
prompt. The subagent should NOT have to find the file — embed the
instructions directly.

**Agent tool call** (one call, foreground):

- `description`: `"execute <type> task <task_id>"`
- `subagent_type`: `"general-purpose"`
- `prompt`: structured as below

```
You are a subagent dispatched by the swarm task-loop. Complete one
<TYPE> task and return a one-line summary.

ENVIRONMENT — every Bash tool call you make MUST start by sourcing the
agent's tm.env (env does NOT propagate between Bash tool calls), then
setting the three per-task vars:

  source "<TM_ENV>"
  export TASK_ID="<TASK_ID>"
  export TOK=<TOK>
  export SPEC_JSON='<SPEC_JSON>'

The `source` line establishes TM_URL_LIST, TM_URL, AGENT_ID, TM_REPO,
TM_WORK, TMPDIR, SKILL_DIR, CLAIM_OUT, TM_ENV, the python launcher (PY),
and the `tm()` bash function. The three exports below it carry the
per-task values from the parent's claim.

The PARENT agent owns ALL background tm-* scripts (heartbeat, wait, any
other long-running poll). A separate background shell is already pinging
/task_heartbeat every 10s on your behalf. DO NOT launch any background
script that talks to tm-server. DO NOT use Bash with run_in_background.
DO NOT call KillShell. You only make foreground tm() calls.

HARD RULE — all RPC against tm-server goes through the `tm` CLI:

  tm complete --id "$TASK_ID" --token "$TOK" \
              [--branch SHA] [--summary "..."] [--children-file path.json]
  tm fail     --id "$TASK_ID" --token "$TOK" --reason "ABANDON: ..."
  tm create   --spec-file path.json [--creator-task ID --creator-token N]
  tm list     [--status STATUS] [--only-mine]
  tm dump
  tm lock-acquire  --merge-task "$TASK_ID" --merge-token "$TOK"
  tm lock-release  --lock-token LOCKTOK

DO NOT hand-roll curl against tm-server. The CLI owns:
  - building the JSON payload with the correct field names (`token`,
    NOT `tok` — a freelanced "tok" payload silently sends token=0 and
    gets fenced even when the agent is the legitimate owner);
  - replica failover on transport-level failure;
  - 307 leader-redirect handling.

Almost every blocker — merge conflicts, scope-too-large, tests that
won't pass, predecessor API mismatches, lock contention — is a hand-off
via `tm complete --children-file ...`, NOT `tm fail`. If you can write
down the prompt for a follow-up task, it's a hand-off.

`tm fail` halts the entire swarm until a human runs /swarm_resume. Use
it only for genuinely unreachable goals (contradictory prompt, missing
environment, corrupt repo). The --reason MUST start with "ABANDON:" so
humans grepping the log can find these. The CLI refuses to send if it
doesn't.

If any tm call returns `"ok":false,"error":"fenced"`, STOP IMMEDIATELY.
Do not push, commit, or call `tm fail`. Return the summary string
"FENCED: another agent took over <TASK_ID>" so the parent can release
the heartbeat and let another agent reclaim the task.

----- BEGIN SUBSKILL: <TYPE> -----
<inlined content of the matching subskill file>
----- END SUBSKILL -----
```

Substitute three values in the dispatch prompt: `<TM_ENV>` (the
absolute path to the agent's tm.env, e.g. `$TM_ENV` from your sourced
preamble), `<TASK_ID>`, `<TOK>`, and `<SPEC_JSON>`. For `<SPEC_JSON>`,
single-quote the compact JSON; if it contains apostrophes, use jq to
escape. Everything else the subagent needs (TM_URL_LIST, AGENT_ID,
TM_REPO, TM_WORK, SKILL_DIR, the python launcher, the `tm()` shim)
comes from sourcing `tm.env`.

The subagent runs in its own context and returns when done. Capture the
returned summary string and print it:

```
[$AGENT_ID] subagent finished <TASK_ID>: <summary>
```

## Step 5: kill heartbeat (via task-kill, NOT KillShell), loop

After the subagent returns:

1. **Kill the heartbeat OS process via the `task-kill` skill.** Do NOT
   use `KillShell` / `TaskStop` — on Windows it reports success while
   the underlying bash subtree keeps pinging tm-server. See
   `.claude/skills/task-kill/SKILL.md` for the full failure-mode
   write-up. The recipe is one tool call:

   **Windows agent** — invoke via the PowerShell tool:

   ```powershell
   & "$env:TM_WORK\.claude\skills\task-kill\tm-kill.ps1" `
       -Mode by-task -AgentId $env:AGENT_ID -TaskId $env:TASK_ID
   ```

   **macOS / Linux agent** — Bash tool:

   ```bash
   # === STANDARD PREAMBLE ===
   "$TM_WORK/.claude/skills/task-kill/tm-kill.sh" by-task \
       --agent-id "$AGENT_ID" --task-id "$TASK_ID"
   ```

   This kills the actual `tm-hb.sh` bash subtree for `$TASK_ID`. It
   matches even if (especially if) the subagent's summary starts with
   `FENCED:` — the orphaned heartbeat would otherwise outlive the
   task and continue pinging tm-server with a stale token.

   The harness's internal `bash_id` tracking will go stale (no one
   calls `TaskStop` anymore). That's fine. The OS process is gone;
   the harness can hold a stale handle harmlessly.

2. Optionally remove the stale claim file (the next claim's response
   is the only source of truth, but cleaning up keeps the dir tidy):

   ```bash
   source "<TM_WORK>/tm.env"   # === STANDARD PREAMBLE ===
   rm -f "$CLAIM_OUT"
   ```

3. Loop back to Step 1.

**The very next call after the `task-kill by-task` invocation MUST be
the Step 1 foreground claim.** Do not read any file, do not consult
any cached claim, do not "verify" anything — go straight to the next
`tm claim`.

The HTTP response is the **only** source of truth for what task
you've claimed. `$CLAIM_OUT` (or any other file containing a prior
claim response) is stale by definition once Step 5 has run. Reading
it instead of issuing a fresh claim risks reusing a token whose lease
has already expired — the SM will then reclaim the task and your next
real claim will see a fresh token, while you've burned a turn on the
stale view.

If a previous `tm-wait.sh` bash is somehow still alive (it shouldn't
be — it self-exits on claim or halt), invoke `task-kill all-for-agent`
to clean up before re-entering Step 1. Do NOT `KillShell WAIT_BASH_ID`
for the reasons above.

## CRITICAL invariants

1. **Every Bash tool call begins with the standard preamble.** Env
   does NOT propagate between Bash tool calls; the preamble is how you
   re-establish state. Skipping it makes `"$SKILL_DIR/..."` invocations
   resolve to `/...` (root) and fail with "No such file or directory".

2. **All tm-server RPC goes through the `tm` CLI.** No hand-rolled
   curl. The CLI owns field-name correctness and replica failover.
   The bash `tm() { python3 "$SKILL_DIR/tm" "$@"; }` shim makes the
   call sites read cleanly.

3. **The parent (you) owns ALL tm-* background scripts.** The subagent
   never starts one and never invokes `task-kill`. If a subagent
   thinks it needs to poll something — it doesn't. Only the parent
   runs background loops.

4. **Background scripts MUST be launched via separate Bash tool calls
   with `run_in_background: true`.** A `( ... ) &` inside one foreground
   Bash call dies when the tool returns.

5. **Do not poll `/task_claim` from foreground Bash with `sleep`.** That
   wastes tokens. Always use the backgrounded `tm-wait.sh` script.

6. **Kill the heartbeat after EVERY task via `task-kill by-task` —
   NOT via `KillShell` / `TaskStop`.** Every Step 5, no exceptions.
   Leaving the heartbeat running between tasks ping-floods tm-server
   with stale-token heartbeats.

   **TaskStop alone does not kill the OS process on Windows.** It
   only marks the bash_id stopped in the harness's bookkeeping; the
   actual `bash.exe` + `python.exe` subtree keeps running until
   something kills it at the OS level
   (anthropics/claude-code#8865, #43944). Empirically (2026-05-13
   swarm run, traces in
   `pset4-testing-grounds/revised-replicas-real/`): every Step 5
   `TaskStop` returned `"Successfully stopped task: ..."` while the
   underlying bash kept pinging. Orphans accumulated at ~1 per task
   per agent and only died when a human ran a manual PowerShell
   sweep.

   The fix is the `task-kill` skill — see
   `.claude/skills/task-kill/SKILL.md`. Step 5 invokes
   `tm-kill.{ps1,sh} by-task --agent-id $AGENT_ID --task-id $TASK_ID`
   which uses `Get-WmiObject Win32_Process` (Windows) or `pkill -f`
   (POSIX) to actually reap the OS process.

   For end-of-session full teardown (user invoked `/task-shutdown`,
   no more tasks coming), use the `task-shutdown` skill — it calls
   `task-kill all-for-agent` plus a `verify` pass.

   The scripts do NOT self-terminate when Claude exits, so closing
   Claude without running `task-kill` (or `task-shutdown`) leaves
   the process alive. There is no automatic cleanup.

7. **Subagent returns a summary string, not a transcript.** Tell it so
   in the prompt — the parent's context only ingests one line per task.

8. **The HTTP response from `/task_claim` is the only source of truth
   for what task you own.** Never read `$CLAIM_OUT` (or any other
   cached file containing a previous claim) after a task has finished.
   That file is written by `tm-wait.sh` purely as a vehicle for
   passing one claim from the background-wait into the foreground; it
   is stale the moment Step 4 begins. The mandatory transition out of
   Step 5 is `task-kill by-task` → fresh foreground `tm claim`, no
   intermediate steps.

## Lease expiry & token bumps

The SM expires a task's claim if `now_unix > heartbeat_unix +
lease_duration`. When that happens, the next `tm claim` finds the
task back in the pending pool and re-hands it out — **with a fresh
`owner_token`**. Any in-flight `/task_heartbeat`, `/task_complete`, or
`/task_fail` stamped with the old token will be rejected as
`"error":"fenced"`.

Symptoms you might see:

- A `tm claim` response for a task you thought you were already
  working on, with a `token` higher than the one you held.
- A `tm hb-once` returning `"error":"fenced"` (the lease expired in
  the gap before your heartbeat fired, and another claim already minted
  token N+1 — possibly your own next claim).

How to avoid it:

- Launch `tm-hb.sh` as the **immediate next Bash call** after a
  successful claim. Don't insert reads, parses, or planning between
  them.
- Never leave a long-running subagent without a heartbeat. The
  parent-side heartbeat must already be in the background before the
  `Agent` tool dispatch (Step 4).
- If you ever observe a token bump on what you thought was a fresh
  claim of a *new* task, it isn't fresh — the SM is handing you the
  same task back because your previous claim's lease lapsed before
  Step 3 finished. Treat that as a bug in this loop and tighten the
  Step 1→Step 3 sequence.

## Handling fenced responses (parent side)

If you ever observe `"ok": false, "error": "fenced"` in any tm call
you make at the parent level (rare — the parent only calls `tm claim`
and `task-kill`), invoke `task-kill by-task` for the current
`$TASK_ID` and loop. The subagent handles fencing inside its own
context.

## Logging

Print one line per significant event:

- `[agent-id] claimed <task_id> (type=...)`
- `[agent-id] dispatched subagent for <task_id>`
- `[agent-id] subagent returned: <summary>`
- `[agent-id] idle; waiting for claim in background`

That's the whole loop. Keep your context light — the subagent does the
heavy lifting.
