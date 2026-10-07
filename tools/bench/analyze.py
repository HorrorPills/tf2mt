#!/usr/bin/env python3
"""Analyze one benchmark run: frames-<tag>.csv from tools/trace (+ threads-<tag>.csv from threadmon).

Usage: analyze.py <run_dir> <tag> [--json out.json]
Frame metrics follow tools/analysis/legacy_parallels_analyze.py (percentiles, 1%/0.1% lows,
spikes > 2x median, frame-to-frame jitter) plus the hitch counts used by PLAN.md gates
(> 20 ms per minute) and present-interval stdev.

The measurement window is the in-game part of the run: frames with >= MIN_DRAWS draws,
starting SETTLE_S after the first such frame and ending 2 s before the last.
"""
import argparse, json, sys
import numpy as np, pandas as pd

MIN_DRAWS = 100
SETTLE_S = 10.0


def demo_start_ms(f):
    """Demo playback starts after the map-load stall: the frame following the last > 1 s frame in the
    first half of the run (loading screens can exceed MIN_DRAWS, so draws alone are not enough)."""
    ing = f[f.draws >= MIN_DRAWS]
    half = ing[ing.t_ms <= ing.t_ms.iloc[0] + (ing.t_ms.iloc[-1] - ing.t_ms.iloc[0]) / 2]
    stalls = half[half.ms_between_presents > 1000]
    return stalls.t_ms.iloc[-1] if len(stalls) else ing.t_ms.iloc[0]


def window(f):
    ing = f[f.draws >= MIN_DRAWS]
    if ing.empty:
        return f.iloc[0:0], (0, 0)
    t0, t1 = demo_start_ms(f) + SETTLE_S * 1000, ing.t_ms.iloc[-1] - 2000
    return f[(f.t_ms >= t0) & (f.t_ms <= t1)], (t0, t1)


def frame_stats(w):
    bp = w.ms_between_presents.values
    dur_s = (w.t_ms.iloc[-1] - w.t_ms.iloc[0]) / 1000
    srt = np.sort(bp)[::-1]
    k1, k01 = max(1, len(srt) // 100), max(1, len(srt) // 1000)
    med = float(np.median(bp))
    return {
        'frames': int(len(bp)), 'seconds': round(dur_s, 1),
        'avg_fps': round(1000 / bp.mean(), 1),
        'p50_ms': round(float(np.percentile(bp, 50)), 2), 'p95_ms': round(float(np.percentile(bp, 95)), 2),
        'p99_ms': round(float(np.percentile(bp, 99)), 2), 'p999_ms': round(float(np.percentile(bp, 99.9)), 2),
        'max_ms': round(float(bp.max()), 2),
        'low1_fps': round(1000 / srt[:k1].mean(), 1), 'low01_fps': round(1000 / srt[:k01].mean(), 1),
        'stdev_ms': round(float(bp.std()), 3),
        'jitter_mean_ms': round(float(np.abs(np.diff(bp)).mean()), 3),
        'spikes_2x_median': int((bp > 2 * med).sum()),
        'hitches_20ms': int((bp > 20).sum()), 'hitches_20ms_per_min': round((bp > 20).sum() / dur_s * 60, 2),
        'in_present_p50_ms': round(float(w.ms_in_present.median()), 3),
        'in_present_p99_ms': round(float(w.ms_in_present.quantile(.99)), 3),
        'draws_p50': int(w.draws.median()), 'draws_p95': int(w.draws.quantile(.95)), 'draws_max': int(w.draws.max()),
        'calls_p50': int(w.calls.median()),
    }


def thread_stats(tm, t_window_s):
    """Per-thread CPU % of one core over the window (threadmon clock is offset from the frame clock;
    we use the whole in-game span after the first SETTLE_S as an approximation)."""
    tm = tm.copy()
    tm['cpu_ms'] = tm.user_ms + tm.sys_ms
    t0, t1 = t_window_s
    sel = tm[(tm.t_s >= t0) & (tm.t_s <= t1)]
    if sel.empty:
        return []
    out = []
    for th, g in sel.groupby('thread'):
        if len(g) < 2:
            continue
        dt = g.t_s.iloc[-1] - g.t_s.iloc[0]
        pct = (g.cpu_ms.iloc[-1] - g.cpu_ms.iloc[0]) / (dt * 1000) * 100 if dt > 0 else 0
        per_s = g.cpu_ms.diff().dropna() / g.t_s.diff().dropna() / 10
        out.append({'thread': th, 'name': (g.name.iloc[-1] if isinstance(g.name.iloc[-1], str) else ''),
                    'cpu_pct': round(pct, 1), 'cpu_pct_max_1s': round(float(per_s.max()), 1)})
    out.sort(key=lambda r: -r['cpu_pct'])
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run_dir'); ap.add_argument('tag'); ap.add_argument('--json')
    a = ap.parse_args()
    f = pd.read_csv(f'{a.run_dir}/frames-{a.tag}.csv')
    f = f[f.frame > 0]
    w, (t0, t1) = window(f)
    if len(w) < 100:
        sys.exit(f'{a.tag}: too few in-game frames ({len(w)})')
    res = {'tag': a.tag, 'frames': frame_stats(w)}
    try:
        tm = pd.read_csv(f'{a.run_dir}/threads-{a.tag}.csv', keep_default_na=False)
        # threadmon starts at process launch ~= trace t0 (both within ~1-2 s); good enough for CPU shares
        res['threads'] = thread_stats(tm, (t0 / 1000, t1 / 1000))[:8]
    except FileNotFoundError:
        pass
    s = res['frames']
    print(f"=== {a.tag}: {s['frames']} frames / {s['seconds']} s")
    print(f"avg {s['avg_fps']} fps | p50 {s['p50_ms']} p95 {s['p95_ms']} p99 {s['p99_ms']} p99.9 {s['p999_ms']} max {s['max_ms']} ms")
    print(f"1% low {s['low1_fps']} | 0.1% low {s['low01_fps']} | stdev {s['stdev_ms']} ms | jitter {s['jitter_mean_ms']} ms")
    print(f"hitches >20ms: {s['hitches_20ms']} ({s['hitches_20ms_per_min']}/min) | spikes >2x median: {s['spikes_2x_median']}")
    print(f"in Present p50 {s['in_present_p50_ms']} p99 {s['in_present_p99_ms']} ms | draws p50 {s['draws_p50']} p95 {s['draws_p95']} max {s['draws_max']} | calls p50 {s['calls_p50']}")
    for r in res.get('threads', []):
        print(f"  thread {r['thread']:>14} {r['name']:<20} {r['cpu_pct']:6.1f}% (max 1s {r['cpu_pct_max_1s']}%)")
    if a.json:
        json.dump(res, open(a.json, 'w'), indent=1)


if __name__ == '__main__':
    main()
