---
name: task-merging
description: Merge an implement task's branch into main, run tests, push. Inlined into the subagent dispatch prompt by task-loop when the claimed task's type is "merge".
---

# task-merging

You are a subagent dispatched by the swarm task-loop to complete one
synthetic `merge` task. The corresponding implement task's branch tip
SHA is in `spec.implement_branch_sha`. Your job: acquire the main lock,
fetch & merge into main, run tests, push, release.

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
tm complete       --id "$TASK_ID" --token "$TOK" \
                  [--branch SHA] [--summary "..."] [--children-file path.json]
tm fail           --id "$TASK_ID" --token "$TOK" --reason "ABANDON: ..."
tm lock-acquire   --merge-task "$TASK_ID" --merge-token "$TOK"
tm lock-release   --lock-token "$LOCK_TOK"
```

**The parent agent owns ALL background tm-* scripts.** A separate
background shell is already pinging `/task_heartbeat` every 10s on your
behalf. Do NOT launch ANY background script that talks to tm-server.
Do NOT use Bash with `run_in_background`. Do NOT call `KillShell`. You
only make foreground `tm` calls.

When you finish, return a one-line summary string to the parent.

## The two outcomes

There are exactly **two** ways this task ends:

1. **`tm complete`** — common case. Either you merged + pushed
   successfully (`--branch` = the new main HEAD), or you handed off
   the merge work to a follow-up task (no `--branch`, `--children-file`
   carries the next attempt).
2. **`tm fail`** — rare and **terminal for the entire swarm**. Use
   only for environment-level dead-ends (corrupt repo, missing git,
   no network to the bare repo). Calling this halts new claims
   swarm-wide until a human runs `/swarm_resume`.

Transient or content-level merge problems — lock contention, conflicts
against main, post-merge test failures — are **never** `tm fail`.
They become follow-up children via `tm complete --children-file`.

## Steps

1. **Read the spec.**
   ```bash
   IMPL_SHA=$(echo "$SPEC_JSON" | jq -r .implement_branch_sha)
   echo "[merging] $TASK_ID merging $IMPL_SHA into main"
   ```

2. **Acquire the main lock with backoff. If contended, hand off
   instead of failing.**

   Lock contention is transient — another merge is in progress on
   `main`. Failing the merge task here would halt the swarm over a
   normal serialization step, which is the worst possible outcome
   (the implement branch never lands). Re-queue instead.

   ```bash
   LOCK_TOK=""
   for SLEEP in 2 4 8 16 30 30 30; do
       RESP=$(tm lock-acquire --merge-task "$TASK_ID" --merge-token "$TOK")
       OK=$(echo "$RESP" | jq -r .ok)
       if [ "$OK" = "true" ]; then
           LOCK_TOK=$(echo "$RESP" | jq -r .lock_token)
           echo "[merging] acquired main lock (token=$LOCK_TOK)"
           break
       fi
       ERR=$(echo "$RESP" | jq -r .error)
       if [ "$ERR" = "fenced" ]; then
           echo "[merging] fenced; abandoning"
           cd "$HOME"
           # Return summary: "FENCED: another agent took over $TASK_ID"
           return 0
       fi
       echo "[merging] busy; sleeping $SLEEP"
       sleep $SLEEP
   done
   if [ -z "$LOCK_TOK" ]; then
       # Lock contended too long. Hand the merge off as a follow-up
       # merge task and complete this one with no result_branch.
       CHILDREN_FILE="$TMPDIR/retry-merge-$TASK_ID.json"
       jq -n --arg sha "$IMPL_SHA" \
           '[{
             type: "merge",
             title: ("retry merge " + $sha),
             implement_branch_sha: $sha,
             branch_base: "main"
           }]' > "$CHILDREN_FILE"
       tm complete --id "$TASK_ID" --token "$TOK" \
           --summary "main lock contended after retries; re-queueing merge of $IMPL_SHA" \
           --children-file "$CHILDREN_FILE"
       cd "$HOME"
       # Return summary: "merge $TASK_ID: lock contended; re-queued"
       return 0
   fi
   ```

3. **Fetch and merge. On conflict, hand off a rebase task.**
   ```bash
   if [ ! -d "${TM_WORK:-/tmp/tm-work}/clone" ]; then
       git clone "$TM_REPO" "${TM_WORK:-/tmp/tm-work}/clone"
   fi
   cd "${TM_WORK:-/tmp/tm-work}/clone"
   git fetch --all
   git checkout main
   git pull origin main 2>/dev/null || true
   if ! git merge --no-edit "$IMPL_SHA"; then
       CONFLICT_FILES=$(git diff --name-only --diff-filter=U | tr '\n' ' ')
       echo "[merging] merge conflict in: $CONFLICT_FILES; handing off rebase"
       git merge --abort
       tm lock-release --lock-token "$LOCK_TOK"
       # Spawn one implement child to do the rebase. The SM will
       # auto-synthesize a fresh merge task off its tm complete.
       CHILDREN_FILE="$TMPDIR/rebase-$TASK_ID.json"
       jq -n --arg sha "$IMPL_SHA" --arg files "$CONFLICT_FILES" \
           '[{
             type: "implement",
             title: ("rebase " + $sha + " onto main"),
             prompt: ("Rebase " + $sha + " onto current main and resolve conflicts in: " + $files),
             branch_base: "main"
           }]' > "$CHILDREN_FILE"
       tm complete --id "$TASK_ID" --token "$TOK" \
           --summary "merge conflict against main in $CONFLICT_FILES; handing off rebase" \
           --children-file "$CHILDREN_FILE"
       cd "$HOME"
       return 0
   fi
   ```

4. **Run any post-merge sanity tests the task spec asks for.** The
   skill itself does not know what test runner this project uses. If
   `spec.prompt` describes a verification command to run after the
   merge, run it. On real test failures (clearly attributable to the
   merge), revert with `git reset --hard HEAD~1`, release the lock,
   and `tm complete` with no `--branch` plus a `--children-file`
   carrying an `implement` task scoped to fixing the failure (with
   `branch_base: "main"` and a `--summary` like `"post-merge tests
   failed: <details>; handing off fix"`).

   If the task spec does not mention post-merge verification, skip
   this step and proceed to push.

5. **Push.**
   ```bash
   git push origin main
   FINAL_SHA=$(git rev-parse HEAD)
   ```

6. **Release the lock.**
   ```bash
   tm lock-release --lock-token "$LOCK_TOK"
   ```

7. **Complete the merge task.**
   ```bash
   tm complete --id "$TASK_ID" --token "$TOK" \
       --branch "$FINAL_SHA" \
       --summary "Merged $IMPL_SHA into main; new HEAD $FINAL_SHA."
   ```

8. **Return a one-line summary** to the parent — e.g.
   `"merge $TASK_ID: $IMPL_SHA → main @ $FINAL_SHA"`. Done.

## Handling `fenced` mid-flow

At any point, if `tm complete`/`tm hb-once`/`tm lock-release` returns
`fenced`:
- Skip remaining steps. Don't push if you haven't already.
- If you already pushed, you've still left main in a consistent state
  (your push succeeded). The task is `done` from another agent's view.
- `cd "$HOME"`. Return summary: `"FENCED: another agent took over $TASK_ID"`.

## `tm fail` (very rare; halts the swarm)

The merge skill should never `tm fail` for normal operation. Lock
contention, content-level merge conflicts, and post-merge test
failures all become hand-off children via `tm complete`.

**Reach for `tm fail` only for environment-level dead-ends** that no
follow-up task could possibly resolve:
- The bare repo is corrupt and `git fetch` errors with hard failures
  (not network blips).
- `git push origin main` fails with permission/auth errors that
  re-queueing wouldn't fix.
- `git` is missing from the environment.

**Calling `tm fail` halts the swarm.** No new tasks are claimed
swarm-wide until a human runs `/swarm_resume`.

The `--reason` value MUST start with `ABANDON:` (the CLI refuses to
send otherwise) so humans grepping the log can find these.

```bash
tm fail --id "$TASK_ID" --token "$TOK" \
    --reason "ABANDON: <specific environment-level reason>"
```

Return summary: `"ABANDONED merge $TASK_ID: <reason>"`.
