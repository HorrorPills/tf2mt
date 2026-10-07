#!/usr/bin/env python3
"""Hitch list + cross-run alignment (PLAN.md P0.5).

Usage: hitches.py <runs_dir> <tag> [<tag> ...] [--ms 20]
Runs replay the same demo, so they are aligned on demo time: t=0 is demo playback start
(after the map-load stall, see analyze.demo_start_ms). For every hitch (> --ms) in each run, prints its demo time and whether the
other runs have a hitch within +-0.25 s (=> the stall is in the game, not the renderer).
"""
import argparse
import numpy as np, pandas as pd
import sys, os
sys.path.insert(0, os.path.dirname(__file__))
from analyze import demo_start_ms

ap = argparse.ArgumentParser()
ap.add_argument('runs_dir'); ap.add_argument('tags', nargs='+'); ap.add_argument('--ms', type=float, default=20)
a = ap.parse_args()

runs = {}
for t in a.tags:
    f = pd.read_csv(f'{a.runs_dir}/{t}/frames-{t}.csv')
    f = f[f.frame > 0]
    start = demo_start_ms(f)
    f = f.assign(demo_s=(f.t_ms - start) / 1000)
    runs[t] = f[(f.demo_s >= 0)]

for t, f in runs.items():
    h = f[f.ms_between_presents > a.ms]
    print(f'--- {t}: {len(h)} hitches > {a.ms} ms (demo span {f.demo_s.iloc[-1]:.0f} s)')
    for _, r in h.iterrows():
        others = []
        for t2, f2 in runs.items():
            if t2 == t:
                continue
            near = f2[(f2.demo_s - r.demo_s).abs() < 0.25]
            others.append(f'{t2}:{near.ms_between_presents.max():.0f}ms' if len(near) else f'{t2}:-')
        print(f'  t={r.demo_s:7.2f}s  {r.ms_between_presents:8.1f} ms  in_present {r.ms_in_present:6.1f}  draws {int(r.draws):4d}  | ' + '  '.join(others))
