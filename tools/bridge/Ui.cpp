// ============================================================================
// Ui.cpp - 功能窗口公共层实现
// ============================================================================

#include "Ui.h"

#include "ClipboardImage.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
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

namespace {

ThemeMode g_themeSetting = THEME_FOLLOW; // 用户设置
bool g_themeDark = true;                 // 当前生效（解析后）

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

void SetPalette(bool dark) {
    if (dark) {
        C_BG = RGB(30, 31, 34);
        C_PANEL = RGB(43, 45, 48);
        C_PANEL2 = RGB(36, 38, 41);
        C_LINE = RGB(62, 65, 68);
        C_TEXT = RGB(228, 229, 232);
        C_DIM = RGB(150, 153, 158);
        C_ACCENT = RGB(10, 132, 255);
        C_ACCENT2 = RGB(38, 88, 158);
        C_WARN = RGB(232, 166, 88);
        C_OK = RGB(96, 190, 130);
        C_CANVAS = RGB(18, 19, 21);
    } else {
        C_BG = RGB(243, 244, 247);
        C_PANEL = RGB(255, 255, 255);
        C_PANEL2 = RGB(233, 235, 239);
        C_LINE = RGB(214, 217, 223);
        C_TEXT = RGB(30, 32, 38);
        C_DIM = RGB(112, 117, 126);
        C_ACCENT = RGB(0, 116, 242);
        C_ACCENT2 = RGB(0, 116, 242);
        C_WARN = RGB(198, 112, 12);
        C_OK = RGB(18, 148, 84);
        C_CANVAS = RGB(205, 207, 212);
    }
}

} // namespace

void ApplyTheme() {
    g_themeSetting = ReadThemeSetting();
    const bool dark = (g_themeSetting == THEME_DARK) ||
                      (g_themeSetting == THEME_FOLLOW && !SystemPrefersLight());
    g_themeDark = dark;
    SetPalette(dark);
    AppState& s = S();
    if (s.bgBrush) DeleteObject(s.bgBrush);
    s.bgBrush = CreateSolidBrush(C_BG);
}

ThemeMode ThemeSetting() { return g_themeSetting; }

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

void EnableRoundedWindow(HWND hwnd) {
    if (!hwnd) return;
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

int Scale(int v) {
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
    v.inner = {win.canvas.left + 1, win.canvas.top + 1,
               win.canvas.right - 1, win.canvas.bottom - 1};
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

    // 液化笔刷指示圈（跟随缩放与平移）
    const int ci = core.CurrentEffect();
    const auto& fx = core.Effects();
    if (allowStroke && ci >= 0 && ci < static_cast<int>(fx.size()) &&
        fx[static_cast<size_t>(ci)].stroke) {
        const double bx =
            v.inner.left + v.ox + core.StrokeX() * img.Width() * v.zoom;
        const double by =
            v.inner.top + v.oy + core.StrokeY() * img.Height() * v.zoom;
        const double br = core.StrokeRadiusNorm() *
                          std::min(img.Width(), img.Height()) * v.zoom;
        HPEN pen = CreatePen(PS_SOLID, 1, C_ACCENT);
        HGDIOBJ oldPen = SelectObject(dc, pen);
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Ellipse(dc, static_cast<int>(bx - br), static_cast<int>(by - br),
                static_cast<int>(bx + br), static_cast<int>(by + br));
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(pen);
    }

    RestoreDC(dc, savedDc);   // 结束"画布框内"裁剪，信息条与缩放控件照常绘制

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
        RECT ir = {box.left + 8, box.bottom - 22, box.right - 296,
                   box.bottom - 4};
        TextC(dc, info, ir, C_DIM, S().fSmall,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    RECT slider = {box.right - 284, box.bottom - 24, box.right - 144,
                   box.bottom - 4};
    RECT z100 = {box.right - 140, box.bottom - 26, box.right - 88,
                 box.bottom - 2};
    RECT zfit = {box.right - 84, box.bottom - 26, box.right - 2,
                 box.bottom - 2};
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
            RoundBox(dc, fill, C_ACCENT, C_ACCENT, Scale(3));
        RECT knob = {knobX - 2, slider.top + 1, knobX + 2, slider.bottom - 1};
        RoundBox(dc, knob, C_TEXT, C_TEXT, 1);
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
int DrawParamRow(HDC dc, UiWindow& win, const RECT& box, int y,
                 const UiParam& p, float value, int paramIndex) {
    AppState& s = S();
    RECT lr = {box.left + 12, y, box.left + 150, y + 18};
    TextC(dc, U8(p.label), lr, C_DIM, s.fSmall,
          DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    RECT vr = {box.right - 100, y, box.right - 12, y + 18};
    TextC(dc, L"", vr, C_TEXT, s.fSmall, DT_RIGHT | DT_VCENTER);

    RECT track = {box.left + 14, y + 26, box.right - 14, y + 30};
    RoundBox(dc, track, C_CANVAS, C_LINE, 2);

    const double t =
        p.max > p.min ? (value - p.min) / (p.max - p.min) : 0.0;
    const int kx = track.left + static_cast<int>(t * RectW(track));
    RECT fill = {track.left, track.top, kx, track.bottom};
    if (fill.right > fill.left) RoundBox(dc, fill, C_ACCENT, C_ACCENT, 2);
    const int ky = (track.top + track.bottom) / 2;
    HBRUSH kb = CreateSolidBrush(C_ACCENT);
    HGDIOBJ ob = SelectObject(dc, kb);
    HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, kx - 6, ky - 6, kx + 6, ky + 6);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(kb);

    UiHit h;
    h.rc = RECT{box.left + 8, y, box.right - 8, track.bottom + 8};
    h.track = track;
    h.kind = HK_SLIDER;
    h.index = paramIndex;
    win.hits.push_back(h);
    return y + 48;
}

void CreateParamEdits(UiWindow& win) {
    if (!win.hwnd) return;
    for (int i = 0; i < 4; ++i) {
        win.hEdits[i] = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 70, 21,
            win.hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(100 + i)),
            reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(win.hwnd,
                                                          GWLP_HINSTANCE)),
            nullptr);
        SendMessageW(win.hEdits[i], WM_SETFONT,
                     reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),
                     TRUE);
    }
}

void SyncParamEdits(UiWindow& win, const std::vector<int>& rowY,
                    const RECT& box) {
    const auto& fx = S().core->Effects();
    const int ci = WindowEffectIndex(win);
    const int n = (ci >= 0 && ci < static_cast<int>(fx.size()))
                      ? std::min<int>(4, static_cast<int>(
                                             fx[static_cast<size_t>(ci)]
                                                 .params.size()))
                      : 0;
    win.editCount = n;
    for (int i = 0; i < 4; ++i) {
        if (i < n && i < static_cast<int>(rowY.size())) {
            SetWindowPos(win.hEdits[i], HWND_TOP, box.right - 104, rowY[i] + 1,
                         92, 20, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        } else {
            ShowWindow(win.hEdits[i], SW_HIDE);
        }
    }
}

void UpdateParamEdits(UiWindow& win) {
    const auto& fx = S().core->Effects();
    const int ci = WindowEffectIndex(win);
    const auto vals = S().core->ParamValues(win.id);
    if (ci < 0 || ci >= static_cast<int>(fx.size())) return;
    const int n = std::min<int>(4, static_cast<int>(
                                       fx[static_cast<size_t>(ci)].params.size()));
    for (int i = 0; i < n && i < static_cast<int>(vals.size()); ++i) {
        if (GetFocus() == win.hEdits[i]) continue; // 正在编辑不打扰
        const UiParam& p = fx[static_cast<size_t>(ci)].params[i];
        wchar_t buf[64];
        if (p.isInt)
            swprintf(buf, 64, L"%d", static_cast<int>(vals[i]));
        else
            swprintf(buf, 64, L"%.2f", vals[i]);
        SetWindowTextW(win.hEdits[i], buf);
    }
}

void CommitParamEdit(UiWindow& win, int index) {
    if (index < 0 || index >= 4) return;
    wchar_t buf[64] = {0};
    GetWindowTextW(win.hEdits[index], buf, 64);
    char narrow[64] = {0};
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, narrow, 64, nullptr, nullptr);
    const float v = static_cast<float>(std::atof(narrow));
    S().core->SetParamValue(static_cast<size_t>(index), v, win.id);
    S().core->ParamChanged(win.id);
    win.bgraDirty = true;
    UpdateParamEdits(win);
    InvalidateRect(win.hwnd, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// 工具条绘制
// ---------------------------------------------------------------------------
namespace {

void DrawButton(HDC dc, UiWindow& win, const RECT& rc, const std::wstring& text,
                bool active, bool primary, int kind, int index) {
    const bool hover = !active && !primary && UiHover(win, rc);
    const COLORREF fill = primary ? C_ACCENT2
                                  : (active ? C_ACCENT2
                                            : (hover ? C_PANEL : C_PANEL2));
    const COLORREF border = (primary || active) ? C_ACCENT
                                                : (hover ? C_ACCENT : C_LINE);
    RoundBox(dc, rc, fill, border, Scale(6));
    TextC(dc, text, rc, (primary || active) ? RGB(255, 255, 255) : C_TEXT,
          active ? S().fBold : S().fBody,
          DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    UiHit h;
    h.rc = rc;
    h.kind = kind;
    h.index = index;
    win.hits.push_back(h);
}

} // namespace

void DrawCategoryStrip(HDC dc, UiWindow& win, const RECT& strip,
                       const std::wstring& activeGroup) {
    static const wchar_t* kCats[4] = {L"模糊", L"滤镜", L"调整", L"液化"};
    RECT title = {strip.left, strip.top, strip.left + 52, strip.bottom};
    TextC(dc, L"功能", title, C_DIM, S().fSmall,
          DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    const int gap = 6;
    const int left = strip.left + 52;
    const int bw = (RectW(strip) - 52 - gap * 3) / 4;
    for (int i = 0; i < 4; ++i) {
        RECT rc = {left + i * (bw + gap), strip.top, left + i * (bw + gap) + bw,
                   strip.bottom};
        const bool active = (U8(BridgeCore::kGroupNames[i]) == activeGroup);
        DrawButton(dc, win, rc, kCats[i], active, false, HK_TOOL,
                   -1 - i); // 负数 = 分类切换（-1 模糊，-2 滤镜，-3 调整，-4 液化）
    }
}

void DrawEffectStrip(HDC dc, UiWindow& win, const RECT& strip,
                     const std::vector<int>& effects) {
    if (effects.empty()) return;
    const auto& fx = S().core->Effects();
    const int ci = WindowEffectIndex(win);
    const int gap = 6;
    const int n = static_cast<int>(effects.size());
    const int bw = (RectW(strip) - gap * (n - 1)) / n;
    for (int i = 0; i < n; ++i) {
        const int e = effects[static_cast<size_t>(i)];
        RECT rc = {strip.left + i * (bw + gap), strip.top,
                   strip.left + i * (bw + gap) + bw, strip.bottom};
        const bool active = (e == ci);
        const bool avail = S().core->EffectAvailable(e);
        DrawButton(dc, win, rc, U8(fx[static_cast<size_t>(e)].name), active,
                   false, HK_TOOL, e);
        if (!avail) {
            RECT t = rc;
            TextC(dc, L"未加载", t, C_WARN, S().fSmall,
                  DT_CENTER | DT_BOTTOM | DT_SINGLELINE);
        }
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
    const int gap = 8;
    const int n = static_cast<int>(cmds.size());
    const int bw = (RectW(strip) - gap * (n - 1)) / n;
    for (int i = 0; i < n; ++i) {
        const int cmd = cmds[static_cast<size_t>(i)];
        RECT rc = {strip.left + i * (bw + gap), strip.top,
                   strip.left + i * (bw + gap) + bw, strip.bottom};
        bool active = false;
        std::wstring label = CmdLabel(cmd);
        if (cmd == CMD_CHAIN) active = s.core->Chain();
        if (cmd == CMD_AUTOPASTE) active = s.autoPaste;
        if (cmd == CMD_GRAB && CaptureInProgress()) label = L"抓取中…";
        DrawButton(dc, win, rc, label, active, cmd == primaryCmd, HK_CMD, cmd);
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
    }
    return out;
}

const sai2bridge::AgentLayerGroup* PanelGroup() {
    const AppState& s = S();
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
    const int m = Scale(12);
    const int gap = Scale(10);
    const int headerH = Scale(34);
    const int stripH = Scale(30);
    const int cmdH = Scale(34);
    const int panel = Scale(panelW);

    UiLayout L;
    L.header = {0, 0, client.right, headerH};
    L.category = {m, headerH + gap, client.right - m - panel - gap,
                  headerH + gap + stripH};
    L.effects = {m, L.category.bottom + Scale(6), L.category.right,
                 L.category.bottom + Scale(6) + stripH};
    L.panel = {client.right - m - panel, L.effects.bottom + gap,
               client.right - m, client.bottom - m - cmdH - gap};
    L.commands = {m, client.bottom - m - cmdH, client.right - m,
                  client.bottom - m};
    L.canvas = {m, L.effects.bottom + gap, L.category.right, L.commands.top - gap};
    return L;
}

void DrawPanelFrame(HDC dc, const UiLayout& L, const std::wstring& title,
                    const std::wstring& hint) {
    RoundBox(dc, L.panel, C_PANEL, C_LINE, Scale(10));
    RECT tr = {L.panel.left + 10, L.panel.top + 6, L.panel.right - 10,
               L.panel.top + 28};
    TextC(dc, title, tr, C_TEXT, S().fBold,
          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (!hint.empty()) {
        RECT hr = {L.panel.left + 10, L.panel.bottom - Scale(46),
                   L.panel.right - 10, L.panel.bottom - 8};
        TextC(dc, hint, hr, C_WARN, S().fSmall,
              DT_LEFT | DT_TOP | DT_WORDBREAK);
    }
}

void DrawWindowChrome(HDC dc, UiWindow& win, const UiLayout& L,
                      const std::wstring& title, const std::wstring& subtitle,
                      const std::vector<int>& effects) {
    AppState& s = S();
    RECT full = {0, 0, 0, 0};
    GetClientRect(win.hwnd, &full);
    FillRect(dc, &full, s.bgBrush);

    // 顶栏
    FillRectC(dc, L.header, C_PANEL2);
    RECT tr = {Scale(14), 0, L.header.right - Scale(240), L.header.bottom};
    TextC(dc, title, tr, C_TEXT, s.fTitle,
          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT sr = {L.header.right - Scale(460), 0, L.header.right - Scale(12),
               L.header.bottom};
    TextC(dc, subtitle, sr, s.core->HasImage() ? C_OK : C_DIM, s.fSmall,
          DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    FrameRectC(dc, RECT{0, L.header.bottom - 1, full.right, L.header.bottom},
               C_LINE);

    // 分组 / 效果 / 命令
    const int idx = win.id;
    const std::wstring active = (idx >= 1 && idx < BridgeCore::kWinCount)
                                    ? U8(BridgeCore::kGroupNames[idx - 1])
                                    : std::wstring();
    DrawCategoryStrip(dc, win, L.category, active);
    DrawEffectStrip(dc, win, L.effects, effects);

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
