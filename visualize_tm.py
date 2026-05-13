"""Serves a web UI to scrub through a tm-replay snapshots.json.

Usage:
  # 1. Run tm-server with logging (default tm-server.log) on each replica:
  #      ./build/tm-server
  # 2. After a run, replay all replicas' logs:
  #      ./build/tm-replay -L logs/tm-0.log -L logs/tm-1.log -L logs/tm-2.log \
  #          -o snapshots.json
  #    (Single-replica is fine too: -L tm-server.log.)
  # 3. Start this UI:
  #      python3 visualize_tm.py
  #    Open http://localhost:5050

The UI shows:
  * Agent swim lanes — each lane is one agent, with the tasks they own
    living inside their lane. Tasks animate on handover.
  * Replica chips — click one to switch the displayed world to that
    replica's view. When a replica is partitioned/lagging, you see its
    stale view by selecting it.
  * Main-lock badge on whichever lane currently holds it.
  * Event tape — only "interesting" events (handovers, lock changes,
    rejects, completes). Vanilla heartbeats are collapsed by tm-replay.
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
    --panel3: #232b36;
    --border: #2a313c;
    --border-soft: #20262f;
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
    --unclaimed: #5a6068;
    /* Agent colors — distinct hues, also used for replica chips. */
    --agent-0: #4cc2ff;
    --agent-1: #ff7b72;
    --agent-2: #56d364;
    --agent-3: #d2a8ff;
    --agent-4: #ffb454;
    --agent-5: #79c0ff;
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
  .opbar .summary.rejected { color: var(--failed); }
  .opbar .errchip {
    padding: 1px 7px; border-radius: 9999px;
    background: rgba(248, 81, 73, 0.12);
    border: 1px solid var(--failed);
    color: var(--failed);
    font-size: 11px;
    font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
    text-transform: uppercase; letter-spacing: 0.4px;
  }
  .opbar .errchip.hidden { display: none; }
  .opbar .ts, .opbar .seq {
    color: var(--muted); font-size: 12px;
    font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
  }
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
  .controls button.toggled {
    background: var(--panel3); border-color: var(--accent); color: var(--accent);
  }
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

  /* Two-column main layout: replicas (left) + agent lanes (right) */
  .main {
    display: grid;
    grid-template-columns: 220px 1fr;
    gap: 12px;
    padding: 12px 16px;
    align-items: start;
  }

  /* ── REPLICAS PANEL ──────────────────────────────────────── */
  .replicas {
    background: var(--panel);
    border: 1px solid var(--border);
    border-radius: 8px;
    padding: 10px;
    display: flex; flex-direction: column; gap: 8px;
  }
  .replicas h3 {
    margin: 0 0 4px 0;
    font-size: 11px; text-transform: uppercase;
    letter-spacing: 0.6px;
    color: var(--muted);
  }
  .replica {
    background: var(--panel2);
    border: 1px solid var(--border);
    border-left: 3px solid var(--border);
    border-radius: 6px;
    padding: 8px 10px;
    cursor: pointer;
    transition: border-color 0.12s, background 0.12s;
  }
  .replica:hover { background: var(--panel3); }
  .replica.selected {
    border-color: var(--accent);
    background: var(--panel3);
    box-shadow: 0 0 0 1px var(--accent) inset;
  }
  .replica .rid {
    font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
    font-size: 12px;
    font-weight: 600;
    display: flex; align-items: center; gap: 6px;
  }
  .replica .dot {
    width: 8px; height: 8px; border-radius: 50%;
    background: var(--muted);
    flex-shrink: 0;
  }
  .replica .dot.alive { background: var(--done); }
  .replica .dot.lagging { background: var(--ip); }
  .replica .dot.dead { background: var(--failed); }
  .replica .dot.unknown { background: var(--muted); }
  .replica .meta {
    margin-top: 4px;
    font-size: 11px; color: var(--muted);
    font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
  }
  .replica .lag-bar {
    margin-top: 5px;
    height: 4px; background: var(--bg); border-radius: 2px; overflow: hidden;
  }
  .replica .lag-bar > span {
    display: block; height: 100%;
    background: var(--ip);
  }
  .replicas .hint {
    font-size: 11px; color: var(--muted);
    padding: 4px 2px;
  }
  .solo-banner {
    margin-top: 4px;
    padding: 6px 8px;
    border: 1px dashed var(--border);
    border-radius: 6px;
    color: var(--muted); font-size: 11px;
    background: var(--panel2);
  }

  /* ── AGENT LANES ─────────────────────────────────────────── */
  .lanes {
    display: flex; flex-direction: column; gap: 10px;
  }
  .lane {
    background: var(--panel);
    border: 1px solid var(--border);
    border-radius: 8px;
    overflow: hidden;
    transition: box-shadow 0.15s;
  }
  .lane.lock-holder {
    box-shadow: 0 0 0 1px var(--pulse);
  }
  .lane-header {
    background: var(--panel2);
    border-bottom: 1px solid var(--border);
    padding: 8px 12px;
    display: flex; align-items: center; gap: 10px;
  }
  .lane-header .agent-dot {
    width: 10px; height: 10px; border-radius: 50%;
    flex-shrink: 0;
  }
  .lane-header .name {
    font-weight: 600;
    font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
    font-size: 13px;
  }
  .lane-header .count {
    color: var(--muted); font-size: 11px;
    font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
  }
  .lane-header .lock-badge {
    margin-left: auto;
    color: var(--pulse);
    font-size: 11px;
    font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
    letter-spacing: 0.4px;
  }
  .lane-cards {
    padding: 8px;
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(220px, 1fr));
    gap: 8px;
    min-height: 50px;
  }
  .lane.unclaimed .lane-cards {
    grid-template-columns: repeat(auto-fill, minmax(170px, 1fr));
  }
  .lane-empty {
    color: var(--muted); font-size: 12px;
    text-align: center; padding: 14px 0;
    grid-column: 1 / -1;
  }
  .card {
    background: var(--panel2);
    border: 1px solid var(--border);
    border-left: 3px solid var(--border);
    border-radius: 6px;
    padding: 8px 10px;
    transition: border-color 0.15s, transform 0.15s, box-shadow 0.15s;
  }
  .card.affected {
    border-color: var(--pulse);
    box-shadow: 0 0 0 2px rgba(255, 180, 84, 0.25);
    animation: pulse 1.4s ease-out;
  }
  .card.lock-holder {
    border-left-color: var(--pulse);
  }
  .card.lock-holder::after {
    content: "🔒";
    float: right;
    font-size: 11px;
    color: var(--pulse);
    margin-left: 4px;
  }
  @keyframes pulse {
    0% { transform: scale(1.02); box-shadow: 0 0 0 4px rgba(255,180,84,0.55); }
    100% { transform: scale(1); box-shadow: 0 0 0 2px rgba(255,180,84,0.25); }
  }
  .card .row1 {
    display: flex; gap: 6px; align-items: center; margin-bottom: 4px;
  }
  .card .id {
    font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
    font-size: 11px;
    color: var(--muted);
  }
  .card.status-done .id::before {
    content: "✓ "; color: var(--done);
  }
  .card.status-failed .id::before {
    content: "✗ "; color: var(--failed);
  }
  .typebadge {
    font-size: 10px;
    text-transform: uppercase;
    letter-spacing: 0.4px;
    padding: 1px 5px; border-radius: 4px;
    background: var(--bg);
  }
  .typebadge.plan { color: var(--plan); border: 1px solid var(--plan); }
  .typebadge.implement { color: var(--implement); border: 1px solid var(--implement); }
  .typebadge.merge { color: var(--merge); border: 1px solid var(--merge); }
  .card .title {
    font-size: 12px; color: var(--text);
    word-break: break-word;
    line-height: 1.35;
  }
  .card .meta {
    margin-top: 5px;
    font-size: 11px; color: var(--muted);
    font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
    display: flex; flex-direction: column; gap: 2px;
  }
  .card .fail { color: var(--failed); }
  .card.status-pending { opacity: 0.85; }
  .card.status-pending .typebadge { opacity: 0.7; }

  .lane.summary-only .lane-cards {
    grid-template-columns: repeat(auto-fill, minmax(110px, 1fr));
  }
  .stat-card {
    background: var(--panel2);
    border: 1px solid var(--border);
    border-radius: 6px;
    padding: 8px 10px;
    text-align: center;
  }
  .stat-card .num {
    font-size: 18px; font-weight: 600;
    font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
  }
  .stat-card .label {
    font-size: 10px; color: var(--muted);
    text-transform: uppercase; letter-spacing: 0.5px;
    margin-top: 2px;
  }
  .stat-card.done .num { color: var(--done); }
  .stat-card.failed .num { color: var(--failed); }
  .stat-card.pending .num { color: var(--pending); }
  .stat-card.in_progress .num { color: var(--ip); }

  /* ── EVENT TAPE ──────────────────────────────────────────── */
  .tape {
    margin: 0 16px 16px;
    background: var(--panel);
    border: 1px solid var(--border);
    border-radius: 8px;
    max-height: 220px;
    overflow-y: auto;
  }
  .tape h3 {
    margin: 0;
    padding: 8px 12px;
    font-size: 11px; text-transform: uppercase;
    letter-spacing: 0.6px;
    border-bottom: 1px solid var(--border);
    background: var(--panel2);
    color: var(--muted);
    position: sticky; top: 0;
  }
  .tape .row {
    padding: 5px 12px;
    border-bottom: 1px solid var(--border-soft);
    display: grid;
    grid-template-columns: 80px 90px 1fr;
    gap: 10px;
    align-items: center;
    cursor: pointer;
    font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
    font-size: 11px;
  }
  .tape .row:hover { background: var(--panel2); }
  .tape .row.current { background: var(--panel3); }
  .tape .row .t { color: var(--muted); }
  .tape .row .kind { font-weight: 600; }
  .tape .row.lock-acquire .kind { color: var(--pulse); }
  .tape .row.lock-release .kind { color: var(--muted); }
  .tape .row.lease_reclaim .kind { color: var(--failed); }
  .tape .row.rejected .kind { color: var(--failed); }

  .help {
    color: var(--muted); font-size: 11px;
    padding: 0 16px 16px;
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
    <div class="errchip hidden" id="errchip">-</div>
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
  <button id="next-interesting" title="Next interesting event (n)">n: next ⚡</button>
  <button id="reload" title="Reload snapshots.json (r)">Reload</button>
</div>
<div class="main">
  <aside class="replicas" id="replicas">
    <h3>Replicas <span style="font-weight:400;text-transform:none;letter-spacing:0">(click to view)</span></h3>
    <div id="replica-list"></div>
    <div class="hint" id="replica-hint">—</div>
  </aside>
  <section class="lanes" id="lanes"></section>
</div>
<div class="tape">
  <h3>Event tape — interesting events only (handovers, lock, rejects, completes)</h3>
  <div id="tape-rows"></div>
</div>
<div class="help">
  <kbd>&larr;</kbd>/<kbd>&rarr;</kbd> step &nbsp;
  <kbd>n</kbd> next interesting event &nbsp;
  <kbd>1</kbd>/<kbd>2</kbd>/<kbd>3</kbd> select replica view &nbsp;
  slider scrubs wall-clock &nbsp;
  <kbd>Home</kbd>/<kbd>End</kbd> jump &nbsp;
  <kbd>r</kbd> reload
</div>
<script>
// ── Global state ────────────────────────────────────────────
let payload = null;          // full {replicas, snapshots, interesting_events}
let snapshots = [];
let replicaIds = [];         // ordered list of replica id strings
let interesting = [];        // derived events from tm-replay
let idx = 0;
let timeMin = 0;
let timeMax = 0;
let scrubTime = 0;
let selectedReplica = null;  // which replica's view to render; null = leader

const AGENT_COLORS = [
  "#4cc2ff", "#ff7b72", "#56d364", "#d2a8ff",
  "#ffb454", "#79c0ff", "#f0883e", "#b083f0",
];

function agentColor(name) {
  if (!name) return "#5a6068";
  // Stable hash → palette index. Same agent_id → same color.
  let h = 0;
  for (let i = 0; i < name.length; i++) {
    h = ((h << 5) - h + name.charCodeAt(i)) | 0;
  }
  return AGENT_COLORS[Math.abs(h) % AGENT_COLORS.length];
}

function fmtTs(unix) {
  if (!unix || unix <= 0) return "(no timestamp)";
  const d = new Date(unix * 1000);
  return d.toISOString().replace("T", " ").slice(0, 19) + "Z";
}

function fmtClock(unix) {
  if (!unix || unix <= 0) return "--:--:--";
  const d = new Date(unix * 1000);
  return d.toISOString().slice(11, 19) + "Z";
}

function fmtRelative(secs) {
  if (secs == null) return "?";
  if (secs < 0) return "?";
  if (secs < 60) return `${secs}s`;
  if (secs < 3600) return `${Math.floor(secs / 60)}m ${secs % 60}s`;
  return `${Math.floor(secs / 3600)}h ${Math.floor((secs % 3600) / 60)}m`;
}

function shorten(s, n) {
  if (!s) return "";
  return s.length > n ? s.slice(0, n) + "..." : s;
}

function escapeHtml(s) {
  return String(s).replace(/[&<>"']/g, c => ({
    "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;"
  }[c]));
}

// Pick the state object for the currently-selected replica. Falls back to
// the snapshot's "state" (leader view) if per-replica data is missing or
// selectedReplica is unset.
function viewState(snap) {
  if (selectedReplica && snap.per_replica_state &&
      snap.per_replica_state[selectedReplica]) {
    return snap.per_replica_state[selectedReplica].state;
  }
  return snap.state;
}

function viewMeta(snap) {
  if (selectedReplica && snap.per_replica_state &&
      snap.per_replica_state[selectedReplica]) {
    const pr = snap.per_replica_state[selectedReplica];
    return { last_applied_seq: pr.last_applied_seq,
             applied_at_unix: pr.applied_at_unix };
  }
  return null;
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

// ── Replica panel ───────────────────────────────────────────
function renderReplicas() {
  const snap = snapshots[idx];
  const listEl = document.getElementById("replica-list");
  const hintEl = document.getElementById("replica-hint");
  listEl.innerHTML = "";

  // Compute cluster max seq across replicas to derive lag.
  let clusterMax = 0;
  for (const rid of replicaIds) {
    const pr = snap.per_replica_state && snap.per_replica_state[rid];
    if (pr && pr.last_applied_seq > clusterMax) clusterMax = pr.last_applied_seq;
  }

  for (const rid of replicaIds) {
    const pr = snap.per_replica_state && snap.per_replica_state[rid];
    const el = document.createElement("div");
    el.className = "replica" + (rid === selectedReplica ? " selected" : "");
    el.onclick = () => { selectedReplica = rid; render(); };

    let dotClass = "unknown";
    let lag = 0;
    let appliedSec = -1;
    if (pr) {
      lag = clusterMax - pr.last_applied_seq;
      appliedSec = (pr.applied_at_unix > 0 && snap.now_unix > 0)
        ? snap.now_unix - pr.applied_at_unix : -1;
      if (lag === 0 && (appliedSec < 0 || appliedSec <= 5)) dotClass = "alive";
      else if (lag > 0 && lag < 10) dotClass = "lagging";
      else if (lag >= 10 || appliedSec > 30) dotClass = "dead";
      else dotClass = "alive";
    }

    const lagPct = clusterMax > 0 ? Math.min(100, (lag / clusterMax) * 100) : 0;
    el.innerHTML = `
      <div class="rid">
        <span class="dot ${dotClass}"></span>
        ${escapeHtml(rid)}
      </div>
      <div class="meta">
        seq ${pr ? pr.last_applied_seq : "?"}
        ${lag > 0 ? `<span style="color:var(--ip)"> · -${lag}</span>` : ""}
      </div>
      ${appliedSec > 0 ? `<div class="meta">applied ${appliedSec}s ago</div>` : ""}
      ${lag > 0 ? `<div class="lag-bar"><span style="width:${lagPct}%"></span></div>` : ""}
    `;
    listEl.appendChild(el);
  }

  if (replicaIds.length === 1) {
    hintEl.textContent = "SOLO (single replica)";
    hintEl.style.color = "var(--muted)";
  } else if (selectedReplica) {
    hintEl.textContent = `Viewing: ${selectedReplica}`;
    hintEl.style.color = "var(--accent)";
  } else {
    hintEl.textContent = "Viewing: leader";
    hintEl.style.color = "var(--muted)";
  }
}

// ── Agent lanes ─────────────────────────────────────────────
function renderLanes() {
  const snap = snapshots[idx];
  const state = viewState(snap);
  const lanesEl = document.getElementById("lanes");
  lanesEl.innerHTML = "";

  const lock = state.main_lock || {};
  const lockHolder = lock.holder_agent || "";
  const lockHolderTaskId = lock.holder_merge_task || "";

  // Partition currently-relevant tasks by owner_agent (only count tasks
  // that are in_progress, since "currently owned" = "has an active lease").
  // pending/done/failed all bucketed into the unclaimed/summary lane.
  const byAgent = new Map();       // agent_id -> tasks[]
  const summary = { pending: [], done: [], failed: [] };
  for (const t of state.tasks || []) {
    if (t.status === "in_progress" && t.owner_agent) {
      if (!byAgent.has(t.owner_agent)) byAgent.set(t.owner_agent, []);
      byAgent.get(t.owner_agent).push(t);
    } else if (t.status in summary) {
      summary[t.status].push(t);
    } else {
      // Unknown status — bucket into pending so it's visible.
      summary.pending.push(t);
    }
  }

  // Always render every agent we've ever seen, sorted. This keeps lanes
  // stable across snapshots — an agent without current work appears
  // empty rather than vanishing.
  const allAgents = new Set();
  for (const t of state.tasks || []) {
    if (t.owner_agent) allAgents.add(t.owner_agent);
  }
  if (lockHolder) allAgents.add(lockHolder);
  const agentList = Array.from(allAgents).sort();

  for (const agent of agentList) {
    const tasks = byAgent.get(agent) || [];
    const isLockHolder = (agent === lockHolder);
    const lane = document.createElement("div");
    lane.className = "lane" + (isLockHolder ? " lock-holder" : "");
    const color = agentColor(agent);

    let cardsHtml = "";
    if (tasks.length === 0) {
      cardsHtml = `<div class="lane-empty">idle — no current claim</div>`;
    } else {
      for (const t of tasks) {
        cardsHtml += renderCard(t, snap, lockHolderTaskId);
      }
    }

    lane.innerHTML = `
      <div class="lane-header">
        <span class="agent-dot" style="background:${color}"></span>
        <span class="name">${escapeHtml(agent)}</span>
        <span class="count">${tasks.length} owned</span>
        ${isLockHolder ? `<span class="lock-badge">🔒 holds main</span>` : ""}
      </div>
      <div class="lane-cards">${cardsHtml}</div>
    `;
    lanesEl.appendChild(lane);
  }

  // Summary lane — pending/done/failed counts + the actual pending cards.
  const sumLane = document.createElement("div");
  sumLane.className = "lane unclaimed";
  const pendingCards = summary.pending.map(t =>
    renderCard(t, snap, lockHolderTaskId)).join("");
  sumLane.innerHTML = `
    <div class="lane-header">
      <span class="agent-dot" style="background:var(--unclaimed)"></span>
      <span class="name">unclaimed / archive</span>
      <span class="count">${summary.pending.length} pending · ${summary.done.length} done · ${summary.failed.length} failed</span>
    </div>
    <div class="lane-cards">
      <div class="stat-card pending"><div class="num">${summary.pending.length}</div><div class="label">pending</div></div>
      <div class="stat-card done"><div class="num">${summary.done.length}</div><div class="label">done</div></div>
      <div class="stat-card failed"><div class="num">${summary.failed.length}</div><div class="label">failed</div></div>
      ${pendingCards}
    </div>
  `;
  lanesEl.appendChild(sumLane);

  // Empty-state — no agents and no tasks yet.
  if (agentList.length === 0 && summary.pending.length === 0 &&
      summary.done.length === 0 && summary.failed.length === 0) {
    lanesEl.innerHTML = `
      <div class="lane">
        <div class="lane-header"><span class="name" style="color:var(--muted)">waiting for first event…</span></div>
      </div>` + lanesEl.innerHTML;
  }
}

function renderCard(t, snap, lockHolderTaskId) {
  const ttype = (t.spec && t.spec.type) || "plan";
  const affected = (t.id === snap.affected_task_id) ? " affected" : "";
  const lockClass = (t.id === lockHolderTaskId) ? " lock-holder" : "";
  const statusClass = ` status-${t.status || "pending"}`;
  const title = (t.spec && t.spec.title) || "(no title)";
  const lines = [];
  if (t.heartbeat_unix) {
    const age = snap.now_unix > 0 ? snap.now_unix - t.heartbeat_unix : -1;
    if (age >= 0) lines.push(`hb ${age}s ago`);
  }
  if (t.result_branch) {
    lines.push(`branch: ${escapeHtml(shorten(t.result_branch, 14))}`);
  }
  if (t.result_summary) {
    lines.push(escapeHtml(shorten(t.result_summary, 70)));
  }
  if (t.fail_reason) {
    lines.push(`<span class="fail">${escapeHtml(shorten(t.fail_reason, 70))}</span>`);
  }
  if (t.spec && t.spec.requires && t.spec.requires.length) {
    lines.push(`requires: ${t.spec.requires.join(", ")}`);
  }
  const tokenStr = t.owner_token ? ` · token ${t.owner_token}` : "";
  return `
    <div class="card${affected}${lockClass}${statusClass}">
      <div class="row1">
        <span class="typebadge ${ttype}">${ttype}</span>
        <span class="id">${escapeHtml(t.id)}${tokenStr}</span>
      </div>
      <div class="title">${escapeHtml(title)}</div>
      ${lines.length ? `<div class="meta">${lines.map(l => `<div>${l}</div>`).join("")}</div>` : ""}
    </div>
  `;
}

// ── Event tape ──────────────────────────────────────────────
function renderTape() {
  const tapeEl = document.getElementById("tape-rows");
  tapeEl.innerHTML = "";
  const curSeq = snapshots[idx] ? snapshots[idx].seq : -1;

  // Combine: tm-replay's interesting_events + completes (extracted from
  // snapshots themselves by checking op_summary). We render in seq order.
  const rows = [];
  for (const ev of interesting) {
    rows.push({
      seq: ev.seq, now_unix: ev.now_unix, kind: ev.kind,
      text: tapeRowText(ev)
    });
  }
  // Add completes from snapshot stream — they're not in "interesting"
  // because completes change state and are already kept in snapshots,
  // but they're meaningful for the tape.
  for (const s of snapshots) {
    if (s.op === "/task_complete" && s.errcode === "ok") {
      rows.push({
        seq: s.seq, now_unix: s.now_unix, kind: "complete",
        text: escapeHtml(s.op_summary || "task_complete")
      });
    }
    if (s.op === "/task_create" && s.errcode === "ok") {
      rows.push({
        seq: s.seq, now_unix: s.now_unix, kind: "create",
        text: escapeHtml(s.op_summary || "task_create")
      });
    }
  }
  rows.sort((a, b) => a.seq - b.seq);

  for (const r of rows) {
    const row = document.createElement("div");
    row.className = "row " + r.kind + (r.seq === curSeq ? " current" : "");
    row.onclick = () => {
      // Jump to the snapshot at-or-after this seq.
      let target = 0;
      for (let i = 0; i < snapshots.length; i++) {
        if (snapshots[i].seq >= r.seq) { target = i; break; }
      }
      setIdx(target);
    };
    row.innerHTML = `
      <span class="t">${fmtClock(r.now_unix)}</span>
      <span class="kind">${r.kind}</span>
      <span class="text">${r.text}</span>
    `;
    tapeEl.appendChild(row);
  }

  if (rows.length === 0) {
    tapeEl.innerHTML = `<div class="row" style="grid-template-columns:1fr;color:var(--muted)">no interesting events</div>`;
  }
}

function tapeRowText(ev) {
  if (ev.kind === "lease_reclaim") {
    return `⚡ ${escapeHtml(ev.task_id)}: ${escapeHtml(ev.from_agent)} → ${escapeHtml(ev.to_agent)} (lease expired)`;
  }
  if (ev.kind === "lock_acquire") {
    return `🔒 ${escapeHtml(ev.holder)} (token ${ev.token})`;
  }
  if (ev.kind === "lock_release") {
    return `🔓 released`;
  }
  if (ev.kind === "rejected") {
    return `${escapeHtml(ev.op_summary)} [${escapeHtml(ev.errcode)}]`;
  }
  return escapeHtml(JSON.stringify(ev));
}

// ── Header bar ──────────────────────────────────────────────
function renderHeader() {
  const snap = snapshots[idx];
  document.getElementById("seq").textContent =
    `seq ${snap.seq} (${idx} / ${snapshots.length - 1})`;
  document.getElementById("ts").textContent = fmtTs(snap.now_unix);

  const summaryEl = document.getElementById("summary");
  summaryEl.textContent = snap.op_summary || "";
  const errchip = document.getElementById("errchip");
  const ec = snap.errcode || "";
  if (ec && ec !== "ok") {
    errchip.textContent = ec;
    errchip.classList.remove("hidden");
    summaryEl.classList.add("rejected");
  } else {
    errchip.classList.add("hidden");
    summaryEl.classList.remove("rejected");
  }
  document.getElementById("scrub-ts").textContent = fmtClock(scrubTime);

  // Lock chip reflects the *viewed* replica's lock state — if a replica
  // is lagging it may still see the old lock-holder.
  const state = viewState(snap);
  const lock = state.main_lock || {};
  const lockChip = document.getElementById("lockchip");
  if (lock.holder_agent) {
    lockChip.classList.add("held");
    lockChip.textContent = `🔒 ${lock.holder_agent} via ${lock.holder_merge_task} (token ${lock.lock_token})`;
  } else {
    lockChip.classList.remove("held");
    lockChip.textContent = "main lock: free";
  }
}

function render() {
  if (!snapshots.length) return;
  renderHeader();
  renderReplicas();
  renderLanes();
  renderTape();
}

function setIdx(i) {
  if (!snapshots.length) return;
  idx = Math.max(0, Math.min(snapshots.length - 1, i));
  scrubTime = snapshots[idx].now_unix || timeMin;
  document.getElementById("slider").value = scrubTime;
  render();
}

function setScrubTime(t) {
  if (!snapshots.length) return;
  scrubTime = Math.max(timeMin, Math.min(timeMax, t));
  idx = idxAtTime(scrubTime);
  render();
}

function jumpToNextInteresting() {
  if (!interesting.length) return;
  const curSeq = snapshots[idx] ? snapshots[idx].seq : -1;
  // Find the next interesting event with seq > curSeq; wrap if none.
  let target = interesting.find(ev => ev.seq > curSeq);
  if (!target) target = interesting[0];
  // Locate the snapshot at-or-after that seq.
  for (let i = 0; i < snapshots.length; i++) {
    if (snapshots[i].seq >= target.seq) { setIdx(i); return; }
  }
}

async function loadSnapshots() {
  try {
    const r = await fetch("/snapshots.json", { cache: "no-store" });
    if (!r.ok) {
      document.getElementById("summary").textContent = `(failed to load: ${r.status})`;
      return;
    }
    const data = await r.json();

    // Accept both the new schema {replicas, snapshots, interesting_events}
    // and the legacy single-array schema for backward replay.
    if (Array.isArray(data)) {
      payload = { replicas: [], snapshots: data, interesting_events: [] };
    } else {
      payload = data;
    }
    snapshots = payload.snapshots || [];
    replicaIds = payload.replicas || [];
    interesting = payload.interesting_events || [];
    if (!snapshots.length) {
      document.getElementById("summary").textContent = "(snapshots.json empty)";
      return;
    }
    // Default selected replica = first one, if any. UI shows "leader view"
    // when selectedReplica is null and per_replica_state is missing.
    if (replicaIds.length > 0 && !selectedReplica) {
      selectedReplica = replicaIds[0];
    }

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
document.getElementById("next-interesting").onclick = jumpToNextInteresting;
document.getElementById("reload").onclick = loadSnapshots;

document.addEventListener("keydown", e => {
  if (e.target.tagName === "INPUT") return;
  if (e.key === "ArrowLeft") { setIdx(idx - 1); e.preventDefault(); }
  else if (e.key === "ArrowRight") { setIdx(idx + 1); e.preventDefault(); }
  else if (e.key === "Home") { setIdx(0); e.preventDefault(); }
  else if (e.key === "End") { setIdx(snapshots.length - 1); e.preventDefault(); }
  else if (e.key === "n" || e.key === "N") { jumpToNextInteresting(); e.preventDefault(); }
  else if (e.key === "r" || e.key === "R") { loadSnapshots(); }
  else if (e.key >= "1" && e.key <= "9") {
    const i = parseInt(e.key, 10) - 1;
    if (i < replicaIds.length) { selectedReplica = replicaIds[i]; render(); }
  }
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
