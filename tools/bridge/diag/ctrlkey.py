"""ctrlkey.py <SCANCODE_HEX> - send Ctrl+<key> to the foreground window using the
same scancode method as sai2_bridge (SendInput + KEYEVENTF_SCANCODE).

Examples: 1E = A (select all), 2E = C (copy), 2F = V (paste), 21 = F (merge down).
Proven to reach SAI2's custom input framework (unlike plain virtual keys).
"""
import ctypes
import sys
import time
from ctypes import wintypes

u = ctypes.windll.user32
u.SendInput.argtypes = [wintypes.UINT, ctypes.c_void_p, ctypes.c_int]


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


KEYEVENTF_KEYUP = 0x0002
KEYEVENTF_SCANCODE = 0x0008
INPUT_KEYBOARD = 1


def scan(code, up):
    a = (INPUT * 1)()
    a[0].type = INPUT_KEYBOARD
    a[0].u.ki.wScan = code
    a[0].u.ki.dwFlags = KEYEVENTF_SCANCODE | (KEYEVENTF_KEYUP if up else 0)
    u.SendInput(1, ctypes.byref(a), ctypes.sizeof(INPUT))


def ctrl_scan(code):
    scan(0x1D, False)      # Ctrl down
    scan(code, False)
    time.sleep(0.03)
    scan(code, True)
    scan(0x1D, True)       # Ctrl up


def send_vk(vk, up):
    a = (INPUT * 1)()
    a[0].type = INPUT_KEYBOARD
    a[0].u.ki.wVk = vk
    a[0].u.ki.dwFlags = KEYEVENTF_KEYUP if up else 0
    u.SendInput(1, ctypes.byref(a), ctypes.sizeof(INPUT))


def ctrl_alt(letter):
    """Global hotkey Ctrl+Alt+<letter> as registered by sai2_bridge."""
    vk = ord(letter.upper())
    send_vk(0x11, False)   # VK_CONTROL
    send_vk(0x12, False)   # VK_MENU (Alt)
    time.sleep(0.05)
    send_vk(vk, False)
    time.sleep(0.05)
    send_vk(vk, True)
    send_vk(0x12, True)
    send_vk(0x11, True)


def force_foreground():
    """Bring SAI2's main window forward (restoring it if minimised)."""
    import subprocess
    out = subprocess.run(["powershell", "-NoProfile", "-Command",
                          "(Get-Process -Name sai2 | Select-Object -First 1).Id"],
                         capture_output=True, text=True)
    try:
        pid = int(out.stdout.strip())
    except Exception:
        print("no sai2 process")
        return False
    found = []

    @ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)
    def cb(h, l):
        p = wintypes.DWORD()
        u.GetWindowThreadProcessId(h, ctypes.byref(p))
        if p.value != pid:
            return True
        n = u.GetWindowTextLengthW(h)
        b = ctypes.create_unicode_buffer(n + 1)
        u.GetWindowTextW(h, b, n + 1)
        if "PaintTool SAI" in b.value:
            found.append(h)
        return True

    u.EnumWindows(cb, 0)
    if not found:
        print("sai2 main window not found")
        return False
    h = found[0]
    if u.IsIconic(h):
        u.ShowWindow(h, 9)          # SW_RESTORE
        time.sleep(0.3)
    u.SetForegroundWindow(h)
    time.sleep(0.35)
    return u.GetForegroundWindow() == h


if __name__ == "__main__":
    if len(sys.argv) >= 3 and sys.argv[1] == "--hotkey":
        letter = sys.argv[2]
        if not force_foreground():
            print("FAIL foreground")
            sys.exit(2)
        ctrl_alt(letter)
        print("sent Ctrl+Alt+%s" % letter.upper())
        sys.exit(0)
    code = int(sys.argv[1], 16)
    if not force_foreground():
        print("FAIL foreground")
        sys.exit(2)
    ctrl_scan(code)
    print("sent Ctrl+scan(0x%02X)" % code)
