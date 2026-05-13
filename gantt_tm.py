"""Render a static gantt chart from a tm-replay snapshots.json.

Usage:
  python3 gantt_tm.py --snapshots logs/snapshots-0.json --out demo/run-1/gantt.png

Each task gets one row:
  - light grey segment = queue time (created -> claimed)
  - colored segment    = work time   (claimed -> completed), color = owner_agent

Optional vertical injection markers for failure-injection runs (#2 / #3):
  --annotate-at 12.5 --annotate-label "demo-pause-1"
  --annotate-at 22.5 --annotate-label "demo-resume-1"
"""
import argparse
import json
import os


def parse_timeline(snapshots):
    """Return id -> {id, type, title, created_at, claimed_at, completed_at, owner_agent}."""
    tasks = {}
    for s in snapshots:
        now = s["now_unix"]
        for t in s["state"]["tasks"]:
            tid = t["id"]
            if tid not in tasks:
                tasks[tid] = {
                    "id": tid,
                    "type": t["spec"]["type"],
                    "title": t["spec"]["title"],
                    "created_at": now,
                    "claimed_at": None,
                    "completed_at": None,
                    "owner_agent": None,
                }
            entry = tasks[tid]
            if t["status"] == "in_progress" and entry["claimed_at"] is None:
                entry["claimed_at"] = now
                entry["owner_agent"] = t["owner_agent"] or "(unknown)"
            if t["status"] == "done" and entry["completed_at"] is None:
                entry["completed_at"] = now
    return tasks


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--snapshots", required=True)
    p.add_argument("--out", required=True, help="PNG path; SVG sibling is auto-written")
    p.add_argument("--annotate-at", type=float, action="append", default=[],
                   help="relative seconds to draw a vertical marker; pair with --annotate-label")
    p.add_argument("--annotate-label", action="append", default=[])
    p.add_argument("--title", default=None)
    args = p.parse_args()

    with open(args.snapshots) as f:
        snapshots = json.load(f)

    tasks = parse_timeline(snapshots)
    if not tasks:
        raise SystemExit("no tasks in snapshots")

    times = [s["now_unix"] for s in snapshots if s["now_unix"] > 0]
    t0 = min(times)
    t_end = max(times)
    duration = t_end - t0

    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.patches import Patch

    agents = sorted({t["owner_agent"] for t in tasks.values() if t["owner_agent"]})
    cmap = plt.get_cmap("tab10")
    agent_color = {a: cmap(i % 10) for i, a in enumerate(agents)}

    rows = sorted(tasks.values(), key=lambda t: (t["created_at"], t["id"]))

    fig_h = max(2.5, 0.55 * len(rows) + 1.2)
    fig, ax = plt.subplots(figsize=(11, fig_h))
    ax.invert_yaxis()

    for i, t in enumerate(rows):
        # queue: created -> claimed (light grey)
        if t["claimed_at"] is not None:
            qs = t["created_at"] - t0
            qe = t["claimed_at"] - t0
            if qe > qs:
                ax.barh(i, qe - qs, left=qs, height=0.55,
                        color="#dddddd", edgecolor="none", zorder=2)

        # work: claimed -> completed (agent-colored)
        if t["claimed_at"] is not None:
            ws = t["claimed_at"] - t0
            we = (t["completed_at"] if t["completed_at"] is not None else t_end) - t0
            color = agent_color.get(t["owner_agent"], "#666666")
            ax.barh(i, max(we - ws, 0.3), left=ws, height=0.55,
                    color=color, edgecolor="black", linewidth=0.6, zorder=3)

    ax.set_yticks(range(len(rows)))
    ax.set_yticklabels([f"{t['id']}  {t['type']}: {t['title']}" for t in rows],
                       fontsize=9)
    ax.set_xlabel("seconds since first task_create")
    ax.set_xlim(-0.5, duration + 1.0)
    ax.set_ylim(len(rows) - 0.5, -1.1)
    ax.grid(axis="x", linestyle=":", alpha=0.5, zorder=1)

    # vertical injection markers — alternate label sides so close-together
    # markers don't overlap (even-i: left of line; odd-i: right of line)
    for i, ts in enumerate(args.annotate_at):
        lab = args.annotate_label[i] if i < len(args.annotate_label) else ""
        ax.axvline(ts, color="red", linestyle="--", linewidth=1.2, zorder=4)
        if lab:
            if i % 2 == 0:
                ha, dx = "right", -0.5
            else:
                ha, dx = "left", 0.5
            ax.text(ts + dx, -0.7, lab, color="red", fontsize=8,
                    rotation=0, ha=ha, va="bottom")

    legend_items = [Patch(facecolor=agent_color[a], edgecolor="black", label=a)
                    for a in agents]
    legend_items.append(Patch(facecolor="#dddddd", edgecolor="none",
                              label="queue (created → claimed)"))
    # Place legend outside the axes (top-right of the figure) so it never
    # overlaps the bars.
    ax.legend(handles=legend_items, loc="upper left",
              bbox_to_anchor=(1.01, 1.0), fontsize=8, framealpha=0.9,
              borderaxespad=0.)

    title = args.title or (f"tm-server task timeline — {len(rows)} tasks, "
                           f"{duration}s wall clock")
    ax.set_title(title, pad=14)

    fig.tight_layout()
    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    fig.savefig(args.out, dpi=150)
    svg_out = os.path.splitext(args.out)[0] + ".svg"
    fig.savefig(svg_out)
    print(f"wrote {args.out} and {svg_out}")


if __name__ == "__main__":
    main()
