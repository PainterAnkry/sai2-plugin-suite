// ============================================================================
// AdjustWnd.cpp - 调整窗口（PS 风格）
//
// 反相 / 去色 / 亮度对比度 / 色相饱和度：普通参数行。
//
// 曲线（Curves）：**自定义面板**，照 Photoshop 的「曲线」对话框做 ——
//   通道下拉（RGB / 红 / 绿 / 蓝）
//   直方图背景 + 4×4 网格 + 对角基线
//   可拖控制点（点空白处新增、拖动改值、Delete 删除、复位按钮清空）
//   下方接"输入黑场 / 白场 / 中间调"三个滑杆（PS 的曲线面板下方就是这三块）
//
// 为什么用曲线替掉原来的色阶：色阶只是"黑场 + 白场 + Gamma"三点映射，
// 用户想要的是能拖出任意形状的曲线（拉 S 形、单独调某个色阶段）。曲线
// 是全功能的超集，参数也还在（黑场/白场/中间调照旧可调）。
// ============================================================================

#include "Ui.h"

#include "../../plugins/adjust/src/AdjustKernels.h" // ParseCurve / BuildCurveLut

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windowsx.h>

namespace sai2 {
namespace bridge {
namespace ui {

namespace {

const wchar_t* kClass = L"SAI2BridgeAdjustWnd";

// ---------------------------------------------------------------------------
// 曲线状态（每个通道一份，RGB = 0）
// ---------------------------------------------------------------------------
struct CurveState {
    std::vector<POINT> pts; // x/y 都在 0..255（图像色阶）
    int sel = 1;            // 当前选中的点（0 与最后一个是端点）
};

CurveState g_curves[4];
bool g_curvesInit = false;
RECT g_curveGrid{};      // 最近一次绘制的网格矩形（拖动时反算坐标用）
int g_hoverPoint = -1;   // 悬停到的点（画高亮用）
// 直方图缓存：内容变了才重算（每帧算 50 万像素没意义）
std::vector<int> g_hist;
int g_histW = 0, g_histH = 0;
unsigned long long g_histStamp = 0;
std::wstring g_histKey;

void InitCurves() {
    if (g_curvesInit) return;
    g_curvesInit = true;
    for (int c = 0; c < 4; ++c) {
        g_curves[c].pts.clear();
        g_curves[c].pts.push_back(POINT{0, 0});
        g_curves[c].pts.push_back(POINT{255, 255});
        g_curves[c].sel = 1;
    }
}

// 曲线点 → 插件参数串 "x:y,x:y,…"
std::string EncodeCurve(const CurveState& c) {
    std::string s;
    char buf[64];
    for (size_t i = 0; i < c.pts.size(); ++i) {
        std::snprintf(buf, sizeof(buf), "%s%d:%d", i ? "," : "",
                      static_cast<int>(c.pts[i].x),
                      static_cast<int>(c.pts[i].y));
        s += buf;
    }
    return s;
}

// 把四条曲线推给引擎（每次改动都要同步，否则预览用的还是上一条曲线）。
// quick = 拖动控制点时的显示帧（半分辨率 + 拉回），松手后再补全分辨率。
void PushCurves(UiWindow& win, bool quick = false) {
    BridgeCore& core = *S().core;
    core.SetExtraParam("adjust.curve.rgb", EncodeCurve(g_curves[0]));
    core.SetExtraParam("adjust.curve.r", EncodeCurve(g_curves[1]));
    core.SetExtraParam("adjust.curve.g", EncodeCurve(g_curves[2]));
    core.SetExtraParam("adjust.curve.b", EncodeCurve(g_curves[3]));
    core.ParamChanged(win.id, quick);
    win.bgraDirty = true;
    InvalidateRect(win.hwnd, nullptr, FALSE);
}

// 当前通道（参数 0）
int ChannelOf(const UiWindow& win) {
    const auto vals = S().core->ParamValues(win.id);
    if (vals.empty()) return 0;
    return std::max(0, std::min(3, static_cast<int>(std::lround(vals[0]))));
}

const wchar_t* AdjustHint(const std::string& id) {
    if (id == "adjust.invert") return L"反相：每个通道取补（含 alpha 保持）。";
    if (id == "adjust.grayscale") return L"去色：按亮度权重转灰度。";
    if (id == "adjust.brightness-contrast")
        return L"亮度/对比度：以中灰为支点缩放对比度。";
    if (id == "adjust.hue-saturation")
        return L"色相/饱和度/明度（HSL 空间，0 = 不变）。";
    if (id == "adjust.curves")
        return L"点曲线空白处新增控制点，拖动改变映射；选中点按 Delete 删除。";
    return L"";
}

// 直方图：按当前显示图统计亮度分布（每 3 个像素取 1 个，够用且快）
//
// 缓存键 = 显示图尺寸 + 当前效果号 + 全部参数值。参数一变（拖滑杆、换
// 通道、点应用）键就变，直方图跟着刷新；同一帧内多次重绘不会重复统计。
std::wstring HistogramKey(const UiWindow& win) {
    std::wstring k;
    const auto& E = S().core->Effects();
    const int ci = WindowEffectIndex(win);
    if (ci >= 0 && ci < static_cast<int>(E.size())) {
        k += std::wstring(E[ci].id.begin(), E[ci].id.end());
        k += L'|';
    }
    const auto vals = S().core->ParamValues(win.id);
    for (double v : vals) {
        wchar_t buf[32];
        swprintf(buf, 32, L"%.4f,", v);
        k += buf;
    }
    return k;
}

const std::vector<int>& Histogram(const UiWindow& win) {
    static std::vector<int> empty;
    const ImageBuffer& img = S().core->DisplayImage(BridgeCore::kWinAdjust);
    if (!img.IsValid()) return empty;

    const std::wstring key = HistogramKey(win);
    if (!g_hist.empty() && g_histW == img.Width() && g_histH == img.Height() &&
        key == g_histKey)
        return g_hist;

    g_hist.assign(128, 0);
    g_histW = img.Width();
    g_histH = img.Height();
    g_histKey = key;
    g_histStamp = GetTickCount64();
    const int stepY = std::max(1, img.Height() / 400);
    const int stepX = std::max(1, img.Width() / 400);
    for (int y = 0; y < img.Height(); y += stepY) {
        for (int x = 0; x < img.Width(); x += stepX) {
            RGBA c;
            if (!img.GetPixel(x, y, c)) continue;
            const int l = (299 * c.r + 587 * c.g + 114 * c.b) / 1000;
            // 亮度 0..255 → 128 个桶：除以 2（原来误写成 /65536，全挤在 0 号桶）
            const int bin = std::max(0, std::min(127, l >> 1));
            ++g_hist[static_cast<size_t>(bin)];
        }
    }
    return g_hist;
}

// 网格坐标换算
POINT CurveToDev(const RECT& g, POINT p) {
    const int w = std::max(1, RectW(g) - 2);
    const int h = std::max(1, RectH(g) - 2);
    return POINT{g.left + 1 + static_cast<LONG>(std::lround(p.x / 255.0 * w)),
                 g.bottom - 1 - static_cast<LONG>(std::lround(p.y / 255.0 * h))};
}
POINT DevToCurve(const RECT& g, int mx, int my) {
    const int w = std::max(1, RectW(g) - 2);
    const int h = std::max(1, RectH(g) - 2);
    const int x = static_cast<int>(std::lround((mx - g.left - 1) * 255.0 / w));
    const int y =
        static_cast<int>(std::lround((g.bottom - 1 - my) * 255.0 / h));
    return POINT{std::max(0, std::min(255, x)), std::max(0, std::min(255, y))};
}

// ---------------------------------------------------------------------------
// 曲线面板
// ---------------------------------------------------------------------------
void DrawCurvesPanel(HDC dc, UiWindow& win, const UiLayout& L) {
    AppState& s = S();
    InitCurves();
    DrawPanelFrame(dc, L, L"曲线", L"");

    std::vector<int> rowY;
    const auto& E = s.core->Effects();
    const int ci = WindowEffectIndex(win);
    const auto vals = s.core->ParamValues(win.id);
    if (ci < 0 || ci >= static_cast<int>(E.size())) return;

    // 通道下拉
    int y = L.panel.top + Scale(56);
    rowY.push_back(y);
    y = DrawParamRow(dc, win, L.panel, y, E[ci].params[0], vals[0], 0);

    // 复位按钮（右上角，与通道同一行）
    {
        RECT br = {L.panel.right - Scale(84), L.panel.top + Scale(56),
                   L.panel.right - Scale(16), L.panel.top + Scale(82)};
        // 复位按钮放在通道行右侧会太挤：改到网格右下
        (void)br;
    }

    // 网格：正方形，宽度吃满面板，高度不超过剩余空间
    const int availW = RectW(L.panel) - Scale(28);
    const int bottomReserve = Scale(230); // 下方三个滑杆 + 按钮 + 提示
    const int availH = RectH(L.panel) - (y - L.panel.top) - bottomReserve;
    const int side = std::max(Scale(140), std::min<int>(availW, availH));
    RECT g = {L.panel.left + (RectW(L.panel) - side) / 2, y, 0, 0};
    g.right = g.left + side;
    g.bottom = g.top + side;
    g_curveGrid = g;

    const COLORREF gridBg = C_CURVE_GRID;
    RoundBox(dc, g, gridBg, C_LINE, RadiusSm());

    // 直方图
    const std::vector<int>& hist = Histogram(win);
    if (!hist.empty()) {
        int maxV = 1;
        for (int v : hist) maxV = std::max(maxV, v);
        const int w = std::max(1, RectW(g) - 2);
        const int h = std::max(1, RectH(g) - 2);
        HBRUSH hb = CreateSolidBrush(C_HIST);
        for (size_t i = 0; i < hist.size(); ++i) {
            if (hist[i] <= 0) continue;
            const int bh = static_cast<int>(
                std::lround(std::sqrt(hist[i] / static_cast<double>(maxV)) *
                            h * 0.78));
            const int x0 = g.left + 1 + static_cast<int>(i) * w / 128;
            const int x1 = g.left + 1 + static_cast<int>(i + 1) * w / 128;
            RECT br = {x0, g.bottom - 1 - bh, std::max(x0 + 1, x1), g.bottom - 1};
            FillRect(dc, &br, hb);
        }
        DeleteObject(hb);
    }

    // 网格线（4×4）+ 对角基线
    const COLORREF gl = MixColor(gridBg, C_TEXT, 0.14);
    HPEN penGrid = CreatePen(PS_SOLID, 1, gl);
    HGDIOBJ op = SelectObject(dc, penGrid);
    for (int i = 1; i < 4; ++i) {
        const int x = g.left + 1 + (RectW(g) - 2) * i / 4;
        const int yy = g.top + 1 + (RectH(g) - 2) * i / 4;
        MoveToEx(dc, x, g.top + 1, nullptr);
        LineTo(dc, x, g.bottom - 1);
        MoveToEx(dc, g.left + 1, yy, nullptr);
        LineTo(dc, g.right - 1, yy);
    }
    SelectObject(dc, op);
    DeleteObject(penGrid);

    HPEN penDiag = CreatePen(PS_DOT, 1, MixColor(gridBg, C_TEXT, 0.30));
    op = SelectObject(dc, penDiag);
    MoveToEx(dc, g.left + 1, g.bottom - 1, nullptr);
    LineTo(dc, g.right - 1, g.top + 1);
    SelectObject(dc, op);
    DeleteObject(penDiag);

    // 曲线本体：用插件同一套插值算 LUT，画出来的形状 = 实际应用的样子
    const int ch = ChannelOf(win);
    CurveState& cur = g_curves[ch];
    std::vector<sai2::adjust::CurvePoint> cp;
    cp.reserve(cur.pts.size());
    for (const POINT& p : cur.pts)
        cp.push_back({static_cast<float>(p.x), static_cast<float>(p.y)});
    float lut[256];
    sai2::adjust::BuildCurveLut(cp, lut);
    std::vector<POINT> line;
    line.reserve(256);
    for (int i = 0; i < 256; ++i) {
        const POINT c = {i, static_cast<int>(std::lround(lut[i]))};
        line.push_back(CurveToDev(g, c));
    }
    HPEN penCurve = CreatePen(PS_SOLID, std::max(2, Scale(2)),
                              ch == 0 ? C_TEXT
                                      : (ch == 1 ? C_CH_R
                                                 : (ch == 2 ? C_CH_G : C_CH_B)));
    op = SelectObject(dc, penCurve);
    Polyline(dc, line.data(), static_cast<int>(line.size()));
    SelectObject(dc, op);
    DeleteObject(penCurve);

    // 控制点
    const int pr = Scale(4);
    for (size_t i = 0; i < cur.pts.size(); ++i) {
        const POINT d = CurveToDev(g, cur.pts[i]);
        RECT hr = {d.x - pr - Scale(3), d.y - pr - Scale(3),
                   d.x + pr + Scale(3), d.y + pr + Scale(3)};
        const bool sel = (static_cast<int>(i) == cur.sel);
        const bool hot = (static_cast<int>(i) == g_hoverPoint);
        RoundBox(dc, RECT{d.x - pr, d.y - pr, d.x + pr, d.y + pr},
                 sel || hot ? C_ACCENT : gridBg, C_TEXT, pr);
        UiHit h;
        h.rc = hr;
        h.kind = HK_CURVE;
        h.index = static_cast<int>(i);
        win.hits.push_back(h);
    }
    {
        UiHit h;
        h.rc = g;
        h.kind = HK_CURVE;
        h.index = -1; // 空白处 = 新增控制点
        win.hits.push_back(h);
    }

    // 选中点的输入/输出读数（PS 网格下方那两个数字）
    wchar_t ro[96];
    if (cur.sel >= 0 && cur.sel < static_cast<int>(cur.pts.size()))
        swprintf(ro, 96, L"输入 %d    输出 %d", cur.pts[cur.sel].x,
                 cur.pts[cur.sel].y);
    else
        swprintf(ro, 96, L"输入 -    输出 -");
    RECT rr = {g.left, g.bottom + Scale(9), g.right - Scale(80),
               g.bottom + Scale(29)};
    TextC(dc, ro, rr, C_DIM, s.fSmall, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // 复位当前通道
    {
        RECT br = {g.right - Scale(72), g.bottom + Scale(8), g.right,
                   g.bottom + Scale(30)};
        const bool hot = UiHover(win, br);
        RoundBox(dc, br, hot ? MixColor(C_PANEL, C_TEXT, 0.08) : C_PANEL2,
                 C_LINE, Scale(6));
        TextC(dc, L"复位通道", br, C_DIM, s.fSmall,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        UiHit h;
        h.rc = br;
        h.kind = HK_CMD;
        h.index = 901;
        win.hits.push_back(h);
    }

    // 黑场 / 白场 / 中间调
    y = g.bottom + Scale(46);
    for (int i = 1; i < 4 && i < static_cast<int>(E[ci].params.size()) &&
                    i < static_cast<int>(vals.size());
         ++i) {
        rowY.push_back(y);
        y = DrawParamRow(dc, win, L.panel, y, E[ci].params[static_cast<size_t>(i)],
                         vals[static_cast<size_t>(i)], i);
    }

    SyncParamEdits(win, rowY, L.panel);
    UpdateParamEdits(win);
}

void DrawAdjust(HDC dc, UiWindow& win, const RECT& client) {
    AppState& s = S();
    const std::vector<int> fx =
        s.core->EffectsInGroup(BridgeCore::kGroupAdjust);

    const int ci = WindowEffectIndex(win);
    const auto& E = s.core->Effects();
    const bool curves =
        (ci >= 0 && ci < static_cast<int>(E.size()) && E[ci].customPanel);

    const UiLayout L = ComputeLayout(client, curves ? 316 : 268);
    win.canvas = L.canvas;

    FillRect(dc, &client, s.bgBrush);
    const ImageBuffer& img = s.core->DisplayImage(BridgeCore::kWinAdjust);
    wchar_t head[200], sub[240];
    GrabSubtitleHead(head, 200, img);
    swprintf(sub, 240, L"%s   预览 %.0f ms", head, s.core->LastPreviewMs());
    DrawWindowChrome(dc, win, L, L"调整（Adjust）", sub, fx);

    if (curves) {
        DrawCurvesPanel(dc, win, L);
        DrawCanvas(dc, win, false);
        return;
    }

    const std::wstring name =
        (ci >= 0 && ci < static_cast<int>(E.size())) ? U8(E[ci].name) : L"-";
    DrawPanelFrame(dc, L, L"参数 — " + name,
                   (ci >= 0 && ci < static_cast<int>(E.size()))
                       ? AdjustHint(E[ci].id)
                       : L"");

    std::vector<int> rowY;
    int y = L.panel.top + Scale(36);
    const auto vals = s.core->ParamValues(win.id);
    if (ci >= 0 && ci < static_cast<int>(E.size())) {
        const std::vector<UiParam>& params = E[ci].params;
        for (size_t i = 0; i < params.size() && i < vals.size(); ++i) {
            rowY.push_back(y);
            y = DrawParamRow(dc, win, L.panel, y, params[i], vals[i],
                             static_cast<int>(i));
        }
        if (params.empty()) {
            RECT hr = {L.panel.left + 12, y, L.panel.right - 12, y + 40};
            TextC(dc, L"该调整没有可调参数（点「应用」即生效）", hr, C_DIM,
                  s.fSmall, DT_LEFT | DT_TOP | DT_WORDBREAK);
        }
    }
    SyncParamEdits(win, rowY, L.panel);
    UpdateParamEdits(win);

    DrawCanvas(dc, win, false);
}

// 在曲线网格里按光标位置更新当前点（拖动中 / 点击时都走这里）
void DragCurveTo(UiWindow& win, int mx, int my) {
    if (g_curveGrid.right <= g_curveGrid.left) return;
    const int ch = ChannelOf(win);
    CurveState& cur = g_curves[ch];
    if (cur.sel < 0 || cur.sel >= static_cast<int>(cur.pts.size())) return;
    const POINT p = DevToCurve(g_curveGrid, mx, my);
    if (getenv("SAI2_UI_DEBUG")) {
        char b[200];
        snprintf(b, sizeof(b),
                 "curve drag sel=%d mouse=%d,%d -> p=%d,%d grid=%d,%d,%d,%d",
                 cur.sel, mx, my, p.x, p.y, g_curveGrid.left, g_curveGrid.top,
                 g_curveGrid.right, g_curveGrid.bottom);
        S().core->Log(b);
    }
    const int last = static_cast<int>(cur.pts.size()) - 1;
    // 端点只能上下移动（PS 里黑场/白场也压在两条边上）
    if (cur.sel == 0) {
        cur.pts[0].y = p.y;
    } else if (cur.sel == last) {
        cur.pts[last].y = p.y;
    } else {
        const int lo = cur.pts[static_cast<size_t>(cur.sel - 1)].x + 1;
        const int hi = cur.pts[static_cast<size_t>(cur.sel + 1)].x - 1;
        cur.pts[static_cast<size_t>(cur.sel)].x =
            std::max(lo, std::min<int>(hi, static_cast<int>(p.x)));
        cur.pts[static_cast<size_t>(cur.sel)].y = p.y;
    }
    PushCurves(win, /*quick=*/true);
}

bool OnAdjustHit(UiWindow& win, const UiHit& h) {
    AppState& s = S();
    if (h.kind == HK_TOOL && h.index >= 0) {
        if (!s.core->EffectAvailable(h.index)) {
            win.flash = L"该调整的插件未加载";
            return true;
        }
        CloseCombo(win);
        s.core->SetCurrentEffect(h.index, BridgeCore::kWinAdjust);
        win.effectIndex = h.index;
        win.bgraDirty = true;
        return true;
    }
    if (h.kind == HK_CURVE) {
        const int ch = ChannelOf(win);
        CurveState& cur = g_curves[ch];
        if (h.index >= 0 && h.index < static_cast<int>(cur.pts.size())) {
            cur.sel = h.index;
            win.dragCurvePoint = h.index;
            SetCapture(win.hwnd);
            InvalidateRect(win.hwnd, nullptr, FALSE);
            return true;
        }
        // 空白处：新增一个控制点并直接进入拖动
        if (h.index < 0 && g_curveGrid.right > g_curveGrid.left) {
            const POINT p = DevToCurve(g_curveGrid, win.mouse.x, win.mouse.y);
            // 插到正确的位置（按 x 排序）
            int at = static_cast<int>(cur.pts.size());
            for (size_t i = 0; i < cur.pts.size(); ++i)
                if (cur.pts[i].x > p.x) { at = static_cast<int>(i); break; }
            if (at <= 0 || at >= static_cast<int>(cur.pts.size())) return true;
            if (p.x - cur.pts[static_cast<size_t>(at - 1)].x < 2 ||
                cur.pts[static_cast<size_t>(at)].x - p.x < 2)
                return true; // 太挤了，忽略
            cur.pts.insert(cur.pts.begin() + at, p);
            cur.sel = at;
            win.dragCurvePoint = at;
            SetCapture(win.hwnd);
            PushCurves(win);
            InvalidateRect(win.hwnd, nullptr, FALSE);
        }
        return true;
    }
    if (h.kind == HK_CMD && h.index == 901) { // 复位当前通道曲线
        const int ch = ChannelOf(win);
        g_curves[ch].pts.clear();
        g_curves[ch].pts.push_back(POINT{0, 0});
        g_curves[ch].pts.push_back(POINT{255, 255});
        g_curves[ch].sel = 1;
        PushCurves(win);
        return true;
    }
    return false;
}

bool AdjustMsg(UiWindow& win, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_MOUSEMOVE && win.dragCurvePoint >= 0) {
        DragCurveTo(win, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return true;
    }
    if (msg == WM_LBUTTONUP && win.dragCurvePoint >= 0) {
        win.dragCurvePoint = -1;
        ReleaseCapture();
        // 松手补一帧全分辨率：拖动期用的是半分辨率预览，不补就会一直软着
        PushCurves(win, /*quick=*/false);
        return true;
    }
    if (msg == WM_KEYDOWN) {
        if (wp == VK_ESCAPE) {
            CloseToolWindow(BridgeCore::kWinAdjust);
            return true;
        }
        // 删除选中的控制点（端点不可删）
        if ((wp == VK_DELETE || wp == VK_BACK) &&
            GetFocus() != win.hEdits[0]) {
            const int ci = WindowEffectIndex(win);
            const auto& E = S().core->Effects();
            if (ci >= 0 && ci < static_cast<int>(E.size()) && E[ci].customPanel) {
                const int ch = ChannelOf(win);
                CurveState& cur = g_curves[ch];
                if (cur.sel > 0 && cur.sel + 1 < static_cast<int>(cur.pts.size())) {
                    cur.pts.erase(cur.pts.begin() + cur.sel);
                    cur.sel = std::max(0, cur.sel - 1);
                    PushCurves(win);
                }
            }
            return true;
        }
    }
    return false;
}

} // namespace

const ToolWindowSpec& AdjustWindowSpec() {
    static const ToolWindowSpec spec = {
        BridgeCore::kWinAdjust, kClass, L"调整 — SAI2 插件（作用于当前图层）",
        1180, 820, &DrawAdjust, &OnAdjustHit, &AdjustMsg};
    return spec;
}

} // namespace ui
} // namespace bridge
} // namespace sai2
