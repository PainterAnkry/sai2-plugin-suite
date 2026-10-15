# test_sai2keys.ps1 - inject Ctrl+A / Ctrl+C into SAI2 using the SAME scancode
# method as sai2_bridge, to determine whether SAI2's custom input framework
# accepts synthesized keys at all. That decides whether the "grab layer" path
# is really grabbing the selected layer, or just reusing whatever is already
# on the clipboard.
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class K {
    [StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT { public int dx, dy; public uint mouseData, dwFlags, time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Sequential)] public struct KEYBDINPUT { public ushort wVk, wScan; public uint dwFlags, time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Sequential)] public struct HARDWAREINPUT { public uint uMsg; public ushort wParamL, wParamH; }
    [StructLayout(LayoutKind.Explicit)] public struct InputUnion {
        [FieldOffset(0)] public MOUSEINPUT mi;
        [FieldOffset(0)] public KEYBDINPUT ki;
        [FieldOffset(0)] public HARDWAREINPUT hi;
    }
    [StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public InputUnion U; }

    [DllImport("user32.dll", SetLastError=true)] public static extern uint SendInput(uint n, INPUT[] p, int cb);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr pid);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool f);
    [DllImport("user32.dll")] public static extern IntPtr SetFocus(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsClipboardFormatAvailable(uint f);

    const uint INPUT_KEYBOARD = 1;
    const uint KEYEVENTF_KEYUP = 0x0002;
    const uint KEYEVENTF_SCANCODE = 0x0008;

    public static int Size() { return Marshal.SizeOf(typeof(INPUT)); }

    public static void Scan(ushort scan, bool up) {
        INPUT[] a = new INPUT[1];
        a[0].type = INPUT_KEYBOARD;
        a[0].U.ki.wScan = scan;
        a[0].U.ki.dwFlags = KEYEVENTF_SCANCODE | (up ? KEYEVENTF_KEYUP : 0);
        SendInput(1, a, Size());
    }
    public static void CtrlScan(ushort scan) {
        Scan(0x1D, false); Scan(scan, false);
        System.Threading.Thread.Sleep(30);
        Scan(scan, true); Scan(0x1D, true);
    }
    public static void ForceForeground(IntPtr target) {
        IntPtr fore = GetForegroundWindow();
        if (fore == target) return;
        uint ft = GetWindowThreadProcessId(fore, IntPtr.Zero);
        uint ot = GetCurrentThreadId();
        AttachThreadInput(ot, ft, true);
        BringWindowToTop(target);
        SetForegroundWindow(target);
        SetFocus(target);
        AttachThreadInput(ot, ft, false);
    }
}
'@

Add-Type -AssemblyName System.Windows.Forms

$sai = Get-Process -Name sai2 -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $sai) { Write-Output "FAIL no sai2 process"; exit 1 }
$h = $sai.MainWindowHandle
Write-Output ("SAI2 pid=" + $sai.Id + " hwnd=" + $h + " title=" + $sai.MainWindowTitle)

$before = [System.Windows.Forms.Clipboard]::GetDataObject()
Write-Output ("formats BEFORE: " + (($before.GetFormats()) -join ', '))

[K]::ForceForeground($h)
Start-Sleep -Milliseconds 400
Write-Output ("foreground is SAI2: " + ([K]::GetForegroundWindow() -eq $h))

[K]::CtrlScan(0x1E)   # Ctrl+A
Start-Sleep -Milliseconds 400
[K]::CtrlScan(0x2E)   # Ctrl+C
Start-Sleep -Milliseconds 900

$after = [System.Windows.Forms.Clipboard]::GetDataObject()
Write-Output ("formats AFTER: " + (($after.GetFormats()) -join ', '))
if ($after.ContainsImage()) {
    $img = $after.GetImage()
    Write-Output ("clipboard image size: " + $img.Width + " x " + $img.Height)
    $img.Dispose()
} else {
    Write-Output "clipboard has NO image after Ctrl+A/Ctrl+C"
}
Write-Output ("CF_DIBV5 available: " + [K]::IsClipboardFormatAvailable(17) + "  CF_DIB available: " + [K]::IsClipboardFormatAvailable(8))
