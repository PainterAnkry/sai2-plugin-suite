// ============================================================================
// AgentToolbar.cpp - SAI2 内浮动「插件」入口（分功能按钮条）
//
// 以前这里是一个「插件 ▾」按钮 + 下拉菜单（所有效果挤在一个菜单里）；
// 现在改成**一排功能按钮**：模糊 | 滤镜 | 调整 | 液化。
// 点哪个按钮就把"打开哪个功能窗口"的请求记下来，桥（sai2_bridge）轮询取走后
// 自动选中并抓取 SAI2 当前图层，然后弹出该功能的**独立窗口**。
//
// 按钮清单由桥通过 set.plugins 推送（见 AgentBridge.h::PushCategories）。
// ============================================================================

#include "AgentCore.h"

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace sai2agent {

namespace {

EXTERN_C IMAGE_DOS_HEADER __ImageBase;

// 类名按次加载唯一化：DLL 卸载不清理已注册的窗口类，
// 固定类名会让第二次注入的 RegisterClassExW 失败
wchar_t g_barClassName[64] = L"SAI2PluginBar";
const wchar_t* kBarTitle = L"SAI2 插件入口";

AgentCore* g_barCore = nullptr;   // 主体
HWND g_barWnd = nullptr;          // 浮窗
HWND g_saiMain = nullptr;         // SAI2 主窗口
HANDLE g_toolbarThread = nullptr; // 线程句柄（卸载前要等待）
UINT g_barTimerId = 1;
int g_hover = -1;                 // 悬停的按钮序号

// 按钮条宽度（按功能数自适应）
const int kBtnH = 24;
const int kBtnMinW = 62;

// ------------------------------------------------------------ 找 SAI2 主窗
struct MainWndCtx {
    HWND found = nullptr;
    long area = 0;
};

BOOL CALLBACK FindMainProc(HWND h, LPARAM lp) {
    MainWndCtx* c = reinterpret_cast<MainWndCtx*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    if (!IsWindowVisible(h)) return TRUE;
    if (GetWindow(h, GW_OWNER) != nullptr) return TRUE;
    if (GetWindowTextLengthW(h) == 0) return TRUE;
    wchar_t cls[64] = {0};
    GetClassNameW(h, cls, 64);
    if (wcsstr(cls, L"SAI2PluginBar") != nullptr) return TRUE;
    RECT r;
    if (!GetWindowRect(h, &r)) return TRUE;
    const long area = (r.right - r.left) * (r.bottom - r.top);
    if (area > c->area) {
        c->area = area;
        c->found = h;
    }
    return FALSE;
}

int ButtonCount() {
    if (!g_barCore) return 0;
    return static_cast<int>(g_barCore->CopyPluginItems().size());
}

void ButtonRect(int i, int count, RECT& out) {
    RECT rc;
    GetClientRect(g_barWnd, &rc);
    const int w = (rc.right - rc.left) / std::max(1, count);
    out = {i * w, 0, (i + 1) * w, rc.bottom};
}

int HitButton(int x, int y) {
    const int n = ButtonCount();
    if (n <= 0) return -1;
    RECT rc;
    GetClientRect(g_barWnd, &rc);
    if (x < 0 || y < 0 || x >= rc.right || y >= rc.bottom) return -1;
    const int w = rc.right / std::max(1, n);
    if (w <= 0) return -1;
    const int i = x / w;
    return (i >= 0 && i < n) ? i : -1;
}

void SyncBarPosition() {
    if (!g_barWnd) return;
    if (!g_saiMain || !IsWindow(g_saiMain)) {
        MainWndCtx ctx;
        EnumWindows(FindMainProc, reinterpret_cast<LPARAM>(&ctx));
        g_saiMain = ctx.found;
        if (!g_saiMain) {
            ShowWindow(g_barWnd, SW_HIDE);
            return;
        }
    }
    if (IsIconic(g_saiMain)) {
        ShowWindow(g_barWnd, SW_HIDE);
        return;
    }

    RECT r;
    if (!GetWindowRect(g_saiMain, &r)) return;
    const int n = std::max(1, ButtonCount());
    const int bw = std::max(kBtnMinW * n, 4 * kBtnMinW);
    const int bh = kBtnH;
    const int x = r.right - bw - 24;
    const int y = r.top + 62;
    ShowWindow(g_barWnd, SW_SHOWNOACTIVATE);
    SetWindowPos(g_barWnd, HWND_TOPMOST, x, y, bw, bh, SWP_NOACTIVATE);
}

// ------------------------------------------------------------ 主题（与桥一致）
// 读桥写入的设置（HKCU\Software\SAI2Bridge\Theme）：0=跟随系统 1=浅色 2=深色
bool BarDark() {
    DWORD v = 0, size = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\SAI2Bridge", L"Theme",
                 RRF_RT_REG_DWORD, nullptr, &v, &size);
    if (v == 1) return false;
    if (v == 2) return true;
    DWORD light = 1;
    size = sizeof(light);
    RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    return light == 0;
}

// Win11 圆角窗口（失败静默）
void BarRounded(HWND hwnd) {
    using Fn = LONG(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    HMODULE m = LoadLibraryW(L"dwmapi.dll");
    if (!m) return;
    auto fn = reinterpret_cast<Fn>(GetProcAddress(m, "DwmSetWindowAttribute"));
    if (fn) {
        DWORD pref = 2; // DWMWCP_ROUND
        fn(hwnd, 33, &pref, sizeof(pref));
    }
    FreeLibrary(m);
}

// ------------------------------------------------------------ 窗口过程
void KickBridge(int winIdx) {
    if (!g_barCore) return;
    {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "bar: request window %d", winIdx);
        g_barCore->LogText(buf);
    }
    // 若桥还没把清单推过来（按钮条是空的），这里先提示
    g_barCore->SetPendingRequest(winIdx);
}

LRESULT CALLBACK BarProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        const bool dark = BarDark();
        const COLORREF bg = dark ? RGB(32, 33, 36) : RGB(250, 250, 252);
        const COLORREF btn = dark ? RGB(50, 52, 56) : RGB(233, 235, 239);
        const COLORREF border = dark ? RGB(62, 65, 68) : RGB(214, 217, 223);
        const COLORREF txt = dark ? RGB(235, 235, 235) : RGB(30, 32, 38);
        const COLORREF txtDim = dark ? RGB(170, 172, 176) : RGB(112, 117, 126);
        const COLORREF accent = dark ? RGB(10, 132, 255) : RGB(0, 116, 242);
        HBRUSH bgb = CreateSolidBrush(bg);
        FillRect(dc, &rc, bgb);
        DeleteObject(bgb);

        std::vector<AgentCore::PluginItem> items;
        if (g_barCore) items = g_barCore->CopyPluginItems();
        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));

        if (items.empty()) {
            SetTextColor(dc, txtDim);
            RECT t = rc;
            t.left += 6;
            DrawTextW(dc, L"插件（等待主程序）", -1, &t,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            EndPaint(hwnd, &ps);
            return 0;
        }

        const int n = static_cast<int>(items.size());
        for (int i = 0; i < n; ++i) {
            RECT br;
            ButtonRect(i, n, br);
            // 内缩 2px 画圆角按钮，底色形成"圆角间隙"
            RECT ib = {br.left + 2, br.top + 2, br.right - 2, br.bottom - 2};
            const bool hover = (g_hover == i);
            HBRUSH bb = CreateSolidBrush(hover ? accent : btn);
            HGDIOBJ ob = SelectObject(dc, bb);
            HPEN pp = CreatePen(PS_SOLID, 1, hover ? accent : border);
            HGDIOBJ op = SelectObject(dc, pp);
            RoundRect(dc, ib.left, ib.top, ib.right, ib.bottom, 10, 10);
            SelectObject(dc, op);
            SelectObject(dc, ob);
            DeleteObject(pp);
            DeleteObject(bb);
            SetTextColor(dc, hover ? RGB(255, 255, 255) : txt);
            RECT t = ib;
            t.left += 4;
            t.right -= 4;
            DrawTextW(dc, items[static_cast<size_t>(i)].name.c_str(), -1, &t,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        const int idx = HitButton(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (idx != g_hover) {
            g_hover = idx;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_hover != -1) {
            g_hover = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP: {
        const int idx = HitButton(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (idx < 0 || !g_barCore) return 0;
        const std::vector<AgentCore::PluginItem> items =
            g_barCore->CopyPluginItems();
        if (idx >= static_cast<int>(items.size())) return 0;
        // index 0..n-1 → 功能窗口索引（桥推送时按 1..4 编号；这里兼容 +1）
        int winIdx = items[static_cast<size_t>(idx)].index;
        if (winIdx <= 0) winIdx = idx + 1;
        KickBridge(winIdx);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_TIMER:
        if (wp == g_barTimerId) {
            SyncBarPosition();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, g_barTimerId);
        g_barWnd = nullptr;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

DWORD WINAPI ToolbarThreadProc(LPVOID) {
    if (!g_barCore) return 1;
    g_barCore->LogText("toolbar thread enter");
    swprintf(g_barClassName, 64, L"SAI2PluginBar_%lx",
             static_cast<unsigned long>(GetTickCount()));
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = BarProc;
    wc.hInstance = reinterpret_cast<HINSTANCE>(&__ImageBase);
    wc.hCursor = LoadCursor(nullptr, IDC_HAND);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = g_barClassName;
    if (!RegisterClassExW(&wc)) {
        g_barCore->LogText("toolbar: RegisterClassExW FAILED");
        return 1;
    }

    g_barWnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW, g_barClassName, kBarTitle,
        WS_POPUP, 0, 0, 4 * kBtnMinW, kBtnH, nullptr, nullptr,
        reinterpret_cast<HINSTANCE>(&__ImageBase), nullptr);
    if (!g_barWnd) {
        g_barCore->LogText("toolbar: CreateWindowExW FAILED");
        return 1;
    }
    BarRounded(g_barWnd); // Win11 圆角
    g_barCore->LogText("toolbar: window created");

    SyncBarPosition();
    SetTimer(g_barWnd, g_barTimerId, 250, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        if (g_barCore && g_barCore->ExitRequested()) break;
    }
    if (g_barWnd) {
        DestroyWindow(g_barWnd);
        g_barWnd = nullptr;
    }
    return 0;
}

} // namespace

void AgentCore::StartToolbar() {
    g_barCore = this;
    // 环境变量 SAI2_AGENT_NOBAR=1 可关闭浮窗（自检用）
    wchar_t buf[8] = {0};
    if (GetEnvironmentVariableW(L"SAI2_AGENT_NOBAR", buf, 8) > 0 && buf[0] == L'1')
        return;
    HANDLE h = CreateThread(nullptr, 0, ToolbarThreadProc, nullptr, 0, nullptr);
    if (h) g_toolbarThread = h; // 句柄由 DllMain DETACH 等待，不在此关闭
}

void AgentCore::StopToolbarAndWait() {
    // 卸载前必须让工具栏线程退出：否则 FreeLibrary 卸载映像时线程
    // 可能正在执行 DLL 代码 → 访问违规 → 宿主进程整个崩掉
    if (!g_toolbarThread) return;
    const DWORD t0 = GetTickCount();
    while (WaitForSingleObject(g_toolbarThread, 50) == WAIT_TIMEOUT) {
        if (GetTickCount() - t0 > 4000) break;
    }
    CloseHandle(g_toolbarThread);
    g_toolbarThread = nullptr;
}

} // namespace sai2agent
