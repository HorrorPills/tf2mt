#!/usr/bin/env python3
"""Overnight study summary: python3 tools/bench/night-summary.py <run> [<run> ...]  (runs in ~/Games/tf2/logs/night)"""
import csv, os, re, sys
H = os.path.expanduser("~/Games/tf2/logs/night")
def q(a, f): a = sorted(a); return a[min(len(a) - 1, int(f * len(a)))] if a else float("nan")
print(f"{'run':22s} {'fps':>6s} {'p99':>6s} {'1%low':>6s} {'>20ms/m':>7s} {'jumps/m':>7s} {'jolts/m':>7s} {'stuck%':>6s} {'lat p50':>7s} {'lat mean':>8s} {'drains':>6s}")
for name in sys.argv[1:]:
    d = f"{H}/{name}"
    try: fr = [(float(r["t_s"]), float(r["frame_ms"])) for r in csv.DictReader(open(f"{d}/frames.csv"))]
    except Exception: print(f"{name:22s} (no frames)"); continue
    if len(fr) < 200: print(f"{name:22s} (too few frames: {len(fr)})"); continue
    t0 = fr[0][0]; fr = [x for x in fr if x[0] > t0 + (60 if os.path.exists(f"{d}/bots.txt") else 20)]   # skip load (+ bots joining)
    f = [x[1] for x in fr]; dur = fr[-1][0] - fr[0][0]
    low = sorted(f)[-max(1, len(f) // 100):]
    lat = []; stuck = 0; jolts = float("nan")
    try:
        L = [r for r in csv.DictReader(open(f"{d}/latency.csv")) if int(r["frame"]) > 400]
        for r in L:
            ts, tot = float(r["to_screen_ms"]), float(r["total_ms"])
            if 0 < tot < 300: lat.append(tot)
        tsl = [float(r["to_screen_ms"]) for r in L if 0 < float(r["to_screen_ms"]) < 200]
        LL = sorted(((int(r["frame"]), float(r["total_ms"]), float(r["start"])) for r in L if 0 < float(r["total_ms"]) < 300))
        if len(LL) > 100:
            jd = sum(1 for a, b in zip(LL, LL[1:]) if b[0] == a[0] + 1 and abs(b[1] - a[1]) > 4)
            jolts = jd / ((LL[-1][2] - LL[0][2]) / 60000)
        if tsl:
            floor = q(tsl, 0.02); per = 1000 / 120
            stuck = 100 * sum(1 for x in tsl if x > floor + 0.5 * per) / len(tsl)
    except Exception: pass
    jumps = shown = 0
    for l in open(f"{d}/unix.log"):
        m = re.search(r"shown intervals >12.5 ms: (\d+) of (\d+)", l)
        if m: jumps += int(m[1]); shown += int(m[2])
    drains = sum(1 for l in open(f"{d}/unix.log") if l.startswith("drain:"))
    jm = jumps / (shown / 120) * 60 if shown else float("nan")
    print(f"{name:22s} {len(f)/dur:6.1f} {q(f,.99):6.2f} {1000/(sum(low)/len(low)):6.1f} {sum(1 for x in f if x > 20)/dur*60:7.1f} "
          f"{jm:7.1f} {jolts:7.1f} {stuck:6.1f} {q(lat,.5):7.1f} {sum(lat)/len(lat) if lat else float('nan'):8.1f} {drains:6d}")
