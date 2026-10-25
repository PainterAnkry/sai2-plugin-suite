// ============================================================================
// Ui.cpp - 功能窗口公共层实现
// ============================================================================

#include "Ui.h"

#include "ClipboardImage.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>

namespace sai2 {
namespace bridge {
namespace ui {

// ---------------------------------------------------------------------------
// 全局状态
// ---------------------------------------------------------------------------
AppState& S() {
    static AppState s;
    return s;
}

int WindowEffectIndex(const UiWindow& win) {
    AppState& s = S();
    if (!s.core) return -1;
    if (win.id == BridgeCore::kWinMain) return s.core->CurrentEffect();
    return win.effectIndex >= 0 ? win.effectIndex : s.core->CurrentEffect();
}

// ---------------------------------------------------------------------------
// 主题：跟随系统 / 浅色 / 深色（注册表记忆，HKCU\Software\SAI2Bridge）
// ---------------------------------------------------------------------------
COLORREF C_BG, C_PANEL, C_PANEL2, C_LINE, C_TEXT, C_DIM;
COLORREF C_ACCENT, C_ACCENT2, C_WARN, C_OK, C_CANVAS;
COLORREF C_INPUT, C_INPUT_LINE, C_TOOLTIP_BG, C_TOOLTIP_TEXT, C_INVERSE_BG,
    C_INVERSE_TEXT;
COLORREF C_CURVE_GRID, C_HIST, C_CH_R, C_CH_G, C_CH_B, C_ON_ACCENT;
COLORREF C_SLIDER_FILL;

namespace {

ThemeMode g_themeSetting = THEME_FOLLOW; // 用户设置
bool g_themeDark = true;                 // 当前生效（解析后）
// 截图/验收用的强制主题：只改内存，不写注册表（别动用户的设置）
bool g_themeForced = false;
ThemeMode g_forcedMode = THEME_FOLLOW;

ThemeMode ReadThemeSetting() {
    DWORD v = 0, size = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\SAI2Bridge", L"Theme",
                 RRF_RT_REG_DWORD, nullptr, &v, &size);
    return (v >= THEME_FOLLOW && v <= THEME_DARK)
               ? static_cast<ThemeMode>(v)
               : THEME_FOLLOW;
}

void WriteThemeSetting(ThemeMode m) {
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\SAI2Bridge", 0, nullptr,
                        0, KEY_SET_VALUE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        DWORD v = static_cast<DWORD>(m);
        RegSetValueExW(k, L"Theme", 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&v), sizeof(v));
        RegCloseKey(k);
    }
}

// 系统应用是否用浅色主题（读不到默认深色）
bool SystemPrefersLight() {
    DWORD v = 1, size = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER,
                 L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &size);
    return v != 0;
}

// ---------------------------------------------------------------------------
// 配色
//
// 设计取向（2026-10 重制）：Photoshop 的**信息架构**（分组、工具列、底部动作条、
// 参数行"标签 + 数值 + 滑杆"）+ OpenAI 的**视觉语言**（近黑中性表面、1px 发丝
// 描边、几乎不用彩色、只用一档强调色、圆角统一、留白充足）。
//
// 具体取舍：
//   * 表面分三层（底 / 卡片 / 悬浮），彼此只差 6~8 级亮度 —— 高级感来自
//     "层次很轻"，而不是彩色块。旧版用 #2B2D30 卡片 + #0A84FF 蓝，视觉噪声很大。
//   * 强调色只出现在"当前值"（滑杆已填充段 / 焦点环），不做按钮底色。
//     按钮的"主操作"用**反色填充**（深色主题下是浅色块），这是 OpenAI 的做法。
//   * 描边一律 1px 且比表面只亮一点点，不用粗边框划分区域。
// ---------------------------------------------------------------------------
void SetPalette(bool dark) {
    if (dark) {
        C_BG = RGB(13, 14, 16);      // 窗口底
        C_PANEL = RGB(23, 24, 27);   // 卡片 / 面板
        C_PANEL2 = RGB(32, 33, 37);  // 悬浮 / 输入框 / 选中
        C_LINE = RGB(45, 46, 51);    // 发丝描边
        C_TEXT = RGB(237, 237, 238);
        C_DIM = RGB(139, 142, 149);
        C_ACCENT = RGB(96, 145, 255); // 只用于数值/焦点
        C_ACCENT2 = RGB(38, 40, 45);
        C_WARN = RGB(226, 164, 88);
        C_OK = RGB(104, 192, 138);
        C_CANVAS = RGB(18, 19, 21);
        C_INPUT = RGB(30, 31, 35);      // 比面板亮一档：输入框才"有边界"
        C_INPUT_LINE = RGB(62, 64, 71); // 深色下发丝描边要够亮才看得见
        C_TOOLTIP_BG = RGB(44, 45, 50);
        C_TOOLTIP_TEXT = RGB(242, 242, 243);
        C_INVERSE_BG = RGB(240, 240, 240);
        C_INVERSE_TEXT = RGB(18, 18, 20);
        C_CURVE_GRID = RGB(17, 18, 20);
        C_HIST = MixColor(RGB(17, 18, 20), C_TEXT, 0.30);
        C_CH_R = RGB(226, 106, 106);
        C_CH_G = RGB(104, 196, 128);
        C_CH_B = RGB(112, 156, 238);
        C_ON_ACCENT = RGB(10, 12, 16);
        C_SLIDER_FILL = MixColor(C_ACCENT, C_PANEL, 0.28);
    } else {
        C_BG = RGB(247, 247, 246);
        C_PANEL = RGB(255, 255, 255);
        C_PANEL2 = RGB(242, 242, 241);
        C_LINE = RGB(226, 226, 224);
        C_TEXT = RGB(24, 24, 26);
        C_DIM = RGB(110, 110, 108);
        C_ACCENT = RGB(24, 24, 26);   // 浅色下强调 = 近黑（OpenAI 语言）
        C_ACCENT2 = RGB(240, 240, 239);
        C_WARN = RGB(186, 108, 14);
        C_OK = RGB(22, 140, 82);
        C_CANVAS = RGB(232, 232, 230);
        C_INPUT = RGB(255, 255, 255);
        C_INPUT_LINE = RGB(211, 211, 208);
        C_TOOLTIP_BG = RGB(28, 28, 30);
        C_TOOLTIP_TEXT = RGB(245, 245, 245);
        C_INVERSE_BG = RGB(26, 26, 28);
        C_INVERSE_TEXT = RGB(250, 250, 250);
        C_CURVE_GRID = RGB(255, 255, 255);
        C_HIST = MixColor(RGB(255, 255, 255), C_TEXT, 0.24);
        C_CH_R = RGB(198, 74, 74);
        C_CH_G = RGB(34, 138, 84);
        C_CH_B = RGB(56, 102, 186);
        C_ON_ACCENT = RGB(252, 252, 251);
        C_SLIDER_FILL = RGB(134, 134, 132);
    }
}

} // namespace

void ApplyTheme() {
    g_themeSetting = g_themeForced ? g_forcedMode : ReadThemeSetting();
    const bool dark = (g_themeSetting == THEME_DARK) ||
                      (g_themeSetting == THEME_FOLLOW && !SystemPrefersLight());
    g_themeDark = dark;
    SetPalette(dark);
    AppState& s = S();
    if (s.bgBrush) DeleteObject(s.bgBrush);
    s.bgBrush = CreateSolidBrush(C_BG);
    if (s.editBrush) DeleteObject(s.editBrush);
    s.editBrush = CreateSolidBrush(C_INPUT);
}

// ---------------------------------------------------------------------------
// 设计令牌：圆角与间距
// ---------------------------------------------------------------------------
int RadiusSm() { return Scale(6); }
int RadiusMd() { return Scale(8); }
int RadiusLg() { return Scale(12); }
int Sp(int step) { return Scale(step * 4); }
int CanvasFooterH() { return Scale(30); }

ThemeMode ThemeSetting() { return g_themeSetting; }

void ForceTheme(int mode) {
    g_themeForced = true;
    g_forcedMode = (mode >= THEME_FOLLOW && mode <= THEME_DARK)
                       ? static_cast<ThemeMode>(mode)
                       : THEME_FOLLOW;
    ApplyTheme();
}

void CycleTheme(HWND hwnd) {
    ThemeMode m = static_cast<ThemeMode>((ThemeSetting() + 1) % 3);
    WriteThemeSetting(m);
    ApplyTheme();
    if (hwnd) InvalidateRect(hwnd, nullptr, FALSE);
    InvalidateAll();
}

const wchar_t* ThemeLabel() {
    switch (ThemeSetting()) {
    case THEME_LIGHT: return L"浅色";
    case THEME_DARK: return L"深色";
    default: return L"跟随系统";
    }
}

bool ThemeIsDark() { return g_themeDark; }

// ---------------------------------------------------------------------------
// 圆角与悬停
// ---------------------------------------------------------------------------
void RoundBox(HDC dc, const RECT& rc, COLORREF fill, COLORREF border,
              int radius) {
    const int maxR = std::min(RectW(rc), RectH(rc)) / 2;
    const int r = std::max(1, std::min(radius, maxR));
    HBRUSH b = CreateSolidBrush(fill);
    HPEN p = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ ob = SelectObject(dc, b);
    HGDIOBJ op = SelectObject(dc, p);
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, r * 2, r * 2);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(p);
    DeleteObject(b);
}

COLORREF MixColor(COLORREF a, COLORREF b, double t) {
    if (t <= 0.0) return a;
    if (t >= 1.0) return b;
    auto ch = [t](int x, int y) {
        return static_cast<int>(std::lround(x + (y - x) * t));
    };
    return RGB(ch(GetRValue(a), GetRValue(b)), ch(GetGValue(a), GetGValue(b)),
               ch(GetBValue(a), GetBValue(b)));
}

void EnableRoundedWindow(HWND hwnd) {    if (!hwnd) return;
    using Fn = LONG(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    static Fn fn = []() -> Fn {
        HMODULE m = LoadLibraryW(L"dwmapi.dll");
        return m ? reinterpret_cast<Fn>(
                       GetProcAddress(m, "DwmSetWindowAttribute"))
                 : nullptr;
    }();
    if (!fn) return;
    DWORD pref = 2; // DWMWCP_ROUND
    fn(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &pref, sizeof(pref));
}

bool UiHover(const UiWindow& win, const RECT& rc) {
    return win.mouseValid && InRect(rc, win.mouse.x, win.mouse.y);
}

void UiTrackMouse(UiWindow& win, HWND hwnd, int mx, int my) {
    const POINT old = win.mouse;
    const bool oldValid = win.mouseValid;
    win.mouse = {mx, my};
    win.mouseValid = true;
    // 悬停目标变化才重绘（避免鼠标移动就整窗重绘大图）
    bool changed = !oldValid;
    for (const UiHit& h : win.hits) {
        if (InRect(h.rc, old.x, old.y) != InRect(h.rc, mx, my)) {
            changed = true;
            break;
        }
    }
    TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
    TrackMouseEvent(&tme);
    if (changed) InvalidateRect(hwnd, nullptr, FALSE);
}

void UiMouseLeave(UiWindow& win, HWND hwnd) {
    win.mouseValid = false;
    InvalidateRect(hwnd, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// 绘制原语
// ---------------------------------------------------------------------------
std::wstring U8(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                      static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        out.data(), n);
    return out;
}

void FillRectC(HDC dc, const RECT& rc, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &rc, b);
    DeleteObject(b);
}

void FrameRectC(HDC dc, const RECT& rc, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FrameRect(dc, &rc, b);
    DeleteObject(b);
}

void RoundedPanel(HDC dc, const RECT& rc, COLORREF fill, COLORREF border) {
    HBRUSH b = CreateSolidBrush(fill);
    HPEN p = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ ob = SelectObject(dc, b);
    HGDIOBJ op = SelectObject(dc, p);
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, 8, 8);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(p);
    DeleteObject(b);
}

void TextC(HDC dc, const std::wstring& s, RECT rc, COLORREF col, HFONT f,
           UINT flags) {
    SelectObject(dc, f);
    SetTextColor(dc, col);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, s.c_str(), static_cast<int>(s.size()), &rc, flags);
}

int RectW(const RECT& r) { return r.right - r.left; }
int RectH(const RECT& r) { return r.bottom - r.top; }

bool InRect(const RECT& r, int x, int y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

namespace {
// 界面缩放倍率覆盖（测试用）：在 100% 缩放的机器上验证 150% 布局，
// 不必改系统 DPI。0 = 不覆盖，走真实 DPI。
double g_uiScale = 0.0;
} // namespace

void SetUiScale(double mult) { g_uiScale = mult; }

int Scale(int v) {
    if (g_uiScale > 0.0) {
        return static_cast<int>(static_cast<double>(v) * g_uiScale + 0.5);
    }
    static const int dpi = [] {
        HDC dc = GetDC(nullptr);
        const int d = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
        if (dc) ReleaseDC(nullptr, dc);
        return d > 0 ? d : 96;
    }();
    return static_cast<int>(static_cast<long long>(v) * dpi / 96);
}

void InitFonts() {
    AppState& s = S();
    if (s.fBody) return;
    const int base = Scale(14);
    s.fTitle = CreateFontW(-Scale(17), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    s.fBody = CreateFontW(-base, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                          DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    s.fSmall = CreateFontW(-Scale(12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    s.fBold = CreateFontW(-base, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                          DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    s.bgBrush = CreateSolidBrush(C_BG);
}

// ---------------------------------------------------------------------------
// 画布绘制
// ---------------------------------------------------------------------------
namespace {

// 把 ImageBuffer 转成自上而下 BGRA（StretchDIBits 直接用），带缓存。
// 透明区域按 PS 风格棋盘格合成，否则透明底黑线稿在黑底上看不见。
// 液化拖笔时每帧都是新显示图（5MP 级），转换按行分波段多线程并行。
void EnsureBgraRows(const ImageBuffer& img, uint8_t* dstBase, uint32_t bpc,
                    int y0, int y1) {
    const int w = img.Width();
    for (int y = y0; y < y1; ++y) {
        const uint8_t* src = img.Data() + static_cast<size_t>(y) * img.Stride();
        uint8_t* dst = dstBase + static_cast<size_t>(y) * w * 4;
        if (bpc == 1) {
            for (int x = 0; x < w; ++x) {
                const uint8_t r = src[x * 4 + 0];
                const uint8_t g = src[x * 4 + 1];
                const uint8_t b = src[x * 4 + 2];
                const uint8_t a = src[x * 4 + 3];
                const bool lightCell = (((x / 8) + (y / 8)) & 1) == 0;
                const uint8_t bg = lightCell ? 255 : 204;
                dst[x * 4 + 0] =
                    static_cast<uint8_t>((b * a + bg * (255 - a)) / 255);
                dst[x * 4 + 1] =
                    static_cast<uint8_t>((g * a + bg * (255 - a)) / 255);
                dst[x * 4 + 2] =
                    static_cast<uint8_t>((r * a + bg * (255 - a)) / 255);
                dst[x * 4 + 3] = 255;
            }
        } else {
            for (int x = 0; x < w; ++x) {
                const uint8_t r = src[x * 8 + 1];
                const uint8_t g = src[x * 8 + 3];
                const uint8_t b = src[x * 8 + 5];
                const uint8_t a = src[x * 8 + 7];
                const bool lightCell = (((x / 8) + (y / 8)) & 1) == 0;
                const uint8_t bg = lightCell ? 255 : 204;
                dst[x * 4 + 0] =
                    static_cast<uint8_t>((b * a + bg * (255 - a)) / 255);
                dst[x * 4 + 1] =
                    static_cast<uint8_t>((g * a + bg * (255 - a)) / 255);
                dst[x * 4 + 2] =
                    static_cast<uint8_t>((r * a + bg * (255 - a)) / 255);
                dst[x * 4 + 3] = 255;
            }
        }
    }
}

bool EnsureBgra(UiWindow& win, const ImageBuffer& img) {
    if (!img.IsValid()) return false;
    if (!win.bgraDirty && win.bgraW == img.Width() && win.bgraH == img.Height() &&
        !win.bgra.empty())
        return true;

    if (win.bgra.size() !=
        static_cast<size_t>(img.Width()) * img.Height() * 4) {
        win.bgra.assign(static_cast<size_t>(img.Width()) * img.Height() * 4, 0);
    }
    win.bgraW = img.Width();
    win.bgraH = img.Height();
    const uint32_t bpc = img.BytesPerChannel();

    unsigned hc = std::thread::hardware_concurrency();
    if (hc == 0) hc = 1;
    const int bands = static_cast<int>(std::min(hc, 16u));
    const int h = img.Height();
    if (bands < 2 || h < 128) {
        EnsureBgraRows(img, win.bgra.data(), bpc, 0, h);
    } else {
        const int band = (h + bands - 1) / bands;
        std::vector<std::thread> pool;
        pool.reserve(static_cast<size_t>(bands));
        for (int i = 0; i < bands; ++i) {
            const int y0 = i * band;
            if (y0 >= h) break;
            pool.emplace_back([&img, dst = win.bgra.data(), bpc, y0,
                               y1 = std::min(h, y0 + band)]() mutable {
                EnsureBgraRows(img, dst, bpc, y0, y1);
            });
        }
        for (auto& t : pool) t.join();
    }
    win.bgraDirty = false;
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// 画布视图：等比缩放 + 信箱式留边（永不拉伸变形）
//   * zoom == 0 → 适配：取 inner/图像 的最小比率，图像完整居中显示
//   * zoom  > 0 → 每图像像素 zoom 个设备像素；图像大于视口时可用 panX/panY 平移
// ---------------------------------------------------------------------------
bool CanvasViewOf(UiWindow& win, CanvasView& v) {
    const ImageBuffer& img = S().core->DisplayImage(win.id);
    if (!img.IsValid()) return false;
    // 底部留出信息条（尺寸 / 效果名 / 耗时 / 缩放）。以前信息条直接压在图像
    // 上，浅色主题遇到亮图像时灰字几乎看不见 —— 现在图像区不再铺到最底。
    v.inner = {win.canvas.left + 1, win.canvas.top + 1,
               win.canvas.right - 1, win.canvas.bottom - 1 - CanvasFooterH()};
    const int iw = img.Width(), ih = img.Height();
    const int cw = std::max(1, RectW(v.inner));
    const int ch = std::max(1, RectH(v.inner));
    v.fit = std::min(cw / (double)iw, ch / (double)ih);
    double zoom = (win.zoom > 0.0) ? win.zoom : v.fit;
    zoom = std::max(kZoomMin, std::min(kZoomMax, zoom));
    v.zoom = zoom;

    // 平移范围：图像大于视口时 [0, 图像-视口]，否则恒为 0（居中）
    const double vw = cw / zoom, vh = ch / zoom;
    win.panX = std::max(0.0, std::min(win.panX, std::max(0.0, iw - vw)));
    win.panY = std::max(0.0, std::min(win.panY, std::max(0.0, ih - vh)));
    if (win.zoom <= 0.0) {
        win.panX = 0;
        win.panY = 0;
    }

    const double dispW = iw * zoom, dispH = ih * zoom;
    v.ox = (dispW <= cw) ? (cw - dispW) / 2.0 : -win.panX * zoom;
    v.oy = (dispH <= ch) ? (ch - dispH) / 2.0 : -win.panY * zoom;
    v.dx = v.inner.left + static_cast<int>(std::floor(v.ox + 0.5));
    v.dy = v.inner.top + static_cast<int>(std::floor(v.oy + 0.5));
    v.dw = std::max(1, static_cast<int>(std::floor(dispW + 0.5)));
    v.dh = std::max(1, static_cast<int>(std::floor(dispH + 0.5)));
    v.valid = true;
    return true;
}

void CanvasZoomAt(UiWindow& win, double newZoom, int mx, int my) {
    CanvasView v;
    if (!CanvasViewOf(win, v)) return;
    const ImageBuffer& img = S().core->DisplayImage(win.id);
    newZoom = std::max(kZoomMin, std::min(kZoomMax, newZoom));
    if (std::fabs(newZoom - 1.0) < 0.06) newZoom = 1.0; // 100% 吸附

    // 光标下的图像坐标（缩放前后保持不动）
    const double u = (mx - v.inner.left - v.ox) / v.zoom;
    const double w = (my - v.inner.top - v.oy) / v.zoom;
    win.zoom = newZoom;

    const int cw = std::max(1, RectW(v.inner));
    const int ch = std::max(1, RectH(v.inner));
    if (img.Width() * newZoom <= cw) {
        win.panX = 0; // 图像比视口窄：居中，无平移
    } else {
        win.panX = u - (mx - v.inner.left) / newZoom;
        win.panX = std::max(0.0,
                            std::min(win.panX, img.Width() - cw / newZoom));
    }
    if (img.Height() * newZoom <= ch) {
        win.panY = 0;
    } else {
        win.panY = w - (my - v.inner.top) / newZoom;
        win.panY = std::max(0.0,
                            std::min(win.panY, img.Height() - ch / newZoom));
    }
}

// 放大镜滑杆：对数映射，左端 = 缩小，右端 = 放大（与 PS 状态栏滑杆一致）
double CanvasZoomFromSlider(double t) {
    t = std::max(0.0, std::min(1.0, t));
    return kZoomMin * std::pow(kZoomMax / kZoomMin, t);
}

double CanvasSliderFromZoom(double zoom) {
    if (zoom <= kZoomMin) return 0.0;
    if (zoom >= kZoomMax) return 1.0;
    return std::log(zoom / kZoomMin) / std::log(kZoomMax / kZoomMin);
}

bool OnZoomSliderDrag(UiWindow& win, int mx) {
    CanvasView v;
    if (!CanvasViewOf(win, v)) return false;
    const int w = std::max(1, RectW(win.zoomTrack));
    const double t = (mx - win.zoomTrack.left) / (double)w;
    const int cx = (v.inner.left + v.inner.right) / 2;
    const int cy = (v.inner.top + v.inner.bottom) / 2;
    CanvasZoomAt(win, CanvasZoomFromSlider(t), cx, cy);
    return true;
}

namespace {

// 缩放档位（+/- 按钮用，乘法步进）
const double kZoomStep = 1.5;

} // namespace

void DrawCanvas(HDC dc, UiWindow& win, bool allowStroke, bool zoomButtons) {
    const RECT box = win.canvas;
    FillRectC(dc, box, C_CANVAS);
    FrameRectC(dc, box, C_LINE);

    RECT inner = {box.left + 1, box.top + 1, box.right - 1, box.bottom - 1};
    BridgeCore& core = *S().core;
    const ImageBuffer& img = core.DisplayImage(win.id);

    if (!img.IsValid()) {
        TextC(dc, L"没有图像 —— 在 SAI2 里框选后按 Ctrl+C，或点「抓取 SAI2 图层」",
              inner, C_DIM, S().fBody, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        UiHit h;
        h.rc = box;
        h.kind = HK_PREVIEW;
        win.hits.push_back(h);
        return;
    }
    if (!EnsureBgra(win, img)) return;

    CanvasView v;
    if (!CanvasViewOf(win, v)) return;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = img.Width();
    bi.bmiHeader.biHeight = -img.Height();
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    // 整幅图像等比画到目标矩形（信箱式：比例永与 SAI2 画布一致，不拉伸不裁切）
    // ★ 裁剪到画布框：不裁剪的话，放大后 StretchDIBits 会把图像**画到左右
    //   参数栏上面**（用户实测："滚轮放大图像都盖住左右边栏了"）。所有功能
    //   窗口共用这里，所以几个窗口都会犯同一个毛病。笔刷指示圈一并裁剪。
    const int savedDc = SaveDC(dc);
    IntersectClipRect(dc, box.left + 1, box.top + 1, box.right - 1,
                      box.bottom - 1);
    const int saved = SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, nullptr);
    StretchDIBits(dc, v.dx, v.dy, v.dw, v.dh, 0, 0, img.Width(), img.Height(),
                  win.bgra.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
    SetStretchBltMode(dc, saved);

    // 液化笔刷指示圈（跟随缩放、平移与鼠标）
    //
    // 以前圈心取自 core.StrokeX/Y —— 那是**笔画**的位置，只在按下/拖动时更新，
    // 所以鼠标在画布上移动时圈定在原地不动（用户反馈"笔刷不跟随鼠标"）。
    // 现在优先用光标位置：win.mouse 由 UiTrackMouse 在每次 mousemove 更新，
    // 与是否按下无关；只有拿不到光标（刚开窗、尚未进入）时才退回笔画位置。
    const int ci = core.CurrentEffect();
    const auto& fx = core.Effects();
    win.brushBoxValid = false;
    win.brushPaintValid = false;
    if (allowStroke && ci >= 0 && ci < static_cast<int>(fx.size()) &&
        fx[static_cast<size_t>(ci)].stroke) {
        double bx = 0, by = 0;
        bool have = false;
        if (win.mouseValid) {
            bx = win.mouse.x;
            by = win.mouse.y;
            have = true;
        } else if (core.StrokeActive()) {
            bx = v.inner.left + v.ox + core.StrokeX() * img.Width() * v.zoom;
            by = v.inner.top + v.oy + core.StrokeY() * img.Height() * v.zoom;
            have = true;
        }
        const double br = core.StrokeRadiusNorm() *
                          std::min(img.Width(), img.Height()) * v.zoom;
        // 光标在画布外（右侧参数栏、按钮区）时不画圈 —— 否则会在画布边缘
        // 贴着一个被裁掉一半的圆，看着像画歪了。
        const bool inside = win.mouseValid && win.mouse.x >= box.left &&
                            win.mouse.x < box.right && win.mouse.y >= box.top &&
                            win.mouse.y < box.bottom;
        if (have && (inside || core.StrokeActive()) && br >= 0.5) {
            const int l = static_cast<int>(bx - br) - 1;
            const int t = static_cast<int>(by - br) - 1;
            const int r = static_cast<int>(bx + br) + 1;
            const int b = static_cast<int>(by + br) + 1;
            HPEN pen = CreatePen(PS_SOLID, 1, C_ACCENT);
            HGDIOBJ oldPen = SelectObject(dc, pen);
            HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
            Ellipse(dc, l, t, r, b);
            SelectObject(dc, oldBrush);
            SelectObject(dc, oldPen);
            DeleteObject(pen);
            win.brushBox = RECT{l, t, r, b};
            win.brushBoxValid = true;
            // 记下"画在哪、多大"：下次移动时按这两个圈求失效区域。
            // 只记矩形是不够的 —— 光标跳到别处时圈会在新位置被**裁剪着**
            // 画出来（只露一小段弧），而矩形还停在旧位置，那段弧就永远擦不掉，
            // 正是用户截图里那些散落的细弧线。
            win.brushPaint = {static_cast<LONG>(std::lround(bx)),
                              static_cast<LONG>(std::lround(by))};
            win.brushPaintR = static_cast<int>(std::lround(br)) + 2;
            win.brushPaintValid = true;
        }
    }

    RestoreDC(dc, savedDc);   // 结束"画布框内"裁剪，信息条与缩放控件照常绘制
    FrameRectC(dc, RECT{box.left + 1, box.bottom - 1 - CanvasFooterH(),
                        box.right - 1, box.bottom - CanvasFooterH()},
               MixColor(C_LINE, C_CANVAS, 0.45));

    // 信息条 + 缩放控制（放大镜滑杆 + 100% + 适配）
    {
        const std::wstring ename =
            (ci >= 0 && ci < static_cast<int>(fx.size())) ? U8(fx[ci].name) : L"";
        wchar_t zl[24];
        if (win.zoom <= 0.0)
            swprintf(zl, 24, L"适配 %.0f%%", v.fit * 100.0);
        else
            swprintf(zl, 24, L"%.0f%%", v.zoom * 100.0);
        wchar_t info[200];
        swprintf(info, 200, L"%d x %d    %s    %.0f ms    [%s]", img.Width(),
                 img.Height(), ename.c_str(), core.LastPreviewMs(), zl);
        RECT ir = {box.left + Sp(3), box.bottom - 24, box.right - 296,
                   box.bottom - 6};
        TextC(dc, info, ir, C_DIM, S().fSmall,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    RECT slider = {box.right - 284, box.bottom - 25, box.right - 144,
                   box.bottom - 7};
    RECT z100 = {box.right - 140, box.bottom - 26, box.right - 88,
                 box.bottom - 6};
    RECT zfit = {box.right - 84, box.bottom - 26, box.right - 2,
                 box.bottom - 6};
    if (zoomButtons) {
        // ---- 放大镜滑杆：左拖缩小、右拖放大 ----
        const bool hoverSlider = UiHover(win, slider);
        RoundBox(dc, slider, hoverSlider ? C_PANEL : C_PANEL2, C_LINE,
                 Scale(5));
        const double t = CanvasSliderFromZoom(v.zoom);
        const int knobX =
            slider.left + 2 + static_cast<int>(t * (RectW(slider) - 4));
        const int inset = Scale(2);
        RECT fill = {slider.left + inset, slider.top + inset, knobX,
                     slider.bottom - inset};
        if (fill.right > fill.left)
            RoundBox(dc, fill, C_SLIDER_FILL, C_SLIDER_FILL, Scale(3));
        const int kh = RectH(slider) - Scale(2);
        RECT knob = {knobX - kh / 2, slider.top + Scale(1),
                     knobX + (kh - kh / 2), slider.bottom - Scale(1)};
        RoundBox(dc, knob, C_TEXT, C_TEXT, kh / 2);
        // 放大镜图标（圆 + 斜柄），画在滑杆左侧
        {
            const int cxr = slider.left - Scale(16), cyr =
                (slider.top + slider.bottom) / 2;
            const int r = Scale(5);
            HPEN pen = CreatePen(PS_SOLID, Scale(1) + 1, C_DIM);
            HGDIOBJ op = SelectObject(dc, pen);
            HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
            Ellipse(dc, cxr - r, cyr - r, cxr - r + 2 * r, cyr - r + 2 * r);
            MoveToEx(dc, cxr + r - 2, cyr + r - 2, nullptr);
            LineTo(dc, cxr + r + Scale(4), cyr + r + Scale(4));
            SelectObject(dc, ob);
            SelectObject(dc, op);
            DeleteObject(pen);
        }
        RECT sliderHit = slider;
        sliderHit.left -= Scale(20); // 图标也归入命中区，方便按住就拖
        UiHit hs;
        hs.rc = sliderHit;
        hs.track = slider;
        hs.kind = HK_ZSLIDER;
        win.hits.push_back(hs);

        // ---- 100% / 适配 ----
        const bool hover100 = UiHover(win, z100);
        const bool hoverFit = UiHover(win, zfit);
        RoundBox(dc, z100, hover100 ? C_PANEL : C_PANEL2,
                 hover100 ? C_ACCENT : C_LINE, Scale(5));
        RoundBox(dc, zfit, hoverFit ? C_PANEL : C_PANEL2,
                 hoverFit ? C_ACCENT : C_LINE, Scale(5));
        TextC(dc, L"100%", z100, C_TEXT, S().fSmall,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        TextC(dc, L"适配", zfit, C_TEXT, S().fSmall,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        UiHit hz;
        hz.rc = z100;
        hz.kind = HK_Z100;
        win.hits.push_back(hz);
        hz.rc = zfit;
        hz.kind = HK_ZFIT;
        win.hits.push_back(hz);
    }

    UiHit h;
    h.rc = box;
    h.kind = HK_PREVIEW;
    win.hits.push_back(h);
}

bool CanvasPointToNorm(UiWindow& win, int mx, int my, float& nx, float& ny) {
    CanvasView v;
    if (!CanvasViewOf(win, v)) return false;
    const ImageBuffer& img = S().core->DisplayImage(win.id);
    const double ix = (mx - v.inner.left - v.ox) / v.zoom;
    const double iy = (my - v.inner.top - v.oy) / v.zoom;
    nx = static_cast<float>(ix / img.Width());
    ny = static_cast<float>(iy / img.Height());
    return nx >= 0.0f && nx <= 1.0f && ny >= 0.0f && ny <= 1.0f;
}

// ---------------------------------------------------------------------------
// 参数行 / 数值框
// ---------------------------------------------------------------------------
int DrawSectionLabel(HDC dc, const RECT& box, int y,
                     const std::wstring& text) {
    AppState& s = S();
    RECT lr = {box.left + Scale(14), y, box.right - Scale(14), y + Scale(16)};
    TextC(dc, text, lr, C_DIM, s.fSmall,
          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    return y + Scale(20);
}

RECT DrawSliderTrack(HDC dc, const RECT& box, int y, double t, bool hot) {
    const int h = Scale(3);
    const int cy = y + h / 2;
    // 先画"槽"，已填充段用强调色，未填充段用比面板略亮的灰 —— 不用彩色，
    // 只让"当前值"这一处有颜色（OpenAI 的做法）。
    RECT track = {box.left + Scale(14), cy - h / 2, box.right - Scale(14),
                  cy + h / 2 + 1};
    RoundBox(dc, track, MixColor(C_PANEL, C_TEXT, 0.10), MixColor(C_PANEL, C_TEXT, 0.10), h + 1);

    const int kx = track.left + static_cast<int>(t * RectW(track));
    if (kx > track.left) {
        RECT fill = {track.left, track.top, kx, track.bottom};
        RoundBox(dc, fill, C_SLIDER_FILL, C_SLIDER_FILL, h + 1);
    }
    // 圆头：实心 + 描边。第一版用"面板色 + 92% 白"，浅色主题下白底白钮，
    // 手柄等于消失 —— 现在两侧都用同一套对比：填充 C_TEXT 的 85%，描边 C_TEXT。
    const int kr = Scale(6);
    RECT thumb = {kx - kr, cy - kr, kx + kr, cy + kr};
    RoundBox(dc, thumb, MixColor(C_PANEL, C_TEXT, hot ? 1.0 : 0.88), C_TEXT,
             kr);
    return RECT{box.left + Scale(8), y - Scale(4), box.right - Scale(8),
                track.bottom + Scale(10)};
}

// 哪一格数值框当前有键盘焦点（画焦点环用）。
// editParam[] 是"参数序号 → EDIT 槽"的映射，由 SyncParamEdits 在绘制时建立；
// 用的是上一帧的映射足够 —— 焦点只会在两帧之间转移。
bool ParamEditFocused(const UiWindow& win, int paramIndex) {
    for (int i = 0; i < kMaxParamEdits; ++i) {
        if (win.editParam[i] != paramIndex) continue;
        return win.hEdits[i] && GetFocus() == win.hEdits[i];
    }
    return false;
}

int DrawParamRow(HDC dc, UiWindow& win, const RECT& box, int y,
                 const UiParam& p, float value, int paramIndex) {
    AppState& s = S();
    // 标签在左、控件在右 —— 数值框是真正的子 EDIT（见 CreateParamEdits），
    // 这里只画它的底：圆角 + 发丝描边，EDIT 自身去掉 3D 边框。
    RECT lr = {box.left + Scale(14), y, box.left + RectW(box) / 2,
               y + Scale(24)};

    if (p.Kind() == ParamKind::Choice) {
        TextC(dc, U8(p.label), lr, C_TEXT, s.fBody,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        RECT fr = {box.right - Scale(146), y + Scale(1), box.right - Scale(14),
                   y + Scale(25)};
        const int n = static_cast<int>(p.choices.size());
        int idx = static_cast<int>(std::lround(value));
        idx = std::max(0, std::min(n - 1, idx));
        const bool open = (win.comboOpen == paramIndex);
        const bool hot = UiHover(win, fr);
        RoundBox(dc, fr, hot || open ? MixColor(C_PANEL2, C_TEXT, 0.06) : C_PANEL2,
                 open ? C_ACCENT : C_LINE, RadiusSm());
        RECT tr = {fr.left + Scale(9), fr.top, fr.right - Scale(22), fr.bottom};
        TextC(dc, idx < n ? U8(p.choices[static_cast<size_t>(idx)]) : L"",
              tr, C_TEXT, s.fBody,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        // ▼ 小三角（两条线，别用 GDI 的字体符号：不同 DPI 下会缺字）
        const int cx = fr.right - Scale(12), cy = (fr.top + fr.bottom) / 2;
        HPEN pen = CreatePen(PS_SOLID, std::max(1, Scale(1)), C_DIM);
        HGDIOBJ op = SelectObject(dc, pen);
        MoveToEx(dc, cx - Scale(4), cy - Scale(2), nullptr);
        LineTo(dc, cx, cy + Scale(2));
        LineTo(dc, cx + Scale(4), cy - Scale(2));
        SelectObject(dc, op);
        DeleteObject(pen);
        UiHit h;
        h.rc = fr;
        h.kind = HK_COMBO;
        h.index = paramIndex;
        win.hits.push_back(h);
        return y + Scale(38);
    }

    if (p.Kind() == ParamKind::Toggle) {
        TextC(dc, U8(p.label), lr, C_TEXT, s.fBody,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        const bool on = value >= 0.5f;
        RECT fr = {box.right - Scale(56), y + Scale(2), box.right - Scale(14),
                   y + Scale(24)};
        const bool hot = UiHover(win, fr);
        RoundBox(dc, fr, on ? C_ACCENT : MixColor(C_PANEL2, C_TEXT, 0.10f),
                 on ? C_ACCENT : C_LINE, RectH(fr) / 2);
        const int r = RectH(fr) / 2 - Scale(2);
        const int kx = on ? fr.right - Scale(2) - r : fr.left + Scale(2) + r;
        const int ky = (fr.top + fr.bottom) / 2;
        RECT knob = {kx - r, ky - r, kx + r, ky + r};
        RoundBox(dc, knob, ThemeIsDark() && on ? C_BG : RGB(255, 255, 255),
                 hot ? C_TEXT : MixColor(C_LINE, C_TEXT, 0.25), r);
        UiHit h;
        h.rc = fr;
        h.kind = HK_TOGGLE;
        h.index = paramIndex;
        win.hits.push_back(h);
        return y + Scale(38);
    }

    TextC(dc, U8(p.label), lr, C_TEXT, s.fBody,
          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    RECT fr = {box.right - Scale(80), y + Scale(2), box.right - Scale(14),
               y + Scale(26)};
    // 数值底框：底与外框分开 —— EDIT 子窗口盖住内部，外面只剩 1px 描边。
    // 深色主题下 C_INPUT 比面板亮一档 + 专门的描边色，才不会"看起来没有边界"；
    // 聚焦时描边换强调色，用户知道键盘输入落在哪一格。
    RoundBox(dc, fr, C_INPUT,
             ParamEditFocused(win, paramIndex) ? C_ACCENT : C_INPUT_LINE,
             RadiusSm());

    const double t = p.max > p.min ? (value - p.min) / (p.max - p.min) : 0.0;
    const RECT hit = DrawSliderTrack(dc, box, y + Scale(40), t,
                                     win.dragSlider == paramIndex);
    UiHit h;
    h.rc = hit;
    h.track = {box.left + Scale(14), hit.top + Scale(4),
               box.right - Scale(14), hit.top + Scale(12)};
    h.kind = HK_SLIDER;
    h.index = paramIndex;
    win.hits.push_back(h);
    return y + Scale(62);
}

// 参数数值框的消息过程。
// 存在的理由：焦点在数值框时，按键由 EDIT 自己的窗口过程消费（它自带文本
// 撤销/光标移动），窗口过程根本收不到。之前只截了 Ctrl+Z，于是：
//   * 输入数字后按回车 → 编辑框自己吞掉（单行 EDIT 只会响一声），值不生效，
//     用户看到的就是"只能拖动，不能输入"；
//   * 按 Esc / 上下键 / 滚轮 也全都没有反应。
// 现在这里负责：回车提交、Esc 取消、上下键微调、滚轮微调、获得焦点全选。
LRESULT CALLBACK ParamEditProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    WNDPROC prev =
        reinterpret_cast<WNDPROC>(GetWindowLongPtrW(h, GWLP_USERDATA));
    UiWindow* w = nullptr;
    {
        HWND parent = GetParent(h);
        if (parent)
            w = reinterpret_cast<UiWindow*>(
                GetWindowLongPtrW(parent, GWLP_USERDATA));
    }
    auto slotOf = [&]() -> int {
        if (!w) return -1;
        for (int i = 0; i < kMaxParamEdits; ++i)
            if (w->hEdits[i] == h) return i;
        return -1;
    };

    // Tab / Shift+Tab 在可见的数值框之间循环。
    // 为什么不用 IsDialogMessage：那是对话框管理器的活，我们的窗口是自绘的，
    // 交给它反而会顺手吞掉回车/Esc（我们自己在下面处理）。EDIT 控件虽然
    // 建的时候带了 WS_TABSTOP，但没有对话框管理器它就是死标志 ——
    // 所以专业软件里最常用的"打完一个数按 Tab 到下一个"一直是失效的。
    auto focusSibling = [&](int dir) {
        if (!w) return;
        const int slot = slotOf();
        if (slot < 0) return;
        for (int step = 1; step <= kMaxParamEdits; ++step) {
            const int j = ((slot + dir * step) % kMaxParamEdits +
                           kMaxParamEdits) % kMaxParamEdits;
            if (j == slot) break;         // 转了一圈
            if (w->editParam[j] < 0) continue;
            if (!w->hEdits[j] || !IsWindowVisible(w->hEdits[j])) continue;
            // 先把当前框的值提交掉：Tab 的语义是"这个填完了，下一个"，
            // 不提交就会丢输入（用户敲完数字直接 Tab，值却没生效）
            CommitParamEdit(*w, slot);
            SetFocus(w->hEdits[j]);
            return;
        }
    };

    if (msg == WM_KEYDOWN && wp == 'Z' &&
        (GetKeyState(VK_CONTROL) & 0x8000) != 0) {
        if (w) {
            RunCmd(*w, CMD_UNDO);
            return 0;
        }
    }
    if (msg == WM_KEYDOWN && w) {
        const int slot = slotOf();
        switch (wp) {
        case VK_RETURN: { // 回车 = 提交并回到窗口（这样值立刻生效）
            if (slot >= 0) CommitParamEdit(*w, slot);
            SetFocus(GetParent(h));
            return 0;
        }
        case VK_ESCAPE: { // Esc = 放弃本次输入（重新显示引擎里的真实值）
            if (slot >= 0) {
                UpdateParamEdits(*w);
                InvalidateRect(GetParent(h), nullptr, FALSE);
            }
            SetFocus(GetParent(h));
            return 0;
        }
        case VK_TAB:
            focusSibling((GetKeyState(VK_SHIFT) & 0x8000) ? -1 : +1);
            return 0;
        case VK_UP:
            if (slot >= 0) NudgeParamEdit(*w, slot, +1, 1.0f);
            return 0;
        case VK_DOWN:
            if (slot >= 0) NudgeParamEdit(*w, slot, -1, 1.0f);
            return 0;
        default:
            break;
        }
    }
    if (msg == WM_MOUSEWHEEL && w) {
        const int slot = slotOf();
        if (slot >= 0) {
            const int delta = GET_WHEEL_DELTA_WPARAM(wp);
            NudgeParamEdit(*w, slot, delta > 0 ? +1 : -1,
                           (GetKeyState(VK_SHIFT) & 0x8000) ? 10.0f : 1.0f);
            return 0;
        }
    }
    if (msg == WM_LBUTTONDOWN) {
        // 单击进入数值框 = 全选旧值，用户敲数字直接替换。
        // 只在 WM_SETFOCUS 里全选是不够的：EDIT 自己的鼠标处理会随后把
        // 光标定位到点击位置、清掉选择 —— 实测"0"里点一下再敲 999，
        // 结果是 "9990"。PS/SAI 的数值框都是点进去即全选。
        const LRESULT r = prev ? CallWindowProcW(prev, h, msg, wp, lp)
                               : DefWindowProcW(h, msg, wp, lp);
        SendMessageW(h, EM_SETSEL, 0, -1);
        return r;
    }
    if (msg == WM_SETFOCUS) {
        // 点进/切进数值框时全选：PS 的行为，输入即替换旧值
        const LRESULT r = prev ? CallWindowProcW(prev, h, msg, wp, lp)
                               : DefWindowProcW(h, msg, wp, lp);
        SendMessageW(h, EM_SETSEL, 0, -1);
        // 焦点环画在父窗口上：焦点变化要立刻重画那一格，否则要等下一次
        // 其它原因的重绘才看得到（用户会以为"点了没反应"）
        if (w) {
            RECT er, pr;
            GetWindowRect(h, &er);
            GetWindowRect(w->hwnd, &pr);
            InflateRect(&er, Scale(4), Scale(4));
            OffsetRect(&er, -pr.left, -pr.top);
            InvalidateRect(w->hwnd, &er, FALSE);
        }
        return r;
    }
    if (msg == WM_KILLFOCUS && w) {
        const LRESULT r = prev ? CallWindowProcW(prev, h, msg, wp, lp)
                               : DefWindowProcW(h, msg, wp, lp);
        RECT er, pr;
        GetWindowRect(h, &er);
        GetWindowRect(w->hwnd, &pr);
        InflateRect(&er, Scale(4), Scale(4));
        OffsetRect(&er, -pr.left, -pr.top);
        InvalidateRect(w->hwnd, &er, FALSE);
        return r;
    }
    return prev ? CallWindowProcW(prev, h, msg, wp, lp)
                : DefWindowProcW(h, msg, wp, lp);
}

void CreateParamEdits(UiWindow& win) {
    if (!win.hwnd) return;
    for (int i = 0; i < kMaxParamEdits; ++i) {
        // 不再用 WS_EX_CLIENTEDGE：那个 3D 凹边是"系统对话框"的观感来源，
        // 和自绘圆角面板完全不搭。边框由 DrawParamRow 画，EDIT 自己只要
        // 平面背景（WM_CTLCOLOREDIT 已处理配色）。
        win.hEdits[i] = CreateWindowExW(
            0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_RIGHT,
            0, 0, 64, 24, win.hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(100 + i)),
            reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(win.hwnd,
                                                          GWLP_HINSTANCE)),
            nullptr);
        if (!win.hEdits[i]) continue;
        SendMessageW(win.hEdits[i], WM_SETFONT,
                     reinterpret_cast<WPARAM>(S().fBody), TRUE);
        // 边距：右侧留 3px，文字不贴边框；EDIT 控件默认还有 1px 内缩
        SendMessageW(win.hEdits[i], EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                     MAKELPARAM(Scale(4), Scale(5)));
        SetWindowLongPtrW(
            win.hEdits[i], GWLP_USERDATA,
            static_cast<LONG_PTR>(SetWindowLongPtrW(
                win.hEdits[i], GWLP_WNDPROC,
                reinterpret_cast<LONG_PTR>(ParamEditProc))));
    }
}

void SyncParamEdits(UiWindow& win, const std::vector<int>& rowY,
                    const RECT& box) {
    const auto& fx = S().core->Effects();
    const int ci = WindowEffectIndex(win);
    const auto* e = (ci >= 0 && ci < static_cast<int>(fx.size()))
                        ? &fx[static_cast<size_t>(ci)]
                        : nullptr;
    int slot = 0;
    for (int i = 0; i < kMaxParamEdits; ++i) win.editParam[i] = -1;
    if (e) {
        for (size_t i = 0; i < e->params.size(); ++i) {
            // 只有滑杆参数用数值框；下拉/开关是自绘控件
            if (e->params[i].Kind() != ParamKind::Slider) continue;
            if (slot >= kMaxParamEdits) break;
            if (static_cast<int>(i) >= static_cast<int>(rowY.size())) break;
            win.editParam[slot] = static_cast<int>(i);
            // 与 DrawParamRow 画的圆角底对齐：底框是 (right-80 .. right-14,
            // y+2 .. y+26)，EDIT 内缩 2px（right-78 / y+4，62x20），
            // 这样四周的描边才完整露出来（以前 y+7 会漏到框外 1px）。
            SetWindowPos(win.hEdits[slot], HWND_TOP, box.right - Scale(78),
                         rowY[i] + Scale(4), Scale(62), Scale(20),
                         SWP_NOACTIVATE | SWP_SHOWWINDOW);
            ++slot;
        }
    }
    win.editCount = slot;
    for (int i = slot; i < kMaxParamEdits; ++i)
        ShowWindow(win.hEdits[i], SW_HIDE);
}

void UpdateParamEdits(UiWindow& win, int forceSlot) {
    const auto& fx = S().core->Effects();
    const int ci = WindowEffectIndex(win);
    const auto vals = S().core->ParamValues(win.id);
    if (ci < 0 || ci >= static_cast<int>(fx.size())) return;
    const auto& ps = fx[static_cast<size_t>(ci)].params;
    for (int slot = 0; slot < kMaxParamEdits; ++slot) {
        const int i = win.editParam[slot];
        if (i < 0 || i >= static_cast<int>(ps.size()) ||
            i >= static_cast<int>(vals.size()))
            continue;
        // 正在编辑的框不打扰（会打断输入）；forceSlot 是刚提交完的那一格，
        // 必须刷成钳制后的真实值（用户敲 999、上限 180，框里不能还留着 999）
        if (GetFocus() == win.hEdits[slot] && slot != forceSlot) continue;
        const UiParam& p = ps[static_cast<size_t>(i)];
        wchar_t buf[64];
        // 显示倍率：液化笔刷 0.15 → "15"，强度 1.0 → "100"（PS 的百分数习惯）
        const float shown = vals[static_cast<size_t>(i)] * p.displayScale;
        if (p.isInt || p.displayScale != 1.0f)
            swprintf(buf, 64, L"%d", static_cast<int>(std::lround(shown)));
        else
            swprintf(buf, 64, L"%.2f", shown);
        SetWindowTextW(win.hEdits[slot], buf);
    }
}

// 把"界面显示值"写回引擎（含显示倍率换算与范围夹紧）
void SetParamDisplayValue(UiWindow& win, int index, float displayVal) {
    const auto& fx = S().core->Effects();
    const int ci = WindowEffectIndex(win);
    if (ci < 0 || ci >= static_cast<int>(fx.size())) return;
    const auto& ps = fx[static_cast<size_t>(ci)].params;
    if (index < 0 || index >= static_cast<int>(ps.size())) return;
    const UiParam& p = ps[static_cast<size_t>(index)];
    float v = p.displayScale != 0.0f ? displayVal / p.displayScale
                                    : displayVal;
    v = std::max(p.min, std::min(p.max, v));
    if (p.isInt) v = std::round(v);
    S().core->SetParamValue(static_cast<size_t>(index), v, win.id);
    S().core->ParamChanged(win.id);
    win.bgraDirty = true;
}

// 解析数值框里的文本。允许百分号/单位/空白混在里面（用户会照着 PS 敲
// "30%"、"-12.5°"），但必须是"一个合法数字"：
//   至少一位数字；符号只允许出现在开头且只能一个；小数点最多一个。
// 返回 false = 非法输入，调用方应当**回滚显示、不要改参数**。
// 以前这里等于 `_wtof(只留数字字符)`："abc" → 0、"1.2.3" → 1.2、"-" → 0，
// 参数被静默改成 0 最难察觉（用户以为输入无效就是"没改"）。
bool ParseParamText(const wchar_t* text, float* out) {
    if (!text || !out) return false;
    std::wstring raw;
    for (const wchar_t* p = text; *p; ++p)
        if ((*p >= L'0' && *p <= L'9') || *p == L'-' || *p == L'+' ||
            *p == L'.')
            raw.push_back(*p);
    int digits = 0, dots = 0, signs = 0;
    for (size_t i = 0; i < raw.size(); ++i) {
        const wchar_t ch = raw[i];
        if (ch >= L'0' && ch <= L'9') {
            ++digits;
        } else if (ch == L'.') {
            if (++dots > 1) return false;
        } else { // '+' / '-'
            if (i != 0 || ++signs > 1) return false;
        }
    }
    if (digits == 0) return false;
    const double v = _wtof(raw.c_str());
    if (!(v == v) || v > 1e9 || v < -1e9) return false; // NaN / 离谱值
    *out = static_cast<float>(v);
    return true;
}

void CommitParamEdit(UiWindow& win, int slot) {
    if (slot < 0 || slot >= kMaxParamEdits) return;
    const int index = win.editParam[slot];
    if (index < 0) return;
    wchar_t buf[64] = {0};
    GetWindowTextW(win.hEdits[slot], buf, 64);
    float v = 0.0f;
    if (!ParseParamText(buf, &v)) {
        // 非法输入：把框恢复成参数真实值，参数一个也不动
        UpdateParamEdits(win, slot);
        InvalidateRect(win.hwnd, nullptr, FALSE);
        return;
    }
    SetParamDisplayValue(win, index, v);
    UpdateParamEdits(win, slot);
    InvalidateRect(win.hwnd, nullptr, FALSE);
}

// 键盘/滚轮微调：步长按参数范围取 1（整数参数）或范围的 1%（浮点）
void NudgeParamEdit(UiWindow& win, int slot, int dir, float mult) {
    if (slot < 0 || slot >= kMaxParamEdits) return;
    const int index = win.editParam[slot];
    if (index < 0) return;
    const auto& fx = S().core->Effects();
    const int ci = WindowEffectIndex(win);
    if (ci < 0 || ci >= static_cast<int>(fx.size())) return;
    const auto& ps = fx[static_cast<size_t>(ci)].params;
    if (index >= static_cast<int>(ps.size())) return;
    const UiParam& p = ps[static_cast<size_t>(index)];
    const auto vals = S().core->ParamValues(win.id);
    if (index >= static_cast<int>(vals.size())) return;
    float step = p.isInt || p.displayScale != 1.0f
                     ? 1.0f
                     : std::max(0.01f, (p.max - p.min) * 0.01f) * p.displayScale;
    if (p.displayScale != 1.0f) step = 1.0f;
    const float shown = vals[static_cast<size_t>(index)] * p.displayScale;
    SetParamDisplayValue(win, index, shown + dir * step * mult);
    UpdateParamEdits(win);
    InvalidateRect(win.hwnd, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// 下拉框
//
// 为什么不直接用 Win32 COMBOBOX：它自带系统主题的边框/滚动条/白色弹出列表，
// 和自绘的圆角面板放一起像两种软件拼起来的；而且中文选项在深色主题下
// 会变成白底黑字方块。所以选项浮层自己画 —— 只有"一行底色 + 文案 + 勾选"
// 三件事，比调 COMBOBOX 的主题消息少得多。
// ---------------------------------------------------------------------------
namespace {
const UiParam* ComboParamOf(const UiWindow& win, int index) {
    const auto& fx = S().core->Effects();
    const int ci = WindowEffectIndex(win);
    if (ci < 0 || ci >= static_cast<int>(fx.size())) return nullptr;
    const auto& ps = fx[static_cast<size_t>(ci)].params;
    if (index < 0 || index >= static_cast<int>(ps.size())) return nullptr;
    const UiParam& p = ps[static_cast<size_t>(index)];
    return p.Kind() == ParamKind::Choice ? &p : nullptr;
}
} // namespace

void CloseCombo(UiWindow& win) {
    if (win.comboOpen < 0) return;
    win.comboOpen = -1;
    InvalidateRect(win.hwnd, nullptr, FALSE);
}

void DrawComboPopup(HDC dc, UiWindow& win, const RECT& client) {
    if (win.comboOpen < 0) return;
    const UiParam* p = ComboParamOf(win, win.comboOpen);
    if (!p) {
        win.comboOpen = -1;
        return;
    }
    AppState& s = S();
    const int n = static_cast<int>(p->choices.size());
    const int itemH = Scale(28);
    const int pad = Scale(4);
    const int x = win.comboAnchor.left;
    const int w = std::max(RectW(win.comboAnchor), Scale(150));
    const int total = n * itemH + pad * 2;
    int y = win.comboAnchor.bottom + Scale(3);
    if (y + total > client.bottom - Scale(6))
        y = win.comboAnchor.top - Scale(3) - total; // 下面放不下就向上翻
    y = std::max(Scale(4), y);

    RECT box = {x, y, x + w, y + total};
    // 影子：两层半透明感的描边（GDI 没有 alpha，用比背景略亮的色阶模拟）
    RECT sh = box;
    OffsetRect(&sh, Scale(2), Scale(3));
    RoundBox(dc, sh, MixColor(C_BG, RGB(0, 0, 0), 0.18), MixColor(C_BG, RGB(0, 0, 0), 0.18),
             Scale(8));
    RoundBox(dc, box, C_PANEL2, MixColor(C_LINE, C_TEXT, 0.22), RadiusMd());

    const auto vals = S().core->ParamValues(win.id);
    const int cur = (win.comboOpen < static_cast<int>(vals.size()))
                        ? static_cast<int>(std::lround(vals[static_cast<size_t>(win.comboOpen)]))
                        : 0;
    for (int i = 0; i < n; ++i) {
        RECT ir = {box.left + pad, box.top + pad + i * itemH,
                   box.right - pad, box.top + pad + (i + 1) * itemH};
        const bool hot = UiHover(win, ir);
        if (i == cur) {
            RoundBox(dc, ir, MixColor(C_PANEL2, C_TEXT, 0.10),
                     MixColor(C_PANEL2, C_TEXT, 0.10), Scale(5));
        } else if (hot) {
            RoundBox(dc, ir, MixColor(C_PANEL2, C_TEXT, 0.06),
                     MixColor(C_PANEL2, C_TEXT, 0.06), Scale(5));
        }
        RECT tr = {ir.left + Scale(10), ir.top, ir.right - Scale(22), ir.bottom};
        TextC(dc, U8(p->choices[static_cast<size_t>(i)]), tr,
              i == cur ? C_TEXT : C_DIM,
              i == cur ? s.fBold : s.fBody,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (i == cur) {
            // 勾：两条线段（对勾在 12px 的尺寸下比字符清晰）
            const int cx = ir.right - Scale(14), cy = (ir.top + ir.bottom) / 2;
            HPEN pen = CreatePen(PS_SOLID, std::max(1, Scale(2)), C_TEXT);
            HGDIOBJ op = SelectObject(dc, pen);
            MoveToEx(dc, cx - Scale(4), cy, nullptr);
            LineTo(dc, cx - Scale(1), cy + Scale(3));
            LineTo(dc, cx + Scale(4), cy - Scale(4));
            SelectObject(dc, op);
            DeleteObject(pen);
        }
        UiHit h;
        h.rc = ir;
        h.kind = HK_COMBOITEM;
        h.index = win.comboOpen;
        h.sub = i;
        win.hits.push_back(h);
    }
}

bool HandleComboClick(UiWindow& win, const UiHit& h) {
    if (h.kind == HK_COMBO) {
        win.comboOpen = (win.comboOpen == h.index) ? -1 : h.index;
        win.comboAnchor = h.rc;
        InvalidateRect(win.hwnd, nullptr, FALSE);
        return true;
    }
    if (h.kind == HK_COMBOITEM) {
        win.comboOpen = -1;
        SetParamDisplayValue(win, h.index, static_cast<float>(h.sub));
        UpdateParamEdits(win);
        InvalidateRect(win.hwnd, nullptr, FALSE);
        return true;
    }
    if (h.kind == HK_TOGGLE) {
        const auto vals = S().core->ParamValues(win.id);
        const float cur = (h.index >= 0 && h.index < static_cast<int>(vals.size()))
                              ? vals[static_cast<size_t>(h.index)]
                              : 0.0f;
        SetParamDisplayValue(win, h.index, cur >= 0.5f ? 0.0f : 1.0f);
        InvalidateRect(win.hwnd, nullptr, FALSE);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// 工具条绘制
// ---------------------------------------------------------------------------
namespace {

void DrawButton(HDC dc, UiWindow& win, const RECT& rc, const std::wstring& text,
                bool active, bool primary, int kind, int index) {
    const bool hover = !active && !primary && UiHover(win, rc);
    // 三种状态，都用"表面 + 发丝边"表达，彩色只留给焦点：
    //   主操作 → 反色填充（深色主题下近白块 + 深字），一眼是"就点这个"
    //   选中   → 比面板亮一档的填充 + 1px 强调描边
    //   普通   → 透明填充 + 发丝边，悬浮时亮一档
    const COLORREF invFill = C_INVERSE_BG;
    const COLORREF invText = C_INVERSE_TEXT;
    COLORREF fill, border, textCol;
    if (primary) {
        fill = invFill;
        border = invFill;
        textCol = invText;
    } else if (active) {
        fill = MixColor(C_PANEL, C_TEXT, 0.14);
        border = MixColor(C_LINE, C_ACCENT, 0.75);
        textCol = C_TEXT;
    } else if (hover) {
        fill = MixColor(C_PANEL, C_TEXT, 0.07);
        border = MixColor(C_LINE, C_TEXT, 0.18);
        textCol = C_TEXT;
    } else {
        fill = C_PANEL;
        border = C_LINE;
        textCol = C_DIM;
    }
    RoundBox(dc, rc, fill, border, RadiusMd());
    TextC(dc, text, rc, textCol, (primary || active) ? S().fBold : S().fBody,
          DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    UiHit h;
    h.rc = rc;
    h.kind = kind;
    h.index = index;
    win.hits.push_back(h);
}

} // namespace

namespace {

int TextWidth(HDC dc, HFONT f, const std::wstring& t) {
    HGDIOBJ old = SelectObject(dc, f);
    SIZE sz{0, 0};
    GetTextExtentPoint32W(dc, t.c_str(), static_cast<int>(t.size()), &sz);
    SelectObject(dc, old);
    return sz.cx;
}

} // namespace

void DrawCategoryStrip(HDC dc, UiWindow& win, const RECT& strip,
                       const std::wstring& activeGroup) {
    static const wchar_t* kCats[4] = {L"模糊", L"滤镜", L"调整", L"液化"};
    // PS 的分段控件：一个容器 + 4 段，选中段只亮一档，没有彩色块。
    // 旧版是 4 个独立彩条按钮，和"胶囊容器"相比边界噪声大得多。
    RoundBox(dc, strip, C_PANEL, C_LINE, RadiusMd());
    const int pad = Scale(3);
    const int gap = Scale(2);
    const int left = strip.left + pad;
    const int bw = (RectW(strip) - pad * 2 - gap * 3) / 4;
    const int bh = RectH(strip) - pad * 2;
    for (int i = 0; i < 4; ++i) {
        RECT rc = {left + i * (bw + gap), strip.top + pad,
                   left + i * (bw + gap) + bw, strip.top + pad + bh};
        const bool active = (U8(BridgeCore::kGroupNames[i]) == activeGroup);
        const bool hover = !active && UiHover(win, rc);
        if (active) {
            RoundBox(dc, rc, MixColor(C_PANEL, C_TEXT, 0.13),
                     MixColor(C_LINE, C_TEXT, 0.10), RadiusSm());
        } else if (hover) {
            RoundBox(dc, rc, MixColor(C_PANEL, C_TEXT, 0.06), C_PANEL,
                     RadiusSm());
        }
        TextC(dc, kCats[i], rc, active ? C_TEXT : C_DIM,
              active ? S().fBold : S().fBody,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        UiHit h;
        h.rc = rc;
        h.kind = HK_TOOL;
        h.index = -1 - i; // 负数 = 分类切换（-1 模糊 … -4 液化）
        win.hits.push_back(h);
    }
}

void DrawEffectStrip(HDC dc, UiWindow& win, const RECT& strip,
                     const std::vector<int>& effects) {
    if (effects.empty()) return;
    const auto& fx = S().core->Effects();
    const int ci = WindowEffectIndex(win);
    const int gap = Scale(6);
    const int padX = Scale(14);
    const int h = RectH(strip);
    int x = strip.left;
    for (size_t i = 0; i < effects.size(); ++i) {
        const int e = effects[i];
        const std::wstring name = U8(fx[static_cast<size_t>(e)].name);
        const bool active = (e == ci);
        const int w = TextWidth(dc, active ? S().fBold : S().fBody, name) +
                      padX * 2;
        RECT rc = {x, strip.top, x + w, strip.top + h};
        if (rc.right > strip.right) break; // 放不下就截断，宁可少画不可越界
        DrawButton(dc, win, rc, name, active, false, HK_TOOL, e);
        if (!S().core->EffectAvailable(e)) {
            // 未加载的插件：右上角一个小圆点，不占文字
            const int r = Scale(3);
            RECT dot = {rc.right - Scale(8) - r, rc.top + Scale(6),
                        rc.right - Scale(8) + r, rc.top + Scale(6) + r * 2};
            RoundBox(dc, dot, C_WARN, C_WARN, r);
        }
        x = rc.right + gap;
    }
}

const wchar_t* CmdLabel(int cmd) {
    switch (cmd) {
    case CMD_APPLY: return L"应用 → 剪贴板";
    case CMD_UNDO: return L"撤销";
    case CMD_RESET: return L"复位";
    case CMD_CLOSE: return L"关闭";
    case CMD_CHAIN: return L"叠加处理";
    case CMD_AUTOPASTE: return L"应用后自动粘贴回 SAI2";
    case CMD_GRAB: return L"抓取 SAI2 图层";
    default: return L"";
    }
}

void DrawCmdButtons(HDC dc, UiWindow& win, const RECT& strip,
                    const std::vector<int>& cmds, int primaryCmd) {
    if (cmds.empty()) return;
    AppState& s = S();
    // PS 的底部动作条：按钮靠右、按文字宽度、主操作在最右。
    // 旧版是"按数量均分整条"，几个短按钮会被拉成很宽的大色块。
    const int gap = Scale(8);
    const int padX = Scale(18);
    const int h = std::min(RectH(strip), Scale(34));
    const int top = strip.top + (RectH(strip) - h) / 2;
    std::vector<int> order(cmds.begin(), cmds.end());
    // 主操作排最后（贴右边）
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        return (a == primaryCmd) < (b == primaryCmd);
    });
    int x = strip.right;
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        const int cmd = *it;
        bool active = false;
        std::wstring label = CmdLabel(cmd);
        if (cmd == CMD_CHAIN) active = s.core->Chain();
        if (cmd == CMD_AUTOPASTE) active = s.autoPaste;
        if (cmd == CMD_GRAB && CaptureInProgress()) label = L"抓取中…";
        const int w = TextWidth(dc,
                                (cmd == primaryCmd) ? s.fBold : s.fBody,
                                label) +
                      padX * 2;
        x -= w;
        RECT rc = {x, top, x + w, top + h};
        DrawButton(dc, win, rc, label, active, cmd == primaryCmd, HK_CMD, cmd);
        x -= gap;
    }
}

// ---------------------------------------------------------------------------
// 图层行聚合（注入代理捕获的文字 → 图层条目）
// ---------------------------------------------------------------------------
namespace {

bool IsAttrRow(const std::wstring& t) {
    const size_t b = t.find_first_not_of(L" \t");
    if (b == std::wstring::npos) return false;
    const size_t e = t.find_last_not_of(L" \t");
    const std::wstring v = t.substr(b, e - b + 1);
    if (v.back() == L'%') return true;
    static const wchar_t* kBlend[] = {
        L"正常", L"明暗", L"滤色", L"正片叠底", L"叠加", L"发光", L"阴影",
        L"二值", L"色彩增值", L"Normal", L"Multiply", L"Screen",
        L"Overlay", L"Shade", L"Luminosity"};
    for (const wchar_t* bl : kBlend)
        if (v == bl) return true;
    return false;
}

bool IsJunkRow(const std::wstring& t) {
    static const wchar_t* kJunk[] = {
        L"选择", L"锁定", L"纸张质感", L"特殊效果", L"混合模式",
        L"不透明度", L"工具首选项", L"创建剪贴蒙版", L"指定为选区样本",
        L"防止超出范围", L"应用到钢笔线"};
    for (const wchar_t* j : kJunk)
        if (t == j) return true;
    return false;
}

} // namespace

LayerAgg AggregateLayers(const std::vector<sai2bridge::AgentLayerRow>& rows) {
    LayerAgg out;
    std::vector<sai2bridge::AgentLayerRow> sorted = rows;
    std::sort(sorted.begin(), sorted.end(),
              [](const sai2bridge::AgentLayerRow& a,
                 const sai2bridge::AgentLayerRow& b) { return a.y < b.y; });
    // 按屏幕 Y 去重：agent 的文字 ring 会把**同一条图层行**记录多次（面板每
    // 重绘一次就追加一条），不去重的话 names/screenY 会出现重复项，导致
    // "行号 → 屏幕 Y" 整体错位 —— 实测表现为多图层流程点"第 2 行"实际点的
    // 还是第 1 行，于是同一层被处理两次（shift 恒为 0）。
    int lastRowY = (1 << 30);
    for (const auto& r : sorted) {
        if (r.screenY == lastRowY) continue;
        lastRowY = r.screenY;
        std::wstring t = r.text;
        const size_t b0 = t.find_first_not_of(L" \t");
        t = (b0 == std::wstring::npos)
                ? L""
                : t.substr(b0, t.find_last_not_of(L" \t") - b0 + 1);
        if (t.empty() || IsJunkRow(t)) continue;
        if (IsAttrRow(t) && !out.names.empty()) {
            std::wstring& attrs = out.attrs.back();
            const bool isPercent = (t.back() == L'%');
            if (isPercent) {
                if (attrs.find(L'%') == std::wstring::npos)
                    attrs = attrs.empty() ? t : attrs + L" · " + t;
            } else if (attrs.empty() || attrs[0] == L'0') {
                attrs = attrs.empty() ? t : (t + L" · " + attrs);
            }
            continue;
        }
        out.names.push_back(t);
        out.attrs.push_back(L"");
        out.screenY.push_back(r.screenY);
        out.screenX.push_back(r.screenX);
    }
    return out;
}

// 点图层行的逻辑 X（见 Ui.h 的说明）。
//
// 三步：
//   1. 有行文字坐标 textX → 落在文字起点右侧 8px（名字列必然在眼睛右侧）；
//   2. 没有文字坐标 → 取面板中部（比贴左边界安全，眼睛在最左）；
//   3. 一律夹在面板内（右边留 10px，避免压到"锁定"类小图标）。
int LayerRowClickX(long long panelLeft, long long panelRight, int textX) {
    const int left = static_cast<int>(panelLeft);
    const int right = static_cast<int>(panelRight);
    const int width = right - left;
    const int hi = (width > 20) ? right - 10 : left + 34;
    int x = (textX > left + 6)
                ? textX + 8
                : (width > 40 ? left + width * 45 / 100 : left + 34);
    if (x > hi) x = hi;
    if (x < left + 2) x = left + 2;
    return x;
}

const sai2bridge::AgentLayerGroup* PanelGroup() {    const AppState& s = S();
    for (const auto& grp : s.layers.groups)
        if (grp.hwnd == s.layers.panelGuess) return &grp;
    const sai2bridge::AgentLayerGroup* best = nullptr;
    for (const auto& grp : s.layers.groups)
        if (!best || grp.rows.size() > best->rows.size()) best = &grp;
    return best;
}

// ---------------------------------------------------------------------------
// 标准布局 / 骨架绘制
// ---------------------------------------------------------------------------
UiLayout ComputeLayout(const RECT& client, int panelW) {
    const int m = Scale(16);
    const int gap = Scale(12);
    const int headerH = Scale(48);
    const int stripH = Scale(34);
    const int catH = Scale(34);
    const int cmdH = Scale(44);
    const int panel = Scale(panelW);

    UiLayout L;
    L.header = {0, 0, client.right, headerH};
    L.category = {m, headerH + gap, client.right - m - panel - gap,
                  headerH + gap + catH};
    L.effects = {m, L.category.bottom + Scale(10), L.category.right,
                 L.category.bottom + Scale(10) + stripH};
    L.panel = {client.right - m - panel, L.category.top,
               client.right - m, client.bottom - m - cmdH - gap};
    L.commands = {m, client.bottom - m - cmdH, client.right - m,
                  client.bottom - m};
    L.canvas = {m, L.effects.bottom + gap, L.category.right, L.commands.top - gap};
    return L;
}

// sub 为标题行右端的小字（滤镜库用它显示"分类：艺术效果"）。
// hint 是面板底部的说明：以前用 C_WARN 画，但这些都是普通描述而不是警告，
// 整块橙色既误导读者的优先级，也和"强调色克制使用"的规矩冲突 —— 现在用
// 次要文字色 + 上方一条发丝线，读起来像脚注而不是告警。
void DrawPanelFrame(HDC dc, const UiLayout& L, const std::wstring& title,
                    const std::wstring& hint, const std::wstring& sub) {
    RoundBox(dc, L.panel, C_PANEL, C_LINE, RadiusLg());
    // 标题区：标题 + 一条极淡的分隔线（不是灰底标题条，那是系统对话框的观感）
    const int padX = Scale(16);
    RECT tr = {L.panel.left + padX, L.panel.top + Scale(12),
               L.panel.right - padX, L.panel.top + Scale(34)};
    if (!sub.empty()) {
        // 副标题占右侧 45%，标题让出一半，避免长效果名把分类挤没
        RECT sr = {L.panel.left + RectW(L.panel) / 2, L.panel.top + Scale(14),
                   L.panel.right - padX, L.panel.top + Scale(34)};
        tr.right = sr.left - Scale(8);
        TextC(dc, sub, sr, C_DIM, S().fSmall,
              DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    TextC(dc, title, tr, C_TEXT, S().fBold,
          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    FrameRectC(dc, RECT{L.panel.left + Scale(1), L.panel.top + Scale(44),
                        L.panel.right - Scale(1), L.panel.top + Scale(45)},
               MixColor(C_LINE, C_PANEL, 0.55));
    if (!hint.empty()) {
        const int top = L.panel.bottom - Scale(54);
        FrameRectC(dc, RECT{L.panel.left + padX, top - Scale(6),
                            L.panel.right - padX, top - Scale(5)},
                   MixColor(C_LINE, C_PANEL, 0.45));
        RECT hr = {L.panel.left + padX, top, L.panel.right - padX,
                   L.panel.bottom - Scale(12)};
        TextC(dc, hint, hr, C_DIM, S().fSmall, DT_LEFT | DT_TOP | DT_WORDBREAK);
    }
}

void DrawWindowChrome(HDC dc, UiWindow& win, const UiLayout& L,
                      const std::wstring& title, const std::wstring& subtitle,
                      const std::vector<int>& effects, bool showEffects) {
    AppState& s = S();
    RECT full = {0, 0, 0, 0};
    GetClientRect(win.hwnd, &full);
    FillRect(dc, &full, s.bgBrush);

    // 顶栏：不铺色带，只留 15px 半粗标题 + 右侧 12px 灰副标题 + 一条发丝线。
    const int subW = Scale(320);
    RECT tr = {Scale(16), 0,
               std::max<int>(static_cast<int>(full.right) - subW - Scale(24),
                             Scale(140)),
               L.header.bottom};
    TextC(dc, title, tr, C_TEXT, s.fTitle,
          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT sr = {full.right - subW - Scale(16), 0, full.right - Scale(16),
               L.header.bottom};
    TextC(dc, subtitle, sr, s.core->HasImage() ? C_DIM : C_WARN, s.fSmall,
          DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    FrameRectC(dc, RECT{0, L.header.bottom - 1, full.right, L.header.bottom},
               MixColor(C_LINE, C_BG, 0.30));

    // 分组 / 效果 / 命令
    const int idx = win.id;
    const std::wstring active = (idx >= 1 && idx < BridgeCore::kWinCount)
                                    ? U8(BridgeCore::kGroupNames[idx - 1])
                                    : std::wstring();
    DrawCategoryStrip(dc, win, L.category, active);
    if (showEffects) DrawEffectStrip(dc, win, L.effects, effects);

    std::vector<int> cmds;
    if (idx == BridgeCore::kWinMain)
        cmds = {CMD_GRAB, CMD_APPLY, CMD_UNDO, CMD_RESET};
    else
        cmds = {CMD_APPLY, CMD_UNDO, CMD_RESET, CMD_CLOSE};
    DrawCmdButtons(dc, win, L.commands, cmds, CMD_APPLY);
}

// ---------------------------------------------------------------------------
// 命中处理
// ---------------------------------------------------------------------------
bool HandleZoomHit(UiWindow& win, const UiHit& h) {
    if (h.kind != HK_ZIN && h.kind != HK_ZOUT && h.kind != HK_Z100 &&
        h.kind != HK_ZFIT)
        return false;
    CanvasView v;
    if (!CanvasViewOf(win, v)) return true;
    // 以画布中心为锚点
    const int cx = (v.inner.left + v.inner.right) / 2;
    const int cy = (v.inner.top + v.inner.bottom) / 2;
    switch (h.kind) {
    case HK_ZIN:
        CanvasZoomAt(win, (win.zoom > 0.0 ? win.zoom : v.fit) * kZoomStep, cx,
                     cy);
        break;
    case HK_ZOUT:
        CanvasZoomAt(win, (win.zoom > 0.0 ? win.zoom : v.fit) / kZoomStep, cx,
                     cy);
        break;
    case HK_Z100:
        CanvasZoomAt(win, 1.0, cx, cy);
        break;
    case HK_ZFIT:
        win.zoom = 0.0;
        win.panX = 0;
        win.panY = 0;
        break;
    default:
        break;
    }
    return true;
}

bool OnSliderDrag(UiWindow& win, int mx) {
    for (const UiHit& h : win.hits) {
        if (h.kind != HK_SLIDER || h.index != win.dragSlider) continue;
        const double t = static_cast<double>(mx - h.track.left) /
                         std::max(1, RectW(h.track));
        const auto& fx = S().core->Effects();
        const int ci = WindowEffectIndex(win);
        if (ci >= 0 && ci < static_cast<int>(fx.size()) &&
            h.index >= 0 &&
            h.index < static_cast<int>(fx[static_cast<size_t>(ci)].params.size())) {
            const UiParam& p = fx[static_cast<size_t>(ci)]
                                   .params[static_cast<size_t>(h.index)];
            S().core->SetParamValue(static_cast<size_t>(h.index),
                                    p.min + static_cast<float>(t) * (p.max - p.min),
                                    win.id);
            UpdateParamEdits(win);
        }
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// 窗口位置记忆
// ---------------------------------------------------------------------------
namespace {

std::wstring PlacementKey(const wchar_t* key) {
    return std::wstring(L"Software\\SAI2Bridge\\Windows\\") + key;
}

} // namespace

void RestoreWindowPlacement(HWND hwnd, int defX, int defY, int w, int h) {
    wchar_t key[64] = {0};
    GetClassNameW(hwnd, key, 64);
    HKEY k = nullptr;
    int x = defX, y = defY;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, PlacementKey(key).c_str(), 0,
                      KEY_READ, &k) == ERROR_SUCCESS) {
        DWORD sz = sizeof(DWORD), type = 0, v = 0;
        if (RegQueryValueExW(k, L"x", nullptr, &type,
                             reinterpret_cast<LPBYTE>(&v), &sz) == ERROR_SUCCESS)
            x = static_cast<int>(v);
        sz = sizeof(DWORD);
        if (RegQueryValueExW(k, L"y", nullptr, &type,
                             reinterpret_cast<LPBYTE>(&v), &sz) == ERROR_SUCCESS)
            y = static_cast<int>(v);
        RegCloseKey(k);
    }
    // 没有记忆位置，或记忆位置越界/不在任何显示器上时：
    // 什么都不做，保留 CreateWindow 时 CW_USEDEFAULT 的默认层叠位置。
    // 千万不能把 CW_USEDEFAULT（INT_MIN）再喂给 SetWindowPos —— 它会被
    // 截断成 -32768 之类的幻影坐标，窗口直接落到屏幕外（owned 窗口又没有
    // 任务栏按钮，用户就永远找不到它：点液化"没反应"就是这个原因）。
    if (x == defX && y == defY) return;
    if (x < -2000 || y < -2000 || x > 20000 || y > 20000) return;
    RECT rc = {x, y, x + w, y + h};
    if (MonitorFromRect(&rc, MONITOR_DEFAULTTONULL) == nullptr) return;
    SetWindowPos(hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

void SaveWindowPlacement(HWND hwnd, const wchar_t* key) {
    if (!hwnd) return;
    RECT r;
    if (!GetWindowRect(hwnd, &r)) return;
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, PlacementKey(key).c_str(), 0, nullptr,
                        0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return;
    const DWORD x = static_cast<DWORD>(r.left), y = static_cast<DWORD>(r.top);
    RegSetValueExW(k, L"x", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&x),
                   sizeof(x));
    RegSetValueExW(k, L"y", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&y),
                   sizeof(y));
    RegCloseKey(k);
}

} // namespace ui
} // namespace bridge
} // namespace sai2
