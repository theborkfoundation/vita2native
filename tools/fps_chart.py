#!/usr/bin/env python3
"""Live FPS chart for vita2native.

The emulator appends one sample per second when VITA2NATIVE_METRICS_FILE is
set:

    <monotonic-ms>,<fps>,<frame-count>

Usage:
    python3 tools/fps_chart.py [metrics.csv] [port]

The server binds to 127.0.0.1, so on a remote device use an SSH tunnel:

    ssh -L 8099:127.0.0.1:8099 opiz3

then open http://127.0.0.1:8099 in a local browser.
"""

import json
import os
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

WINDOW = 300

HTML = r"""<!doctype html>
<html>
<head>
<meta charset="utf-8">
<title>vita2native FPS</title>
<style>
  html, body { margin: 0; height: 100%; background: #0d1117; color: #c9d1d9;
               font-family: ui-monospace, SFMono-Regular, Menlo, monospace; }
  #wrap { display: flex; height: 100%; }
  #chart { flex: 1; min-width: 0; }
  #stats { width: 190px; padding: 18px; line-height: 1.7; border-left: 1px solid #21262d; }
  .label { color: #8b949e; font-size: 12px; text-transform: uppercase; letter-spacing: .08em; }
  .value { font-size: 26px; color: #58a6ff; }
  .value.small { font-size: 18px; color: #c9d1d9; }
  #error { color: #f85149; font-size: 12px; margin-top: 12px; }
</style>
</head>
<body>
<div id="wrap">
  <canvas id="chart"></canvas>
  <div id="stats">
    <div class="label">current</div><div class="value" id="cur">--</div>
    <div class="label">average</div><div class="value small" id="avg">--</div>
    <div class="label">max</div><div class="value small" id="max">--</div>
    <div class="label">samples</div><div class="value small" id="n">0</div>
    <div id="error"></div>
  </div>
</div>
<script>
const canvas = document.getElementById('chart');
const ctx = canvas.getContext('2d');
let points = [];

function resize() {
  const r = canvas.getBoundingClientRect();
  const dpr = window.devicePixelRatio || 1;
  canvas.width = Math.max(1, Math.floor(r.width * dpr));
  canvas.height = Math.max(1, Math.floor(r.height * dpr));
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  draw();
}

function draw() {
  const r = canvas.getBoundingClientRect();
  const w = r.width, h = r.height;
  ctx.clearRect(0, 0, w, h);
  const pad = {l: 56, r: 18, t: 18, b: 30};
  const pw = w - pad.l - pad.r, ph = h - pad.t - pad.b;
  if (pw <= 0 || ph <= 0 || points.length === 0) return;

  let max = 5;
  for (const p of points) max = Math.max(max, p[1]);
  max = Math.ceil(max * 1.2);

  ctx.strokeStyle = '#21262d'; ctx.fillStyle = '#8b949e';
  ctx.font = '12px ui-monospace, monospace'; ctx.textAlign = 'right';
  for (let i = 0; i <= 4; i++) {
    const y = pad.t + ph - (ph * i) / 4;
    ctx.beginPath(); ctx.moveTo(pad.l, y); ctx.lineTo(pad.l + pw, y); ctx.stroke();
    ctx.fillText(((max * i) / 4).toFixed(1), pad.l - 8, y + 4);
  }

  const step = pw / Math.max(1, points.length - 1);
  ctx.beginPath();
  points.forEach((p, i) => {
    const x = pad.l + i * step;
    const y = pad.t + ph - ph * Math.min(1, p[1] / max);
    if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
  });
  ctx.strokeStyle = '#58a6ff'; ctx.lineWidth = 2; ctx.stroke();
  ctx.lineTo(pad.l + pw, pad.t + ph); ctx.lineTo(pad.l, pad.t + ph); ctx.closePath();
  ctx.fillStyle = 'rgba(88,166,255,0.12)'; ctx.fill();

  ctx.fillStyle = '#8b949e'; ctx.textAlign = 'left';
  ctx.fillText('fps', 8, 16);
}

async function tick() {
  try {
    const res = await fetch('/metrics', {cache: 'no-store'});
    if (!res.ok) throw new Error('HTTP ' + res.status);
    const data = await res.json();
    points = data.points || [];
    document.getElementById('error').textContent = '';
    const fps = points.map(p => p[1]);
    const cur = fps.length ? fps[fps.length - 1] : null;
    const avg = fps.length ? fps.reduce((a, b) => a + b, 0) / fps.length : null;
    document.getElementById('cur').textContent = cur === null ? '--' : cur.toFixed(1);
    document.getElementById('avg').textContent = avg === null ? '--' : avg.toFixed(2);
    document.getElementById('max').textContent = fps.length ? Math.max(...fps).toFixed(1) : '--';
    document.getElementById('n').textContent = fps.length;
    draw();
  } catch (e) {
    document.getElementById('error').textContent = String(e);
  }
}

window.addEventListener('resize', resize);
resize();
tick();
setInterval(tick, 1000);
</script>
</body>
</html>
"""


def read_points(path, limit):
    points = []
    try:
        with open(path, "r", errors="replace") as f:
            lines = f.readlines()[-limit:]
    except OSError:
        return points

    for line in lines:
        fields = line.strip().split(",")
        if len(fields) < 2:
            continue
        try:
            points.append([int(fields[0]), float(fields[1])])
        except ValueError:
            continue
    return points


class Handler(BaseHTTPRequestHandler):
    metrics_path = "fps.csv"

    def do_GET(self):
        if self.path.startswith("/metrics"):
            body = json.dumps({"points": read_points(self.metrics_path, WINDOW)}).encode()
            content_type = "application/json"
        else:
            body = HTML.encode()
            content_type = "text/html; charset=utf-8"

        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "fps.csv"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 8099

    Handler.metrics_path = path
    server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    print(f"serving {os.path.abspath(path)} on http://127.0.0.1:{port}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
