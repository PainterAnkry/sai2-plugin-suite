"""bbox_cmp.py <before.bmp> <after.bmp> - blob/paper bounding boxes for two
2880-wide SAI2 view snapshots. Used to judge whether "apply + paste back" landed
in place (bbox ~unchanged) or was offset (bbox grows by the bbox origin ~350px).
"""
import os
import sys

import numpy as np
from PIL import Image

sys.stdout.reconfigure(encoding="utf-8", errors="replace")


def bbox(a, pred):
    m = pred(a)
    ys, xs = np.nonzero(m)
    if not len(xs):
        return None
    return (int(xs.min()), int(ys.min()), int(xs.max()), int(ys.max()))


def load(p):
    return np.asarray(Image.open(p).convert("RGB")).astype(np.int16)


def main():
    a = load(sys.argv[1])
    b = load(sys.argv[2])
    print("before %s   after %s" % (a.shape[:2], b.shape[:2]))

    is_paper = lambda x: (x[:, :, 0] > 235) & (x[:, :, 1] > 235) & (x[:, :, 2] > 235)
    is_blob = lambda x: (x[:, :, 0] > 90) & (x[:, :, 1] < 90) & (x[:, :, 2] < 90)

    pa, pb = bbox(a, is_paper), bbox(b, is_paper)
    ba, bb = bbox(a, is_blob), bbox(b, is_blob)
    print("paper before=%s  after=%s" % (pa, pb))
    print("blob  before=%s  after=%s" % (ba, bb))
    if ba and bb:
        print("blob 变化: x0 %+d  y0 %+d  x1 %+d  y1 %+d"
              % (bb[0] - ba[0], bb[1] - ba[1], bb[2] - ba[2], bb[3] - ba[3]))
        print("判读: 错位(按包围盒原点 ~350px) 会让 x0/y0 出现数百像素的负向增长；")
        print("      模糊只会让四边均匀外扩十几个像素。")

    d = np.abs(b - a).sum(axis=2)
    ys, xs = np.nonzero(d > 8)
    print("\n差异像素=%d  平均=%.3f" % (int((d > 8).sum()), d.mean()))
    if len(xs):
        print("差异包围盒: x %d..%d  y %d..%d" % (xs.min(), xs.max(), ys.min(), ys.max()))


main()
