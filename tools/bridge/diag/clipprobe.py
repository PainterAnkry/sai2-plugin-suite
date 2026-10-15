"""clipprobe.py - inspect what SAI2 puts on the clipboard after Ctrl+C.

Answers three questions that decide how the grab->paste offset must be solved:
  1. Is the copied bitmap the full canvas (1920x1080 for 测试.sai2) or a crop?
  2. Does SAI2's CF_DIBV5 declare an alpha mask (alphaMask != 0)?
  3. Does SAI2's private registered clipboard format carry the layer origin?
"""
import ctypes
import struct
import sys
from ctypes import c_void_p, c_uint, c_int, c_size_t, c_wchar_p

from PIL import Image

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

u = ctypes.windll.user32
k = ctypes.windll.kernel32
k.GlobalLock.restype = c_void_p
k.GlobalLock.argtypes = [c_void_p]
k.GlobalSize.restype = c_size_t
k.GlobalSize.argtypes = [c_void_p]
k.GlobalUnlock.argtypes = [c_void_p]
u.GetClipboardData.restype = c_void_p
u.GetClipboardData.argtypes = [c_uint]
u.OpenClipboard.argtypes = [c_void_p]
u.EnumClipboardFormats.restype = c_uint
u.EnumClipboardFormats.argtypes = [c_uint]
u.GetClipboardFormatNameW.argtypes = [c_uint, c_wchar_p, c_int]

CF_DIB = 8
CF_DIBV5 = 17

NAMES = {2: "CF_BITMAP", 3: "CF_METAFILEPICT", 8: "CF_DIB", 14: "CF_ENHMETAFILE",
         15: "CF_HDROP", 17: "CF_DIBV5"}


def fmt_name(f):
    if f in NAMES:
        return NAMES[f]
    if f >= 0xC000:
        buf = ctypes.create_unicode_buffer(256)
        n = u.GetClipboardFormatNameW(f, buf, 256)
        return buf.value if n else ("registered#%d" % f)
    return "fmt#%d" % f


def get_bytes(fmt):
    h = u.GetClipboardData(fmt)
    if not h:
        return None
    p = k.GlobalLock(h)
    if not p:
        return None
    size = k.GlobalSize(h)
    data = ctypes.string_at(p, size)
    k.GlobalUnlock(h)
    return data


def parse_dib(b, label):
    if len(b) < 40:
        print("  %s: too small" % label)
        return None
    hdr = struct.unpack_from("<I", b, 0)[0]
    w, h, planes, bpp, comp = struct.unpack_from("<iiHHI", b, 4)
    print("  %s: size=%d headerSize=%d w=%d h=%d bpp=%d compression=%d"
          % (label, len(b), hdr, w, h, bpp, comp))
    alpha_mask = 0
    if hdr >= 108 and len(b) >= 56:
        rm, gm, bm, am = struct.unpack_from("<IIII", b, 40)
        alpha_mask = am
        print("    masks R=%08X G=%08X B=%08X A=%08X" % (rm, gm, bm, am))
    stride = ((abs(w) * bpp + 31) // 32) * 4
    total = stride * abs(h)
    print("    stride=%d pixels=%d => dataStart=%d (headerEnd=%d)"
          % (stride, total, len(b) - total, hdr))
    return (w, h, bpp, hdr, comp, alpha_mask, stride, len(b) - total)


def dib_to_image(b, info):
    w, h, bpp, hdr, comp, amask, stride, off = info
    img = Image.new("RGBA", (w, abs(h)))
    px = img.load()
    top_down = h < 0
    for y in range(abs(h)):
        src_row = y if top_down else (abs(h) - 1 - y)
        row = b[off + src_row * stride: off + src_row * stride + stride]
        for x in range(w):
            if bpp == 32:
                bb, gg, rr, aa = row[x * 4], row[x * 4 + 1], row[x * 4 + 2], row[x * 4 + 3]
            else:
                bb, gg, rr = row[x * 3], row[x * 3 + 1], row[x * 3 + 2]
                aa = 255
            if amask == 0:
                aa = 255
            px[x, y] = (rr, gg, bb, aa)
    return img


def main():
    out_dib = sys.argv[1] if len(sys.argv) > 1 else None
    if not u.OpenClipboard(None):
        print("OpenClipboard failed")
        return 1
    try:
        print("=== clipboard formats ===")
        fmts = []
        f = 0
        while True:
            f = u.EnumClipboardFormats(f)
            if f == 0:
                break
            fmts.append(f)
            print("  %-6d %s" % (f, fmt_name(f)))

        info = None
        for cand, lbl in ((CF_DIBV5, "CF_DIBV5"), (CF_DIB, "CF_DIB")):
            if cand in fmts:
                b = get_bytes(cand)
                if b:
                    print("=== %s ===" % lbl)
                    info = parse_dib(b, lbl)
                    if cand == CF_DIBV5 and out_dib and info:
                        img = dib_to_image(b, info)
                        img.save(out_dib)
                        print("    saved -> %s" % out_dib)
                    break

        for f in fmts:
            if f >= 0xC000:
                b = get_bytes(f)
                if not b:
                    continue
                print("=== private format %s (%d bytes) ===" % (fmt_name(f), len(b)))
                n = min(128, len(b))
                print("  hex: " + " ".join("%02X" % c for c in b[:n]))
                asc = "".join(chr(c) if 32 <= c < 127 else "." for c in b[:n])
                print("  asc: " + asc)
                for i in range(0, min(40, len(b) - 3), 4):
                    v = struct.unpack_from("<I", b, i)[0]
                    print("  u32@%-3d = %-12d 0x%08X" % (i, v, v))
    finally:
        u.CloseClipboard()
    return 0


if __name__ == "__main__":
    sys.exit(main())
