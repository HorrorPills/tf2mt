#!/usr/bin/env python3
"""Table for tools/bench/relmouse-matrix.sh runs: fps still / turning, Present time while turning, and (Metal) frame
start -> on screen latency while turning.   usage: relmouse-summary.py <prefix> [--md]"""
import csv, glob, os, re, statistics as st, sys

H = os.path.expanduser("~/Games/tf2/logs/mousefps")
prefix = sys.argv[1]; md = "--md" in sys.argv


def phases(d):
    return [(p[1], float(p[2]), float(p[2]) + float(p[3]) * 1000) for p in (l.split() for l in open(f"{d}/phases.txt")) if p[0] == "phase"]


def lat_on(d):
    """median frame start -> on screen (ms) of shown frames during 'on' phases (Metal latency CSV)"""
    f = f"{d}/latency.csv"
    if not os.path.exists(f): return None
    on = [(a + 1000, b - 1000) for k, a, b in phases(d) if k == "on"]
    v = [float(r["total_ms"]) for r in csv.DictReader(open(f)) if any(a <= float(r["start"]) < b for a, b in on)]
    return st.median(v) if v else None


rows = []
for d in sorted(glob.glob(f"{H}/{prefix}-*")):
    s = open(f"{d}/summary.txt").read() if os.path.exists(f"{d}/summary.txt") else ""
    m = re.search(r"fps still ([\d.]+) / on ([\d.]+)", s)
    pres = [float(l.split()[4]) for l in s.splitlines() if l.startswith("on ")]
    rows.append((os.path.basename(d)[len(prefix) + 1:], m and float(m[1]), m and float(m[2]), st.mean(pres) if pres else None, lat_on(d)))

hdr = ("variant", "fps still", "fps turning", "turning/still", "Present p50 turning (ms)", "latency p50 turning (ms)")
fmt = lambda x, f="{:.1f}": "—" if x is None else f.format(x)
if md:
    print("| " + " | ".join(hdr) + " |"); print("|" + "---|" * len(hdr))
for n, a, b, p, l in rows:
    cells = (n, fmt(a), fmt(b), fmt(b / a if a and b else None, "{:.2f}"), fmt(p, "{:.2f}"), fmt(l))
    print("| " + " | ".join(cells) + " |" if md else "  ".join(f"{c:>14s}" if i else f"{c:22s}" for i, c in enumerate(cells)))
