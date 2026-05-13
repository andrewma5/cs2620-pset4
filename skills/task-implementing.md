---
name: task-implementing
description: Do the actual coding for an `implement` task. Inlined into the subagent dispatch prompt by task-loop when the claimed task's type is "implement".
---

# task-implementing

You are a subagent dispatched by the swarm task-loop to complete one
`implement` task. Do the git work, write the code, commit, and report
your branch's tip SHA back to the task manager.

**Env does NOT propagate between Bash tool calls.** Every Bash tool
call you make MUST start by sourcing the agent's `tm.env` and setting
the three per-task vars from the dispatch prompt:

```bash
# === STANDARD PREAMBLE — paste at the top of every Bash tool call ===
source "<TM_ENV>"                 # from dispatch prompt (absolute path)
export TASK_ID="<TASK_ID>"        # from dispatch prompt
export TOK=<TOK>                  # from dispatch prompt
export SPEC_JSON='<SPEC_JSON>'    # from dispatch prompt (single-quoted)
# === END PREAMBLE ===
```

Sourcing `tm.env` establishes `TM_URL_LIST`, `TM_URL`, `AGENT_ID`,
`TM_REPO`, `TM_WORK`, `TMPDIR`, `SKILL_DIR`, `CLAIM_OUT`, picks the
python launcher (`PY`), and defines the `tm()` bash function.

## HARD RULE: all RPC goes through the `tm` CLI

Never hand-roll `curl` against tm-server. The `tm` CLI owns:

- **Field-name correctness.** The server reads the fencing token from
  the JSON key `token`. A freelanced payload that uses `tok` silently
  sends `token=0` and gets fenced.
- **Replica failover.** `TM_URL_LIST` holds the paxos replicas; the
  CLI rotates on transport failure.

The subcommands you'll use:

```bash
tm complete --id "$TASK_ID" --token "$TOK" \
            [--branch SHA] [--summary "..."] [--children-file path.json]
tm fail     --id "$TASK_ID" --token "$TOK" --reason "ABANDON: ..."
tm dump
```

**The parent agent owns ALL background tm-* scripts.** A separate
background shell is already pinging `/task_heartbeat` every 10s on your
behalf. Do NOT launch ANY background script that talks to tm-server
(no heartbeat, no claim-poll, nothing). Do NOT use Bash with
`run_in_background`. Do NOT call `KillShell`. You only make foreground
`tm` calls.

When you finish, return a one-line summary string to the parent.

## The two outcomes

There are exactly **two** ways this task ends:

1. **`tm complete`** — the common case. You either shipped the code
   (with a `--branch SHA`), or you handed off follow-up work via
   `--children-file` (with or without `--branch`). Almost every
   blocker — merge conflicts, scope-too-large, tests that won't pass,
   API mismatches with a predecessor — is a hand-off, not a failure.
2. **`tm fail`** — rare and **terminal for the entire swarm**. Use
   only when the goal is genuinely unreachable (see the dedicated
   section near the end). Calling this halts new claims swarm-wide
   until a human runs `/swarm_resume`.

The default mental model: if you can describe a follow-up task that
would unblock the goal, hand it off via `tm complete --children-file`.
Don't `tm fail`.

## Steps

1. **Read the spec.**
   ```bash
   TITLE=$(echo "$SPEC_JSON" | jq -r .title)
   PROMPT=$(echo "$SPEC_JSON" | jq -r .prompt)
   BRANCH_BASE=$(echo "$SPEC_JSON" | jq -r '.branch_base // "main"')
   REQS=$(echo "$SPEC_JSON" | jq -r '.requires[]?')
   echo "[implementing] $TITLE on top of $BRANCH_BASE"
   ```

2. **Scope check — before any code.** Read `$PROMPT` carefully and
   honestly ask: is this ~30 minutes of coherent work for one agent,
   or is it really a multi-component thing that wants further
   decomposition?

   If it's too big — multiple subsystems, unclear requirements, more
   than a handful of files, or you'd be guessing about how pieces fit
   together — **don't write any code**. Treat this `implement` task as
   a `plan` task instead. You're given that freedom explicitly: an
   over-large `implement` should be decomposed, not muscled through.

   Build a `new_children` JSON array in a file under `$TMPDIR`, then
   jump to step 9 with no `--branch` and a `--summary` like `"scope too
   large; decomposed into N children instead of implementing"`. No
   merge task will be synthesized — there's nothing to merge.

   Example decomposition payload:
   ```bash
   CHILDREN_FILE="$TMPDIR/children-$TASK_ID.json"
   jq -n '[
     {
       type: "implement",
       title: "step A",
       prompt: "...narrow, ~30-min slice...",
       branch_base: "main"
     },
     {
       type: "implement",
       title: "step B",
       prompt: "...the next slice...",
       branch_base: "main"
     }
   ]' > "$CHILDREN_FILE"
   ```

3. **Set up the worktree.** (Only if you're proceeding to write code.)
   ```bash
   WT="${TM_WORK:-/tmp/tm-work}/wt-$(echo "$TASK_ID" | tr '/' '-')"
   mkdir -p "$(dirname "$WT")"
   cd "$(dirname "$WT")"
   if [ ! -d "$TM_WORK/clone" ]; then
       git clone "$TM_REPO" "$TM_WORK/clone"
   fi
   cd "$TM_WORK/clone"
   git fetch origin
   git worktree add "$WT" -b "task/$TASK_ID" "origin/$BRANCH_BASE" 2>/dev/null \
       || git worktree add "$WT" "task/$TASK_ID"
   cd "$WT"
   ```

4. **Merge in predecessor branches.** For each `dep` in `requires`,
   look up its `result_branch` from `tm dump` and merge it.

   ```bash
   for DEP in $REQS; do
       DEP_INFO=$(tm dump | jq --arg d "$DEP" '.tasks[]|select(.id==$d)')
       DEP_BRANCH=$(echo "$DEP_INFO" | jq -r .result_branch)
       if [ -n "$DEP_BRANCH" ] && [ "$DEP_BRANCH" != "null" ] && [ "$DEP_BRANCH" != "" ]; then
           echo "[implementing] merging predecessor $DEP @ $DEP_BRANCH"
           git fetch origin "task/$DEP" || true
           if ! git merge --no-edit "$DEP_BRANCH"; then
               # Conflict with a predecessor — hand off, don't fail.
               # Record which files conflict, abort the merge, push
               # whatever progress we already have, then complete with
               # a follow-up child.
               CONFLICT_FILES=$(git diff --name-only --diff-filter=U | tr '\n' ' ')
               git merge --abort
               # Commit and push current progress (may be empty — fine).
               git add -A
               git -c user.email="$AGENT_ID@swarm.local" -c user.name="$AGENT_ID" \
                   commit --allow-empty -m "WIP $TITLE (pre-conflict)" || true
               git push origin "task/$TASK_ID"
               PROGRESS_SHA=$(git rev-parse HEAD)
               CHILDREN_FILE="$TMPDIR/conflict-$TASK_ID.json"
               jq -n \
                   --arg dep "$DEP" --arg files "$CONFLICT_FILES" \
                   --arg this_task "$TASK_ID" --arg dep_branch "$DEP_BRANCH" \
                   '[{
                     type: "implement",
                     title: ("resolve conflict between " + $this_task + " and " + $dep),
                     prompt: ("Rebase task/" + $this_task + " onto " + $dep_branch +
                              " and resolve conflicts in: " + $files),
                     branch_base: $dep_branch,
                     requires: [$this_task, $dep]
                   }]' > "$CHILDREN_FILE"
               tm complete --id "$TASK_ID" --token "$TOK" \
                   --branch "$PROGRESS_SHA" \
                   --summary "merge conflict with $DEP in $CONFLICT_FILES; handing off resolution" \
                   --children-file "$CHILDREN_FILE"
               cd "$HOME"
               # Return summary: "implement $TASK_ID: handed off conflict with $DEP"
               return 0
           fi
           DEP_HANDOFF="handoff/$(echo "$DEP" | tr '/' '-').md"
           if [ -f "$DEP_HANDOFF" ]; then
               echo "[implementing] predecessor handoff ($DEP_HANDOFF):"
               sed 's/^/    /' "$DEP_HANDOFF"
           fi
       fi
   done
   ```

   **API mismatch.** If a predecessor's handoff file says the API
   doesn't actually support what your prompt assumes, you have two
   choices, both via `tm complete`:
   - If you can write a smaller, useful slice on the actual API,
     do that and ship it normally.
   - Otherwise, complete with **no `--branch`** and a follow-up `plan`
     child whose prompt is "re-decompose `<original goal>` given
     `<dep>` exposes `<actual API>` not `<assumed API>`."

5. **Implement the work.** Standard tools (Read, Edit, Write, Bash).

   **Mid-implementation pivot.** If, partway through, you realize the
   scope is bigger than expected or your approach is wrong:
   - Stop coding. Don't push your branch (it'll be forgotten — the
     worktree leaks until `git worktree prune`, which is fine).
   - Build a `new_children` JSON file describing the better
     decomposition. The children's `branch_base` should be `main` (or
     another fresh base), **not** your abandoned branch — nothing
     depends on it.
   - Skip to step 9 with no `--branch` and a `--summary` like
     `"abandoned partial work; decomposed instead — <one-line
     reason>"`. No merge task synthesizes.

6. **Run any tests the task spec asks for.** The skill itself does not
   know what test runner this project uses. If `spec.prompt` describes
   tests to run, run them as described and self-fix once on failure;
   if you still can't make them pass, commit + push what you have,
   then `tm complete` with that SHA as `--branch` plus a follow-up
   child scoped narrowly to fixing the failing test (`type:
   "implement"`, `requires: [$TASK_ID]`, `branch_base: "main"`).

   If the task spec does not mention tests, skip this step.

7. **Write your handoff file.** Each task writes to a uniquely-named
   file under `handoff/` so concurrent siblings don't collide on merge.
   The filename is the task ID with slashes flattened to dashes — e.g.
   task `t/0003` writes `handoff/t-0003.md`.

   ```bash
   mkdir -p handoff
   HANDOFF_FILE="handoff/$(echo "$TASK_ID" | tr '/' '-').md"
   cat > "$HANDOFF_FILE" <<EOF
   # Handoff for $TASK_ID ($AGENT_ID)

   ## What this branch contains
   $TITLE

   ## API exposed
   <list functions/files/modules>

   ## How to use it from a downstream task
   <one paragraph>
   EOF
   ```

8. **Commit.**
   ```bash
   git add -A
   git -c user.email="$AGENT_ID@swarm.local" -c user.name="$AGENT_ID" \
       commit -m "$TITLE

   task: $TASK_ID
   "
   git push origin "task/$TASK_ID"
   SHA=$(git rev-parse HEAD)
   ```

9. **Optionally create follow-up tasks.** If you discovered work that
   should be a new task (a separate test scaffold, a follow-on cleanup,
   etc.), write the JSON array to a file. Otherwise, skip the
   `--children-file` flag entirely.

10. **Call `tm complete`.** When you have a real branch to ship, pass
    `--branch "$SHA"`. When you're handing off without a branch
    (scope-too-large, mid-flight pivot, API-mismatch with no
    salvageable work), omit `--branch` — the SM will not synthesize a
    merge task in that case.

    ```bash
    tm complete --id "$TASK_ID" --token "$TOK" \
        --branch "$SHA" \
        --summary "Shipped $TITLE on branch task/$TASK_ID @ $SHA."
    ```

    Or with follow-up children:
    ```bash
    tm complete --id "$TASK_ID" --token "$TOK" \
        --branch "$SHA" \
        --summary "..." \
        --children-file "$TMPDIR/children-$TASK_ID.json"
    ```

11. **Return a one-line summary** to the parent — e.g.
    `"implement $TASK_ID: shipped task/$TASK_ID @ $SHA"`. Done.
    `cd "$HOME"` is fine; the worktree stays for downstream tasks.

## Handling `fenced`

If at any point a `tm ...` call returns `"ok":false,"error":"fenced"`:
- Stop. Don't push, don't commit, don't `tm fail`.
- `cd "$HOME"`. The worktree leaks; humans run `git worktree prune` in
  the clone to clean up later.
- Return summary: `"FENCED: another agent took over $TASK_ID"`.

`fenced` is not a failure of the goal — another agent has taken over
this task. It's a different beast from "blocked" or "abandon."

## `tm fail` (rare; halts the swarm)

Call `tm fail` ONLY if the goal is genuinely unreachable: the
codebase is unsalvageable for this task, the prompt contradicts itself
in a way no follow-up could resolve, or a hard environment constraint
(no compiler, no required library, the assumed external service
doesn't exist) blocks every path.

**Calling `tm fail` halts the swarm.** The task manager will stop
handing out new tasks to any agent until a human investigates and
calls `/swarm_resume`. Already-running peers finish what they're doing
and will see `halted: true` on their next claim.

**Almost every blocker has a hand-off path via `tm complete
--children-file`. Use that.** Reach for `tm fail` only when no
follow-up could plausibly help. If you can write down the prompt for a
follow-up task, this isn't a fail — it's a hand-off.

The `--reason` value MUST start with `ABANDON:` (the CLI refuses to
send otherwise) so humans grepping the log can find these.

```bash
tm fail --id "$TASK_ID" --token "$TOK" \
    --reason "ABANDON: <specific, concrete reason no follow-up can help>"
```

Return summary: `"ABANDONED implement $TASK_ID: <reason>"`.
