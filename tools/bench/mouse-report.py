#!/usr/bin/env python3
"""mouse-report.py <tag>: seconds with camera motion; reports camera updates/s while moving."""
import sys, pandas as pd
t=sys.argv[1]; import os; f=pd.read_csv(os.path.join(os.environ.get('TF2_HOME', os.path.expanduser('~/Games/tf2')), f'logs/runs/{t}/frames-{t}.csv'))
f['s']=(f.t_ms//1000).astype(int)
g=f.groupby('s').agg(fps=('frame','count'),cam=('cam_changed',lambda x:(x==1).sum()))
mv=g[(g.cam>5)&(g.fps>60)]
print(f"{t}: {len(mv)} seconds with camera motion | camera updates/s median {mv.cam.median():.0f} (p10 {mv.cam.quantile(.1):.0f}, p90 {mv.cam.quantile(.9):.0f}) | fps median {mv.fps.median():.0f}")
print('per-second camera updates while moving:', ' '.join(str(int(v)) for v in mv.cam))
