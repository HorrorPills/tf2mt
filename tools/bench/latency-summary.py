#!/usr/bin/env python3
"""Summarise tools/bench/latency-ab.sh runs: python3 tools/bench/latency-summary.py base ahead0 ..."""
import os, re, sys, csv
H = os.path.expanduser("~/Games/tf2/logs")
pat = re.compile(r"latency: .*?last (\d+) frames: p50 ([\d.]+) p90 ([\d.]+) p99 ([\d.]+) max ([\d.]+) ms; (\d+) drawables not shown; shown intervals >12.5 ms: (\d+) of (\d+)")
print(f"{'variant':12s} {'lat p50':>8s} {'lat p90':>8s} {'lat p99':>8s} {'jumps/min':>9s} {'not shown':>9s} {'game fps':>8s}")
for name in sys.argv[1:]:
    d = f"{H}/lat-{name}"
    try: lines = [pat.search(l) for l in open(f"{d}/unix.log") if l.startswith("latency:")]
    except OSError: print(f"{name:12s} (no data)"); continue
    lines = [m for m in lines if m][2:]   # skip the first 1,200 frames (load / warm-up)
    if not lines: print(f"{name:12s} (no latency lines)"); continue
    w = sum(int(m[1]) for m in lines)
    avg = lambda i: sum(float(m[i]) * int(m[1]) for m in lines) / w
    shown = sum(int(m[8]) for m in lines); jumps = sum(int(m[7]) for m in lines); hidden = sum(int(m[6]) for m in lines)
    fps = 0
    try:
        t = [float(r["t_s"]) for r in csv.DictReader(open(f"{d}/frames.csv"))]
        fps = len(t) / (t[-1] - t[0]) if len(t) > 1 else 0
    except Exception: pass
    print(f"{name:12s} {avg(2):8.1f} {avg(3):8.1f} {avg(4):8.1f} {jumps / (shown / 120.0) * 60 if shown else 0:9.1f} {hidden:9d} {fps:8.1f}")
