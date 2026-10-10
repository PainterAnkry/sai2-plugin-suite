// ============================================================================
// FakeHost.cpp - sai2_fakehost.exe：假 SAI2 宿主（注入链路端到端自检用）
//
// 模拟 SAI2 的绘制方式：
//   * 主窗口用 DrawTextW 画一些 UI 标签
//   * 右侧"图层面板"子窗口用 ExtTextOutW 画图层名行
//     用 SetDIBitsToDevice 画 28x28 缩略图色块
// 这样 sai2_agent 的四个 IAT 钩子全部会被真实调用，自检就能验证
// "文字捕获 → 行重建 → 缩略图落盘 → 动态更新"整条链路，而不用碰真 SAI2。
//
// 控制消息（自检驱动用）：
//   WM_USER+2  重命名第 0 行 → "描线" 并重绘
//   WM_USER+3  删除第 1 行并重绘
//   WM_USER+9  退出
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <windows.h>

#include <string>
#include <vector>

namespace {

const wchar_t* kMainClass = L"FakeSai2Main";
const wchar_t* kPanelClass = L"FakeSai2Panel";

struct State {
    std::vector<std::wstring> layers;
    HWND panel = nullptr;
} g;

// 28x28 BGRA 色块（每行颜色不同，验证像素方向没写错）
struct ThumbDib {
    BITMAPINFO bi = {};
    std::vector<uint32_t> px;
} g_thumb;

void BuildThumb(int seed) {
    g_thumb.bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    g_thumb.bi.bmiHeader.biWidth = 28;
    g_thumb.bi.bmiHeader.biHeight = 28; // bottom-up
    g_thumb.bi.bmiHeader.biPlanes = 1;
    g_thumb.bi.bmiHeader.biBitCount = 32;
    g_thumb.bi.bmiHeader.biCompression = BI_RGB;
    g_thumb.px.resize(28 * 28);
    for (int y = 0; y < 28; ++y) {
        for (int x = 0; x < 28; ++x) {
            const uint8_t b = static_cast<uint8_t>((x * 8 + seed * 60) & 0xFF);
            const uint8_t gch = static_cast<uint8_t>((y * 8 + seed * 40) & 0xFF);
            const uint8_t r = static_cast<uint8_t>(((x ^ y) * 5 + seed * 20) & 0xFF);
            g_thumb.px[static_cast<size_t>(y) * 28 + x] =
                static_cast<uint32_t>(b) | (static_cast<uint32_t>(gch) << 8) |
                (static_cast<uint32_t>(r) << 16) | 0xFF000000u;
        }
    }
}

void PaintPanel(HDC dc, const RECT& client) {
    FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));
    SetBkMode(dc, TRANSPARENT);
    for (size_t i = 0; i < g.layers.size(); ++i) {
        const int rowY = static_cast<int>(i) * 36;
        // 缩略图（与 SAI2 一样是小尺寸位块传输）
        SetDIBitsToDevice(dc, 8, rowY + 4, 28, 28, 0, 0, 0, 28,
                          g_thumb.px.data(), &g_thumb.bi, DIB_RGB_COLORS);
        // 图层名
        SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
        SetTextColor(dc, RGB(20, 20, 20));
        ExtTextOutW(dc, 60, rowY + 10, 0, nullptr, g.layers[i].c_str(),
                    static_cast<int>(g.layers[i].size()), nullptr);
    }
}

LRESULT CALLBACK PanelProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        PaintPanel(dc, rc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        g.panel = CreateWindowExW(WS_EX_CLIENTEDGE, kPanelClass, nullptr,
                                  WS_CHILD | WS_VISIBLE, rc.right - 220, 8,
                                  210, 400, hwnd, nullptr,
                                  reinterpret_cast<HINSTANCE>(
                                      GetWindowLongPtrW(hwnd, GWLP_HINSTANCE)),
                                  nullptr);
        return 0;
    }
    case WM_TIMER:
        // autotest 模式下周期重绘：真实面板会随操作不断重绘，
        // 且注入发生在窗口创建之后，必须有重绘才能被钩子捕获
        if (wp == 1 && g.panel) InvalidateRect(g.panel, nullptr, TRUE);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(dc, &rc, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
        RECT label = {10, 10, 300, 34};
        DrawTextW(dc, const_cast<LPWSTR>(L"FakeHost 工具栏"), -1, &label,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_USER + 2: // 重命名第 0 行 → 描线
        if (!g.layers.empty()) g.layers[0] = L"描线";
        InvalidateRect(g.panel, nullptr, TRUE);
        return 0;
    case WM_USER + 3: // 删除第 1 行
        if (g.layers.size() > 1) g.layers.erase(g.layers.begin() + 1);
        InvalidateRect(g.panel, nullptr, TRUE);
        return 0;
    case WM_USER + 9:
        PostQuitMessage(0);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR cmdLine, int) {
    const bool autotest = cmdLine && std::wstring(cmdLine).find(L"--autotest") !=
                                              std::wstring::npos;

    g.layers = {L"图层 1", L"图层 2", L"图层 3", L"图层 4"};
    BuildThumb(1);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MainProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kMainClass;
    RegisterClassExW(&wc);

    WNDCLASSEXW pc = {};
    pc.cbSize = sizeof(pc);
    pc.lpfnWndProc = PanelProc;
    pc.hInstance = hInst;
    pc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    pc.lpszClassName = kPanelClass;
    RegisterClassExW(&pc);

    HWND hwnd = CreateWindowExW(0, kMainClass,
                                autotest ? L"FakeHost (autotest)" : L"FakeHost",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                CW_USEDEFAULT, 820, 600, nullptr, nullptr,
                                hInst, nullptr);
    if (!hwnd) return 1;
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    if (autotest) SetTimer(hwnd, 1, 700, nullptr); // 周期重绘面板

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
