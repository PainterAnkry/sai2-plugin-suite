// 功能演示编排：详见 Demo.h。
// 截帧方式：脚本线程直接 GetDC(NULL)+BitBlt 从屏幕抓"当前演示窗口"的客户区
// （窗口均为 TOPMOST 且演示中互相遮挡的窗口会被隐藏，因此屏幕抓取内容正确；
// 不走 PrintWindow——它会向窗口线程发消息，与脚本线程触发的渲染互等会死锁；
// WM_TIMER 又会被界面重绘饿到 2fps）。每帧等比合成到 900x900 画布导出 BMP。
// 时间线：0-2.5 主面板 → 2.5-7 模糊调参 → 7 应用 → 7-9 主面板 →
// 9-18 液化涂抹（向前变形拖拽 + 膨胀 + 顺时针旋转 + 收缩，作用在方格/同心圆
// 测试图上，形变一眼可见）→ 18 应用 → 18-19.5 主面板 →
// 19.5-23 滤镜库（水彩/素描切换）→ 23-27 调整（反相/去色/亮度/色相）→
// 27-28 主面板 → 28-29 深色主题 → 29-30.5 液化窗口（深色）→ 退出。
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

// 生成高对比度测试图：方格 + 同心圆 + 斜纹。
// 平滑渐变被位移/模糊后几乎看不出差别，这套图形能直接看出直线弯曲、圆环被拧。
sai2::ImageBuffer MakeTestChart(int W, int H) {
    sai2::ImageBuffer img(W, H, sai2::PixelFormat::RGBA8);
    const double cx = W * 0.5, cy = H * 0.5;
    const auto c255 = [](double v) {
        return static_cast<uint16_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
    };
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            // 底色：柔和彩色渐变（保证观感，同时提供低频参照）
            double r = 238.0 - 52.0 * (x / (double)W);
            double g = 241.0 - 56.0 * (y / (double)H);
            double b = 250.0 - 30.0 * ((x + y) / (double)(W + H));
            // 64px 方格（4px 粗线）：形变后直线会明显弯曲
            if (x % 64 < 4 || y % 64 < 4) {
                r = 48; g = 55; b = 72;
            }
            // 中心同心圆（每 20px 一圈，3px 线）：膨胀/收缩/旋转最容易看出来
            const double dx = x - cx, dy = y - cy;
            const double d = std::sqrt(dx * dx + dy * dy);
            if (d < 232.0 && std::fmod(d, 20.0) < 3.0) {
                r = 84; g = 94; b = 116;
            }
            // 右下角斜纹（3px）：高频参照，形变后纹路会散开/挤紧
            if (x > W * 0.58 && y > H * 0.56 && ((x + y) % 11) < 3) {
                r = 42; g = 50; b = 66;
            }
            sai2::RGBA c;
            c.r = c255(r);
            c.g = c255(g);
            c.b = c255(b);
            c.a = 255;
            img.SetPixel(x, y, c);
        }
    }
    return img;
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
        // ---- 素材准备：高对比度测试图 + 模拟图层 ----
        {
            core.SetImage(MakeTestChart(512, 512), "demo");
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

        // 9-18s 液化窗口：先换回干净测试图（上一步模糊已应用在结果上，
        // 直接涂会看不清形变），再做 4 种形变
        core.SetImage(MakeTestChart(512, 512), "demo");
        OpenAndShow(BridgeCore::kWinLiquify, 960, 760);
        {
            const auto stroke = [&core](double x0, double y0, double x1,
                                        double y1, int steps, double step) {
                core.StrokeBegin(static_cast<float>(x0),
                                 static_cast<float>(y0));
                for (int i = 1; i <= steps; ++i) {
                    const double t = i / (double)steps;
                    core.StrokeMove(static_cast<float>(x0 + (x1 - x0) * t),
                                    static_cast<float>(y0 + (y1 - y0) * t));
                    // StrokeMove 只累积置换场；显示帧要自己渲染一次
                    core.StrokeRender(BridgeCore::kWinLiquify);
                    InvalidateWin(BridgeCore::kWinLiquify);
                    Pause(step);
                }
                core.StrokeEnd();
                core.StrokeRender(BridgeCore::kWinLiquify);
                InvalidateWin(BridgeCore::kWinLiquify);
                Pause(0.35);
            };
            const auto pick = [&core](const char* id, float paramIdx, float val,
                                      double wait) {
                const int e = core.FindEffectById(id);
                if (e >= 0) {
                    core.SetCurrentEffect(e, BridgeCore::kWinLiquify);
                    if (paramIdx >= 0) {
                        core.SetParamValue(paramIdx, val,
                                           BridgeCore::kWinLiquify);
                    }
                    core.UpdatePreview(BridgeCore::kWinLiquify);
                    InvalidateWin(BridgeCore::kWinLiquify);
                }
                Pause(wait);
            };
            pick("liquify.forward-warp", 0, 0.17f, 0.4);           // 工具 + 笔刷大小
            core.SetParamValue(1, 2.6f, BridgeCore::kWinLiquify);  // 强度
            core.UpdatePreview(BridgeCore::kWinLiquify);
            InvalidateWin(BridgeCore::kWinLiquify);
            Pause(0.4);

            stroke(0.20, 0.26, 0.62, 0.50, 12, 0.14); // 向前变形：方格被拉弯
            pick("liquify.bloat", 1, 2.4f, 0.5);
            stroke(0.50, 0.50, 0.50, 0.50, 5, 0.16); // 膨胀：同心圆外鼓
            pick("liquify.twirl-cw", 1, 120.0f, 0.5);
            stroke(0.30, 0.70, 0.30, 0.70, 6, 0.16); // 顺时针旋转：圆环被拧
            pick("liquify.pinch", 1, 2.2f, 0.5);
            stroke(0.74, 0.36, 0.74, 0.36, 5, 0.16); // 收缩：圆环向内吸
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

// ---------------------------------------------------------------------------
// 真机 SAI2 演示（--sai2demo）
//
// 走应用自己的真实链路（不注入外部输入，受限环境也能录）：
//   刷新注入图层 → RequestToolWindow（= 点功能按钮：抓当前图层 + 弹窗口）
//   → 向液化窗口投递鼠标消息真涂抹（走真实消息路径，能看到实时形变）
//   → 在窗口自己的命中矩形上点"应用 → 剪贴板"（等价于真实点击，自动贴回 SAI2）
// ---------------------------------------------------------------------------
void RunSai2Demo(int seconds, const std::string& outDir) {
    (void)outDir;
    AppState& s0 = S();
    s0.watchClipboard = false; // 别让剪贴板回写把自己抓回去
    std::thread([seconds]() {
        AppState& s = S();
        BridgeCore& core = *s.core;
        RefreshLayers(); // 注入代理已就绪时能拿到真实图层行
        Sleep(1500);

        // 1) 等价于"点液化按钮"：抓 SAI2 当前图层并弹出液化窗口
        RequestToolWindow(s.mainHwnd, BridgeCore::kWinLiquify, true);
        for (int i = 0; i < 80 && !core.HasImage(); ++i) Sleep(250);
        Sleep(2000);
        if (!core.HasImage()) {
            core.Log("sai2demo: 没有抓到 SAI2 图层，退出");
            ExitProcess(5);
        }

        const HWND lw = s.toolWindows[BridgeCore::kWinLiquify].hwnd;
        if (lw) {
            SetWindowPos(lw, HWND_TOPMOST, 760, 90, 1000, 850, SWP_SHOWWINDOW);
            SetForegroundWindow(lw);
            InvalidateRect(lw, nullptr, FALSE);
        }
        Sleep(1500);

        // 2) 真实消息路径涂抹两笔
        if (lw) {
            const RECT c = s.toolWindows[BridgeCore::kWinLiquify].canvas;
            const int cw = c.right - c.left, ch = c.bottom - c.top;
            const auto st = [&](double ax, double ay, double bx, double by,
                                int steps, int ms) {
                const int x0 = c.left + static_cast<int>(cw * ax);
                const int y0 = c.top + static_cast<int>(ch * ay);
                const int x1 = c.left + static_cast<int>(cw * bx);
                const int y1 = c.top + static_cast<int>(ch * by);
                PostMessageW(lw, WM_MOUSEMOVE, 0, MAKELPARAM(x0, y0));
                Sleep(150);
                PostMessageW(lw, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x0, y0));
                for (int i = 1; i <= steps; ++i) {
                    const int x = x0 + (x1 - x0) * i / steps;
                    const int y = y0 + (y1 - y0) * i / steps;
                    PostMessageW(lw, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x, y));
                    Sleep(ms);
                }
                Sleep(150);
                PostMessageW(lw, WM_LBUTTONUP, 0, MAKELPARAM(x1, y1));
                Sleep(900);
            };
            st(0.24, 0.30, 0.66, 0.58, 34, 24); // 向前变形：斜向拖
            st(0.42, 0.72, 0.60, 0.66, 14, 26); // 第二笔
        }

        // 3) 点"应用 → 剪贴板"（用窗口自己的命中矩形 = 真实点击）
        if (lw) {
            for (const UiHit& h : s.toolWindows[BridgeCore::kWinLiquify].hits) {
                if (h.kind == HK_CMD && h.index == CMD_APPLY) {
                    const int cx = (h.rc.left + h.rc.right) / 2;
                    const int cy = (h.rc.top + h.rc.bottom) / 2;
                    PostMessageW(lw, WM_MOUSEMOVE, 0, MAKELPARAM(cx, cy));
                    Sleep(120);
                    PostMessageW(lw, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(cx, cy));
                    Sleep(90);
                    PostMessageW(lw, WM_LBUTTONUP, 0, MAKELPARAM(cx, cy));
                    break;
                }
            }
        }
        Sleep(static_cast<DWORD>(seconds > 0 ? seconds : 4) * 1000);
        ExitProcess(0);
    }).detach();
}

// ---------------------------------------------------------------------------
// 笔刷流畅度自检（--brushbench）
//
// 复现用户报的问题：涂抹时画面不动、切换工具才看到变化。
// 机理：渲染发生在鼠标消息处理里（一次几十毫秒），而鼠标事件 125Hz 灌进来
// → 输入队列越积越多；WM_PAINT 是队列空闲时才产生的低优先级消息，于是拖动
// 期间窗口根本不重画，松手/切换工具后才补画出来。
//
// 本自检用真实消息队列（PostMessage，不是直接调 StrokeMove）灌一段拖动，
// 同时屏抓画布统计"画面真的刷新了几次"。
// ---------------------------------------------------------------------------
namespace {

// 抓 hwnd 的某个客户区子矩形 → 60x60 灰度签名（只看画布，灵敏度才够）
bool CaptureSigRect(HWND hwnd, const RECT& rc, std::vector<uint8_t>& sig) {
    if (!hwnd) return false;
    POINT tl = {rc.left, rc.top};
    ClientToScreen(hwnd, &tl);
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0 || w > 4000 || h > 4000) return false;

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
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, nullptr, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, screen, tl.x, tl.y, SRCCOPY | CAPTUREBLT);
    GetDIBits(mem, bmp, 0, static_cast<UINT>(h), src.data(), &bi, DIB_RGB_COLORS);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);

    const int N = 60;
    sig.assign(N * N, 0);
    for (int y = 0; y < N; ++y) {
        const int sy = std::min(h - 1, y * h / N);
        for (int x = 0; x < N; ++x) {
            const int sx = std::min(w - 1, x * w / N);
            const uint8_t* p = &src[(static_cast<size_t>(sy) * w + sx) * 4];
            sig[static_cast<size_t>(y) * N + x] =
                static_cast<uint8_t>((p[0] + p[1] + p[2]) / 3);
        }
    }
    return true;
}

bool CaptureSig(HWND hwnd, std::vector<uint8_t>& sig) {
    RECT rc;
    if (!hwnd || !GetClientRect(hwnd, &rc)) return false;
    return CaptureSigRect(hwnd, rc, sig);
}

double SigDiff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    if (a.size() != b.size() || a.empty()) return 0.0;
    double s = 0;
    for (size_t i = 0; i < a.size(); ++i)
        s += std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i]));
    return s / static_cast<double>(a.size());
}

} // namespace

void RunBrushBench(int imgW, int imgH, const std::string& outDir) {
    AppState& s = S();
    s.watchClipboard = false;
    s.autoPaste = false;
    BridgeCore& core = *s.core;

    core.SetImage(MakeTestChart(imgW, imgH), "brushbench");
    OpenToolWindow(BridgeCore::kWinLiquify);
    const HWND hwnd = s.toolWindows[BridgeCore::kWinLiquify].hwnd;
    if (!hwnd) {
        ExitProcess(3);
    }
    const int e = core.FindEffectById("liquify.forward-warp");
    if (e >= 0) {
        core.SetCurrentEffect(e, BridgeCore::kWinLiquify);
        core.SetParamValue(0, 0.10f, BridgeCore::kWinLiquify); // 笔刷
        core.SetParamValue(1, 2.0f, BridgeCore::kWinLiquify);  // 强度
        core.UpdatePreview(BridgeCore::kWinLiquify);
    }
    // 主面板藏起来，保证屏抓内容干净
    ShowWindow(s.mainHwnd, SW_HIDE);
    SetWindowPos(hwnd, HWND_TOPMOST, 60, 40, 1000, 820, SWP_SHOWWINDOW);
    SetForegroundWindow(hwnd);
    InvalidateRect(hwnd, nullptr, FALSE);

    std::thread([hwnd, imgW, imgH, outDir]() {
        Sleep(1200); // 等首帧渲染

        RECT c = S().toolWindows[BridgeCore::kWinLiquify].canvas;
        const int cw = c.right - c.left, ch = c.bottom - c.top;
        if (cw < 40 || ch < 40) {
            ExitProcess(4);
        }
        const int x0 = c.left + cw * 28 / 100, y0 = c.top + ch * 30 / 100;
        const int x1 = c.left + cw * 72 / 100, y1 = c.top + ch * 66 / 100;

        std::vector<uint8_t> prev, cur;
        CaptureSigRect(hwnd, c, prev);

        const ULONGLONG t0 = GetTickCount64();
        int during = 0, after = 0;
        ULONGLONG firstChange = 0, lastChange = 0, maxGap = 0;
        double maxDiffDuring = 0, maxDiffAfter = 0;

        auto sample = [&](int& counter, double& maxDiff) {
            if (!CaptureSigRect(hwnd, c, cur) || prev.empty()) return;
            const double d = SigDiff(prev, cur);
            prev = cur;
            if (d > maxDiff) maxDiff = d;
            if (d > 0.30) {
                ++counter;
                const ULONGLONG now = GetTickCount64() - t0;
                if (!firstChange) firstChange = now;
                if (lastChange) maxGap = std::max(maxGap, now - lastChange);
                lastChange = now;
            }
        };

        PostMessage(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x0, y0));
        // 60 次移动 × 8ms ≈ 480ms 的"手速"，与真实鼠标一致
        for (int i = 1; i <= 60; ++i) {
            const double t = i / 60.0;
            const int x = x0 + static_cast<int>((x1 - x0) * t);
            const int y = y0 + static_cast<int>((y1 - y0) * t);
            PostMessage(hwnd, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x, y));
            Sleep(8);
            sample(during, maxDiffDuring);
        }
        const ULONGLONG postDone = GetTickCount64() - t0;
        PostMessage(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(x1, y1));

        // 松手后再看 4 秒：变化到底是"拖动期间"出现的，还是"之后才补出来"
        for (int i = 0; i < 80; ++i) {
            Sleep(50);
            sample(after, maxDiffAfter);
        }
        const ULONGLONG total = GetTickCount64() - t0;

        char line[512];
        std::string rep;
        snprintf(line, sizeof(line), "image %dx%d\n", imgW, imgH);
        rep += line;
        snprintf(line, sizeof(line), "posted 60 moves in %llu ms\n",
                 static_cast<unsigned long long>(postDone));
        rep += line;
        snprintf(line, sizeof(line),
                 "visible updates DURING drag : %d  (max delta %.2f)\n", during,
                 maxDiffDuring);
        rep += line;
        snprintf(line, sizeof(line),
                 "visible updates AFTER  drag : %d  (max delta %.2f)\n", after,
                 maxDiffAfter);
        rep += line;
        snprintf(line, sizeof(line), "first visible change at    : %llu ms\n",
                 static_cast<unsigned long long>(firstChange));
        rep += line;
        snprintf(line, sizeof(line), "max gap between changes    : %llu ms\n",
                 static_cast<unsigned long long>(maxGap));
        rep += line;
        snprintf(line, sizeof(line), "bench wall time            : %llu ms\n",
                 static_cast<unsigned long long>(total));
        rep += line;

        const std::string path = outDir + "\\brushbench.txt";
        if (FILE* f = std::fopen(path.c_str(), "wb")) {
            std::fwrite(rep.data(), 1, rep.size(), f);
            std::fclose(f);
        }
        std::string msg = "brushbench:\n" + rep;
        S().core->Log(msg);
        ExitProcess(0);
    }).detach();
}
