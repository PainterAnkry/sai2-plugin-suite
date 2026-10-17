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

// 输入驱动的"重渲染"触发消息：鼠标拖动（涂抹/滑杆）时输入远密于渲染速度，
// 所以消息处理里只更新最新状态，整图渲染由这一条**投递消息**驱动。
// 不能用 WM_TIMER/WM_PAINT：它们都是"队列空闲时才生成"的低优先级消息，输入一
// 密集就被饿死 —— 用户报的"涂抹时画面不动、切换工具才看到变化"正是这个
// （渲染堵住输入队列 → 画面不刷新 → 松手/切换后才补画出来）。
const UINT kRenderMsg = WM_APP + 40;

// 渲染完立刻把这一帧贴到屏幕上（只 Invalidate 会排队等系统施舍 WM_PAINT）
void RepaintNow(HWND hwnd) {
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
}

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

// 功能窗口副标题的前半段：抓取尺寸 → 画布尺寸。
// 为什么要并排显示这两个数：用户看到"已抓取图层 971 x 593"会以为抓错了尺寸，
// 其实 **SAI2 的 Ctrl+C 只给图层不透明部分的包围盒**，补边到画布尺寸发生在
// "应用"那一步。把两个尺寸摆在一起，并在没拿到画布尺寸时直接预警，
// 就不会再出现"画布大小和截取大小不一致 → 以为是 bug"的误解。
// （画布尺寸 = 精确回贴的补边目标；拿不到就必然错位，故用显式警告。）
void GrabSubtitleHead(wchar_t* out, size_t n, const ImageBuffer& img) {
    AppState& s = S();
    const int w = img.IsValid() ? img.Width() : 0;
    const int h = img.IsValid() ? img.Height() : 0;
    const wchar_t* state = s.core->HasImage() ? L"已抓取图层" : L"等待抓取";
    if (s.core->HasCanvasContext()) {
        swprintf(out, n, L"%s   %d x %d  →  画布 %d x %d（整幅画布）", state,
                 w, h, s.core->CanvasW(), s.core->CanvasH());
    } else {
        swprintf(out, n, L"%s   %d x %d  ⚠ 无画布尺寸：粘贴会错位", state, w, h);
    }
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

    // 拖动过程中的显示帧：渲染一次整图并立刻贴屏（见 kRenderMsg 注释）
    case kRenderMsg: {
        win.renderPosted = false;
        bool rendered = false;
        if (win.dragStroke && win.strokeDirty) {
            win.strokeDirty = false;
            s.core->StrokeRender(win.id);
            win.bgraDirty = true;
            rendered = true;
        } else if (win.dragSlider >= 0 && win.sliderPending) {
            win.sliderPending = false;
            OnSliderDrag(win, win.sliderPendX);
            win.bgraDirty = true;
            rendered = true;
        }
        if (rendered) RepaintNow(hwnd);
        return 0;
    }

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
                // 抓手模式（抓手工具 / 空格按住）：左键拖动 = 平移，不涂抹。
                const bool spacePan = (GetKeyState(VK_SPACE) & 0x8000) != 0;
                if ((win.handTool || spacePan) && s.core->HasImage()) {
                    SetCapture(hwnd);
                    win.panning = true;
                    win.panLast = {mx, my};
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
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
                        win.strokeDirty = false;
                        // 拖动期间的整图渲染由 kRenderMsg 驱动（见上）
                        if (!win.renderPosted) {
                            win.renderPosted = true;
                            PostMessageW(hwnd, kRenderMsg, 0, 0);
                        }
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
            // 只记最新位置；渲染交给 kRenderMsg（大图一次渲染几十毫秒，
            // 在消息处理里同步渲染会把输入队列堵死、画面反而不刷新）
            win.sliderPendX = mx;
            win.sliderPending = true;
            if (!win.renderPosted) {
                win.renderPosted = true;
                PostMessageW(hwnd, kRenderMsg, 0, 0);
            }
        } else if (win.dragStroke) {
            float nx = 0, ny = 0;
            if (CanvasPointToNorm(win, mx, my, nx, ny)) {
                // 每个采样点都累积进置换场（这条很便宜）；整图渲染由 kRenderMsg 驱动
                s.core->StrokeMove(nx, ny, win.id);
                win.strokeLast = {mx, my};
                win.strokeDirty = true;
                if (!win.renderPosted) {
                    win.renderPosted = true;
                    PostMessageW(hwnd, kRenderMsg, 0, 0);
                }
            }
        }
        return 0;
    }

    case WM_LBUTTONUP:
        if (win.dragZoom) win.dragZoom = false;
        if (win.dragSlider >= 0) {
            win.dragSlider = -1;
            win.sliderPending = false;
            s.core->ParamChanged(win.id);
            win.bgraDirty = true;
        }
        if (win.dragStroke) {
            win.dragStroke = false;
            win.strokeDirty = false;
            win.bgraDirty = true;
            s.core->StrokeEnd(win.id);
            // 收笔：按最终置换场渲染一帧（最后一个采样点已经累积过了）
            s.core->StrokeRender(win.id);
        }
        ReleaseCapture();
        RepaintNow(hwnd);
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

    case WM_MBUTTONDOWN:
        // 中键拖动 = 平移画布（PS/SAI 习惯），所有功能窗口生效
        SetCapture(hwnd);
        win.panning = true;
        win.panLast = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        return 0;

    case WM_MBUTTONUP:
        if (win.panning) {
            win.panning = false;
            ReleaseCapture();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_SETCURSOR:
        // 抓手/平移中 → 移动光标；液化涂抹工具 → 十字；其余箭头
        if (LOWORD(lp) == HTCLIENT) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            const bool pan = win.panning || win.handTool ||
                             (GetKeyState(VK_SPACE) & 0x8000) != 0;
            if (pan) {
                SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
                return 0;
            }
            if (win.id == BridgeCore::kWinLiquify &&
                InRect(win.canvas, pt.x, pt.y) && s.core->HasImage()) {
                const auto& fxs = s.core->Effects();
                const int ci = WindowEffectIndex(win);
                if (ci >= 0 && ci < static_cast<int>(fxs.size()) &&
                    fxs[static_cast<size_t>(ci)].stroke) {
                    SetCursor(LoadCursor(nullptr, IDC_CROSS));
                    return 0;
                }
            }
            SetCursor(LoadCursor(nullptr, IDC_ARROW));
            return 0;
        }
        break;

    case WM_MOUSEWHEEL: {
        // 自由缩放：滚轮以光标为锚点连续缩放（与 PS/SAI 一致）
        // ★ 先给"窗口自己"处理的机会：滤镜库的左栏列表滚动走这里。
        //   原来本分支处理完无条件 return，窗口的 Msg 收不到滚轮 →
        //   滤镜库左边栏滚不动（用户反馈"左边栏不能下拉"）。
        if (win.id == BridgeCore::kWinFilter && FilterWheelScroll(win, wp, lp))
            return 0;
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

// 功能窗口副标题的前半段：抓取尺寸 → 画布尺寸。
// 为什么要并排显示这两个数：用户看到"已抓取图层 971 x 593"会以为抓错了尺寸，
// 其实 **SAI2 的 Ctrl+C 只给图层不透明部分的包围盒**，补边到画布尺寸发生在
// "应用"那一步。把两个尺寸摆在一起，并在没拿到画布尺寸时直接预警，
// 就不会再出现"画布大小和截取大小不一致 → 以为是 bug"的误解。
// （画布尺寸 = 精确回贴的补边目标；拿不到就必然错位，故用显式警告。）
// 注意：必须定义在匿名 namespace **之外**，否则只有内部链接，四类窗口链接不到。
void GrabSubtitleHead(wchar_t* out, size_t n, const ImageBuffer& img) {
    AppState& s = S();
    const int w = img.IsValid() ? img.Width() : 0;
    const int h = img.IsValid() ? img.Height() : 0;
    const wchar_t* state = s.core->HasImage() ? L"已抓取图层" : L"等待抓取";
    if (s.core->HasCanvasContext()) {
        swprintf(out, n, L"%s   %d x %d  →  画布 %d x %d（整幅画布）", state,
                 w, h, s.core->CanvasW(), s.core->CanvasH());
    } else {
        swprintf(out, n, L"%s   %d x %d  ⚠ 无画布尺寸：粘贴会错位", state, w, h);
    }
}

// ---------------------------------------------------------------------------
// 窗口类注册 / 创建
// ---------------------------------------------------------------------------
// 应用图标：sai2_bridge.exe 的资源 IDI 固定 101（resources/sai2_suite.ico，
// 见 scripts/build-zig.sh 的 make_res 第 6 参 + resources/version.rc.in）。
// 为什么必须显式设置：面板和 4 个功能窗口此前**没有图标**（也没有资源），
// Windows 只好用"未知应用"的默认图标 —— 任务栏里那一格看着比旁边的图标
// 偏低/不齐（用户反馈"程序图标莫名其妙下移"），标题栏图标也是通用图标。
static HICON AppIconLarge(HINSTANCE hInst) {
    HICON h = static_cast<HICON>(LoadImageW(hInst, MAKEINTRESOURCEW(101),
                                           IMAGE_ICON, 0, 0,
                                           LR_DEFAULTSIZE | LR_SHARED));
    if (!h) h = LoadIconW(hInst, MAKEINTRESOURCEW(101));
    return h;
}
static HICON AppIconSmall(HINSTANCE hInst) {
    return static_cast<HICON>(LoadImageW(hInst, MAKEINTRESOURCEW(101),
                                         IMAGE_ICON,
                                         GetSystemMetrics(SM_CXSMICON),
                                         GetSystemMetrics(SM_CYSMICON),
                                         LR_SHARED));
}

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
        wc.hIcon = AppIconLarge(hInst);
        wc.hIconSm = AppIconSmall(hInst);
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
        wc.hIcon = AppIconLarge(hInst);
        wc.hIconSm = AppIconSmall(hInst);
        wc.lpszClassName = sp.cls;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return false;
    }
    return true;
}

// 把窗口可靠地切到前台。桥此时通常是后台进程（用户在 SAI2 里点的浮窗），
// 直接 SetForegroundWindow 会被 Windows 前台锁拒绝——窗口只闪任务栏，
// 用户必须手动点任务栏才能把它"点出来"（1.15 用户反馈的现象）。
// 三级策略：AttachThreadInput 借前台线程权限 → Alt 键解锁 → 闪任务栏兜底。
static void RaiseToForeground(HWND h) {
    if (!h || !IsWindow(h)) return;
    HWND fore = GetForegroundWindow();
    if (fore == h) return;
    const DWORD foreThread =
        fore ? GetWindowThreadProcessId(fore, nullptr) : 0;
    const DWORD ourThread = GetWindowThreadProcessId(h, nullptr);
    if (foreThread && foreThread != ourThread)
        AttachThreadInput(foreThread, ourThread, TRUE);
    BringWindowToTop(h);
    SetForegroundWindow(h);
    SetFocus(h);
    if (foreThread && foreThread != ourThread)
        AttachThreadInput(foreThread, ourThread, FALSE);
    if (GetForegroundWindow() != h) {
        // 按一次 Alt 可解除调用进程的前台限制（系统级规则）
        keybd_event(VK_MENU, 0, 0, 0);
        SetForegroundWindow(h);
        keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
    }
    if (GetForegroundWindow() != h) {
        FlashWindow(h, TRUE); // 仍被拒：至少让任务栏图标亮起来提示用户
        SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        SetWindowPos(h, HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

void OpenToolWindow(int winIdx) {
    const int idx = (winIdx >= 1 && winIdx < BridgeCore::kWinCount) ? winIdx
                                                                   : 0;
    if (idx == 0) return;
    AppState& s = S();
    UiWindow& win = s.toolWindows[idx];
    if (win.hwnd && IsWindow(win.hwnd)) {
        // 窗口已存在（第二次及以后的浮窗点击）：
        // 最小化过要先还原，再抢前台——SW_SHOW + 裸 SetForegroundWindow
        // 在桥为后台进程时大概率被前台锁拒绝，只闪任务栏不弹出。
        ShowWindow(win.hwnd, IsIconic(win.hwnd) ? SW_RESTORE : SW_SHOW);
        RaiseToForeground(win.hwnd);
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
    // 每次拿到新图都回到"适配"（整幅可见）。
    // 为什么：窗口一直被复用时 zoom/pan 会留着上一张图的取值 —— 用户改过画布
    // 大小（或换了图层）之后，旧 zoom 在新图上只显示一部分，看起来就像"这几个
    // 窗口没跟着改"。实测用户反馈"滤镜正常、模糊/液化还是显示不全"就是这个：
    // 滤镜窗口被关掉重开过（关窗时本来就会复位），其它窗口一直开着。
    win.zoom = 0.0;
    win.panX = 0.0;
    win.panY = 0.0;
    EnableRoundedWindow(h); // Win11 圆角窗口（旧系统静默跳过）
    RestoreWindowPlacement(h, CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left,
                           rc.bottom - rc.top);
    ShowWindow(h, SW_SHOW);
    UpdateWindow(h);
    // 抓图流程会把前台切到 SAI2 / 主面板；新窗口必须抢回前台，
    // 否则用户点"液化"后看到的还是主面板（窗口其实已经开在后面了）
    RaiseToForeground(h);
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
        // 液化窗口：撤销上一笔；其他窗口：撤销上一次"应用"
        if (!S().core->UndoStroke(win.id)) DoUndo(win);
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
        // 主面板「抓取 SAI2 图层」：整层抓取（此前误走 RequestToolWindow，
        // winIdx=0 直接 return，按钮等于没功能）
        DoGrabWholeLayer(win.hwnd);
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
