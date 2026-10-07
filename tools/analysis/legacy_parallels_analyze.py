#!/usr/bin/env python3
"""Analyze a tf2diag run: python3 analyze.py runs/<tag>"""
import sys, os, json
import numpy as np, pandas as pd

d = sys.argv[1]
f = pd.read_csv(f"{d}/frames.csv", na_values="NA")
f = f[f.Application.str.contains("tf_win64")]
n = len(f)
dur = f.TimeInMs.iloc[-1] - f.TimeInMs.iloc[0]
bp = f.MsBetweenPresents.dropna().values
disp = f.MsBetweenDisplayChange.dropna().values
cpu = f.MsCPUBusy.dropna().values
gpu = f.MsGPUTime.dropna().values
def pct(a, p): return float(np.percentile(a, p)) if len(a) else float('nan')
print(f"=== {d}  ({n} frames, {dur/1000:.1f}s) ===")
print("Present mode:", f.PresentMode.value_counts().to_dict(), "| SyncInterval:", f.SyncInterval.value_counts().to_dict(), "| Tearing:", f.AllowsTearing.value_counts().to_dict())
fps = 1000/np.mean(bp)
print(f"Avg FPS {fps:.1f}  | frametime p50 {pct(bp,50):.2f}  p95 {pct(bp,95):.2f}  p99 {pct(bp,99):.2f}  p99.9 {pct(bp,99.9):.2f}  max {bp.max():.2f} ms")
srt = np.sort(bp)[::-1]
k1 = max(1,int(len(srt)*0.01)); k01 = max(1,int(len(srt)*0.001))
print(f"1% low {1000/srt[:k1].mean():.1f} fps | 0.1% low {1000/srt[:k01].mean():.1f} fps")
med = np.median(bp)
spikes = bp > 2*med
print(f"Frame-time spikes >2x median ({2*med:.1f}ms): {spikes.sum()} ({spikes.mean()*100:.2f}%)   >3x: {(bp>3*med).sum()}")
jit = np.abs(np.diff(bp))
print(f"Frame-to-frame jitter: mean {jit.mean():.2f} ms, p95 {pct(jit,95):.2f}, stdev of frametime {bp.std():.2f}")
print("--- Display pacing (what the monitor actually showed)")
if len(disp):
    print(f"MsBetweenDisplayChange: p50 {pct(disp,50):.2f} p95 {pct(disp,95):.2f} p99 {pct(disp,99):.2f} stdev {disp.std():.2f}")
    vals, edges = np.histogram(disp, bins=[0,4,6,10,13,15,18,21,25,34,50,100,1000])
    print("  display-interval histogram (ms bins):", {f"{edges[i]:.0f}-{edges[i+1]:.0f}": int(v) for i, v in enumerate(vals) if v})
    zero = (f.MsBetweenDisplayChange.fillna(-1) == 0).sum()
    print(f"  frames never displayed / dropped: {int(f.MsBetweenDisplayChange.isna().sum())}  (zero-interval: {zero})")
    # judder: ratio display interval / present interval
    m = f.dropna(subset=["MsBetweenDisplayChange","MsBetweenPresents"])
    err = (m.MsBetweenDisplayChange - m.MsBetweenPresents).abs()
    print(f"  |display - present| interval error: mean {err.mean():.2f} ms, p95 {pct(err.values,95):.2f}")
print("--- Where is the time going?")
print(f"CPU busy (app) p50 {pct(cpu,50):.2f} p99 {pct(cpu,99):.2f} ms | GPU time p50 {pct(gpu,50):.2f} p99 {pct(gpu,99):.2f} ms | MsInPresentAPI p50 {pct(f.MsInPresentAPI.dropna().values,50):.2f} p99 {pct(f.MsInPresentAPI.dropna().values,99):.2f}")
if "MsCPUWait" in f: print(f"CPUWait p50 {pct(f.MsCPUWait.dropna().values,50):.2f} p99 {pct(f.MsCPUWait.dropna().values,99):.2f}")
if "MsUntilDisplayed" in f: print(f"Present->display latency p50 {pct(f.MsUntilDisplayed.dropna().values,50):.2f} p99 {pct(f.MsUntilDisplayed.dropna().values,99):.2f}")
sp = f[(f.MsBetweenPresents > 2*med)]
if len(sp):
    print(f"During spikes (n={len(sp)}): CPU busy avg {sp.MsCPUBusy.mean():.1f}  GPU time avg {sp.MsGPUTime.mean():.1f}  InPresentAPI avg {sp.MsInPresentAPI.mean():.1f}  CPUWait avg {sp.MsCPUWait.mean():.1f}")
    cause = np.where(sp.MsInPresentAPI > 0.5*sp.MsBetweenPresents, "blocked in Present()", np.where(sp.MsCPUBusy > 0.7*sp.MsBetweenPresents, "CPU-bound (game thread)", np.where(sp.MsGPUTime > 0.7*sp.MsBetweenPresents, "GPU-bound", "stall elsewhere (wait/descheduled)")))
    print("  spike attribution:", pd.Series(cause).value_counts().to_dict())
    # periodicity
    t = f.TimeInMs.values[(f.MsBetweenPresents > 2*med).values]
    if len(t) > 5:
        gaps = np.diff(t)/1000
        print(f"  spike spacing (s): median {np.median(gaps):.2f}, p25 {pct(gaps,25):.2f}, p75 {pct(gaps,75):.2f}  (tight IQR => periodic cause)")
# frametime over time (per-second worst frame)
f = f.assign(sec=(f.TimeInMs - f.TimeInMs.iloc[0]).astype(float)//1000)
g = f.groupby("sec").MsBetweenPresents.agg(["count","max"])
print("--- per-second fps / worst frame (every 5s):")
print(g.iloc[::5].to_string())

# system / thread / host
try:
    s = pd.read_csv(f"{d}/system.csv")
    cores = sorted({c.split("_")[0] for c in s.columns if c.startswith("cpu") and "_processortime" in c and "total" not in c})
    pc = s[[f"{c}_processortime" for c in cores]]
    print("--- Guest CPU (6 vCPU): per-core avg %:", dict(zip(cores, pc.mean().round(0))), "| max core avg-peak:", round(pc.max().max()))
    for c in ["_Process_tf_win64___Processor_Time","_Process_MsMpEng__Processor_Time","_Process_dwm__Processor_Time","_Process_steamwebhelper___Processor_Time"]:
        if c in s: print(f"   {c}: avg {s[c].mean():.0f}%  max {s[c].max():.0f}%")
    dpc = s[[c for c in s.columns if c.endswith("_dpctime") and "total" not in c]]; it = s[[c for c in s.columns if c.endswith("_interrupttime") and "total" not in c]]
    print(f"   DPC time per core avg {dpc.mean().mean():.2f}% max {dpc.max().max():.1f}% | interrupt avg {it.mean().mean():.2f}% max {it.max().max():.1f}%")
    for c in s.columns:
        if c.startswith("gpu_"): print(f"   {c}: avg {s[c].mean():.0f}% max {s[c].max():.0f}%")
    for c in ["_Memory_Pages_sec","_Memory_Page_Faults_sec","_Memory_Available_MBytes","_PhysicalDisk__Total__Avg__Disk_Queue_Length"]:
        if c in s: print(f"   {c}: avg {s[c].mean():.1f} max {s[c].max():.1f}")
except Exception as e: print("system.csv:", e)
try:
    t = pd.read_csv(f"{d}/threads.csv")
    top = t.groupby("tid").pct.agg(["mean","max","count"]).sort_values("mean", ascending=False).head(5)
    print("--- tf_win64 hottest threads (% of one core, avg/max over time):"); print(top.round(1).to_string())
except Exception as e: print("threads.csv:", e)
hp = f"{d}_host.csv"
if os.path.exists(hp):
    h = pd.read_csv(hp)
    print(f"--- Host: prl_vm_app CPU avg {h.prl_cpu.mean():.0f}% max {h.prl_cpu.max():.0f}% (of 1000% = 10 cores) | RSS {h.prl_rss_mb.iloc[-1]:.0f}MB | swap {h.swap_used_mb.iloc[-1]}MB | pageouts delta {int(h.pageouts.iloc[-1]-h.pageouts.iloc[0])} | thermal limit min {h.therm.min()}")
    print("    top non-Parallels host procs:", h.top_other_proc.value_counts().head(3).to_dict())
