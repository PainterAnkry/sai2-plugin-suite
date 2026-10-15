"""probe2.py - (a) read SAI2's private clipboard format with error reporting,
(b) find SAI2's real main window (largest / by title) and print its rect.
"""
import ctypes
import struct
import sys
from ctypes import c_void_p, c_uint, c_int, c_size_t, c_wchar_p, wintypes

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
u.RegisterClipboardFormatW.restype = c_uint
u.RegisterClipboardFormatW.argtypes = [c_wchar_p]
u.GetClipboardFormatNameW.argtypes = [c_uint, c_wchar_p, c_int]

PRIVATE = "A44EB595-DAFE-40EC-9FA4-628BA75C0B0B"

print("=== (a) SAI2 private clipboard format ===")
if not u.OpenClipboard(None):
    print("OpenClipboard failed")
else:
    try:
        fid = u.RegisterClipboardFormatW(PRIVATE)
        print("RegisterClipboardFormatW -> %d" % fid)
        k.SetLastError(0)
        h = u.GetClipboardData(fid)
        print("GetClipboardData -> %s  GetLastError=%d" % (h, k.GetLastError()))
        if h:
            p = k.GlobalLock(h)
            size = k.GlobalSize(h)
            print("GlobalLock -> %s  GlobalSize=%d" % (p, size))
            if p:
                b = ctypes.string_at(p, min(size, 256))
                print("hex: " + " ".join("%02X" % c for c in b[:96]))
                print("asc: " + "".join(chr(c) if 32 <= c < 127 else "." for c in b[:96]))
                for i in range(0, min(48, len(b) - 3), 4):
                    v = struct.unpack_from("<I", b, i)[0]
                    print("  u32@%-3d = %-12d 0x%08X" % (i, v, v))
                k.GlobalUnlock(h)
        # also list what else is there
        f = 0
        print("formats:", end=" ")
        while True:
            f = u.EnumClipboardFormats(f)
            if f == 0:
                break
            if f >= 0xC000:
                buf = ctypes.create_unicode_buffer(256)
                u.GetClipboardFormatNameW(f, buf, 256)
                print("%d=%s" % (f, buf.value), end="  ")
            else:
                print("%d" % f, end="  ")
        print()
    finally:
        u.CloseClipboard()

print()
print("=== (b) SAI2 windows ===")
import subprocess
out = subprocess.run(["powershell", "-NoProfile", "-Command",
                      "(Get-Process -Name sai2 | Select-Object -First 1).Id"],
                     capture_output=True, text=True)
pid = int(out.stdout.strip())
print("pid =", pid)
rows = []


@ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)
def cb(h, l):
    p = wintypes.DWORD()
    u.GetWindowThreadProcessId(h, ctypes.byref(p))
    if p.value != pid:
        return True
    r = wintypes.RECT()
    u.GetWindowRect(h, ctypes.byref(r))
    n = u.GetWindowTextLengthW(h)
    buf = ctypes.create_unicode_buffer(n + 1)
    u.GetWindowTextW(h, buf, n + 1)
    rows.append((h, r.left, r.top, r.right - r.left, r.bottom - r.top,
                 bool(u.IsWindowVisible(h)), buf.value))
    return True


u.EnumWindows(cb, 0)
rows.sort(key=lambda t: -(t[3] * t[4]))
for h, x, y, w, hh, vis, title in rows[:8]:
    print("hwnd=%-10d rect=(%d,%d %dx%d) visible=%s title=%r" % (h, x, y, w, hh, vis, title))
