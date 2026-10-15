"""align_verdict.py - distinguish "aligned duplicate" from "positional offset".

Two hypotheses both produce differences:
  A) aligned  : the duplicate sits exactly on top -> only the semi-transparent
                edge band changes (alpha compositing of two identical edges),
                the fully-opaque interior is byte-identical, and the change is a
                uniform band around the whole blob.
  B) offset   : a crescent of difference appears on ONE side, and the interior
                near the leading edge differs.
So: check the eroded interior (>=12px inside the blob). Aligned => diff ~ 0 there.
"""
import os
import sys

import numpy as np
from PIL import Image, ImageFilter

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
root = r"F:\workspace\SAI2  chajian\SAI2PluginSystem\build-zig"


def erode(mask, r):
    im = Image.fromarray((mask * 255).astype(np.uint8), "L")
    return np.asarray(im.filter(ImageFilter.MinFilter(2 * r + 1))) > 127


def dilate(mask, r):
    im = Image.fromarray((mask * 255).astype(np.uint8), "L")
    return np.asarray(im.filter(ImageFilter.MaxFilter(2 * r + 1))) > 127


before = np.asarray(Image.open(os.path.join(root, "before.bmp")).convert("RGB")).astype(np.int16)
after = np.asarray(Image.open(os.path.join(root, "after.bmp")).convert("RGB")).astype(np.int16)

# 纸内区域（排除选区蚂蚁线等界面元素）
paper = (816, 466, 1775, 1005)
sub_b = before[paper[1]:paper[3], paper[0]:paper[2]]
sub_a = after[paper[1]:paper[3], paper[0]:paper[2]]

d = np.abs(sub_a - sub_b).sum(axis=2)
print("纸内: 尺寸 %s  不同像素(>8)=%d  平均=%.4f"
      % (sub_b.shape[:2], int((d > 8).sum()), d.mean()))

blob = (sub_b[:, :, 0] > 90) & (sub_b[:, :, 1] < 90) & (sub_b[:, :, 2] < 90)
core = erode(blob, 12)
print("blob 像素=%d   腐蚀 12px 后核心=%d" % (int(blob.sum()), int(core.sum())))

dc = d[core]
print("\n[核心内部] 不同像素(>8)=%d / %d   平均=%.5f   最大=%d"
      % (int((dc > 8).sum()), dc.size, dc.mean(), dc.max()))
print("=> 若为 0/极小，说明补边后的图层与原件**逐像素重合**（假设 A：对齐）")

ys, xs = np.nonzero(d > 8)
if len(xs):
    print("\n差异像素空间分布: 左外侧=%d  右外侧=%d  （真实偏移会造成明显不对称）"
          % (int((xs < 86).sum()), int((xs > 266).sum())))
    ring = dilate(blob, 25) & ~erode(blob, 25)
    print("blob 边界 ±25px 环带内差异: %d / 环带面积 %d" % (int((d[ring] > 8).sum()), int(ring.sum())))
    far = ~dilate(blob, 40)
    print("远离 blob(>40px) 的差异: %d  （应接近 0）" % int((d[far] > 8).sum()))

vis = np.clip(d, 0, 255).astype(np.uint8)
Image.fromarray(vis).save(os.path.join(root, "diff_vis.png"))
print("\n差异图: build-zig/diff_vis.png")
