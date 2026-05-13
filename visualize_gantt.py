"""Gantt-style visualization of a tm-replay snapshots.json.

Usage:
  ./build/tm-replay -L logs/tm-0.log -L logs/tm-1.log -L logs/tm-2.log \
      -o snapshots.json
  python3 visualize_gantt.py
  # open http://localhost:5060

This view answers "what happened over time", complementing the snapshot
scrubber in visualize_tm.py (which answers "what is the world right now").
Each task gets a horizontal lane; segments are colored by the agent that
owned the task at that moment; lease handovers render as a red glyph at
the moment ownership flipped. The main lock and per-replica health get
their own pinned lanes at the top/bottom.
"""

import argparse
import json
import os

from flask import Flask, jsonify, send_file


HERE = os.path.dirname(os.path.abspath(__file__))


app = Flask(__name__)


def make_app(snapshots_path: str):
    @app.route("/")
    def index():
        return INDEX_HTML

    @app.route("/snapshots.json")
    def snapshots():
        if not os.path.exists(snapshots_path):
            return jsonify({"error": f"missing {snapshots_path}"}), 404
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
<title>tm-replay · Gantt</title>
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
    --done: #2da44e;
    --failed: #f85149;
    --ip: #d29922;
    --lock-gold: #ffb454;
    --reclaim-red: #f85149;
  }
  * { box-sizing: border-box; }
  html, body {
    margin: 0; padding: 0; height: 100%;
    background: var(--bg); color: var(--text);
    font: 13px/1.45 -apple-system, BlinkMacSystemFont, "Segoe UI", system-ui,
          "Helvetica Neue", Arial, sans-serif;
    overflow: hidden;
  }
  header {
    background: var(--panel);
    border-bottom: 1px solid var(--border);
    padding: 10px 16px;
    display: flex; gap: 14px; align-items: center;
  }
  .title { font-weight: 600; letter-spacing: 0.4px; }
  .meta {
    color: var(--muted);
    font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
    font-size: 12px;
  }
  .controls {
    margin-left: auto;
    display: flex; gap: 8px; align-items: center;
  }
  .controls button, .controls label {
    background: var(--panel2); color: var(--text);
    border: 1px solid var(--border);
    padding: 4px 10px; border-radius: 6px;
    cursor: pointer; font: inherit;
    user-select: none;
  }
  .controls button:hover { border-color: var(--accent); }
  .controls input[type=checkbox] { margin-right: 4px; }
  .legend {
    background: var(--panel);
    border-bottom: 1px solid var(--border);
    padding: 6px 16px;
    display: flex; gap: 16px; align-items: center;
    font-size: 11px; color: var(--muted);
    overflow-x: auto;
  }
  .legend .item {
    display: flex; gap: 5px; align-items: center;
    white-space: nowrap;
    font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
  }
  .legend .swatch {
    width: 18px; height: 10px; border-radius: 2px;
    border: 1px solid rgba(255,255,255,0.1);
  }
  .legend .agent-swatches {
    display: flex; gap: 8px;
  }

  .canvas-wrap {
    position: relative;
    height: calc(100vh - 90px);
    overflow: auto;
  }
  #gantt { display: block; }

  .tooltip {
    position: fixed;
    background: var(--panel3);
    border: 1px solid var(--accent);
    color: var(--text);
    padding: 6px 8px;
    border-radius: 4px;
    font: 11px/1.35 ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
    pointer-events: none;
    max-width: 320px;
    z-index: 100;
    display: none;
    box-shadow: 0 2px 8px rgba(0,0,0,0.5);
  }
</style>
</head>
<body>
<header>
  <div class="title">tm-replay · Gantt</div>
  <div class="meta" id="meta">loading…</div>
  <div class="controls">
    <label><input type="checkbox" id="show-rejects" checked>rejects</label>
    <label><input type="checkbox" id="show-reclaims" checked>reclaims</label>
    <button id="zoom-fit" title="Fit to window">fit</button>
    <button id="zoom-in" title="Zoom in (+)">+</button>
    <button id="zoom-out" title="Zoom out (-)">−</button>
    <button id="reload" title="Reload (r)">↻</button>
  </div>
</header>
<div class="legend" id="legend">
  <div class="item"><span class="swatch" style="background:var(--lock-gold)"></span>main lock held</div>
  <div class="item"><span class="swatch" style="background:var(--reclaim-red);border-radius:50%"></span>lease reclaim</div>
  <div class="item"><span class="swatch" style="background:var(--failed)"></span>rejected</div>
  <div class="item"><span class="swatch" style="background:var(--done)"></span>complete</div>
  <div class="item">agents:</div>
  <div class="agent-swatches" id="agent-swatches"></div>
</div>
<div class="canvas-wrap" id="wrap">
  <canvas id="gantt"></canvas>
</div>
<div class="tooltip" id="tooltip"></div>

<script>
// ── Constants ───────────────────────────────────────────────
const AGENT_COLORS = [
  "#4cc2ff", "#ff7b72", "#56d364", "#d2a8ff",
  "#ffb454", "#79c0ff", "#f0883e", "#b083f0",
];
const ROW_H = 28;             // height of each task row
const HEADER_H = 22;          // axis header height
const LOCK_ROW_H = 24;        // pinned main-lock lane height
const REPLICA_ROW_H = 14;     // per-replica health strip height
const LABEL_W = 220;          // left gutter width for task ids/titles
const PADDING = 8;
const TICK_EVERY_SEC = 10;    // wall-clock grid spacing
const MIN_PX_PER_SEC = 1;     // zoom-out limit
const MAX_PX_PER_SEC = 200;   // zoom-in limit

// ── State ───────────────────────────────────────────────────
let payload = null;
let snapshots = [];
let replicaIds = [];
let interesting = [];

let pxPerSec = 8;             // horizontal scale
let timeMin = 0, timeMax = 0;
let showRejects = true;
let showReclaims = true;

// Derived task ribbons. Each ribbon is: {
//   task_id, title, type, segments: [{from, to, owner, token}], complete_ts,
//   fail_ts, reclaims: [{ts, from_agent, to_agent}]
// }
let ribbons = [];
// Lock spans: [{from, to, holder, token}]
let lockSpans = [];
// Rejection events: [{ts, op_summary, errcode}]
let rejects = [];
// Per-replica timeline: replicaIds -> [{from, to, status}] where status is
// "in_sync" or "lagging" — computed from per_replica_state[].last_applied_seq
// compared to cluster max at each event.
let replicaTimelines = new Map();

// Hit-test items collected during draw, used by tooltip.
let hitItems = [];

// ── Color helpers ───────────────────────────────────────────
function agentColor(name) {
  if (!name) return "#5a6068";
  let h = 0;
  for (let i = 0; i < name.length; i++) {
    h = ((h << 5) - h + name.charCodeAt(i)) | 0;
  }
  return AGENT_COLORS[Math.abs(h) % AGENT_COLORS.length];
}

function fmtClock(unix) {
  if (!unix || unix <= 0) return "--:--:--";
  const d = new Date(unix * 1000);
  return d.toISOString().slice(11, 19) + "Z";
}

function fmtDelta(secs) {
  if (secs == null || secs < 0) return "?";
  if (secs < 60) return `${secs}s`;
  return `${Math.floor(secs / 60)}m ${secs % 60}s`;
}

// ── Build ribbons from snapshots ────────────────────────────
// Walks the snapshot stream and reconstructs each task's ownership
// timeline. A new segment starts whenever owner_agent changes; the prior
// segment ends at the previous snapshot's now_unix. If the new owner
// differs from a non-empty prior owner, we record a reclaim event.
function buildRibbons() {
  ribbons = [];
  lockSpans = [];
  rejects = [];
  replicaTimelines.clear();

  if (!snapshots.length) return;

  // task_id -> { ribbon, prev: {owner, token, ts} | null }
  const trackers = new Map();
  // Lock tracker.
  let lockOpen = null;  // {from, holder, token}

  // For replica health: cluster_max per snapshot is the max last_applied_seq
  // across replicas at that step. We bucket consecutive snapshots into
  // "in_sync" / "lagging" spans per replica.
  const replicaOpen = new Map();  // rid -> {from, status}
  for (const rid of replicaIds) {
    replicaTimelines.set(rid, []);
    replicaOpen.set(rid, null);
  }

  function flushReplicaSpan(rid, until) {
    const open = replicaOpen.get(rid);
    if (open) {
      replicaTimelines.get(rid).push({
        from: open.from, to: until, status: open.status,
      });
      replicaOpen.set(rid, null);
    }
  }

  for (let si = 0; si < snapshots.length; si++) {
    const snap = snapshots[si];
    const ts = snap.now_unix;
    if (!ts) continue;
    const state = snap.state || {};

    // ── Tasks ──
    const seenThisSnap = new Set();
    for (const t of state.tasks || []) {
      seenThisSnap.add(t.id);
      let tr = trackers.get(t.id);
      if (!tr) {
        tr = {
          ribbon: {
            task_id: t.id,
            title: (t.spec && t.spec.title) || "(no title)",
            type: (t.spec && t.spec.type) || "plan",
            first_seen: ts,
            segments: [],
            reclaims: [],
            complete_ts: null,
            fail_ts: null,
            final_status: t.status,
          },
          prev: null,
        };
        trackers.set(t.id, tr);
        ribbons.push(tr.ribbon);
      }
      tr.ribbon.final_status = t.status;

      // Detect ownership transitions.
      const curOwner = t.owner_agent || "";
      const curToken = t.owner_token || 0;
      const prev = tr.prev;
      const ownerChanged = !prev || prev.owner !== curOwner || prev.token !== curToken;

      if (ownerChanged) {
        // Close previous segment.
        if (prev && prev.owner) {
          // Find the open segment and close it.
          const segs = tr.ribbon.segments;
          if (segs.length && segs[segs.length - 1].to == null) {
            segs[segs.length - 1].to = ts;
          }
          // Reclaim if both old and new are non-empty.
          if (curOwner && curOwner !== prev.owner) {
            tr.ribbon.reclaims.push({
              ts, from_agent: prev.owner, to_agent: curOwner,
            });
          }
        }
        if (curOwner) {
          tr.ribbon.segments.push({
            from: ts, to: null, owner: curOwner, token: curToken,
          });
        }
        tr.prev = { owner: curOwner, token: curToken, ts };
      }

      if (t.status === "done" && tr.ribbon.complete_ts == null) {
        tr.ribbon.complete_ts = ts;
        // Close any open segment.
        const segs = tr.ribbon.segments;
        if (segs.length && segs[segs.length - 1].to == null) {
          segs[segs.length - 1].to = ts;
        }
      }
      if (t.status === "failed" && tr.ribbon.fail_ts == null) {
        tr.ribbon.fail_ts = ts;
        const segs = tr.ribbon.segments;
        if (segs.length && segs[segs.length - 1].to == null) {
          segs[segs.length - 1].to = ts;
        }
      }
    }

    // ── Lock ──
    const lock = state.main_lock || {};
    const holder = lock.holder_agent || "";
    const token = lock.lock_token || 0;
    if (holder && !lockOpen) {
      lockOpen = { from: ts, holder, token };
    } else if (!holder && lockOpen) {
      lockSpans.push({ ...lockOpen, to: ts });
      lockOpen = null;
    } else if (holder && lockOpen && (holder !== lockOpen.holder || token !== lockOpen.token)) {
      lockSpans.push({ ...lockOpen, to: ts });
      lockOpen = { from: ts, holder, token };
    }

    // ── Rejects ──
    if (snap.errcode && snap.errcode !== "ok") {
      rejects.push({
        ts, op_summary: snap.op_summary, errcode: snap.errcode,
      });
    }

    // ── Replica health ──
    if (snap.per_replica_state) {
      let clusterMax = 0;
      for (const rid of replicaIds) {
        const pr = snap.per_replica_state[rid];
        if (pr && pr.last_applied_seq > clusterMax) clusterMax = pr.last_applied_seq;
      }
      for (const rid of replicaIds) {
        const pr = snap.per_replica_state[rid];
        const lag = pr ? clusterMax - pr.last_applied_seq : 0;
        const status = lag === 0 ? "in_sync" : (lag < 10 ? "lagging" : "dead");
        const open = replicaOpen.get(rid);
        if (!open || open.status !== status) {
          if (open) {
            replicaTimelines.get(rid).push({
              from: open.from, to: ts, status: open.status,
            });
          }
          replicaOpen.set(rid, { from: ts, status });
        }
      }
    }
  }

  // Close any still-open segments at timeMax.
  for (const tr of trackers.values()) {
    const segs = tr.ribbon.segments;
    if (segs.length && segs[segs.length - 1].to == null) {
      segs[segs.length - 1].to = timeMax;
    }
  }
  if (lockOpen) lockSpans.push({ ...lockOpen, to: timeMax });
  for (const rid of replicaIds) {
    flushReplicaSpan(rid, timeMax);
  }

  // Sort ribbons by first appearance time.
  ribbons.sort((a, b) => a.first_seen - b.first_seen);
}

// ── Drawing ─────────────────────────────────────────────────
function totalHeight() {
  return HEADER_H + LOCK_ROW_H + PADDING +
         ribbons.length * ROW_H + PADDING +
         (replicaIds.length ? PADDING + replicaIds.length * REPLICA_ROW_H : 0) +
         PADDING;
}

function totalWidth() {
  const span = Math.max(1, timeMax - timeMin);
  return LABEL_W + Math.ceil(span * pxPerSec) + PADDING * 2;
}

function xForTime(t) {
  return LABEL_W + (t - timeMin) * pxPerSec;
}

function timeForX(x) {
  return timeMin + (x - LABEL_W) / pxPerSec;
}

function draw() {
  const canvas = document.getElementById("gantt");
  const W = totalWidth();
  const H = totalHeight();
  const dpr = window.devicePixelRatio || 1;
  canvas.width = W * dpr;
  canvas.height = H * dpr;
  canvas.style.width = W + "px";
  canvas.style.height = H + "px";
  const ctx = canvas.getContext("2d");
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);

  hitItems = [];

  // Background.
  ctx.fillStyle = "#0e1116";
  ctx.fillRect(0, 0, W, H);

  // ── Time axis ──
  ctx.fillStyle = "#161b22";
  ctx.fillRect(LABEL_W, 0, W - LABEL_W, HEADER_H);
  ctx.strokeStyle = "#2a313c";
  ctx.lineWidth = 1;
  ctx.beginPath();
  ctx.moveTo(0, HEADER_H + 0.5);
  ctx.lineTo(W, HEADER_H + 0.5);
  ctx.stroke();

  // Tick marks every N seconds, aligned to multiples of TICK_EVERY_SEC.
  ctx.fillStyle = "#7d8590";
  ctx.font = "10px ui-monospace, SFMono-Regular, Menlo, Consolas, monospace";
  ctx.textBaseline = "middle";
  const tickSpacing = Math.max(TICK_EVERY_SEC,
    Math.round(80 / pxPerSec / TICK_EVERY_SEC) * TICK_EVERY_SEC);
  const firstTick = Math.ceil(timeMin / tickSpacing) * tickSpacing;
  ctx.strokeStyle = "#20262f";
  ctx.beginPath();
  for (let t = firstTick; t <= timeMax; t += tickSpacing) {
    const x = xForTime(t);
    ctx.moveTo(x + 0.5, HEADER_H);
    ctx.lineTo(x + 0.5, H);
    ctx.fillText(fmtClock(t), x + 3, HEADER_H / 2);
  }
  ctx.stroke();

  // ── Main lock lane (pinned just below header) ──
  const lockY = HEADER_H + PADDING;
  ctx.fillStyle = "#1c232c";
  ctx.fillRect(0, lockY, W, LOCK_ROW_H);
  ctx.fillStyle = "#d6d9de";
  ctx.font = "11px ui-monospace, monospace";
  ctx.textBaseline = "middle";
  ctx.fillText("main lock", PADDING, lockY + LOCK_ROW_H / 2);

  for (const span of lockSpans) {
    const x = xForTime(span.from);
    const w = Math.max(2, (span.to - span.from) * pxPerSec);
    ctx.fillStyle = agentColor(span.holder);
    ctx.globalAlpha = 0.35;
    ctx.fillRect(x, lockY + 3, w, LOCK_ROW_H - 6);
    ctx.globalAlpha = 1;
    ctx.strokeStyle = "#ffb454";
    ctx.lineWidth = 1.5;
    ctx.strokeRect(x + 0.5, lockY + 3.5, w, LOCK_ROW_H - 7);
    // Holder label inside if there's room.
    if (w > 50) {
      ctx.fillStyle = "#ffb454";
      ctx.font = "10px ui-monospace, monospace";
      ctx.fillText("🔒 " + span.holder, x + 4, lockY + LOCK_ROW_H / 2);
    }
    hitItems.push({
      kind: "lock", x, y: lockY + 3, w, h: LOCK_ROW_H - 6,
      data: span,
    });
  }

  // ── Task ribbons ──
  const ribbonsY0 = lockY + LOCK_ROW_H + PADDING;
  for (let ri = 0; ri < ribbons.length; ri++) {
    const r = ribbons[ri];
    const y = ribbonsY0 + ri * ROW_H;
    // Row band.
    if (ri % 2 === 0) {
      ctx.fillStyle = "#161b22";
      ctx.fillRect(0, y, W, ROW_H);
    }
    // Left label.
    ctx.fillStyle = "#d6d9de";
    ctx.font = "11px ui-monospace, monospace";
    ctx.textBaseline = "middle";
    ctx.fillText(r.task_id, PADDING, y + ROW_H / 2);
    ctx.fillStyle = "#7d8590";
    ctx.font = "10px -apple-system, system-ui, sans-serif";
    const titleText = r.title.length > 24 ? r.title.slice(0, 22) + "…" : r.title;
    ctx.fillText(titleText, PADDING + 56, y + ROW_H / 2);

    // Type badge color stripe at far left of label area.
    const typeColor = { plan: "#7ee787", implement: "#79c0ff", merge: "#d2a8ff" }[r.type] || "#7d8590";
    ctx.fillStyle = typeColor;
    ctx.fillRect(LABEL_W - 4, y + 4, 3, ROW_H - 8);

    // Segments.
    for (const seg of r.segments) {
      const x = xForTime(seg.from);
      const w = Math.max(2, (seg.to - seg.from) * pxPerSec);
      const col = agentColor(seg.owner);
      ctx.fillStyle = col;
      ctx.fillRect(x, y + 5, w, ROW_H - 10);
      // Outline.
      ctx.strokeStyle = "rgba(0,0,0,0.4)";
      ctx.lineWidth = 1;
      ctx.strokeRect(x + 0.5, y + 5.5, w, ROW_H - 11);
      // Owner name inside if room.
      if (w > 40) {
        ctx.fillStyle = "#0e1116";
        ctx.font = "10px ui-monospace, monospace";
        ctx.fillText(seg.owner, x + 4, y + ROW_H / 2);
      }
      hitItems.push({
        kind: "segment", x, y: y + 5, w, h: ROW_H - 10,
        data: { ribbon: r, seg },
      });
    }

    // Reclaim glyphs (red circles at handover time).
    if (showReclaims) {
      for (const rc of r.reclaims) {
        const x = xForTime(rc.ts);
        ctx.fillStyle = "#f85149";
        ctx.beginPath();
        ctx.arc(x, y + ROW_H / 2, 4, 0, Math.PI * 2);
        ctx.fill();
        ctx.strokeStyle = "#0e1116";
        ctx.lineWidth = 1.5;
        ctx.stroke();
        hitItems.push({
          kind: "reclaim", x: x - 5, y: y + ROW_H / 2 - 5, w: 10, h: 10,
          data: { ribbon: r, rc },
        });
      }
    }

    // Complete diamond.
    if (r.complete_ts) {
      const x = xForTime(r.complete_ts);
      ctx.fillStyle = "#2da44e";
      ctx.beginPath();
      ctx.moveTo(x, y + ROW_H / 2 - 5);
      ctx.lineTo(x + 5, y + ROW_H / 2);
      ctx.lineTo(x, y + ROW_H / 2 + 5);
      ctx.lineTo(x - 5, y + ROW_H / 2);
      ctx.closePath();
      ctx.fill();
      ctx.strokeStyle = "#0e1116";
      ctx.lineWidth = 1;
      ctx.stroke();
      hitItems.push({
        kind: "complete", x: x - 5, y: y + ROW_H / 2 - 5, w: 10, h: 10,
        data: { ribbon: r },
      });
    }
    // Fail X.
    if (r.fail_ts) {
      const x = xForTime(r.fail_ts);
      ctx.strokeStyle = "#f85149";
      ctx.lineWidth = 2;
      ctx.beginPath();
      ctx.moveTo(x - 4, y + ROW_H / 2 - 4);
      ctx.lineTo(x + 4, y + ROW_H / 2 + 4);
      ctx.moveTo(x + 4, y + ROW_H / 2 - 4);
      ctx.lineTo(x - 4, y + ROW_H / 2 + 4);
      ctx.stroke();
    }
  }

  // ── Rejection markers ──
  if (showRejects) {
    for (const rj of rejects) {
      const x = xForTime(rj.ts);
      ctx.fillStyle = "#f85149";
      ctx.fillRect(x - 1, HEADER_H, 2, H - HEADER_H);
      ctx.fillStyle = "#f85149";
      ctx.font = "10px ui-monospace, monospace";
      ctx.fillText("✗", x + 3, HEADER_H + 10);
      hitItems.push({
        kind: "reject", x: x - 4, y: HEADER_H, w: 14, h: 14,
        data: rj,
      });
    }
  }

  // ── Replica health strips at the bottom ──
  if (replicaIds.length) {
    const replY0 = ribbonsY0 + ribbons.length * ROW_H + PADDING;
    ctx.fillStyle = "#161b22";
    ctx.fillRect(0, replY0, W, replicaIds.length * REPLICA_ROW_H + PADDING);
    for (let i = 0; i < replicaIds.length; i++) {
      const rid = replicaIds[i];
      const y = replY0 + i * REPLICA_ROW_H;
      // Row label.
      ctx.fillStyle = "#7d8590";
      ctx.font = "10px ui-monospace, monospace";
      ctx.textBaseline = "middle";
      ctx.fillText(rid, PADDING, y + REPLICA_ROW_H / 2);
      // Spans.
      const spans = replicaTimelines.get(rid) || [];
      for (const sp of spans) {
        const x = xForTime(sp.from);
        const w = Math.max(1, (sp.to - sp.from) * pxPerSec);
        if (sp.status === "in_sync") ctx.fillStyle = "#2da44e";
        else if (sp.status === "lagging") ctx.fillStyle = "#d29922";
        else ctx.fillStyle = "#f85149";
        ctx.globalAlpha = 0.7;
        ctx.fillRect(x, y + 3, w, REPLICA_ROW_H - 6);
        ctx.globalAlpha = 1;
        hitItems.push({
          kind: "replica", x, y: y + 3, w, h: REPLICA_ROW_H - 6,
          data: { rid, ...sp },
        });
      }
    }
  }

  // Legend agent swatches (populate once).
  const swatchEl = document.getElementById("agent-swatches");
  if (swatchEl && swatchEl.children.length === 0) {
    const agents = new Set();
    for (const r of ribbons) for (const s of r.segments) if (s.owner) agents.add(s.owner);
    for (const a of Array.from(agents).sort()) {
      const item = document.createElement("div");
      item.className = "item";
      item.innerHTML = `<span class="swatch" style="background:${agentColor(a)}"></span>${a}`;
      swatchEl.appendChild(item);
    }
  }
}

// ── Hit-testing for tooltip ─────────────────────────────────
function hitTest(mx, my) {
  // Iterate in reverse so later (smaller, on-top) items win.
  for (let i = hitItems.length - 1; i >= 0; i--) {
    const it = hitItems[i];
    if (mx >= it.x && mx <= it.x + it.w &&
        my >= it.y && my <= it.y + it.h) {
      return it;
    }
  }
  return null;
}

function tooltipFor(it) {
  if (it.kind === "segment") {
    const { ribbon, seg } = it.data;
    const dur = (seg.to - seg.from);
    return `${ribbon.task_id} — ${ribbon.title}\n` +
           `owner: ${seg.owner} (token ${seg.token})\n` +
           `${fmtClock(seg.from)} → ${fmtClock(seg.to)} (${fmtDelta(dur)})`;
  }
  if (it.kind === "reclaim") {
    const { ribbon, rc } = it.data;
    return `⚡ lease reclaim — ${ribbon.task_id}\n` +
           `${rc.from_agent} → ${rc.to_agent}\n` +
           `at ${fmtClock(rc.ts)}`;
  }
  if (it.kind === "complete") {
    return `✓ complete — ${it.data.ribbon.task_id}\nat ${fmtClock(it.data.ribbon.complete_ts)}`;
  }
  if (it.kind === "lock") {
    const sp = it.data;
    const dur = sp.to - sp.from;
    return `🔒 main lock — ${sp.holder} (token ${sp.token})\n` +
           `${fmtClock(sp.from)} → ${fmtClock(sp.to)} (${fmtDelta(dur)})`;
  }
  if (it.kind === "reject") {
    const rj = it.data;
    return `✗ rejected [${rj.errcode}]\n${rj.op_summary}\nat ${fmtClock(rj.ts)}`;
  }
  if (it.kind === "replica") {
    const d = it.data;
    return `${d.rid} — ${d.status}\n${fmtClock(d.from)} → ${fmtClock(d.to)}`;
  }
  return "";
}

// ── Mouse handling ──────────────────────────────────────────
function attachMouse() {
  const wrap = document.getElementById("wrap");
  const canvas = document.getElementById("gantt");
  const tip = document.getElementById("tooltip");
  canvas.addEventListener("mousemove", e => {
    const rect = canvas.getBoundingClientRect();
    const mx = e.clientX - rect.left;
    const my = e.clientY - rect.top;
    const it = hitTest(mx, my);
    if (it) {
      tip.textContent = tooltipFor(it);
      tip.style.display = "block";
      // Position near cursor but clamp to viewport.
      const tipW = 320;
      let left = e.clientX + 12;
      let top = e.clientY + 12;
      if (left + tipW > window.innerWidth) left = e.clientX - tipW - 12;
      tip.style.left = left + "px";
      tip.style.top = top + "px";
    } else {
      tip.style.display = "none";
    }
  });
  canvas.addEventListener("mouseleave", () => {
    tip.style.display = "none";
  });
  // Mouse wheel + ctrl = zoom; plain wheel = scroll (default).
  wrap.addEventListener("wheel", e => {
    if (e.ctrlKey || e.metaKey) {
      e.preventDefault();
      const factor = e.deltaY < 0 ? 1.2 : 1 / 1.2;
      zoomAround(e.clientX, factor);
    }
  }, { passive: false });
}

function zoomAround(clientX, factor) {
  const wrap = document.getElementById("wrap");
  const rect = wrap.getBoundingClientRect();
  const localX = clientX - rect.left + wrap.scrollLeft;
  const t = timeForX(localX);
  pxPerSec = Math.max(MIN_PX_PER_SEC, Math.min(MAX_PX_PER_SEC, pxPerSec * factor));
  draw();
  // Re-anchor so the cursor stays over the same time.
  const newX = xForTime(t);
  wrap.scrollLeft = newX - (clientX - rect.left);
}

function fitToWindow() {
  const wrap = document.getElementById("wrap");
  const usableW = wrap.clientWidth - LABEL_W - PADDING * 2;
  const span = Math.max(1, timeMax - timeMin);
  pxPerSec = Math.max(MIN_PX_PER_SEC, usableW / span);
  draw();
  wrap.scrollLeft = 0;
}

// ── Header meta line ────────────────────────────────────────
function updateMeta() {
  const m = document.getElementById("meta");
  const totalSec = timeMax - timeMin;
  m.textContent = `${ribbons.length} tasks · ${lockSpans.length} lock spans · ` +
                  `${rejects.length} rejects · ${replicaIds.length} replica(s) · ` +
                  `span ${fmtDelta(totalSec)}`;
}

// ── Boot ────────────────────────────────────────────────────
async function loadSnapshots() {
  try {
    const r = await fetch("/snapshots.json", { cache: "no-store" });
    if (!r.ok) {
      document.getElementById("meta").textContent = `(failed to load: ${r.status})`;
      return;
    }
    const data = await r.json();
    if (Array.isArray(data)) {
      payload = { replicas: [], snapshots: data, interesting_events: [] };
    } else {
      payload = data;
    }
    snapshots = payload.snapshots || [];
    replicaIds = payload.replicas || [];
    interesting = payload.interesting_events || [];

    // Time range from real (non-synthetic) snapshots.
    const realTimes = snapshots.slice(1).map(s => s.now_unix).filter(t => t > 0);
    if (realTimes.length) {
      timeMin = realTimes[0];
      timeMax = realTimes[realTimes.length - 1];
    } else {
      timeMin = 0; timeMax = 1;
    }

    buildRibbons();
    updateMeta();
    fitToWindow();
  } catch (e) {
    document.getElementById("meta").textContent = `(error: ${e.message})`;
  }
}

document.getElementById("zoom-in").onclick = () => {
  const wrap = document.getElementById("wrap");
  zoomAround(wrap.getBoundingClientRect().left + wrap.clientWidth / 2, 1.4);
};
document.getElementById("zoom-out").onclick = () => {
  const wrap = document.getElementById("wrap");
  zoomAround(wrap.getBoundingClientRect().left + wrap.clientWidth / 2, 1 / 1.4);
};
document.getElementById("zoom-fit").onclick = fitToWindow;
document.getElementById("reload").onclick = loadSnapshots;
document.getElementById("show-rejects").addEventListener("change", e => {
  showRejects = e.target.checked; draw();
});
document.getElementById("show-reclaims").addEventListener("change", e => {
  showReclaims = e.target.checked; draw();
});

document.addEventListener("keydown", e => {
  if (e.target.tagName === "INPUT") return;
  if (e.key === "r" || e.key === "R") { loadSnapshots(); }
  else if (e.key === "+" || e.key === "=") {
    const wrap = document.getElementById("wrap");
    zoomAround(wrap.getBoundingClientRect().left + wrap.clientWidth / 2, 1.4);
  }
  else if (e.key === "-" || e.key === "_") {
    const wrap = document.getElementById("wrap");
    zoomAround(wrap.getBoundingClientRect().left + wrap.clientWidth / 2, 1 / 1.4);
  }
  else if (e.key === "0") { fitToWindow(); }
});

window.addEventListener("resize", () => { draw(); });

attachMouse();
loadSnapshots();
</script>
</body>
</html>
"""


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--snapshots", default=os.path.join(HERE, "snapshots.json"),
                   help="path to snapshots.json (from tm-replay)")
    p.add_argument("--port", type=int, default=5060,
                   help="local port to serve UI on (default: 5060)")
    p.add_argument("--host", default="127.0.0.1")
    args = p.parse_args()

    make_app(args.snapshots)
    print(f"visualize_gantt: serving on http://{args.host}:{args.port}")
    print(f"  snapshots: {args.snapshots}")
    if not os.path.exists(args.snapshots):
        print(f"  (file not found yet — run tm-replay first, then refresh)")
    app.run(host=args.host, port=args.port, debug=False, use_reloader=False)


if __name__ == "__main__":
    main()
