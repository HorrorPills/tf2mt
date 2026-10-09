#!/usr/bin/env python3
"""Summarise tools/bench/queue-ab.sh runs: python3 tools/bench/queue-summary.py auto q2 q0"""
import csv, os, sys
H = os.path.expanduser("~/Games/tf2/logs")
def q(a, f): a = sorted(a); return a[min(len(a) - 1, int(f * len(a)))] if a else 0
print(f"{'variant':8s} {'fps':>6s} {'p50 ms':>7s} {'p99 ms':>7s} {'1% low':>7s} {'>8.3ms %':>9s} {'>20ms/min':>9s} {'thread1 %':>9s} {'thread2 %':>9s}")
for name in sys.argv[1:]:
    d = f"{H}/qm-{name}"
    try: rows = [(float(r["t_s"]), float(r["frame_ms"])) for r in csv.DictReader(open(f"{d}/frames.csv"))]
    except OSError: print(f"{name:8s} (no frames.csv)"); continue
    if len(rows) < 100: print(f"{name:8s} (too few frames)"); continue
    t0 = rows[0][0]; rows = [r for r in rows if r[0] > t0 + 20]          # skip load / warm-up
    f = [r[1] for r in rows]; dur = rows[-1][0] - rows[0][0]
    low = sorted(f)[-max(1, len(f) // 100):]
    th = [l.split() for l in open(f"{d}/threads.txt")] if os.path.exists(f"{d}/threads.txt") else []
    th = [list(map(float, x)) for x in th if len(x) >= 2][2:]           # skip load samples
    t1 = sum(x[0] for x in th) / len(th) if th else 0; t2 = sum(x[1] for x in th) / len(th) if th else 0
    print(f"{name:8s} {len(f) / dur:6.1f} {q(f, .5):7.2f} {q(f, .99):7.2f} {1000 / (sum(low) / len(low)):7.1f} "
          f"{100 * sum(1 for x in f if x > 8.33) / len(f):9.1f} {sum(1 for x in f if x > 20) / dur * 60:9.1f} {t1:9.0f} {t2:9.0f}")
