"""Per-replica decided_slot timeline ("3 replicas, 1 timeline" figure).

For each replica's decision log (logs/tm-N.log), plot:
  X = apply_at_local (wall-clock when THAT replica applied the slot)
  Y = decided_slot (line entry's seq)

Under normal operation the three lines overlap (paxos invariant).
Under failure injection (pause / kill) one line flatlines or shoots
vertically as it catches up after recovery.

Usage:
  python3 paxos_viz.py --logs-dir demo/runs/clean-1 \
      --out demo/runs/clean-1/paxos-timeline.png

Optional vertical injection markers:
  --annotate-at 165 --annotate-label "demo-pause-1"
"""
import argparse
import json
import os
from collections import defaultdict


def parse_log(path):
    """Return list of (apply_at_local, seq, replica_index) tuples."""
    out = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            d = json.loads(line)
            apply_at = d.get("apply_at_local")
            if apply_at is None:
                # Old uninstrumented log: fall back to now_unix so the script
                # doesn't crash on pre-instrumentation data.
                apply_at = d["now_unix"]
            out.append((apply_at, d["seq"], d.get("replica_index", -1)))
    return out


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--logs-dir", required=True,
                   help="dir with tm-0.log, tm-1.log, tm-2.log")
    p.add_argument("--out", required=True, help="PNG path; SVG sibling auto-written")
    p.add_argument("--annotate-at", type=float, action="append", default=[])
    p.add_argument("--annotate-label", action="append", default=[])
    p.add_argument("--title", default=None)
    args = p.parse_args()

    runs = {}
    for r in [0, 1, 2]:
        path = os.path.join(args.logs_dir, f"tm-{r}.log")
        if not os.path.exists(path):
            print(f"warning: {path} missing — skipping")
            continue
        runs[r] = parse_log(path)

    if not runs:
        raise SystemExit("no logs found")

    # Find t0 = first apply across any replica.
    t0 = min(rec[0] for entries in runs.values() for rec in entries)
    t_end = max(rec[0] for entries in runs.values() for rec in entries)
    duration = t_end - t0

    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    cmap = plt.get_cmap("tab10")
    colors = {0: cmap(0), 1: cmap(1), 2: cmap(2)}

    fig, ax = plt.subplots(figsize=(11, 5.0))

    # Use distinct linestyles + slightly different linewidths/alpha so all
    # three lines remain visible even when they overlap (the clean-baseline
    # case, where the "3 replicas, 1 timeline" invariant means they're
    # byte-identical). Without this, only replica-2 (drawn last) is visible.
    styles = {0: ("solid",   3.2, 1.0),
              1: ("dashed",  2.2, 0.95),
              2: ("dotted",  1.6, 0.95)}
    for r in sorted(runs):
        xs = [(rec[0] - t0) for rec in runs[r]]
        ys = [rec[1] for rec in runs[r]]
        ls, lw, alpha = styles[r]
        ax.step(xs, ys, where="post", color=colors[r],
                linewidth=lw, linestyle=ls, alpha=alpha,
                label=f"replica-{r}", zorder=3 + r)

    ax.set_xlabel("seconds since first apply (wall-clock, per-replica)")
    ax.set_ylabel("decided_slot (= log line count)")
    ax.set_xlim(-1, duration + 2)
    ax.grid(axis="both", linestyle=":", alpha=0.5, zorder=1)

    # Injection markers — alternate label sides to avoid overlap.
    ymax = max(rec[1] for entries in runs.values() for rec in entries)
    for i, ts in enumerate(args.annotate_at):
        lab = args.annotate_label[i] if i < len(args.annotate_label) else ""
        ax.axvline(ts, color="red", linestyle="--", linewidth=1.2, zorder=4)
        if lab:
            ha, dx = ("right", -0.5) if i % 2 == 0 else ("left", 0.5)
            ax.text(ts + dx, ymax * 1.02, lab, color="red", fontsize=8,
                    rotation=0, ha=ha, va="bottom")

    ax.legend(loc="lower right", fontsize=9, framealpha=0.9)

    n_slots = max(len(entries) for entries in runs.values())
    title = args.title or (f"paxos timeline: {len(runs)} replicas, "
                           f"{n_slots} decisions, {duration}s wall clock")
    ax.set_title(title, pad=14)

    fig.tight_layout()
    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    fig.savefig(args.out, dpi=150)
    svg_out = os.path.splitext(args.out)[0] + ".svg"
    fig.savefig(svg_out)
    print(f"wrote {args.out} and {svg_out}")


if __name__ == "__main__":
    main()
