---
name: task-planning
description: Decompose a `plan` task into child tasks. Inlined into the subagent dispatch prompt by task-loop when the claimed task's type is "plan".
---

# task-planning

You are a subagent dispatched by the swarm task-loop to complete one
`plan` task. Decompose its prompt into child tasks (further plans or
implements) and report them back via `task_complete`.

**Env does NOT propagate between Bash tool calls.** Every Bash tool
call you make MUST start with the standard preamble below. The parent
gave you the values for `TM_URL`, `AGENT_ID`, `TM_REPO`, `TM_WORK`,
`TASK_ID`, `TOK`, `SPEC_JSON` in the dispatch prompt — paste them in
verbatim:

```bash
# === STANDARD PREAMBLE — paste at the top of every Bash tool call ===
export TM_URL="..."          # from dispatch prompt
export AGENT_ID="..."        # from dispatch prompt
export TM_REPO="..."         # from dispatch prompt
export TM_WORK="..."         # from dispatch prompt
export TMPDIR="$TM_WORK/tmp"
export TASK_ID="..."         # from dispatch prompt
export TOK=...               # from dispatch prompt
export SPEC_JSON='...'       # from dispatch prompt (single-quoted)
mkdir -p "$TMPDIR"
tm() { curl -fsSL -X POST "$TM_URL$1" -H 'Content-Type: application/json' -d "$2"; }
# === END PREAMBLE ===
```

Access the spec with `echo "$SPEC_JSON" | jq -r .prompt`.

**The parent agent owns ALL background tm-* scripts.** A separate
background shell is already pinging `/task_heartbeat` every 10s on your
behalf. Do NOT launch ANY background script that talks to tm-server
(no heartbeat, no claim-poll, nothing). Do NOT use Bash with
`run_in_background`. Do NOT call `KillShell`. You only make foreground
`tm()` calls.

When you finish, return a one-line summary string to the parent.

## Steps

1. **Read the prompt.**
   ```bash
   PROMPT=$(echo "$SPEC_JSON" | jq -r .prompt)
   TITLE=$(echo "$SPEC_JSON" | jq -r .title)
   echo "[planning] $TITLE: $PROMPT"
   ```

2. **Think about decomposition.** Output a numbered list of child tasks.
   Rules:
   - Each child should be ~30 minutes of work for a competent agent.
   - Each child needs an unambiguous acceptance criterion.
   - Children that share state must be ordered via `requires`.
   - Implementation children must specify `branch_base` (usually `main`,
     unless they build on a sibling — in which case use the sibling's
     branch as the base).
   - Plan children are fine for further decomposition (depth-capped at 5).
   - Pass-off between sibling implements happens through per-task files
     in `handoff/<flattened-task-id>.md` (e.g. `handoff/t-0003.md`)
     committed to the predecessor's branch. Each task writes its own
     uniquely-named handoff file so concurrent siblings don't collide
     when their branches are merged.

3. **Build the `new_children` JSON array.** Use `jq` to construct it
   safely. Example for a calculator:

   ```bash
   NEW_CHILDREN_JSON=$(jq -n '[
     {
       type: "implement",
       title: "core arithmetic ops",
       prompt: "Implement add, subtract, multiply, divide as pure functions in calc/ops.py. Unit tests in tests/test_ops.py.",
       branch_base: "main"
     },
     {
       type: "implement",
       title: "REPL loop",
       prompt: "Implement a REPL in calc/repl.py that reads a line, parses it, and prints the result. Use the ops module from the previous task.",
       branch_base: "main",
       requires: []
     }
   ]')
   ```

   **Note:** the `requires` field uses the *task IDs of completed
   sibling tasks*, which you don't know yet at planning time. So if you
   need ordering, you have two options:
   - **Sequential planning:** decompose into one child at a time (call
     `task_complete` with one new_child each time, then claim its
     successor as another `plan` task — recursive).
   - **Use parent_id chain:** the simpler approach for the demo is to
     leave `requires` empty and let the agent claim implements in any
     order. The merge tasks created downstream will serialize via the
     main lock.

   For the calculator demo, the second approach is fine.

4. **Write a brief plan summary.**

   ```bash
   RESULT_SUMMARY="Decomposed '$TITLE' into $(echo "$NEW_CHILDREN_JSON" | jq length) tasks."
   ```

5. **Call `task_complete`** using the parent-provided `tm` helper:

   ```bash
   tm /task_complete "$(jq -n \
       --arg aid "$AGENT_ID" --argjson s $((RANDOM*RANDOM)) \
       --arg id "$TASK_ID" --argjson tok "$TOK" \
       --arg sum "$RESULT_SUMMARY" \
       --argjson kids "$NEW_CHILDREN_JSON" \
       '{agent_id:$aid,serial:$s,id:$id,token:$tok,
         result_summary:$sum,new_children:$kids}')"
   ```

6. **Return a one-line summary** to the parent — e.g.
   `"plan $TASK_ID: decomposed into N children"`. Done.

## Handling `fenced`

If `task_complete` returns `"ok":false,"error":"fenced"`:
- Don't retry. Don't call `task_fail`. The work is wasted but harmless.
- Return summary: `"FENCED: another agent took over $TASK_ID"`.

## Abandoning a plan (rare; halts the swarm)

`task_fail` is **terminal for the entire swarm**: calling it halts new
claims swarm-wide until a human runs `/swarm_resume`. Reach for it
only when no follow-up task could possibly help.

**Before abandoning, try once to decompose into a clarification
child.** A contradictory prompt usually has a workable reading; a
follow-up agent re-decomposing with explicit interpretations is almost
always better than halting:

```bash
NEW_CHILDREN_JSON=$(jq -n --arg p "$PROMPT" \
  '[{
    type: "plan",
    title: "clarify and re-decompose",
    prompt: ("The prior prompt was unclear or self-contradictory: " + $p +
             ". Identify the contradiction, pick the most useful interpretation, and decompose accordingly.")
  }]')
tm /task_complete "$(jq -n \
    --arg aid "$AGENT_ID" --argjson s $((RANDOM*RANDOM)) \
    --arg id "$TASK_ID" --argjson tok "$TOK" \
    --arg sum "prompt unclear; handing off to clarify-and-re-decompose" \
    --argjson kids "$NEW_CHILDREN_JSON" \
    '{agent_id:$aid,serial:$s,id:$id,token:$tok,
      result_summary:$sum,new_children:$kids}')"
```

Only abandon if even that won't yield a workable child — e.g. the
prompt asks for something physically impossible, or a hard environment
constraint blocks every interpretation. The `reason` field MUST start
with `ABANDON:` so humans grepping the log can find these.

```bash
REASON="ABANDON: <specific reason no follow-up plan could help>"
tm /task_fail "$(jq -n --arg aid "$AGENT_ID" --argjson s $((RANDOM*RANDOM)) \
    --arg id "$TASK_ID" --argjson tok "$TOK" --arg r "$REASON" \
    '{agent_id:$aid,serial:$s,id:$id,token:$tok,reason:$r}')"
```
Return summary: `"ABANDONED plan $TASK_ID: <reason>"`.
