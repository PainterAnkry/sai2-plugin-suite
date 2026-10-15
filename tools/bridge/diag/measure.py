"""measure.py - locate the paper and the painted blob inside the canvas snapshot,
so we can tell whether the clipboard grab (969x593) is the blob's bounding box."""
import os
import sys

import numpy as np
from PIL import Image

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
root = r"F:\workspace\SAI2  chajian\SAI2PluginSystem"
snap = Image.open(os.path.join(root, "build-zig", "tools", "thumbs", "canvas_snap.bmp")).convert("RGB")
a = np.asarray(snap).astype(np.int16)
print("snapshot:", snap.size)

# white paper: near-white pixels
white = (a[:, :, 0] > 235) & (a[:, :, 1] > 235) & (a[:, :, 2] > 235)
ys, xs = np.nonzero(white)
if len(xs):
    print("paper(white) bbox: x %d..%d (%d)  y %d..%d (%d)"
          % (xs.min(), xs.max(), xs.max() - xs.min() + 1,
             ys.min(), ys.max(), ys.max() - ys.min() + 1))
else:
    print("no white paper found")

# the red blob: strongly red, not white/gray
red = (a[:, :, 0] > 90) & (a[:, :, 1] < 90) & (a[:, :, 2] < 90)
ys, xs = np.nonzero(red)
if len(xs):
    print("red blob bbox:      x %d..%d (%d)  y %d..%d (%d)"
          % (xs.min(), xs.max(), xs.max() - xs.min() + 1,
             ys.min(), ys.max(), ys.max() - ys.min() + 1))
    print("blob size / paper size = %.3f x %.3f"
          % ((xs.max() - xs.min() + 1) / (white_x := 1), 1)) if False else None

g = Image.open(os.path.join(root, "build-zig", "grabbed.png")).convert("RGBA")
print("grabbed(clipboard):", g.size)
ga = np.asarray(g)
alpha = ga[:, :, 3]
op = alpha >= 250
print("  opaque pixels: %d / %d (%.1f%%)" % (op.sum(), alpha.size, 100.0 * op.sum() / alpha.size))
ys, xs = np.nonzero(op)
if len(xs):
    print("  opaque bbox: x %d..%d (%d)  y %d..%d (%d)  -> tight=%s"
          % (xs.min(), xs.max(), xs.max() - xs.min() + 1,
             ys.min(), ys.max(), ys.max() - ys.min() + 1,
             (xs.min() == 0 and ys.min() == 0 and xs.max() == g.size[0] - 1
              and ys.max() == g.size[1] - 1)))
