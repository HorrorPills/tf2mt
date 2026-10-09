#!/usr/bin/env python3
"""Camera smoothness from per-frame yaw: python3 tools/bench/camera-smoothness.py <csv> [<csv> ...]
Works on the Metal latency log (columns frame,cam_yaw) and the DXVK trace frame log (frame,cam_yaw).
While the view is turning, a smooth stream has similar per-frame yaw steps. Counts frames that stall (no
movement while neighbours move), jump (> 2x neighbours) or are uneven (off by > 50 %)."""
import csv, sys, os

def load(path):
    rows = list(csv.DictReader(open(path)))
    out = []
    for r in rows:
        try:
            f, y = int(r["frame"]), float(r["cam_yaw"])
        except (KeyError, ValueError, TypeError):
            continue
        if y > -900: out.append((f, y))
    return out

for path in sys.argv[1:]:
    fr = load(path)
    if len(fr) < 100: print(f"{path}: not enough camera data ({len(fr)})"); continue
    fr = fr[len(fr) // 20:]                       # skip load
    d = []
    for (f0, y0), (f1, y1) in zip(fr, fr[1:]):
        if f1 != f0 + 1: d.append(None); continue
        dy = (y1 - y0 + 180) % 360 - 180
        d.append(dy)
    turning = stall = jump = uneven = 0
    for i in range(1, len(d) - 1):
        a, b, c = d[i - 1], d[i], d[i + 1]
        if a is None or b is None or c is None: continue
        nb = (a + c) / 2
        if abs(nb) < 0.05 or (a > 0) != (c > 0): continue      # not a steady turn
        turning += 1
        r = b / nb
        if abs(b) < 0.1 * abs(nb): stall += 1
        elif r > 2.0: jump += 1
        elif r < 0.5 or r > 1.5: uneven += 1
    if not turning: print(f"{os.path.basename(os.path.dirname(path))}/{os.path.basename(path)}: no steady turning found"); continue
    print(f"{os.path.basename(os.path.dirname(path)):24s} turning frames {turning:6d}   stalls {100*stall/turning:5.2f}%   jumps {100*jump/turning:5.2f}%   uneven {100*uneven/turning:5.2f}%   (total irregular {100*(stall+jump+uneven)/turning:5.2f}%)")
