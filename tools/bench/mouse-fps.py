#!/usr/bin/env python3
"""Per-phase frame rate for tools/bench/mouse-fps.sh runs (issue #2: mouse input locks fps to the display rate).
usage: mouse-fps.py <run dir> [...]
Phase times come from mousedrive (mach ms). Metal runs: frames.csv (PE, every frame) is put on the mach clock with
latency.csv (frame index + mach start). DXVK runs (trace CSV, no mach clock): the first camera change = first "on"."""
import csv, glob, os, statistics as st, sys

TRIM = 1000.0   # ms ignored at each phase edge


def phases(d):
    ph = []
    for l in open(os.path.join(d, "phases.txt")):
        p = l.split()
        if p[0] == "phase": ph.append([p[1], float(p[2]), float(p[2]) + float(p[3]) * 1000])
    return ph


def metal_frames(d):
    """[(mach_ms, frame_ms, present_ms, cam)]"""
    rows = list(csv.DictReader(open(os.path.join(d, "frames.csv"))))
    lat = {int(r["frame"]): r for r in csv.DictReader(open(os.path.join(d, "latency.csv")))}
    # frames.csv row i is the (i+1)-th Present; latency frame k is the k-th present (g_frames_total + 1)
    offs = [float(lat[i + 1]["start"]) + float(lat[i + 1]["game_ms"]) - float(rows[i]["t_s"]) * 1000
            for i in range(len(rows)) if i + 1 in lat]
    if not offs: return None, None
    off = st.median(offs)
    spread = st.median([abs(o - off) for o in offs])
    out = []
    for i, r in enumerate(rows):
        L = lat.get(i + 1)
        cam = None
        if L and float(L["cam_yaw"]) > -999: cam = (float(L["cam_yaw"]), float(L["cam_pitch"]))
        out.append((float(r["t_s"]) * 1000 + off, float(r["frame_ms"]), float(r["present_ms"]), cam))
    return out, spread


def dxvk_frames(d, ph):
    f = sorted(glob.glob(os.path.join(d, "frames-*.csv")) + glob.glob(os.path.join(d, "*.csv")), key=os.path.getsize)[-1]
    rows = [l.strip().split(",") for l in open(f) if l[0].isdigit()]
    first_on = next(p[1] for p in ph if p[0] == "on")
    # first frame of a sustained run of camera changes (>= 30 changes/s for 2 s; the setup probe turn is ~0.4 s).
    # Counted per second, not per frame: uncapped, the game draws ~350 fps but macOS sends mouse events at the
    # display rate (~63/s at 60 Hz), so most frames have no camera change.
    t = [float(r[1]) for r in rows]; cc = [int(r[9]) for r in rows]
    def sustained(i):
        return sum(1 for j in range(i, len(rows)) if t[j] < t[i] + 2000 and cc[j] == 1) >= 60
    start = next(i for i in range(30, len(rows)) if cc[i] == 1 and all(c != 1 for c in cc[i - 30:i]) and sustained(i))
    off = first_on - t[start]
    out = [(t[i] + off, float(rows[i][2]), float(rows[i][3]), (float(rows[i][10]), float(rows[i][11])) if cc[i] == 1 else None)
           for i in range(len(rows))]
    return out, None


def summarize(d):
    ph = phases(d)
    if os.path.exists(os.path.join(d, "latency.csv")): fr, spread = metal_frames(d)
    else: fr, spread = dxvk_frames(d, ph)
    print(f"== {os.path.basename(d)}  {open(os.path.join(d, 'setup.txt')).readline().strip() if os.path.exists(os.path.join(d, 'setup.txt')) else ''}"
          + (f"  (clock match ±{spread:.2f} ms)" if spread is not None else ""))
    print(f"{'phase':6s} {'fps':>6s} {'ms p50':>7s} {'ms p90':>7s} {'present p50':>11s} {'present p90':>11s} {'cam/s':>6s}")
    res = []
    for name, a, b in ph:
        a, b = a + TRIM, b - TRIM
        sel = [x for x in fr if a <= x[0] < b]
        if len(sel) < 2: print(f"{name:6s} (no frames)"); continue
        fps = (len(sel) - 1) / ((sel[-1][0] - sel[0][0]) / 1000)
        fm = sorted(x[1] for x in sel); pm = sorted(x[2] for x in sel)
        cams = [x[3] for x in sel if x[3] is not None]
        moves = sum(1 for i in range(1, len(cams)) if cams[i] != cams[i - 1])
        q = lambda v, f: v[min(len(v) - 1, int(f * len(v)))]
        print(f"{name:6s} {fps:6.1f} {q(fm,.5):7.2f} {q(fm,.9):7.2f} {q(pm,.5):11.2f} {q(pm,.9):11.2f} {moves / ((b - a) / 1000):6.1f}")
        res.append((name, fps))
    off = [f for n, f in res if n == "off"]
    for kind in sorted({n for n, _ in res} - {"off"}):
        on = [f for n, f in res if n == kind]
        if on and off: print(f"verdict: fps still {st.mean(off):.1f} / {kind} {st.mean(on):.1f}  ({kind}/still = {st.mean(on) / st.mean(off):.2f})")


for d in sys.argv[1:]: summarize(d)
