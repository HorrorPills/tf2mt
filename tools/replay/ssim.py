#!/usr/bin/env python3
"""ssim.py — compare frame dumps of two providers (M6/M7 pixel gates).
   ssim.py <dirA> <dirB> [--diff OUTDIR]     (dirs of frame-NNNNN.ppm from run-replay.sh DUMP=...)
SSIM per frame on luma (Rec. 601), 8x8 uniform windows with stride 4 (Wang et al. 2004 constants, L = 255);
also mean absolute error and the share of pixels whose max channel differs by > 16. --diff writes amplified diffs."""
import os, sys
import numpy as np

def read_ppm(p):
    with open(p, 'rb') as f:
        data = f.read()
    parts = data.split(maxsplit=4)
    assert parts[0] == b'P6'
    w, h = int(parts[1]), int(parts[2])
    return np.frombuffer(parts[4][: w * h * 3], np.uint8).reshape(h, w, 3)

def ssim(a, b):
    ya = a.astype(np.float64) @ [0.299, 0.587, 0.114]
    yb = b.astype(np.float64) @ [0.299, 0.587, 0.114]
    win, step = 8, 4
    h, w = ya.shape
    sh = ((h - win) // step + 1, (w - win) // step + 1, win, win)
    st = (ya.strides[0] * step, ya.strides[1] * step, ya.strides[0], ya.strides[1])
    wa = np.lib.stride_tricks.as_strided(ya, sh, st); wb = np.lib.stride_tricks.as_strided(yb, sh, st)
    ma, mb = wa.mean(axis=(2, 3)), wb.mean(axis=(2, 3))
    va, vb = wa.var(axis=(2, 3)), wb.var(axis=(2, 3))
    cov = (wa * wb).mean(axis=(2, 3)) - ma * mb
    c1, c2 = (0.01 * 255) ** 2, (0.03 * 255) ** 2
    s = ((2 * ma * mb + c1) * (2 * cov + c2)) / ((ma * ma + mb * mb + c1) * (va + vb + c2))
    return float(s.mean())

def main():
    a, b = sys.argv[1], sys.argv[2]
    out = sys.argv[sys.argv.index('--diff') + 1] if '--diff' in sys.argv else None
    if out: os.makedirs(out, exist_ok=True)
    names = sorted(set(os.listdir(a)) & set(os.listdir(b)))
    worst = 1.0
    for n in names:
        if not n.endswith('.ppm'): continue
        x, y = read_ppm(os.path.join(a, n)), read_ppm(os.path.join(b, n))
        if x.shape != y.shape: print(f'{n}: size differs {x.shape} vs {y.shape}'); worst = 0; continue
        s = ssim(x, y)
        d = np.abs(x.astype(int) - y.astype(int))
        print(f'{n}: SSIM {s:.4f}  MAE {d.mean():.2f}  >16: {100 * (d.max(axis=2) > 16).mean():.2f}%')
        worst = min(worst, s)
        if out:
            img = np.clip(d * 4, 0, 255).astype(np.uint8)
            with open(os.path.join(out, n), 'wb') as f:
                f.write(b'P6\n%d %d\n255\n' % (img.shape[1], img.shape[0])); f.write(img.tobytes())
    print(f'min SSIM {worst:.4f} over {len(names)} frames')

main()
