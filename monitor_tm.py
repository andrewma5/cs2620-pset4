"""Polls tm-server /dump endpoint and prints a line only when state changes.

Stops early when:
  - all tasks done AND no new tasks for 3 consecutive polls
  - server unreachable for 3 consecutive polls
  - 120 polls reached
"""
import json
import subprocess
import sys
import time
from datetime import datetime


URL = "http://localhost:8080/dump"
MAX_POLLS = 120
SLEEP_SECS = 10


def fetch():
    try:
        # use curl directly; -s = silent, -m timeout
        out = subprocess.run(
            ["curl", "-s", "-m", "5", URL],
            capture_output=True, text=True, timeout=10,
        )
        if out.returncode != 0:
            return None
        if not out.stdout.strip():
            return None
        return json.loads(out.stdout)
    except Exception:
        return None


def task_key(t):
    """Tuple representing the visible state of a task; change => print."""
    return (
        t.get("id"),
        (t.get("spec") or {}).get("type"),
        t.get("status"),
        t.get("owner_agent") or "",
        (t.get("result_branch") or "")[:10],
        t.get("fail_reason") or "",
    )


def lock_key(d):
    ml = d.get("main_lock") or {}
    return (ml.get("holder_agent") or "", ml.get("holder_merge_task") or "")


def short(s, n=14):
    if not s:
        return "-"
    return s if len(s) <= n else s[:n]


def fmt_change_line(now, prev_tasks, cur_tasks, prev_lock, cur_lock):
    """Build a dense diff line."""
    parts = []
    prev_ids = {t.get("id"): t for t in (prev_tasks or [])}
    cur_ids = {t.get("id"): t for t in cur_tasks}
    for tid, t in cur_ids.items():
        old = prev_ids.get(tid)
        ttype = (t.get("spec") or {}).get("type", "?")
        st = t.get("status", "?")
        owner = t.get("owner_agent") or ""
        rb = short(t.get("result_branch") or "")
        fr = t.get("fail_reason") or ""
        if old is None:
            seg = f"{tid} {ttype}: {st} (new"
            if owner:
                seg += f" {owner}"
            seg += ")"
            parts.append(seg)
            continue
        old_st = old.get("status", "?")
        old_owner = old.get("owner_agent") or ""
        old_rb = short(old.get("result_branch") or "")
        old_fr = old.get("fail_reason") or ""
        changed = []
        if old_st != st:
            changed.append(f"{old_st}->{st}")
        if old_owner != owner:
            changed.append(f"owner:{old_owner or '-'}->{owner or '-'}")
        if old_rb != rb:
            changed.append(f"branch:{rb}")
        if old_fr != fr and fr:
            changed.append(f"fail:{fr[:30]}")
        if changed:
            seg = f"{tid} {ttype}: " + ",".join(changed)
            if owner and "owner:" not in ",".join(changed):
                seg += f" ({owner})"
            parts.append(seg)
    # tasks that disappeared (unlikely but track)
    for tid in prev_ids:
        if tid not in cur_ids:
            parts.append(f"{tid} REMOVED")
    if prev_lock != cur_lock:
        holder = cur_lock[0] or "-"
        mt = cur_lock[1]
        seg = f"lock: {holder}"
        if mt:
            seg += f"/{mt}"
        parts.append(seg)
    if not parts:
        return None
    return f"[{now}] " + "  | ".join(parts)


def main():
    prev_tasks = None
    prev_lock = ("", "")
    prev_task_count = 0
    consecutive_all_done_no_new = 0
    consecutive_unreachable = 0
    poll_count = 0
    change_lines = 0
    last_dump = None
    early_exit_reason = None

    for i in range(MAX_POLLS):
        poll_count += 1
        d = fetch()
        now = datetime.now().strftime("%H:%M:%S")
        if d is None:
            consecutive_unreachable += 1
            if consecutive_unreachable >= 3:
                print(f"[{now}] server unreachable", flush=True)
                early_exit_reason = "server unreachable"
                break
            time.sleep(SLEEP_SECS)
            continue
        consecutive_unreachable = 0
        last_dump = d
        cur_tasks = d.get("tasks") or []
        cur_lock = lock_key(d)

        line = fmt_change_line(now, prev_tasks, cur_tasks, prev_lock, cur_lock)
        if line:
            print(line, flush=True)
            change_lines += 1

        # early-exit check: all done + no new tasks for 3 polls
        all_done = bool(cur_tasks) and all(
            t.get("status") == "done" for t in cur_tasks
        )
        new_count = len(cur_tasks)
        if all_done and new_count == prev_task_count and prev_task_count > 0:
            consecutive_all_done_no_new += 1
        else:
            consecutive_all_done_no_new = 0
        prev_task_count = new_count

        prev_tasks = cur_tasks
        prev_lock = cur_lock

        if consecutive_all_done_no_new >= 3:
            print("all tasks done", flush=True)
            early_exit_reason = None
            break

        if i < MAX_POLLS - 1:
            time.sleep(SLEEP_SECS)

    # final summary
    if last_dump is None:
        print("STOPPED: no successful poll", flush=True)
        return
    cur_tasks = last_dump.get("tasks") or []
    counts = {"done": 0, "failed": 0, "in_progress": 0, "pending": 0, "other": 0}
    for t in cur_tasks:
        s = t.get("status", "other")
        if s in counts:
            counts[s] += 1
        else:
            counts["other"] += 1
    if early_exit_reason:
        print(f"STOPPED: {early_exit_reason}", flush=True)
    elif all(t.get("status") == "done" for t in cur_tasks) and cur_tasks:
        print(f"DONE: {len(cur_tasks)} tasks all done", flush=True)
    else:
        print(
            f"STOPPED: max polls reached "
            f"(done={counts['done']} fail={counts['failed']} "
            f"ip={counts['in_progress']} pend={counts['pending']})",
            flush=True,
        )
    for t in cur_tasks:
        print(
            f"  {t.get('id')} {t.get('status')} "
            f"{t.get('owner_agent') or '-'} "
            f"{short(t.get('result_branch') or '', 16)}",
            flush=True,
        )

    # machine-readable trailer for the agent itself
    print(
        f"::META poll_count={poll_count} change_lines={change_lines} "
        f"tasks={len(cur_tasks)} done={counts['done']} "
        f"failed={counts['failed']} in_progress={counts['in_progress']} "
        f"pending={counts['pending']}",
        flush=True,
    )


if __name__ == "__main__":
    main()
