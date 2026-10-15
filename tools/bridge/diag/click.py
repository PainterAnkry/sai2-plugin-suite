"""click.py - send a real mouse click (and optional key combos) at SCREEN coords.

usage:
  click.py <x> <y> [--ctrl] [--shift] [--double] [--hold-ctrl-during]
  click.py --key <SCANCODE> [...]      # Ctrl+<scancode>, same as ctrlkey.py

Why a separate file: the SAI2 experiments need reliable real input, and mixing
"move mouse" / "ctrl+click" / "drag" in one place keeps the coordinate math in
one spot (screen coords, no client-area conversion).
"""
import ctypes
import sys
import time
from ctypes import wintypes

u = ctypes.windll.user32
# 必须声明 DPI 感知：否则 SendInput 的绝对坐标（物理像素）与
# GetSystemMetrics 返回的虚拟化坐标不一致，点击会整体偏移。
try:
    ctypes.windll.user32.SetProcessDPIAware()
except Exception:
    pass
u.SendInput.argtypes = [wintypes.UINT, ctypes.c_void_p, ctypes.c_int]

MOUSEEVENTF_MOVE = 0x0001
MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004
MOUSEEVENTF_ABSOLUTE = 0x8000
MOUSEEVENTF_VIRTUALDESK = 0x4000
KEYEVENTF_KEYUP = 0x0002
KEYEVENTF_SCANCODE = 0x0008
INPUT_MOUSE = 0
INPUT_KEYBOARD = 1

VK_CONTROL = 0x11
VK_SHIFT = 0x10
VK_MENU = 0x12


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [("dx", ctypes.c_long), ("dy", ctypes.c_long),
                ("mouseData", wintypes.DWORD), ("dwFlags", wintypes.DWORD),
                ("time", wintypes.DWORD), ("dwExtraInfo", ctypes.c_void_p)]


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wintypes.WORD), ("wScan", wintypes.WORD),
                ("dwFlags", wintypes.DWORD), ("time", wintypes.DWORD),
                ("dwExtraInfo", ctypes.c_void_p)]


class HARDWAREINPUT(ctypes.Structure):
    _fields_ = [("uMsg", wintypes.DWORD), ("wParamL", wintypes.WORD),
                ("wParamH", wintypes.WORD)]


class INPUTUNION(ctypes.Union):
    _fields_ = [("mi", MOUSEINPUT), ("ki", KEYBDINPUT), ("hi", HARDWAREINPUT)]


class INPUT(ctypes.Structure):
    _fields_ = [("type", wintypes.DWORD), ("u", INPUTUNION)]


def _send(inp):
    u.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))


def mouse_move(x, y):
    # MOUSEEVENTF_ABSOLUTE 要求坐标归一化到 0..65535（不是像素！）。
    # 早前这里直接把像素填进 mi.dx/dy，于是每次点击都被解释成"屏幕极左上角"，
    # 表现为"点了完全没反应"（新建图层/涂抹/点图层行全都失效）。
    vx = u.GetSystemMetrics(76)
    vy = u.GetSystemMetrics(77)
    vw = max(1, u.GetSystemMetrics(78))
    vh = max(1, u.GetSystemMetrics(79))
    i = INPUT()
    i.type = INPUT_MOUSE
    i.u.mi.dx = int((x - vx) * 65535 / vw)
    i.u.mi.dy = int((y - vy) * 65535 / vh)
    i.u.mi.dwFlags = (MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE |
                      MOUSEEVENTF_VIRTUALDESK)
    _send(i)
    time.sleep(0.05)


def mouse_btn(down):
    i = INPUT()
    i.type = INPUT_MOUSE
    i.u.mi.dwFlags = MOUSEEVENTF_LEFTDOWN if down else MOUSEEVENTF_LEFTUP
    _send(i)


def key_vk(vk, up):
    i = INPUT()
    i.type = INPUT_KEYBOARD
    i.u.ki.wVk = vk
    i.u.ki.dwFlags = KEYEVENTF_KEYUP if up else 0
    _send(i)


def key_scan(code, up):
    i = INPUT()
    i.type = INPUT_KEYBOARD
    i.u.ki.wScan = code
    i.u.ki.dwFlags = KEYEVENTF_SCANCODE | (KEYEVENTF_KEYUP if up else 0)
    _send(i)


def click(x, y, ctrl=False, shift=False, double=False):
    mouse_move(x, y)
    time.sleep(0.08)
    if ctrl:
        key_vk(VK_CONTROL, False)
    if shift:
        key_vk(VK_SHIFT, False)
    time.sleep(0.05)
    mouse_btn(True)
    time.sleep(0.05)
    mouse_btn(False)
    if double:
        time.sleep(0.05)
        mouse_btn(True)
        time.sleep(0.05)
        mouse_btn(False)
    time.sleep(0.05)
    if ctrl:
        key_vk(VK_CONTROL, True)
    if shift:
        key_vk(VK_SHIFT, True)


def drag(x0, y0, x1, y1, steps=25, ctrl=False):
    mouse_move(x0, y0)
    time.sleep(0.1)
    if ctrl:
        key_vk(VK_CONTROL, False)
    mouse_btn(True)
    for i in range(1, steps + 1):
        mouse_move(x0 + (x1 - x0) * i // steps, y0 + (y1 - y0) * i // steps)
        time.sleep(0.02)
    mouse_btn(False)
    if ctrl:
        key_vk(VK_CONTROL, True)
    time.sleep(0.15)


if __name__ == "__main__":
    a = sys.argv[1:]
    if not a:
        print(__doc__)
        sys.exit(2)
    if a[0] == "--key":
        code = int(a[1], 16)
        key_vk(VK_CONTROL, False)
        key_scan(code, False)
        time.sleep(0.03)
        key_scan(code, True)
        key_vk(VK_CONTROL, True)
        print("sent Ctrl+scan(0x%02X)" % code)
        sys.exit(0)
    if a[0] == "--drag":
        x0, y0, x1, y1 = (int(v) for v in a[1:5])
        drag(x0, y0, x1, y1, steps=30)
        print("dragged (%d,%d)->(%d,%d)" % (x0, y0, x1, y1))
        sys.exit(0)
    x, y = int(a[0]), int(a[1])
    click(x, y, ctrl="--ctrl" in a, shift="--shift" in a, double="--double" in a)
    print("clicked (%d,%d)%s%s%s" % (x, y, " ctrl" if "--ctrl" in a else "",
                                     " shift" if "--shift" in a else "",
                                     " double" if "--double" in a else ""))
