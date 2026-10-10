// ============================================================================
// UiWindow.cpp - 功能窗口的创建/消息处理/公共命中分发
//
// 每个功能窗口都是独立的顶层窗口（WS_OVERLAPPED 可缩放），内部布局统一：
//   ┌ 标题栏（窗口自己的） ────────────────────────────────┐
//   │ 功能按钮条（模糊 | 滤镜 | 调整 | 液化）                │
//   │ 组内工具条（液化：7 个工具 / 模糊：4 种 …）             │
//   │ ┌ 画布 ────────────────────────────┐ ┌ 参数面板 ─┐   │
//   │ └──────────────────────────────────┘ └───────────┘   │
//   │ 命令条（应用 → 剪贴板 / 撤销 / 复位 / 关闭）           │
//   └──────────────────────────────────────────────────────┘
// ============================================================================

#include "Ui.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windowsx.h>

namespace sai2 {
namespace bridge {
namespace ui {

namespace {

// 主面板的类名与窗口过程由 BridgeApp.cpp 提供（唯一来源，见 Ui.h）
const ToolWindowSpec* Specs() {
    static ToolWindowSpec specs[BridgeCore::kWinCount];
    static bool init = false;
    if (!init) {
        init = true;
        // 0 号是主面板占位（类与窗口过程由 RegisterWindowClasses 单独处理）
        specs[BridgeCore::kWinMain] = {BridgeCore::kWinMain, nullptr, nullptr,
                                       780, 900, nullptr, nullptr, nullptr};
        specs[BridgeCore::kWinBlur] = BlurWindowSpec();
        specs[BridgeCore::kWinFilter] = FilterWindowSpec();
        specs[BridgeCore::kWinAdjust] = AdjustWindowSpec();
        specs[BridgeCore::kWinLiquify] = LiquifyWindowSpec();
    }
    return specs;
}

UiWindow* WindowById(int id) {
    AppState& s = S();
    if (id == BridgeCore::kWinMain) return &s.main;
    if (id >= 1 && id < BridgeCore::kWinCount) return &s.toolWindows[id];
    return nullptr;
}

// 工具窗口的命中/绘制/命令分别由各自文件实现
void WindowPaint(UiWindow& win, HDC dc, const RECT& rc) {
    const ToolWindowSpec* sp = &Specs()[win.id];
    win.hits.clear();
    if (sp->draw) sp->draw(dc, win, rc);
}

bool WindowHit(UiWindow& win, const UiHit& h) {
    // 公共：分类按钮（HK_TOOL 且 index < 0）
    if (h.kind == HK_TOOL && h.index < 0) {
        const int idx = -h.index; // 1..4
        if (idx >= 1 && idx < BridgeCore::kWinCount) {
            if (idx == win.id) return true;
            RequestToolWindow(win.hwnd, idx, true);
            return true;
        }
        return true;
    }
    if (HandleZoomHit(win, h)) {
        InvalidateRect(win.hwnd, nullptr, FALSE);
        return true;
    }
    if (h.kind == HK_CMD) {
        RunCmd(win, h.index);
        return true;
    }
    const ToolWindowSpec* sp = &Specs()[win.id];
    if (sp->hit && sp->hit(win, h)) {
        InvalidateRect(win.hwnd, nullptr, FALSE);
        return true;
    }
    InvalidateRect(win.hwnd, nullptr, FALSE);
    return true;
}

LRESULT CALLBACK ToolWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    AppState& s = S();
    UiWindow* pw = reinterpret_cast<UiWindow*>(GetWindowLongPtrW(hwnd,
                                                                 GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        pw = reinterpret_cast<UiWindow*>(cs->lpCreateParams);
        if (pw) {
            pw->hwnd = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(pw));
        }
    }
    if (!pw) return DefWindowProcW(hwnd, msg, wp, lp);
    UiWindow& win = *pw;

    switch (msg) {
    case WM_CREATE:
        InitFonts();
        CreateParamEdits(win);
        return 0;

    case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(wp);
        SetTextColor(dc, C_TEXT);
        SetBkColor(dc, ThemeIsDark() ? RGB(26, 27, 29) : RGB(255, 255, 255));
        static HBRUSH editBgDark = CreateSolidBrush(RGB(26, 27, 29));
        static HBRUSH editBgLight = CreateSolidBrush(RGB(255, 255, 255));
        return reinterpret_cast<LRESULT>(ThemeIsDark() ? editBgDark
                                                       : editBgLight);
    }

    case WM_COMMAND: {
        const int id = LOWORD(wp);
        const int notif = HIWORD(wp);
        if (notif == EN_KILLFOCUS && id >= 100 && id < 104) {
            CommitParamEdit(win, id - 100);
            win.bgraDirty = true;
        }
        return 0;
    }

    case WM_TIMER:
        if (wp == 3) { // 滤镜缩略图后台完成
            KillTimer(hwnd, 3);
            win.thumbsBuilding = false;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);
        WindowPaint(win, mem, rc);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        const int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);
        for (const UiHit& h : win.hits) {
            if (!InRect(h.rc, mx, my)) continue;
            if (h.kind == HK_SLIDER) {
                SetCapture(hwnd);
                win.dragSlider = h.index;
                OnSliderDrag(win, mx);
                win.bgraDirty = true;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (h.kind == HK_ZSLIDER) {
                SetCapture(hwnd);
                win.dragZoom = true;
                win.zoomTrack = h.track;
                OnZoomSliderDrag(win, mx);
                win.bgraDirty = true;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (h.kind == HK_PREVIEW) {
                const auto& fx = s.core->Effects();
                const int ci = s.core->CurrentEffect();
                if (ci >= 0 && ci < static_cast<int>(fx.size()) &&
                    fx[static_cast<size_t>(ci)].stroke && s.core->HasImage()) {
                    float nx = 0, ny = 0;
                    if (CanvasPointToNorm(win, mx, my, nx, ny)) {
                        SetCapture(hwnd);
                        s.core->StrokeBegin(nx, ny, win.id);
                        win.dragStroke = true;
                        win.strokeLast = {mx, my};
                        win.bgraDirty = true;
                    }
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            WindowHit(win, h);
            return 0;
        }
        return 0;
    }

    case WM_MOUSEMOVE: {
        const int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);
        UiTrackMouse(win, hwnd, mx, my);
        if (win.dragZoom) {
            OnZoomSliderDrag(win, mx);
            win.bgraDirty = true;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (win.panning) {
            CanvasView v;
            if (CanvasViewOf(win, v)) {
                win.panX -= (mx - win.panLast.x) / v.zoom;
                win.panY -= (my - win.panLast.y) / v.zoom;
                win.panLast = {mx, my};
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        if (win.dragSlider >= 0) {
            OnSliderDrag(win, mx);
            win.bgraDirty = true;
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (win.dragStroke) {
            float nx = 0, ny = 0;
            if (CanvasPointToNorm(win, mx, my, nx, ny)) {
                s.core->StrokeMove(nx, ny, win.id);
                win.bgraDirty = true;
                win.strokeLast = {mx, my};
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;
    }

    case WM_LBUTTONUP:
        if (win.dragZoom) win.dragZoom = false;
        if (win.dragSlider >= 0) {
            win.dragSlider = -1;
            s.core->ParamChanged(win.id);
            win.bgraDirty = true;
        }
        if (win.dragStroke) {
            win.dragStroke = false;
            s.core->StrokeEnd(win.id);
        }
        ReleaseCapture();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_RBUTTONDOWN:
        SetCapture(hwnd);
        win.panning = true;
        win.panLast = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        return 0;

    case WM_RBUTTONUP:
        if (win.panning) {
            win.panning = false;
            ReleaseCapture();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_MOUSEWHEEL: {
        // 自由缩放：滚轮以光标为锚点连续缩放（与 PS/SAI 一致）
        const int delta = GET_WHEEL_DELTA_WPARAM(wp);
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &pt);
        if (InRect(win.canvas, pt.x, pt.y) && s.core->HasImage()) {
            CanvasView v;
            if (CanvasViewOf(win, v)) {
                const double cur = (win.zoom > 0.0) ? win.zoom : v.fit;
                const double factor = (delta > 0) ? 1.25 : 1.0 / 1.25;
                CanvasZoomAt(win, cur * factor, pt.x, pt.y);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        UiMouseLeave(win, hwnd);
        return 0;

    case WM_SIZE:
        win.bgraDirty = true;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_CLOSE:
        SaveWindowPlacement(hwnd, Specs()[win.id].cls);
        if (win.id == BridgeCore::kWinMain) {
            QuitApp();
        } else {
            CloseToolWindow(win.id);
        }
        return 0;

    default:
        break;
    }
    const ToolWindowSpec* sp = &Specs()[win.id];
    if (sp->msg && sp->msg(win, msg, wp, lp)) return 0;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

// ---------------------------------------------------------------------------
// 窗口类注册 / 创建
// ---------------------------------------------------------------------------
bool RegisterWindowClasses(HINSTANCE hInst) {
    // 主面板窗口类：用 BridgeApp.cpp 里的 MainWndProc（主面板自己的过程）。
    // 这是注册主面板类的**唯一**地方 —— 千万不能在别处再注册一次同名类，
    // 第二次注册必然 ERROR_CLASS_ALREADY_EXISTS，而窗口会挂上前一次注册的
    // 过程，表现为"主面板整块空白"（这个 bug 已经踩过一次）。
    {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = MainWndProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = MainWindowClassName();
        if (!RegisterClassExW(&wc) &&
            GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return false;
    }

    const ToolWindowSpec* specs = Specs();
    for (int i = 1; i < BridgeCore::kWinCount; ++i) {
        const ToolWindowSpec& sp = specs[i];
        if (!sp.cls || !sp.cls[0]) continue;
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = ToolWndProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = sp.cls;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return false;
    }
    return true;
}

void OpenToolWindow(int winIdx) {
    const int idx = (winIdx >= 1 && winIdx < BridgeCore::kWinCount) ? winIdx
                                                                   : 0;
    if (idx == 0) return;
    AppState& s = S();
    UiWindow& win = s.toolWindows[idx];
    if (win.hwnd && IsWindow(win.hwnd)) {
        ShowWindow(win.hwnd, SW_SHOW);
        SetForegroundWindow(win.hwnd);
        InvalidateRect(win.hwnd, nullptr, FALSE);
        return;
    }
    const ToolWindowSpec& sp = Specs()[idx];
    if (!s.core->HasWindow(idx)) s.core->OpenWindow(idx);
    win.id = idx;
    win.effectIndex = s.core->CurrentEffect();

    RECT rc = {0, 0, Scale(sp.w), Scale(sp.h)};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND h = CreateWindowExW(
        0, sp.cls, sp.title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top, s.mainHwnd, nullptr,
        reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(s.mainHwnd,
                                                      GWLP_HINSTANCE)),
        &win);
    if (!h) return;
    win.hwnd = h;
    EnableRoundedWindow(h); // Win11 圆角窗口（旧系统静默跳过）
    RestoreWindowPlacement(h, CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left,
                           rc.bottom - rc.top);
    ShowWindow(h, SW_SHOW);
    UpdateWindow(h);
    // 抓图流程会把前台切到 SAI2 / 主面板；新窗口必须抢回前台，
    // 否则用户点"液化"后看到的还是主面板（窗口其实已经开在后面了）
    SetForegroundWindow(h);
    s.core->Log(std::string("tool window created: ") +
                std::string(1, static_cast<char>('0' + idx)));
}

void CloseToolWindow(int winIdx) {
    if (winIdx < 1 || winIdx >= BridgeCore::kWinCount) return;
    AppState& s = S();
    UiWindow& win = s.toolWindows[winIdx];
    if (win.hwnd && IsWindow(win.hwnd)) {
        DestroyWindow(win.hwnd);
    }
    win.hwnd = nullptr;
    win.dragStroke = false;
    win.dragSlider = -1;
    win.panning = false;
    win.zoom = 0.0; // 重开窗口时回到适配显示
    win.panX = 0;
    win.panY = 0;
    if (s.core) s.core->CloseWindow(winIdx);
}

void CloseAllToolWindows() {
    for (int i = 1; i < BridgeCore::kWinCount; ++i) CloseToolWindow(i);
}

bool ToolWindowOpen(int winIdx) {
    if (winIdx < 1 || winIdx >= BridgeCore::kWinCount) return false;
    HWND h = S().toolWindows[winIdx].hwnd;
    return h && IsWindow(h) && IsWindowVisible(h);
}

void InvalidateAll() {
    AppState& s = S();
    if (s.mainHwnd) InvalidateRect(s.mainHwnd, nullptr, FALSE);
    for (int i = 1; i < BridgeCore::kWinCount; ++i) {
        UiWindow& w = s.toolWindows[i];
        w.bgraDirty = true;
        if (w.hwnd && IsWindow(w.hwnd)) InvalidateRect(w.hwnd, nullptr, FALSE);
    }
}

void QuitApp() {
    PostQuitMessage(0);
}

// ---------------------------------------------------------------------------
// 命令按钮
// ---------------------------------------------------------------------------
void RunCmd(UiWindow& win, int cmd) {
    AppState& s = S();
    switch (cmd) {
    case CMD_APPLY:
        DoApply(win);
        break;
    case CMD_UNDO:
        DoUndo(win);
        break;
    case CMD_RESET:
        DoReset(win);
        break;
    case CMD_CLOSE:
        if (win.id == BridgeCore::kWinMain)
            QuitApp();
        else
            CloseToolWindow(win.id);
        break;
    case CMD_CHAIN:
        s.core->SetChain(!s.core->Chain());
        for (int i = 1; i < BridgeCore::kWinCount; ++i)
            s.toolWindows[i].bgraDirty = true;
        break;
    case CMD_AUTOPASTE:
        s.autoPaste = !s.autoPaste;
        break;
    case CMD_GRAB:
        RequestToolWindow(win.hwnd, win.id, true);
        break;
    case CMD_THEME:
        CycleTheme(win.hwnd ? win.hwnd : S().mainHwnd);
        break;
    default:
        break;
    }
    InvalidateAll();
}

} // namespace ui
} // namespace bridge
} // namespace sai2
