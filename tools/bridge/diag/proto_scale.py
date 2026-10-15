"""proto_scale.py - prototype a bias-free scale estimate.

Why: the current matcher builds samples only from alpha>=250 (the eroded opaque
core), so a template scaled slightly LARGER than truth still fits inside the
view's blob -> SAD stays low -> "largest passing scale" overshoots (measured
0.519 instead of the true 0.500), which shifts the recovered offset by ~(38,21)px.

Fix idea: use the FULL silhouette. Composite the template over the detected
paper colour and compare against the view, including the semi-transparent edge
band and the fully transparent margin (which must land on paper). Then the score
has a sharp minimum exactly at the true scale.
"""
import math
import os
import sys

import numpy as np
from PIL import Image

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
root = r"F:\workspace\SAI2  chajian\SAI2PluginSystem"

view = np.asarray(Image.open(os.path.join(root, "build-zig", "tools", "thumbs",
                                          "canvas_snap.bmp")).convert("RGBA"), dtype=np.uint8)
tmpl = np.asarray(Image.open(os.path.join(root, "build-zig", "grabbed.png")).convert("RGBA"),
                  dtype=np.uint8)
vh, vw = view.shape[0], view.shape[1]
th, tw = tmpl.shape[0], tmpl.shape[1]
print("view %dx%d  template %dx%d" % (vw, vh, tw, th))


def downscale_rgba(src, step):
    h, w = max(1, src.shape[0] // step), max(1, src.shape[1] // step)
    blk = src[:h * step, :w * step].reshape(h, step, w, step, 4)
    out = np.zeros((h, w, 4), dtype=np.uint8)
    out[:, :, :3] = (blk[:, :, :, :, :3].sum(axis=(1, 3)) // (step * step)).astype(np.uint8)
    out[:, :, 3] = blk[:, :, :, :, 3].max(axis=(1, 3))
    return out


def resample_nearest(src, w, h):
    sh, sw = src.shape[0], src.shape[1]
    ys = np.minimum(sh - 1, (np.arange(max(1, h)) * sh) // max(1, h))
    xs = np.minimum(sw - 1, (np.arange(max(1, w)) * sw) // max(1, w))
    return src[ys][:, xs]


# --- 纸色：在粗匹配内容矩形外围环带里取模态色（这里用已知的粗结果）---
CRUDE_S, CRUDE_X, CRUDE_Y = 0.5, 992, 512
bw, bh = int(round(tw * CRUDE_S)), int(round(th * CRUDE_S))
m = 24
ring = []
for y in range(max(0, CRUDE_Y - m), min(vh, CRUDE_Y + bh + m), 3):
    for x in range(max(0, CRUDE_X - m), min(vw, CRUDE_X + bw + m), 3):
        if CRUDE_X <= x < CRUDE_X + bw and CRUDE_Y <= y < CRUDE_Y + bh:
            continue
        ring.append(view[y, x, :3])
ring = np.array(ring)
vals, counts = np.unique(ring, axis=0, return_counts=True)
paper = vals[counts.argmax()].astype(np.float64)
print("paper colour (ring modal) =", paper, " frac=%.2f" % (counts.max() / len(ring)))

v2 = downscale_rgba(view, 2).astype(np.float64)   # 1296x736
base_x, base_y = CRUDE_X // 2, CRUDE_Y // 2

print("\nscale sweep (template composited over paper, full silhouette; view/2)")
print("  s      n_ink n_blank   SAD_straight  SAD_premul   best_pos")
results = []
for s in np.arange(0.44, 0.585, 0.005):
    rw2 = max(2, int(round(tw * s / 2)))
    rh2 = max(2, int(round(th * s / 2)))
    t2 = resample_nearest(tmpl, rw2, rh2).astype(np.float64)
    a = (t2[:, :, 3:4] / 255.0)
    ink = t2[:, :, 3] >= 128
    blank = t2[:, :, 3] < 8
    sel = ink | blank
    ys, xs = np.nonzero(sel)
    if len(xs) == 0:
        continue
    step = 3
    ys, xs = ys[::step], xs[::step]
    rgb = t2[ys, xs, :3]
    aa = a[ys, xs, 0]
    exp_straight = rgb * aa[:, None] + paper[None, :] * (1 - aa[:, None])
    exp_premul = rgb + paper[None, :] * (1 - aa[:, None])
    is_ink = ink[ys, xs]
    wgt = np.where(is_ink, 1.0, 0.6)

    best = (1e9, 1e9, -1, -1)
    for dy in range(-8, 9):
        for dx in range(-8, 9):
            py, px = base_y + dy, base_x + dx
            if py < 0 or px < 0 or py + rh2 > v2.shape[0] or px + rw2 > v2.shape[1]:
                continue
            c = v2[py + ys, px + xs, :3]
            d1 = (np.abs(c - exp_straight).sum(axis=1) * wgt).mean()
            d2 = (np.abs(c - exp_premul).sum(axis=1) * wgt).mean()
            if d1 + d2 < best[0] + best[1]:
                best = (d1, d2, px, py)
    results.append((s, int(is_ink.sum()), int((~is_ink).sum()), best[0], best[1], best[2], best[3]))
    print("  %-6.3f %5d %6d   %10.2f  %10.2f   (%d,%d)"
          % (s, is_ink.sum(), (~is_ink).sum(), best[0], best[1], best[2], best[3]))

rs = np.array([[r[0], r[3], r[4]] for r in results])
i1 = int(rs[:, 1].argmin())
i2 = int(rs[:, 2].argmin())
print("\nSAD_straight 最小在 s=%.3f  (期望 0.500)" % rs[i1, 0])
print("SAD_premul   最小在 s=%.3f  (期望 0.500)" % rs[i2, 0])
print("对比：当前代码选到 s=0.519 -> 画布 1847x1038；真值 s=0.500 -> 1920x1080")
