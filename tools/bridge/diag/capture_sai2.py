"""capture_sai2.py - screenshot the SAI2 window so we can SEE whether Ctrl+A
created a real selection (marching ants), and compare the grabbed layer's core
colour against the composited view's core colour (decides if RGB matching can work).
"""
import ctypes
import os
import sys
from ctypes import wintypes

import numpy as np
from PIL import Image

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
root = r"F:\workspace\SAI2  chajian\SAI2PluginSystem"

u = ctypes.windll.user32
g = ctypes.windll.gdi32

u.PrintWindow.argtypes = [wintypes.HWND, wintypes.HDC, ctypes.c_uint]
u.GetWindowRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", wintypes.DWORD), ("biWidth", ctypes.c_long),
                ("biHeight", ctypes.c_long), ("biPlanes", wintypes.WORD),
                ("biBitCount", wintypes.WORD), ("biCompression", wintypes.DWORD),
                ("biSizeImage", wintypes.DWORD),
                ("biXPelsPerMeter", ctypes.c_long),
                ("biYPelsPerMeter", ctypes.c_long),
                ("biClrUsed", wintypes.DWORD), ("biClrImportant", wintypes.DWORD)]


class BITMAPINFO(ctypes.Structure):
    _fields_ = [("bmiHeader", BITMAPINFOHEADER), ("bmiColors", wintypes.DWORD * 3)]


def find_sai2():
    pid = None
    import subprocess
    out = subprocess.run(["powershell", "-NoProfile", "-Command",
                          "(Get-Process -Name sai2 | Select-Object -First 1).Id"],
                         capture_output=True, text=True)
    try:
        pid = int(out.stdout.strip())
    except Exception:
        return None, None
    found = []

    @ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)
    def cb(h, l):
        p = wintypes.DWORD()
        u.GetWindowThreadProcessId(h, ctypes.byref(p))
        if p.value != pid:
            return True
        n = u.GetWindowTextLengthW(h)
        buf = ctypes.create_unicode_buffer(n + 1)
        u.GetWindowTextW(h, buf, n + 1)
        r = wintypes.RECT()
        u.GetWindowRect(h, ctypes.byref(r))
        area = (r.right - r.left) * (r.bottom - r.top)
        # 主窗口 = 标题含 PaintTool SAI（注入的「插件」浮窗也属于本进程）
        found.append((1 if "PaintTool SAI" in buf.value else 0, area, h))
        return True

    u.EnumWindows(cb, 0)
    found.sort(reverse=True)
    return pid, (found[0][2] if found else None)


def capture(hwnd, path):
    r = wintypes.RECT()
    u.GetWindowRect(hwnd, ctypes.byref(r))
    w, h = r.right - r.left, r.bottom - r.top
    hdc = u.GetWindowDC(hwnd)
    mem = g.CreateCompatibleDC(hdc)
    bmp = g.CreateCompatibleBitmap(hdc, w, h)
    g.SelectObject(mem, bmp)
    ok = u.PrintWindow(hwnd, mem, 2)  # PW_RENDERFULLCONTENT
    if not ok:
        g.BitBlt(mem, 0, 0, w, h, hdc, 0, 0, 0x00CC0020)
    bi = BITMAPINFO()
    bi.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bi.bmiHeader.biWidth = w
    bi.bmiHeader.biHeight = -h
    bi.bmiHeader.biPlanes = 1
    bi.bmiHeader.biBitCount = 32
    buf = ctypes.create_string_buffer(w * h * 4)
    g.GetDIBits(mem, bmp, 0, h, buf, ctypes.byref(bi), 0)
    img = Image.frombuffer("RGBA", (w, h), buf, "raw", "BGRA", 0, 1).convert("RGB")
    img.save(path)
    g.DeleteObject(bmp)
    g.DeleteDC(mem)
    u.ReleaseDC(hwnd, hdc)
    return img, ok


pid, hwnd = find_sai2()
print("sai2 pid=%s hwnd=%s" % (pid, hwnd))
if not hwnd:
    sys.exit(1)
img, ok = capture(hwnd, os.path.join(root, "build-zig", "sai2_window.png"))
print("PrintWindow ok=%s  size=%s -> build-zig/sai2_window.png" % (ok, img.size))

# 颜色对比：图层(剪贴板)核心 vs 视图(快照)核心
tmpl = np.asarray(Image.open(os.path.join(root, "build-zig", "grabbed.png")).convert("RGBA"))
core = tmpl[:, :, 3] >= 250
print("template core px=%d  median RGB=%s"
      % (core.sum(), np.median(tmpl[core][:, :3], axis=0) if core.sum() else "n/a"))

snap = np.asarray(Image.open(os.path.join(root, "build-zig", "tools", "thumbs",
                                          "canvas_snap.bmp")).convert("RGB")).astype(np.int16)
red = (snap[:, :, 0] > 90) & (snap[:, :, 1] < 90) & (snap[:, :, 2] < 90)
print("view blob px=%d  median RGB=%s"
      % (red.sum(), np.median(snap[red], axis=0) if red.sum() else "n/a"))
