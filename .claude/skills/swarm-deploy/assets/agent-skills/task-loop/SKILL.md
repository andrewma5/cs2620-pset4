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

The rule: **every Bash tool call in this skill MUST start with the
preamble below.** Copy it verbatim, fill in the agent-specific values
from CLAUDE.md, and put it before any other shell logic. No exceptions.

```bash
# === STANDARD PREAMBLE — paste at the top of every Bash tool call ===
export TM_URL="http://localhost:8080"
export AGENT_ID="<from CLAUDE.md>"
export TM_REPO="<from CLAUDE.md>"
export TM_WORK="<from CLAUDE.md>"
export TMPDIR="$TM_WORK/tmp"
export SKILL_DIR="$TM_WORK/.claude/skills/task-loop"
export CLAIM_OUT="$TMPDIR/tm-claim-$AGENT_ID.json"
mkdir -p "$TMPDIR"
tm() { curl -fsS -X POST "$TM_URL$1" -H 'Content-Type: application/json' -d "$2"; }
# === END PREAMBLE ===
```

`TASK_ID`, `TOK`, and `SPEC_JSON` are per-task — set them only inside
the Bash calls that need them, after a successful claim. They never
need to survive across calls (Step 4 hands them to the subagent via
the `Agent` prompt).

`AGENT_ID` must be unique among concurrent agents. `jq` and `curl` are
required and assumed installed.

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
5. **KillShell the heartbeat.** Loop.

## Step 1: foreground claim attempt

```bash
# === STANDARD PREAMBLE === (paste verbatim, fill in values)
SERIAL=$((RANDOM * RANDOM))
RESP=$(tm /task_claim "$(jq -n --arg aid "$AGENT_ID" --argjson s "$SERIAL" \
    '{agent_id:$aid,serial:$s}')")
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
server-side and exits the moment it gets a claim or the swarm halts.

**Bash tool call with `run_in_background: true`** — paste the preamble
plus the script invocation:

```bash
# === STANDARD PREAMBLE ===
"$SKILL_DIR/tm-wait.sh"
```

The script reads `TM_URL`, `AGENT_ID`, `CLAIM_OUT` from the env you
just exported in the preamble. It writes the claim JSON to `$CLAIM_OUT`
and exits with `GOT_TASK` or `HALTED`.

Save the returned `bash_id` as `WAIT_BASH_ID`. Wait for the completion
notification (you spend zero tokens during this wait). When notified,
read the claim JSON in a NEW foreground Bash call:

```bash
# === STANDARD PREAMBLE ===
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
# === STANDARD PREAMBLE ===
export TASK_ID="t/0001"      # <-- substitute the actual claimed id
export TOK=3                 # <-- substitute the actual token
"$SKILL_DIR/tm-hb.sh"
```

The script reads `TM_URL`, `AGENT_ID`, `TASK_ID`, `TOK` from the env
the preamble + these two lines just exported. It pings
`/task_heartbeat` every 10s and runs until you `KillShell` it in Step 5.

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

ENVIRONMENT — every Bash tool call you make MUST start with these
exports (env does NOT propagate between Bash tool calls):

  export TM_URL="<TM_URL>"
  export AGENT_ID="<AGENT_ID>"
  export TM_REPO="<TM_REPO>"
  export TM_WORK="<TM_WORK>"
  export TMPDIR="$TM_WORK/tmp"
  export TASK_ID="<TASK_ID>"
  export TOK=<TOK>
  export SPEC_JSON='<SPEC_JSON>'
  tm() { curl -fsS -X POST "$TM_URL$1" -H 'Content-Type: application/json' -d "$2"; }

The PARENT agent owns ALL background tm-* scripts (heartbeat, wait, any
other long-running poll). A separate background shell is already pinging
/task_heartbeat every 10s on your behalf. DO NOT launch any background
script that talks to tm-server. DO NOT use Bash with run_in_background.
DO NOT call KillShell. You only make foreground tm() calls (claim is
already done, so you'll only call /task_complete or /task_fail).

When you finish, end with EXACTLY ONE call to /task_complete (the
common case — including hand-offs via `new_children`) or, RARELY, to
/task_fail (only if the goal is genuinely unreachable; this halts the
swarm). Then return a one-line summary string to the parent.

Almost every blocker — merge conflicts, scope-too-large, tests that
won't pass, predecessor API mismatches, lock contention — is a hand-off
via /task_complete with new_children, NOT a /task_fail. If you can
write down the prompt for a follow-up task, it's a hand-off.

/task_fail halts the entire swarm until a human runs /swarm_resume.
Use it only for genuinely unreachable goals (contradictory prompt,
missing environment, corrupt repo). Reason field MUST start with
"ABANDON:" so humans grepping the log can find these.

If any tm call returns "ok":false,"error":"fenced", STOP IMMEDIATELY.
Do not push, commit, or call task_fail. Just return the summary
"FENCED: another agent took over <TASK_ID>".

----- BEGIN SUBSKILL: <TYPE> -----
<inlined content of the matching subskill file>
----- END SUBSKILL -----
```

Substitute the env values in. For `<SPEC_JSON>`, single-quote the
compact JSON; if it contains apostrophes, use jq to escape.

The subagent runs in its own context and returns when done. Capture the
returned summary string and print it:

```
[$AGENT_ID] subagent finished <TASK_ID>: <summary>
```

## Step 5: kill heartbeat, loop

After the subagent returns:

1. Call `KillShell` on `HB_BASH_ID`.
2. Optionally remove the stale claim file (the next claim's response
   is the only source of truth, but cleaning up keeps the dir tidy):

   ```bash
   # === STANDARD PREAMBLE ===
   rm -f "$CLAIM_OUT"
   ```

3. Loop back to Step 1.

**The very next Bash tool call after `KillShell HB_BASH_ID` MUST be
the Step 1 foreground claim.** Do not read any file, do not consult
any cached claim, do not "verify" anything — go straight to the next
`/task_claim`.

The HTTP response is the **only** source of truth for what task
you've claimed. `$CLAIM_OUT` (or any other file containing a prior
claim response) is stale by definition once Step 5 has run. Reading
it instead of issuing a fresh claim risks reusing a token whose lease
has already expired — the SM will then reclaim the task and your next
real claim will see a fresh token, while you've burned a turn on the
stale view.

If `WAIT_BASH_ID` is still set from the previous idle wait and is alive
(it shouldn't be — it exits as soon as it grabs a claim), `KillShell
WAIT_BASH_ID` too.

## CRITICAL invariants

1. **Every Bash tool call begins with the standard preamble.** Env
   does NOT propagate between Bash tool calls; the preamble is how you
   re-establish state. Skipping it makes `"$SKILL_DIR/..."` invocations
   resolve to `/...` (root) and fail with "No such file or directory".

2. **The parent (you) owns ALL tm-* background scripts.** The subagent
   never starts one and never calls KillShell. If a subagent thinks it
   needs to poll something — it doesn't. Only the parent runs background
   curl loops.

3. **Background scripts MUST be launched via separate Bash tool calls
   with `run_in_background: true`.** A `( ... ) &` inside one foreground
   Bash call dies when the tool returns.

4. **Do not poll `/task_claim` from foreground Bash with `sleep`.** That
   wastes tokens. Always use the backgrounded `tm-wait.sh` script.

5. **KillShell the heartbeat after EVERY task** — every Step 5, no
   exceptions. Leaving it running between tasks ping-floods tm-server
   with stale-token heartbeats and clutters the decision log. The
   scripts do NOT self-terminate when Claude exits, so if you forget
   `KillShell` and then close Claude, the script keeps running until
   the human kills it manually.

6. **Subagent returns a summary string, not a transcript.** Tell it so
   in the prompt — the parent's context only ingests one line per task.

7. **The HTTP response from `/task_claim` is the only source of truth
   for what task you own.** Never read `$CLAIM_OUT` (or any other
   cached file containing a previous claim) after a task has finished.
   That file is written by `tm-wait.sh` purely as a vehicle for
   passing one claim from the background-wait into the foreground; it
   is stale the moment Step 4 begins. The mandatory transition out of
   Step 5 is `KillShell HB_BASH_ID` → fresh foreground `/task_claim`,
   no intermediate steps.

## Lease expiry & token bumps

The SM expires a task's claim if `now_unix > heartbeat_unix +
lease_duration`. When that happens, the next `/task_claim` finds the
task back in the pending pool and re-hands it out — **with a fresh
`owner_token`**. Any in-flight `/task_heartbeat`, `/task_complete`, or
`/task_fail` stamped with the old token will be rejected as
`"error":"fenced"`.

Symptoms you might see:

- A `/task_claim` response for a task you thought you were already
  working on, with a `token` higher than the one you held.
- A `/task_heartbeat` returning `"error":"fenced"` (the lease
  expired in the gap before your heartbeat fired, and another claim
  already minted token N+1 — possibly your own next claim).

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

If you ever observe `"ok": false, "error": "fenced"` in any tm call you
make at the parent level (rare — the parent only calls /task_claim and
KillShell), kill the heartbeat and loop. The subagent handles fencing
inside its own context.

## Logging

Print one line per significant event:

- `[agent-id] claimed <task_id> (type=...)`
- `[agent-id] dispatched subagent for <task_id>`
- `[agent-id] subagent returned: <summary>`
- `[agent-id] idle; waiting for claim in background`

That's the whole loop. Keep your context light — the subagent does the
heavy lifting.
