"""verify_match.py - replicate the canvas-context matcher from BridgeApp.cpp
faithfully, and show that BuildSamples' alpha test is the single point of failure.

Faithful details copied from BridgeApp.cpp:
  DownscaleRGBA  : box average RGB (integer division), alpha = max
  ResampleNearest: sy = y*sh/oh (integer division)
  BuildSamples   : stride = max(1, int(sqrt(w*h/cap)));  skip if p.a < (250<<8)
  MaskedSearch   : mean over samples of |dr|+|dg|+|db|, compared to raw bytes
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
print("view %s   template %s" % (view.shape, tmpl.shape))


def downscale_rgba(src, step):
    h, w = src.shape[0] // step, src.shape[1] // step
    h, w = max(1, h), max(1, w)
    blk = src[:h * step, :w * step].reshape(h, step, w, step, 4)
    out = np.zeros((h, w, 4), dtype=np.uint8)
    # C++: o.r = sum(p.r)/n  with p.r raw 8-bit -> integer division
    out[:, :, :3] = (blk[:, :, :, :, :3].sum(axis=(1, 3)) // (step * step)).astype(np.uint8)
    out[:, :, 3] = blk[:, :, :, :, 3].max(axis=(1, 3))
    return out


def resample_nearest(src, w, h):
    sh, sw = src.shape[0], src.shape[1]
    ys = np.minimum(sh - 1, (np.arange(max(1, h)) * sh) // max(1, h))
    xs = np.minimum(sw - 1, (np.arange(max(1, w)) * sw) // max(1, w))
    return src[ys][:, xs]


def build_samples(img, cap, alpha_thresh):
    h, w = img.shape[0], img.shape[1]
    stride = max(1, int(math.sqrt(w * h / cap)))
    samples = []
    for y in range(0, h, stride):
        for x in range(0, w, stride):
            a = int(img[y, x, 3])
            if a < alpha_thresh:
                continue
            rr, gg, bb = int(img[y, x, 0]), int(img[y, x, 1]), int(img[y, x, 2])
            samples.append((x, y, rr, gg, bb))
    return samples


def masked_search(canvas, samples, tw, th, x0, y0, x1, y1, pos_step):
    if not samples:
        return -1, -1, 1e9
    ty = np.array([s[1] for s in samples])
    tx = np.array([s[0] for s in samples])
    sr = np.array([s[2] for s in samples], dtype=np.int16)
    sg = np.array([s[3] for s in samples], dtype=np.int16)
    sb = np.array([s[4] for s in samples], dtype=np.int16)
    ch, cw = canvas.shape[0], canvas.shape[1]
    best = (1e9, -1, -1)
    for py in range(y0, y1 + 1, pos_step):
        if py < 0 or py + th > ch:
            continue
        for px in range(x0, x1 + 1, pos_step):
            if px < 0 or px + tw > cw:
                continue
            c = canvas[py + ty, px + tx].astype(np.int16)
            acc = (np.abs(c[:, 0] - sr) + np.abs(c[:, 1] - sg) + np.abs(c[:, 2] - sb)).mean()
            if acc < best[0]:
                best = (acc, px, py)
    return best[1], best[2], best[0]


# ---- 1) 当前代码：alpha 阈值写成 250<<8 ----
s_bug = build_samples(tmpl, 220, 250 << 8)
print("\n[BUG]  BuildSamples(alpha >= %d) -> %d samples  <-- 永远为 0"
      % (250 << 8, len(s_bug)))

# ---- 2) 修正：8 位 alpha 阈值 250 ----
s_fix = build_samples(tmpl, 220, 250)
print("[FIX]  BuildSamples(alpha >= 250) -> %d samples" % len(s_fix))

if not s_fix:
    print("no samples even after fix; abort")
    sys.exit(1)

kScales = [1.0, 0.9, 0.8, 0.7, 0.6, 0.5, 0.45, 0.4, 0.35, 0.3, 0.25, 0.22,
           0.2, 0.18, 0.16, 0.15, 0.14, 0.13, 0.12, 0.11, 0.1,
           1.15, 1.3, 1.5, 1.75, 2.0, 2.3, 2.6, 3.0, 3.5, 4.0]
SAD_EPS = 15.0
vh, vw = view.shape[0], view.shape[1]
v8 = downscale_rgba(view, 8)
th_, tw_ = tmpl.shape[0], tmpl.shape[1]

print("\nPass A (coarse, view downsampled 8x, pos step 2), SAD_EPS=%.0f" % SAD_EPS)
best = None
for s in kScales:
    rw, rh = int(round(tw_ * s)), int(round(th_ * s))
    if rw < 16 or rh < 16 or rw > vw or rh > vh:
        continue
    t = resample_nearest(tmpl, rw, rh)
    t8 = downscale_rgba(t, 8)
    if t8.shape[0] < 2 or t8.shape[1] < 2:
        continue
    smp = build_samples(t8, 220, 250)
    bx, by, bs = masked_search(v8, smp, t8.shape[1], t8.shape[0],
                               0, 0, v8.shape[1] - t8.shape[1],
                               v8.shape[0] - t8.shape[0], 2)
    flag = "  <== <=%.0f" % SAD_EPS if bs <= SAD_EPS else ""
    print("  s=%-5.2f t8=%2dx%-2d n=%-4d bestSad=%8.2f at (%d,%d)%s"
          % (s, t8.shape[1], t8.shape[0], len(smp), bs, bx, by, flag))
    if bx >= 0 and bs <= SAD_EPS and (best is None or s > best[0]):
        best = (s, bs, bx, by)

if best:
    s, bs, bx8, by8 = best
    print("\n选中最大尺度 s=%.2f (SAD=%.2f) 8x位置=(%d,%d) -> 视图坐标约 (%d,%d)"
          % (s, bs, bx8, by8, bx8 * 8, by8 * 8))
    print("模板在视图里的尺寸 %dx%d" % (round(tw_ * s), round(th_ * s)))
else:
    print("\n没有尺度通过阈值")
