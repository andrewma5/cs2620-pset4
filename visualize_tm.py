"""Serves a small web UI to scrub through a tm-replay snapshots.json.

Usage:
  # 1. Run tm-server with logging (default tm-server.log):
  #      ./build/tm-server
  # 2. After a run, replay the log into snapshots.json:
  #      ./build/tm-replay -L tm-server.log -o snapshots.json
  # 3. Start this UI:
  #      python3 visualize_tm.py
  #    Open http://localhost:5050

The page loads snapshots.json once, then keeps state client-side. Use the
slider, left/right arrow keys, or the buttons to step through. Trello-style
columns show task status; the affected task pulses on each step.

Re-run tm-replay while the page is open and click 'Reload' (or hit 'r') to
re-fetch without restarting the server.
"""

import argparse
import json
import os

from flask import Flask, jsonify, send_file


HERE = os.path.dirname(os.path.abspath(__file__))


app = Flask(__name__)


def make_app(snapshots_path: str, port: int):
    @app.route("/")
    def index():
        return INDEX_HTML

    @app.route("/snapshots.json")
    def snapshots():
        if not os.path.exists(snapshots_path):
            return jsonify({"error": f"missing {snapshots_path} - run tm-replay first"}), 404
        return send_file(snapshots_path, mimetype="application/json")

    @app.route("/health")
    def health():
        return jsonify({
            "snapshots_path": snapshots_path,
            "exists": os.path.exists(snapshots_path),
            "size": os.path.getsize(snapshots_path) if os.path.exists(snapshots_path) else 0,
        })


INDEX_HTML = r"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>tm-replay</title>
<style>
  :root {
    --bg: #0e1116;
    --panel: #161b22;
    --panel2: #1c232c;
    --border: #2a313c;
    --text: #d6d9de;
    --muted: #7d8590;
    --accent: #4cc2ff;
    --pulse: #ffb454;
    --plan: #7ee787;
    --implement: #79c0ff;
    --merge: #d2a8ff;
    --done: #2da44e;
    --failed: #f85149;
    --ip: #d29922;
    --pending: #6e7681;
  }
  * { box-sizing: border-box; }
  body {
    margin: 0; padding: 0;
    background: var(--bg); color: var(--text);
    font: 13px/1.45 -apple-system, BlinkMacSystemFont, "Segoe UI", system-ui,
          "Helvetica Neue", Arial, sans-serif;
  }
  header {
    background: var(--panel);
    border-bottom: 1px solid var(--border);
    padding: 10px 16px;
    display: grid;
    grid-template-columns: auto 1fr auto;
    gap: 14px;
    align-items: center;
  }
  .title { font-weight: 600; letter-spacing: 0.4px; }
  .opbar {
    display: flex; gap: 14px; align-items: center;
    overflow: hidden;
  }
  .opbar .summary {
    font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
    font-size: 12px;
    color: var(--accent);
    white-space: nowrap; overflow: hidden; text-overflow: ellipsis;
  }
  .opbar .ts { color: var(--muted); font-size: 12px; }
  .opbar .seq { color: var(--muted); font-size: 12px; }
  .lockchip {
    padding: 2px 8px; border-radius: 9999px;
    background: var(--panel2);
    border: 1px solid var(--border);
    font-size: 12px;
    color: var(--muted);
  }
  .lockchip.held {
    color: var(--pulse);
    border-color: var(--pulse);
  }
  .controls {
    background: var(--panel);
    border-bottom: 1px solid var(--border);
    padding: 8px 16px;
    display: flex; gap: 10px; align-items: center;
  }
  .controls button {
    background: var(--panel2); color: var(--text);
    border: 1px solid var(--border);
    padding: 4px 10px; border-radius: 6px;
    cursor: pointer; font: inherit;
  }
  .controls button:hover { border-color: var(--accent); }
  .controls input[type=range] {
    flex: 1;
    accent-color: var(--accent);
  }
  .controls .scrub-ts {
    color: var(--muted);
    font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
    font-size: 11px;
    min-width: 70px;
    text-align: right;
  }
  .board {
    display: grid;
    grid-template-columns: repeat(4, 1fr);
    gap: 12px;
    padding: 16px;
    align-items: start;
  }
  .col {
    background: var(--panel);
    border: 1px solid var(--border);
    border-radius: 8px;
    overflow: hidden;
  }
  .col h3 {
    margin: 0;
    padding: 8px 12px;
    font-size: 12px; text-transform: uppercase;
    letter-spacing: 0.6px;
    border-bottom: 1px solid var(--border);
    background: var(--panel2);
    color: var(--muted);
    display: flex; justify-content: space-between;
  }
  .col h3 .count {
    background: var(--bg); padding: 0 6px; border-radius: 8px; font-size: 11px;
  }
  .col.pending h3 { color: var(--pending); }
  .col.in_progress h3 { color: var(--ip); }
  .col.done h3 { color: var(--done); }
  .col.failed h3 { color: var(--failed); }
  .cards { padding: 8px; min-height: 60px; }
  .card {
    background: var(--panel2);
    border: 1px solid var(--border);
    border-radius: 6px;
    padding: 8px 10px;
    margin-bottom: 8px;
    transition: border-color 0.15s, transform 0.15s;
  }
  .card.affected {
    border-color: var(--pulse);
    box-shadow: 0 0 0 2px rgba(255, 180, 84, 0.25);
    animation: pulse 1.4s ease-out;
  }
  @keyframes pulse {
    0% { transform: scale(1.02); box-shadow: 0 0 0 4px rgba(255,180,84,0.55); }
    100% { transform: scale(1); box-shadow: 0 0 0 2px rgba(255,180,84,0.25); }
  }
  .card.lock-holder {
    border-left: 3px solid var(--pulse);
    padding-left: 8px;
    position: relative;
  }
  .card.lock-holder::after {
    content: "🔒 main";
    position: absolute;
    top: 6px; right: 8px;
    font-size: 10px;
    color: var(--pulse);
    letter-spacing: 0.4px;
    font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
  }
  .card .row1 {
    display: flex; gap: 8px; align-items: center; margin-bottom: 4px;
  }
  .card .id {
    font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
    font-size: 11px;
    color: var(--muted);
  }
  .typebadge {
    font-size: 10px;
    text-transform: uppercase;
    letter-spacing: 0.4px;
    padding: 1px 6px; border-radius: 4px;
    background: var(--bg);
  }
  .typebadge.plan { color: var(--plan); border: 1px solid var(--plan); }
  .typebadge.implement { color: var(--implement); border: 1px solid var(--implement); }
  .typebadge.merge { color: var(--merge); border: 1px solid var(--merge); }
  .card .title {
    font-size: 13px; color: var(--text);
    word-break: break-word;
  }
  .card .meta {
    margin-top: 6px;
    font-size: 11px; color: var(--muted);
    font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
    display: flex; flex-direction: column; gap: 2px;
  }
  .card .fail { color: var(--failed); }
  .empty {
    color: var(--muted); font-size: 12px; text-align: center; padding: 18px 0;
  }
  .help {
    color: var(--muted); font-size: 11px; padding: 8px 16px 16px;
  }
  kbd {
    background: var(--panel2); border: 1px solid var(--border);
    border-bottom-width: 2px; border-radius: 4px;
    padding: 1px 5px; font-family: ui-monospace, monospace; font-size: 11px;
  }
</style>
</head>
<body>
<header>
  <div class="title">tm-replay</div>
  <div class="opbar">
    <div class="seq" id="seq">-</div>
    <div class="ts" id="ts">-</div>
    <div class="summary" id="summary">(no snapshots loaded)</div>
  </div>
  <div class="lockchip" id="lockchip">main lock: free</div>
</header>
<div class="controls">
  <button id="first" title="Home">|&lt;</button>
  <button id="prev" title="Left arrow">&lt;</button>
  <input type="range" id="slider" min="0" max="0" value="0" step="1" list="ticks">
  <datalist id="ticks"></datalist>
  <span class="scrub-ts" id="scrub-ts">-</span>
  <button id="next" title="Right arrow">&gt;</button>
  <button id="last" title="End">&gt;|</button>
  <button id="reload" title="Reload snapshots.json (r)">Reload</button>
</div>
<div class="board" id="board">
  <div class="col pending"><h3>pending <span class="count" id="c-pending">0</span></h3><div class="cards" id="col-pending"></div></div>
  <div class="col in_progress"><h3>in_progress <span class="count" id="c-in_progress">0</span></h3><div class="cards" id="col-in_progress"></div></div>
  <div class="col done"><h3>done <span class="count" id="c-done">0</span></h3><div class="cards" id="col-done"></div></div>
  <div class="col failed"><h3>failed <span class="count" id="c-failed">0</span></h3><div class="cards" id="col-failed"></div></div>
</div>
<div class="help">
  <kbd>&larr;</kbd> / <kbd>&rarr;</kbd> step event-by-event &nbsp; slider scrubs wall-clock time &nbsp; <kbd>Home</kbd> / <kbd>End</kbd> jump &nbsp; <kbd>r</kbd> reload
</div>
<script>
let snapshots = [];
let idx = 0;
let timeMin = 0;
let timeMax = 0;
let scrubTime = 0;

function fmtTs(unix) {
  if (!unix) return "(no timestamp)";
  const d = new Date(unix * 1000);
  return d.toISOString().replace("T", " ").slice(0, 19) + "Z";
}

function fmtClock(unix) {
  if (!unix) return "--:--:--";
  const d = new Date(unix * 1000);
  return d.toISOString().slice(11, 19) + "Z";
}

function shorten(s, n) {
  if (!s) return "";
  return s.length > n ? s.slice(0, n) + "..." : s;
}

// Largest i with snapshots[i].now_unix <= t. The synthetic seq-0
// snapshot has now_unix=0, which we treat as "before timeMin" so any
// real time t lands on a real snapshot, not on init.
function idxAtTime(t) {
  if (!snapshots.length) return 0;
  if (t < timeMin) return 0;
  let lo = 1, hi = snapshots.length - 1, ans = 0;
  while (lo <= hi) {
    const mid = (lo + hi) >> 1;
    if (snapshots[mid].now_unix <= t) { ans = mid; lo = mid + 1; }
    else { hi = mid - 1; }
  }
  return ans;
}

function render() {
  if (!snapshots.length) return;
  const snap = snapshots[idx];
  document.getElementById("seq").textContent = `seq ${snap.seq} (${idx} / ${snapshots.length - 1})`;
  document.getElementById("ts").textContent = fmtTs(snap.now_unix);
  document.getElementById("summary").textContent = snap.op_summary || "";
  document.getElementById("scrub-ts").textContent = fmtClock(scrubTime);

  const lock = snap.state.main_lock || {};
  const lockChip = document.getElementById("lockchip");
  const lockHolderId = lock.holder_agent ? lock.holder_merge_task : "";
  if (lock.holder_agent) {
    lockChip.classList.add("held");
    lockChip.textContent = `main lock: ${lock.holder_agent} via ${lock.holder_merge_task} (token ${lock.lock_token})`;
  } else {
    lockChip.classList.remove("held");
    lockChip.textContent = "main lock: free";
  }

  const cols = { pending: [], in_progress: [], done: [], failed: [] };
  for (const t of snap.state.tasks || []) {
    const status = t.status in cols ? t.status : "pending";
    cols[status].push(t);
  }
  for (const status of Object.keys(cols)) {
    const container = document.getElementById("col-" + status);
    const count = document.getElementById("c-" + status);
    count.textContent = cols[status].length;
    container.innerHTML = "";
    if (!cols[status].length) {
      const e = document.createElement("div");
      e.className = "empty";
      e.textContent = "—";
      container.appendChild(e);
      continue;
    }
    for (const t of cols[status]) {
      const card = document.createElement("div");
      card.className = "card";
      if (t.id === snap.affected_task_id) card.classList.add("affected");
      if (lockHolderId && t.id === lockHolderId) card.classList.add("lock-holder");
      const ttype = (t.spec && t.spec.type) || "plan";
      card.innerHTML = `
        <div class="row1">
          <span class="typebadge ${ttype}">${ttype}</span>
          <span class="id">${t.id}</span>
        </div>
        <div class="title">${escapeHtml(t.spec && t.spec.title || "(no title)")}</div>
        <div class="meta">
          ${t.owner_agent ? `<div>owner: ${escapeHtml(t.owner_agent)} (token ${t.owner_token})</div>` : ""}
          ${t.heartbeat_unix ? `<div>hb: ${fmtTs(t.heartbeat_unix)}</div>` : ""}
          ${t.result_branch ? `<div>branch: ${escapeHtml(shorten(t.result_branch, 16))}</div>` : ""}
          ${t.result_summary ? `<div>${escapeHtml(shorten(t.result_summary, 80))}</div>` : ""}
          ${t.fail_reason ? `<div class="fail">${escapeHtml(shorten(t.fail_reason, 80))}</div>` : ""}
          ${t.spec && t.spec.requires && t.spec.requires.length ? `<div>requires: ${t.spec.requires.join(", ")}</div>` : ""}
        </div>
      `;
      container.appendChild(card);
    }
  }
}

function escapeHtml(s) {
  return String(s).replace(/[&<>"']/g, c => ({
    "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;"
  }[c]));
}

// Step to event N (used by arrows/buttons). Snaps slider to the event's time.
function setIdx(i) {
  if (!snapshots.length) return;
  idx = Math.max(0, Math.min(snapshots.length - 1, i));
  scrubTime = snapshots[idx].now_unix || timeMin;
  document.getElementById("slider").value = scrubTime;
  render();
}

// Set continuous scrub time (used by slider). Selects the latest event
// whose now_unix <= t but does not snap the slider position.
function setScrubTime(t) {
  if (!snapshots.length) return;
  scrubTime = Math.max(timeMin, Math.min(timeMax, t));
  idx = idxAtTime(scrubTime);
  render();
}

async function loadSnapshots() {
  try {
    const r = await fetch("/snapshots.json", { cache: "no-store" });
    if (!r.ok) {
      document.getElementById("summary").textContent = `(failed to load snapshots: ${r.status})`;
      return;
    }
    const data = await r.json();
    if (!Array.isArray(data) || !data.length) {
      document.getElementById("summary").textContent = "(snapshots.json empty)";
      return;
    }
    snapshots = data;
    // Determine slider time range from real (non-synthetic) snapshots.
    // snapshots[0] is the seq-0 init with now_unix=0; skip it.
    const realTimes = snapshots.slice(1).map(s => s.now_unix).filter(t => t > 0);
    if (realTimes.length) {
      timeMin = realTimes[0];
      timeMax = realTimes[realTimes.length - 1];
    } else {
      timeMin = 0;
      timeMax = 0;
    }
    const slider = document.getElementById("slider");
    slider.min = timeMin;
    slider.max = timeMax > timeMin ? timeMax : timeMin + 1;
    // Populate tick datalist (one tick per real snapshot timestamp).
    const dl = document.getElementById("ticks");
    dl.innerHTML = "";
    const seen = new Set();
    for (const t of realTimes) {
      if (seen.has(t)) continue;
      seen.add(t);
      const opt = document.createElement("option");
      opt.value = t;
      dl.appendChild(opt);
    }
    setIdx(Math.min(idx, snapshots.length - 1));
  } catch (e) {
    document.getElementById("summary").textContent = `(error: ${e.message})`;
  }
}

document.getElementById("slider").addEventListener("input", e => {
  setScrubTime(parseInt(e.target.value, 10));
});
document.getElementById("first").onclick = () => setIdx(0);
document.getElementById("prev").onclick = () => setIdx(idx - 1);
document.getElementById("next").onclick = () => setIdx(idx + 1);
document.getElementById("last").onclick = () => setIdx(snapshots.length - 1);
document.getElementById("reload").onclick = loadSnapshots;

document.addEventListener("keydown", e => {
  if (e.target.tagName === "INPUT") return;
  if (e.key === "ArrowLeft") { setIdx(idx - 1); e.preventDefault(); }
  else if (e.key === "ArrowRight") { setIdx(idx + 1); e.preventDefault(); }
  else if (e.key === "Home") { setIdx(0); e.preventDefault(); }
  else if (e.key === "End") { setIdx(snapshots.length - 1); e.preventDefault(); }
  else if (e.key === "r" || e.key === "R") { loadSnapshots(); }
});

loadSnapshots();
</script>
</body>
</html>
"""


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--snapshots", default=os.path.join(HERE, "snapshots.json"),
                   help="path to snapshots.json (from tm-replay)")
    p.add_argument("--port", type=int, default=5050,
                   help="local port to serve UI on (default: 5050)")
    p.add_argument("--host", default="127.0.0.1")
    args = p.parse_args()

    make_app(args.snapshots, args.port)
    print(f"visualize_tm: serving on http://{args.host}:{args.port}")
    print(f"  snapshots: {args.snapshots}")
    if not os.path.exists(args.snapshots):
        print(f"  (file not found yet — run tm-replay first, then refresh)")
    app.run(host=args.host, port=args.port, debug=False, use_reloader=False)


if __name__ == "__main__":
    main()
