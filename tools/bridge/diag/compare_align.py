"""compare_align.py - measure whether the re-pasted layer landed exactly on the original.

If the padded paste is aligned, the composite canvas is unchanged (the duplicate
sits exactly on top of the original) -> blob bbox identical, diff ~ 0.
If it is offset by (dx,dy) document px, the union bbox grows by ~|dx|,|dy|.
"""
import os
import sys

import numpy as np
from PIL import Image

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
root = r"F:\workspace\SAI2  chajian\SAI2PluginSystem\build-zig"
before = np.asarray(Image.open(os.path.join(root, "before.bmp")).convert("RGB")).astype(np.int16)
after = np.asarray(Image.open(os.path.join(root, "after.bmp")).convert("RGB")).astype(np.int16)
print("before %s   after %s" % (before.shape, after.shape))


def blob_bbox(a):
    """reddish blob (the artwork) inside the view"""
    m = (a[:, :, 0] > 90) & (a[:, :, 1] < 90) & (a[:, :, 2] < 90)
    ys, xs = np.nonzero(m)
    if not len(xs):
        return None
    return (int(xs.min()), int(ys.min()), int(xs.max()), int(ys.max()), int(m.sum()))


def paper_bbox(a):
    m = (a[:, :, 0] > 235) & (a[:, :, 1] > 235) & (a[:, :, 2] > 235)
    ys, xs = np.nonzero(m)
    if not len(xs):
        return None
    return (int(xs.min()), int(ys.min()), int(xs.max()), int(ys.max()))


bb, ba = blob_bbox(before), blob_bbox(after)
pb, pa = paper_bbox(before), paper_bbox(after)
print("paper  before=%s  after=%s" % (pb, pa))
print("blob   before=%s  after=%s" % (bb, ba))
if bb and ba:
    print("blob bbox 变化:  x0 %+d  y0 %+d  x1 %+d  y1 %+d   (像素)"
          % (ba[0] - bb[0], ba[1] - bb[1], ba[2] - bb[2], ba[3] - bb[3]))
    print("blob 面积: before=%d after=%d  (+%d)"
          % (bb[4], ba[4], ba[4] - bb[4]))

d = np.abs(after - before).sum(axis=2)
print("\n整体差异: 不同像素=%d (%.3f%%)  平均=%.3f  最大=%d"
      % (int((d > 8).sum()), 100.0 * (d > 8).sum() / d.size, d.mean(), d.max()))

# 差异集中在哪儿
ys, xs = np.nonzero(d > 8)
if len(xs):
    print("差异包围盒: x %d..%d  y %d..%d" % (xs.min(), xs.max(), ys.min(), ys.max()))
    # 把差异限制在纸内（排除选区蚂蚁线/界面）
    if pb:
        inner = (xs >= pb[0]) & (xs <= pb[2]) & (ys >= pb[1]) & (ys <= pb[3])
        print("其中落在纸内: %d 像素" % int(inner.sum()))
else:
    print("差异包围盒: 无")
