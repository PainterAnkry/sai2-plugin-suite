// 功能演示编排：详见 Demo.h。
// 截帧方式：脚本线程直接 GetDC(NULL)+BitBlt 从屏幕抓"当前演示窗口"的客户区
// （窗口均为 TOPMOST 且演示中互相遮挡的窗口会被隐藏，因此屏幕抓取内容正确；
// 不走 PrintWindow——它会向窗口线程发消息，与脚本线程触发的渲染互等会死锁；
// WM_TIMER 又会被界面重绘饿到 2fps）。每帧等比合成到 900x900 画布导出 BMP。
// 时间线：0-2.5 主面板 → 2.5-7 模糊调参 → 7 应用 → 7-9 主面板 →
// 9-16 液化涂抹（向前变形拖拽 + 膨胀 + 旋转）→ 16 应用 → 16-17.5 主面板 →
// 17.5-21 滤镜库（水彩/素描切换）→ 21-25 调整（反相/去色/亮度/色相）→
// 25-26 主面板 → 26-27 深色主题 → 27-28.5 液化窗口（深色）→ 退出。
#include "Demo.h"
#include "Ui.h"
#include "BmpIO.h"

#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace sai2::bridge;     // WriteBmp32 / ImageBuffer
using namespace sai2::bridge::ui; // AppState / S() / OpenToolWindow ...

namespace {

int g_frameNo = 0;
std::string g_outDir;
HWND g_capHwnd = nullptr; // 当前演示窗口（脚本线程内使用）

const int kCanvas = 900;      // 输出帧边长（正方形）
const int kMargin = 8;        // 内容与画布边缘最小留白
const double kFrameDt = 0.15; // 每帧 150ms ≈ 6.7fps

// 把屏幕上 hwnd 客户区抓下来，等比缩放 + 居中，写一帧 BMP
void CaptureNow() {
    if (!g_capHwnd) return;
    RECT rc;
    if (!GetClientRect(g_capHwnd, &rc)) return;
    POINT org = {rc.left, rc.top};
    ClientToScreen(g_capHwnd, &org);
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0 || w > 4000 || h > 4000) return;

    std::vector<uint8_t> src(static_cast<size_t>(w) * h * 4, 0);
    HDC screen = GetDC(nullptr);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, nullptr,
                                   nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, screen, org.x, org.y, SRCCOPY | CAPTUREBLT);
    GetDIBits(mem, bmp, 0, static_cast<UINT>(h), src.data(), &bi,
              DIB_RGB_COLORS);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);

    // BGRA → RGBA8 画布，等比缩放（最近邻）+ 居中留白
    sai2::ImageBuffer img(kCanvas, kCanvas, sai2::PixelFormat::RGBA8);
    if (!img.IsValid()) return;
    uint8_t* dst = img.Data();
    for (size_t i = 0; i < static_cast<size_t>(kCanvas) * kCanvas * 4;
         i += 4) {
        dst[i + 0] = 247;
        dst[i + 1] = 246;
        dst[i + 2] = 246;
        dst[i + 3] = 255;
    }
    const double s = std::min((kCanvas - 2 * kMargin) / (double)w,
                              (kCanvas - 2 * kMargin) / (double)h);
    const int dw = static_cast<int>(w * s), dh = static_cast<int>(h * s);
    const int x0 = (kCanvas - dw) / 2, y0 = (kCanvas - dh) / 2;
    for (int y = 0; y < dh; ++y) {
        const int sy = std::min(h - 1, static_cast<int>((y + 0.5) / s));
        const uint8_t* sr = &src[static_cast<size_t>(sy) * w * 4];
        uint8_t* dr =
            &dst[(static_cast<size_t>(y0 + y) * kCanvas + x0) * 4];
        for (int x = 0; x < dw; ++x) {
            const int sx = std::min(w - 1, static_cast<int>((x + 0.5) / s));
            dr[x * 4 + 0] = sr[sx * 4 + 2]; // R <- B
            dr[x * 4 + 1] = sr[sx * 4 + 1]; // G
            dr[x * 4 + 2] = sr[sx * 4 + 0]; // B <- R
            dr[x * 4 + 3] = 255;
        }
    }
    std::string err;
    char path[MAX_PATH * 2];
    snprintf(path, sizeof(path), "%s/frame_%05d.bmp", g_outDir.c_str(),
             g_frameNo);
    WriteBmp32(img, path, err);
    ++g_frameNo;
}

// 演示推进：睡 sec 秒，期间每 kFrameDt 抓一帧（编排动作与帧严格对齐）
void Pause(double sec) {
    int ms = static_cast<int>(sec * 1000);
    while (ms > 0) {
        const int step = std::min(ms, static_cast<int>(kFrameDt * 1000));
        Sleep(static_cast<DWORD>(step));
        CaptureNow();
        ms -= step;
    }
}

// ---- 编排动作 ------------------------------------------------------------

void ShowMain() {
    AppState& s = S();
    // 藏开着的功能窗口，保证屏幕抓主面板时不被遮挡
    for (int i = 1; i < BridgeCore::kWinCount; ++i) {
        const HWND hwnd = s.toolWindows[i].hwnd;
        if (hwnd) ShowWindow(hwnd, SW_HIDE);
    }
    SetWindowPos(s.mainHwnd, HWND_TOPMOST, 30, 30, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE);
    SetForegroundWindow(s.mainHwnd);
    g_capHwnd = s.mainHwnd;
}

void OpenAndShow(int winIdx, int w, int h) {
    AppState& s = S();
    OpenToolWindow(winIdx);
    const HWND hwnd = s.toolWindows[winIdx].hwnd;
    if (hwnd) {
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        // 摆到主面板右侧（屏幕放不下就放原位，此时主面板已被藏起）
        RECT screenRc;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &screenRc, 0);
        RECT mainRc;
        GetWindowRect(s.mainHwnd, &mainRc);
        int x = mainRc.right + 12;
        if (x + w > screenRc.right) x = mainRc.left;
        SetWindowPos(hwnd, HWND_TOPMOST, x, mainRc.top, w, h,
                     SWP_NOACTIVATE);
        SetForegroundWindow(hwnd);
        g_capHwnd = hwnd;
    }
    Pause(0.45); // 等首帧渲染
}

void InvalidateWin(int winIdx) {
    const HWND hwnd = S().toolWindows[winIdx].hwnd;
    if (hwnd) InvalidateRect(hwnd, nullptr, FALSE);
}

void InvalidateMain() {
    if (S().mainHwnd) InvalidateRect(S().mainHwnd, nullptr, FALSE);
}

void ApplyCurrent() {
    AppState& s = S();
    s.core->ApplyToClipboard();
    s.core->UpdatePreview(BridgeCore::kWinMain);
    InvalidateMain();
}

} // namespace

void RunDemo(int seconds, const std::string& outDir) {
    (void)seconds;
    AppState& s = S();
    // 演示期间关掉剪贴板监听（避免应用写剪贴板后又被自己抓回去打乱流程）
    s.watchClipboard = false;
    s.autoPaste = false;

    // 关键：窗口必须属于 UI 线程（有消息泵）才能收到 WM_PAINT。
    // RunDemo 在 UI 线程上被调用，这里预创建全部功能窗口并隐藏，
    // 之后脚本线程只做显示/切换。
    for (int i = 1; i < BridgeCore::kWinCount; ++i) {
        OpenToolWindow(i);
        if (s.toolWindows[i].hwnd) ShowWindow(s.toolWindows[i].hwnd, SW_HIDE);
    }

    std::thread([outDir]() {
        g_outDir = outDir;
        BridgeCore& core = *S().core;
        // ---- 素材准备：渐变测试图 + 模拟图层 ----
        {
            sai2::ImageBuffer probe(512, 512, sai2::PixelFormat::RGBA8);
            for (int y = 0; y < probe.Height(); ++y)
                for (int x = 0; x < probe.Width(); ++x) {
                    sai2::RGBA c;
                    c.r = static_cast<uint16_t>(x * 255 / probe.Width());
                    c.g = static_cast<uint16_t>(y * 255 / probe.Height());
                    c.b = static_cast<uint16_t>(((x / 32 + y / 32) % 2) ? 210
                                                                        : 70);
                    c.a = 255;
                    probe.SetPixel(x, y, c);
                }
            core.SetImage(std::move(probe), "demo");
            for (int i = 1; i <= 12; ++i) {
                S().layerNames.push_back(L"图层 " + std::to_wstring(i));
                S().layerAttrs.push_back(L"");
            }
            S().agentOnline = true;
            S().selLayer = 0;
            InvalidateMain();
        }
        ShowMain();

        // 0-2.5s 主面板
        Pause(2.5);

        // 2.5-7s 模糊窗口：高斯模糊半径动画
        OpenAndShow(BridgeCore::kWinBlur, 960, 700);
        {
            const int gaussian = core.FindEffectById("blur.gaussian");
            if (gaussian >= 0) {
                core.SetCurrentEffect(gaussian, BridgeCore::kWinBlur);
                core.UpdatePreview(BridgeCore::kWinBlur);
                InvalidateWin(BridgeCore::kWinBlur);
            }
            for (int v = 2; v <= 26; v += 2) {
                core.SetParamValue(0, static_cast<float>(v),
                                   BridgeCore::kWinBlur);
                core.UpdatePreview(BridgeCore::kWinBlur);
                InvalidateWin(BridgeCore::kWinBlur);
                Pause(0.33);
            }
        }

        // 7s 应用 → 主面板
        ApplyCurrent();
        ShowMain();
        Pause(1.8);

        // 9-16s 液化窗口：向前变形拖一笔 + 膨胀 + 顺时针旋转
        OpenAndShow(BridgeCore::kWinLiquify, 960, 760);
        {
            core.SetParamValue(0, 0.18f, BridgeCore::kWinLiquify); // 笔刷
            core.SetParamValue(1, 1.8f, BridgeCore::kWinLiquify);  // 强度
            const auto stroke = [&core](double x0, double y0, double x1,
                                        double y1, int steps) {
                core.StrokeBegin(static_cast<float>(x0),
                                 static_cast<float>(y0));
                for (int i = 1; i <= steps; ++i) {
                    const double t = i / (double)steps;
                    core.StrokeMove(static_cast<float>(x0 + (x1 - x0) * t),
                                    static_cast<float>(y0 + (y1 - y0) * t));
                    InvalidateWin(BridgeCore::kWinLiquify);
                    Pause(0.11);
                }
                core.StrokeEnd();
                InvalidateWin(BridgeCore::kWinLiquify);
                Pause(0.3);
            };
            stroke(0.22, 0.30, 0.72, 0.62, 12); // 向前变形：斜向拖拽

            const int bloat = core.FindEffectById("liquify.bloat");
            if (bloat >= 0) {
                core.SetCurrentEffect(bloat, BridgeCore::kWinLiquify);
                core.UpdatePreview(BridgeCore::kWinLiquify);
                InvalidateWin(BridgeCore::kWinLiquify);
                Pause(0.4);
                stroke(0.70, 0.28, 0.70, 0.28, 3); // 膨胀：原地按一下
            }
            const int twirl = core.FindEffectById("liquify.twirl-cw");
            if (twirl >= 0) {
                core.SetCurrentEffect(twirl, BridgeCore::kWinLiquify);
                core.UpdatePreview(BridgeCore::kWinLiquify);
                InvalidateWin(BridgeCore::kWinLiquify);
                Pause(0.4);
                stroke(0.32, 0.72, 0.32, 0.72, 4); // 旋转：原地按住
            }
        }

        // 16s 应用 → 主面板
        ApplyCurrent();
        ShowMain();
        Pause(1.5);

        // 17.5-21s 滤镜库：水彩 → 素描
        OpenAndShow(BridgeCore::kWinFilter, 960, 760);
        {
            const int wc = core.FindEffectById("filter.watercolor");
            if (wc >= 0) {
                core.SetCurrentEffect(wc, BridgeCore::kWinFilter);
                core.UpdatePreview(BridgeCore::kWinFilter);
                InvalidateWin(BridgeCore::kWinFilter);
            }
            Pause(1.6);
            const int pencil = core.FindEffectById("filter.pencil-sketch");
            if (pencil >= 0) {
                core.SetCurrentEffect(pencil, BridgeCore::kWinFilter);
                core.UpdatePreview(BridgeCore::kWinFilter);
                InvalidateWin(BridgeCore::kWinFilter);
            }
            Pause(1.6);
        }

        // 21-25s 调整：反相 → 去色 → 亮度/对比度 → 色相
        OpenAndShow(BridgeCore::kWinAdjust, 960, 700);
        {
            auto pick = [&](const char* id, double sec) {
                const int e = core.FindEffectById(id);
                if (e >= 0) {
                    core.SetCurrentEffect(e, BridgeCore::kWinAdjust);
                    core.UpdatePreview(BridgeCore::kWinAdjust);
                    InvalidateWin(BridgeCore::kWinAdjust);
                }
                Pause(sec);
            };
            pick("adjust.invert", 1.0);
            pick("adjust.grayscale", 1.0);
            pick("adjust.brightness-contrast", 0.4);
            for (int v = 0; v <= 40; v += 8) {
                core.SetParamValue(0, static_cast<float>(v),
                                   BridgeCore::kWinAdjust);
                core.SetParamValue(1, static_cast<float>(v) * 0.6f,
                                   BridgeCore::kWinAdjust);
                core.UpdatePreview(BridgeCore::kWinAdjust);
                InvalidateWin(BridgeCore::kWinAdjust);
                Pause(0.22);
            }
            pick("adjust.hue-saturation", 0.4);
            for (int v = 0; v <= 120; v += 24) {
                core.SetParamValue(0, static_cast<float>(v),
                                   BridgeCore::kWinAdjust);
                core.UpdatePreview(BridgeCore::kWinAdjust);
                InvalidateWin(BridgeCore::kWinAdjust);
                Pause(0.22);
            }
        }

        // 25-26s 主面板
        ShowMain();
        Pause(1.0);

        // 26s 切深色主题展示跟随
        CycleTheme(S().mainHwnd);
        InvalidateMain();
        for (int i = 1; i < BridgeCore::kWinCount; ++i) InvalidateWin(i);
        Pause(1.5);

        // 27s 深色液化窗口
        OpenAndShow(BridgeCore::kWinLiquify, 960, 760);
        Pause(1.5);

        // 收尾：主面板定格
        ShowMain();
        Pause(0.8);
        PostMessageW(S().mainHwnd, WM_APP + 11, 0, 0); // 请求正常退出
        Sleep(1200);
        ExitProcess(0); // 兜底，保证一定退出
    }).detach();
}
