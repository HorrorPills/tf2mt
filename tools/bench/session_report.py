#!/usr/bin/env python3
"""session_report.py — stats for tf2mt play sessions (owner-run online benchmarks).
   session_report.py [session dir ...]   (default: every dir in ~/Games/tf2/logs/tf2mt, oldest first)
Per session: mastercomfig preset, play time, avg fps, p50/p99/p99.9 frame time, 1 % low, hitches > 20 ms per minute,
present-interval stdev, renderer stalls. Frames > 250 ms (map loads, alt-tab) are excluded as non-gameplay."""
import glob, os, statistics, sys

def stats(d):
    f = os.path.join(d, 'frames.csv')
    if not os.path.exists(f): return None
    ms = []
    for line in open(f).read().splitlines()[1:]:
        try: ms.append(float(line.split(',')[1]))   # columns 3-4 (layer_ms, present_ms) in newer logs
        except (IndexError, ValueError): pass
    play = [x for x in ms if x < 250]
    if len(play) < 600: return None
    s = sorted(play); n = len(s)
    mins = sum(play) / 60000
    low1 = 1000 / statistics.mean(s[int(n * 0.99):])
    preset = open(os.path.join(d, 'preset.txt')).read().strip().split('preset=')[-1] if os.path.exists(os.path.join(d, 'preset.txt')) else '?'
    stalls = sum(1 for l in open(os.path.join(d, 'unix.log'), errors='ignore') if l.startswith('stall')) if os.path.exists(os.path.join(d, 'unix.log')) else 0
    return dict(session=os.path.basename(d.rstrip('/')), preset=preset, minutes=mins, fps=n / (sum(play) / 1000),
                p50=s[n // 2], p99=s[int(n * 0.99)], p999=s[min(n - 1, int(n * 0.999))], low1=low1,
                hitch=sum(1 for x in play if x > 20) / mins, sd=statistics.pstdev(play), stalls=stalls)

dirs = sys.argv[1:] or sorted(glob.glob(os.path.expanduser('~/Games/tf2/logs/tf2mt/*/')))
print(f"{'session':17} {'preset':7} {'min':>5} {'fps':>6} {'p50':>6} {'p99':>6} {'p99.9':>6} {'1%low':>6} {'hitch/min':>9} {'sd ms':>6} {'stalls':>6}")
for d in dirs:
    r = stats(d)
    if r: print(f"{r['session']:17} {r['preset']:7} {r['minutes']:5.1f} {r['fps']:6.1f} {r['p50']:6.2f} {r['p99']:6.2f} {r['p999']:6.2f} {r['low1']:6.1f} {r['hitch']:9.2f} {r['sd']:6.2f} {r['stalls']:6d}")
