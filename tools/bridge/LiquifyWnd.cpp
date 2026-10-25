// ============================================================================
// LiquifyWnd.cpp - 液化窗口
//
// 版式（2026-10 第二次重制）：严格照 Photoshop 的液化对话框摆：
//
//   ┌ 标题 + 副标题 ──────────────────────────────────────────────┐
//   │ [模糊|滤镜|调整|液化]                                        │
//   │ ┌──┬──────────────────────────────┐ ┌─ 工具选项 ─────────┐ │
//   │ │图│                              │ │ 画笔大小  ▭   15    │ │
//   │ │标│        画布（按住即施力）      │ │   ─────●──────      │ │
//   │ │列│                              │ │ 画笔压力  ▭  100    │ │
//   │ │  │                              │ │ 重建选项 / 说明      │ │
//   │ └──┴──────────────────────────────┘ └────────────────────┘ │
//   │                                  [撤销] [复位] [应用 → 剪贴板] │
//   └────────────────────────────────────────────────────────────┘
//
// 与上一版的差别（用户反馈）：
//   * 顶部那排**文字工具按钮**去掉 —— 工具全部挪到左侧图标列（PS 就是这样），
//     少了整整一行，画布变高；
//   * 图标列按 PS 的顺序与图标语义重排：向前变形 / 重建 / 平滑 / 顺时针扭曲 /
//     逆时针扭曲 / 褶皱 / 膨胀 / 左推 / 抓手 / 放大镜；
//   * 抓手、放大镜是 UI 工具（不对应插件效果）：抓手拖动平移、放大镜点击缩放。
//
// 笔刷手感：按住不动会持续施力（BridgeCore::StrokeHold + UiWindow 的
// kHoldTimer），向前变形/左推仍然只认拖动方向 —— 与 PS 一致。
// ============================================================================

#include "Ui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace sai2 {
namespace bridge {
namespace ui {

namespace {

const wchar_t* kClass = L"SAI2BridgeLiquifyWnd";

// 重建模式（仅"重建"工具有意义）：0 = 恢复全部，1..3 = 按笔刷逐次恢复
int g_reconstructMode = 0;

// 图标按钮的悬浮提示（10 个纯图标按钮，没有文字标签，必须能问出"这是什么"）
std::wstring g_tipText;
RECT g_tipAnchor{};
bool g_tipValid = false;

// UI 层的两个非效果工具：抓手（平移）与放大镜（缩放）
constexpr int kHandToolHit = -2;
constexpr int kZoomToolHit = -3;

std::vector<int> LiquifyEffects() {
    return S().core->EffectsInGroup(BridgeCore::kGroupLiquify);
}

// PS 液化工具栏的顺序（tool 值；"__hand__"/"__zoom__" 是 UI 工具）
const char* const kRailTools[] = {
    "forward-warp", "reconstruct", "smooth", "twirl-cw", "twirl-ccw",
    "pinch",        "bloat",       "push",   "__hand__", "__zoom__",
};
constexpr int kRailCount = static_cast<int>(sizeof(kRailTools) / sizeof(kRailTools[0]));

const char* ToolOfEffect(int effectIdx) {
    const auto& fx = S().core->Effects();
    if (effectIdx < 0 || effectIdx >= static_cast<int>(fx.size())) return "";
    const auto& f = fx[static_cast<size_t>(effectIdx)].fixed;
    return f.empty() ? "" : f[0].second.c_str();
}

int EffectByTool(const char* tool) {
    const auto& fx = S().core->Effects();
    for (size_t i = 0; i < fx.size(); ++i) {
        if (fx[i].group != BridgeCore::kGroupLiquify) continue;
        if (!fx[i].fixed.empty() && fx[i].fixed[0].second == tool)
            return static_cast<int>(i);
    }
    return -1;
}

const wchar_t* ToolTitle(const std::string& tool) {
    if (tool == "forward-warp") return L"向前变形";
    if (tool == "reconstruct") return L"重建";
    if (tool == "smooth") return L"平滑";
    if (tool == "twirl-cw") return L"顺时针扭曲";
    if (tool == "twirl-ccw") return L"逆时针扭曲";
    if (tool == "pinch") return L"褶皱";
    if (tool == "bloat") return L"膨胀";
    if (tool == "push") return L"左推";
    if (tool == "__hand__") return L"抓手";
    if (tool == "__zoom__") return L"放大镜";
    return L"";
}

const wchar_t* ToolTipText(const std::string& tool) {
    if (tool == "forward-warp")
        return L"按住拖动：像素沿拖动方向被推走。";
    if (tool == "reconstruct")
        return L"把笔刷范围内的形变还原；下方可选还原方式。";
    if (tool == "smooth")
        return L"抹平笔刷范围内的置换场，收拾抖动造成的小毛刺。";
    if (tool == "twirl-cw" || tool == "twirl-ccw")
        return L"按住不放会持续扭转，角度决定每次扭转的幅度。";
    if (tool == "pinch")
        return L"按住不放持续向中心挤压，适合收细轮廓。";
    if (tool == "bloat")
        return L"按住不放持续向外吹胀；移动鼠标改变受力中心。";
    if (tool == "push")
        return L"沿垂直于拖动方向推动像素：往上拖 = 向左推。";
    if (tool == "__hand__") return L"拖动平移画布（任何工具下按住空格也可以）。";
    if (tool == "__zoom__")
        return L"点一下放大；按住 Alt 点击缩小。";
    return L"";
}

const wchar_t* ReconstructLabel(int mode) {
    switch (mode) {
    case 0: return L"恢复全部";
    case 1: return L"恢复平滑";
    case 2: return L"恢复锐利";
    default: return L"恢复边界";
    }
}

// 把参数值按 step 调整并刷新（[ / ] 快捷键用）
void NudgeParam(UiWindow& win, int index, float step) {
    BridgeCore& core = *S().core;
    const int ci = WindowEffectIndex(win);
    const auto& fx = core.Effects();
    if (ci < 0 || ci >= static_cast<int>(fx.size())) return;
    const auto& ps = fx[static_cast<size_t>(ci)].params;
    if (index < 0 || index >= static_cast<int>(ps.size())) return;
    const auto vals = core.ParamValues(win.id);
    if (index >= static_cast<int>(vals.size())) return;
    float v = vals[static_cast<size_t>(index)] + step;
    core.SetParamValue(static_cast<size_t>(index), v, win.id);
    core.ParamChanged(win.id);
    win.bgraDirty = true;
    if (win.hwnd) {
        UpdateParamEdits(win);
        InvalidateRect(win.hwnd, nullptr, FALSE);
    }
}

// ---------------------------------------------------------------------------
// 工具图标（GDI 手绘）
//
// 为什么不用位图：10 个图标 × 2 种颜色 × DPI 会变成一堆资源；而这些都是
// 简单几何（圆弧 + 箭头 + 波浪），用 2px 画笔现画在任何 DPI 下都清晰，
// 颜色也能跟着主题走。
// ---------------------------------------------------------------------------
void GlyphArrowHead(HDC dc, int tipX, int tipY, double ang, int size,
                    COLORREF col) {
    const double a1 = ang + 2.5, a2 = ang - 2.5;
    POINT p[3] = {{tipX, tipY},
                  {tipX + static_cast<LONG>(std::lround(std::cos(a1) * size)),
                   tipY + static_cast<LONG>(std::lround(std::sin(a1) * size))},
                  {tipX + static_cast<LONG>(std::lround(std::cos(a2) * size)),
                   tipY + static_cast<LONG>(std::lround(std::sin(a2) * size))}};
    HBRUSH b = CreateSolidBrush(col);
    HGDIOBJ ob = SelectObject(dc, b);
    HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
    Polygon(dc, p, 3);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(b);
}

void GlyphCircle(HDC dc, int cx, int cy, int r, int w, COLORREF col) {
    HPEN p = CreatePen(PS_SOLID, w, col);
    HGDIOBJ op = SelectObject(dc, p);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Ellipse(dc, cx - r, cy - r, cx + r, cy + r);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(p);
}

void GlyphLine(HDC dc, int x0, int y0, int x1, int y1, int w, COLORREF col) {
    HPEN p = CreatePen(PS_SOLID, w, col);
    HGDIOBJ op = SelectObject(dc, p);
    MoveToEx(dc, x0, y0, nullptr);
    LineTo(dc, x1, y1);
    SelectObject(dc, op);
    DeleteObject(p);
}

// 带箭头的线段：1px 的三角在 22px 图形里等于看不见，所以头部按 Scale(4)
// 的实心三角画，线条短于头部时只画三角。
void GlyphArrow(HDC dc, int x0, int y0, int x1, int y1, int w, int head,
                COLORREF col) {
    const double ang = std::atan2(static_cast<double>(y1 - y0),
                                  static_cast<double>(x1 - x0));
    const double len = std::hypot(static_cast<double>(x1 - x0),
                                  static_cast<double>(y1 - y0));
    const int tail = static_cast<int>(len) - head;
    if (tail > 0)
        GlyphLine(dc, x0, y0,
                  x1 - static_cast<int>(std::cos(ang) * head),
                  y1 - static_cast<int>(std::sin(ang) * head), w, col);
    GlyphArrowHead(dc, x1, y1, ang, head, col);
}

void GlyphPoly(HDC dc, const POINT* pts, int n, int w, COLORREF col) {
    HPEN p = CreatePen(PS_SOLID, w, col);
    HGDIOBJ op = SelectObject(dc, p);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Polyline(dc, pts, n);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(p);
}

void GlyphArc(HDC dc, int cx, int cy, int r, double a0, double a1, int w,
              COLORREF col) {
    const int n = 24;
    std::vector<POINT> pts;
    pts.reserve(static_cast<size_t>(n + 1));
    for (int i = 0; i <= n; ++i) {
        const double t = a0 + (a1 - a0) * i / n;
        pts.push_back({cx + static_cast<LONG>(std::lround(std::cos(t) * r)),
                       cy + static_cast<LONG>(std::lround(std::sin(t) * r))});
    }
    GlyphPoly(dc, pts.data(), static_cast<int>(pts.size()), w, col);
}

// tool = 插件侧的 liquify.tool 值（"__hand__" / "__zoom__" = UI 工具）
//
// 光学尺寸固定为 Scale(11) 半径，**不随按钮大小变** —— 统一光学尺寸后
// 所有图标才有一致的"重量"。
void DrawToolGlyph(HDC dc, const RECT& rc, const std::string& tool,
                   COLORREF col) {
    const int cx = (rc.left + rc.right) / 2;
    const int cy = (rc.top + rc.bottom) / 2;
    const int s = std::max(9, Scale(12));
    const int w = std::max(2, Scale(2));
    const int rIn = std::max(3, static_cast<int>(s * 0.40));
    const int rArc = static_cast<int>(s * 0.82);
    const int head = std::max(3, Scale(4));

    if (tool == "__hand__") {
        // 抓手 = PS 的"手"形简化为四向移动箭头（比画一只手更耐看）
        GlyphLine(dc, cx - s, cy, cx + s, cy, w, col);
        GlyphLine(dc, cx, cy - s, cx, cy + s, w, col);
        GlyphArrowHead(dc, cx + s, cy, 0.0, head, col);
        GlyphArrowHead(dc, cx - s, cy, 3.1416, head, col);
        GlyphArrowHead(dc, cx, cy - s, -1.5708, head, col);
        GlyphArrowHead(dc, cx, cy + s, 1.5708, head, col);
        return;
    }
    if (tool == "__zoom__") {
        // 放大镜：圆 + 斜柄 + 中间的 "+"
        const int r = std::max(3, static_cast<int>(s * 0.62));
        const int ox = cx - Scale(1), oy = cy - Scale(1);
        GlyphCircle(dc, ox, oy, r, w, col);
        GlyphLine(dc, ox + static_cast<int>(r * 0.72),
                  oy + static_cast<int>(r * 0.72), cx + s, cy + s, w, col);
        GlyphLine(dc, ox - Scale(3), oy, ox + Scale(3), oy, std::max(1, Scale(1)), col);
        GlyphLine(dc, ox, oy - Scale(3), ox, oy + Scale(3), std::max(1, Scale(1)), col);
        return;
    }
    if (tool == "forward-warp") {
        // 向前变形：一段被"推"出来的弧线 + 前进箭头（PS 里是用手指推）
        const int y0 = cy + s / 2, x0 = cx - s;
        const int y1 = cy - s / 3, x1 = cx + s;
        std::vector<POINT> pts;
        for (int i = 0; i <= 10; ++i) {
            const double t = i / 10.0;
            const double u = 1.0 - t;
            const double px = u * u * x0 + 2 * u * t * (x0 + s) + t * t * x1;
            const double py = u * u * y0 + 2 * u * t * (cy - s) + t * t * y1;
            pts.push_back({static_cast<LONG>(std::lround(px)),
                           static_cast<LONG>(std::lround(py))});
        }
        GlyphPoly(dc, pts.data(), static_cast<int>(pts.size()), w, col);
        GlyphArrowHead(dc, x1, y1, -0.35, head, col);
    } else if (tool == "push") {
        // 左推：一根竖线 + 由它向左折出的箭头（垂直推动语义）
        GlyphLine(dc, cx + s / 2, cy - s, cx + s / 2, cy + s, w, col);
        std::vector<POINT> pts = {{cx + s / 2, cy},
                                  {cx + s / 2 - Scale(2), cy + s - Scale(2)},
                                  {cx - s, cy + s - Scale(2)}};
        GlyphPoly(dc, pts.data(), 3, w, col);
        GlyphArrowHead(dc, cx - s, cy + s - Scale(2), 3.1416, head, col);
    } else if (tool == "bloat" || tool == "pinch") {
        // 膨胀 / 褶皱：圆环 + 四条**带实心头**的箭头（外 / 内）。
        // 原来是四个 V 形折线，20px 尺寸下两个工具看起来一模一样（都像雪花）；
        // 换成带三角头的短箭头后，"朝外 / 朝内"一眼就能分。
        GlyphCircle(dc, cx, cy, rIn, w, col);
        const bool out = (tool == "bloat");
        for (int k = 0; k < 4; ++k) {
            const double a = k * 3.14159265 / 2.0 + 0.7853982;
            const int rTail = out ? rIn + Scale(1) : s;
            const int rTip = out ? s : rIn + Scale(1);
            const int tx = cx + static_cast<int>(std::lround(std::cos(a) * rTail));
            const int ty = cy + static_cast<int>(std::lround(std::sin(a) * rTail));
            const int hx = cx + static_cast<int>(std::lround(std::cos(a) * rTip));
            const int hy = cy + static_cast<int>(std::lround(std::sin(a) * rTip));
            GlyphArrow(dc, tx, ty, hx, hy, w, head, col);
        }
    } else if (tool == "twirl-cw" || tool == "twirl-ccw") {
        // 扭曲：3/4 圈弧 + 切向箭头 + 中心点
        const double sgn = (tool == "twirl-cw") ? 1.0 : -1.0;
        const double a0 = sgn > 0 ? -2.35 : -0.75;
        const double a1 = sgn > 0 ? 1.75 : -3.95;
        GlyphArc(dc, cx, cy, rArc, a0, a1, w, col);
        const int ex = cx + static_cast<int>(std::lround(std::cos(a1) * rArc));
        const int ey = cy + static_cast<int>(std::lround(std::sin(a1) * rArc));
        // 切向箭头：比"只画弧"更能读出旋转方向
        GlyphArrow(dc, ex - static_cast<int>(std::lround(std::cos(a1) * head * 1.6)),
                   ey - static_cast<int>(std::lround(std::sin(a1) * head * 1.6)),
                   ex + static_cast<int>(std::lround(std::cos(a1) * head * 0.4)),
                   ey + static_cast<int>(std::lround(std::sin(a1) * head * 0.4)),
                   w, head, col);
        HBRUSH b = CreateSolidBrush(col);
        HGDIOBJ ob = SelectObject(dc, b);
        HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, cx - Scale(1), cy - Scale(1), cx + Scale(1), cy + Scale(1));
        SelectObject(dc, op);
        SelectObject(dc, ob);
        DeleteObject(b);
    } else if (tool == "smooth") {
        // 平滑：一条被抹平的正弦（毛刺 → 顺滑）
        const int n = 20;
        std::vector<POINT> pts;
        for (int i = 0; i <= n; ++i) {
            const double t = static_cast<double>(i) / n;
            pts.push_back({cx - s + static_cast<LONG>(t * 2 * s),
                           cy + static_cast<LONG>(std::lround(
                                    std::sin(t * 6.2832) * s * 0.45))});
        }
        GlyphPoly(dc, pts.data(), static_cast<int>(pts.size()), w, col);
    } else if (tool == "reconstruct") {
        // 重建：双左向尖角（回退 / 还原），和"旋转"的圆弧明显不同
        const int h = static_cast<int>(s * 0.75);
        for (int k = 0; k < 2; ++k) {
            const int x = cx + s / 2 - k * (s / 2 + Scale(1));
            POINT p3[3] = {{x, cy - h}, {x - h, cy}, {x, cy + h}};
            GlyphPoly(dc, p3, 3, w, col);
        }
    } else {
        GlyphCircle(dc, cx, cy, rIn, w, col);
    }
}

// 画左侧图标工具列
void DrawToolColumn(HDC dc, UiWindow& win, const RECT& rail) {
    AppState& s = S();
    g_tipValid = false;
    RoundBox(dc, rail, C_PANEL, C_LINE, RadiusLg());

    const int btn = Scale(40);
    const int gap = Scale(5);
    const int x = rail.left + (RectW(rail) - btn) / 2;
    int y = rail.top + Scale(8);
    const int ci = WindowEffectIndex(win);

    auto drawOne = [&](const std::string& tool, bool active, int hitIndex) {
        RECT rc = {x, y, x + btn, y + btn};
        const bool hover = !active && UiHover(win, rc);
        if (hover) {
            g_tipText = ToolTitle(tool);
            g_tipAnchor = rc;
            g_tipValid = true;
        }
        if (active) {
            RoundBox(dc, rc, MixColor(C_PANEL, C_TEXT, 0.13),
                     MixColor(C_LINE, C_ACCENT, 0.75), RadiusMd());
        } else if (hover) {
            RoundBox(dc, rc, MixColor(C_PANEL, C_TEXT, 0.07), C_PANEL, RadiusMd());
        }
        DrawToolGlyph(dc, rc, tool, active ? C_TEXT : C_DIM);
        UiHit hit;
        hit.rc = rc;
        hit.kind = HK_TOOL;
        hit.index = hitIndex;
        win.hits.push_back(hit);
        y += btn + gap;
    };

    for (int i = 0; i < kRailCount; ++i) {
        const std::string tool = kRailTools[i];
        if (tool == "__hand__") {
            // 抓手 / 放大镜与变形工具分开（PS 也是把这两个单独放）
            y += Scale(4);
            FrameRectC(dc, RECT{rail.left + Scale(8), y, rail.right - Scale(8),
                                y + 1},
                       MixColor(C_LINE, C_PANEL, 0.5));
            y += Scale(8);
            drawOne(tool, win.handTool, kHandToolHit);
            continue;
        }
        if (tool == "__zoom__") {
            drawOne(tool, win.zoomTool, kZoomToolHit);
            continue;
        }
        const int e = EffectByTool(tool.c_str());
        if (e < 0 || !s.core->EffectAvailable(e)) continue; // 没加载的插件不占位
        drawOne(tool, e == ci && !win.handTool && !win.zoomTool, e);
    }
}

// 画右侧参数面板
void DrawLiquifyPanel(HDC dc, UiWindow& win, const UiLayout& L) {
    AppState& s = S();
    const int ci = WindowEffectIndex(win);
    const auto& E = s.core->Effects();
    const std::string tool = win.handTool    ? "__hand__"
                             : win.zoomTool  ? "__zoom__"
                             : ToolOfEffect(ci);
    DrawPanelFrame(dc, L, std::wstring(L"液化 · ") + ToolTitle(tool), L"");

    std::vector<int> rowY;
    int y = L.panel.top + Scale(58);
    if (tool == "__hand__" || tool == "__zoom__") {
        y = DrawSectionLabel(dc, L.panel, y, L"工具选项");
        RECT hr = {L.panel.left + Scale(14), y, L.panel.right - Scale(14),
                   y + Scale(44)};
        TextC(dc, ToolTipText(tool), hr, C_DIM, s.fSmall,
              DT_LEFT | DT_TOP | DT_WORDBREAK);
        SyncParamEdits(win, rowY, L.panel);
        UpdateParamEdits(win);
        return;
    }

    y = DrawSectionLabel(dc, L.panel, y, L"工具选项");
    const auto vals = s.core->ParamValues(win.id);
    if (ci >= 0 && ci < static_cast<int>(E.size())) {
        const std::vector<UiParam>& params = E[ci].params;
        for (size_t i = 0; i < params.size() && i < vals.size(); ++i) {
            rowY.push_back(y);
            y = DrawParamRow(dc, win, L.panel, y, params[i], vals[i],
                             static_cast<int>(i));
        }
    }

    // 重建选项（仅"重建"工具）：PS 里有 模式 + 重建/恢复按钮
    if (tool == "reconstruct") {
        y += Scale(6);
        FrameRectC(dc,
                   RECT{L.panel.left + Scale(14), y - Scale(10),
                        L.panel.right - Scale(14), y - Scale(9)},
                   MixColor(C_LINE, C_PANEL, 0.5));
        y = DrawSectionLabel(dc, L.panel, y, L"重建选项");
        RECT br = {L.panel.left + Scale(14), y, L.panel.right - Scale(14),
                   y + Scale(30)};
        const bool hoverRec = UiHover(win, br);
        RoundBox(dc, br, hoverRec ? MixColor(C_PANEL, C_TEXT, 0.08) : C_PANEL2,
                 C_LINE, RadiusMd());
        TextC(dc, ReconstructLabel(g_reconstructMode), br, C_TEXT, s.fBody,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        UiHit hit;
        hit.rc = br;
        hit.kind = HK_CMD;
        hit.index = 900; // 私有命令：切换重建模式
        win.hits.push_back(hit);
        y += Scale(38);
    }

    // 说明：当前工具的行为（填住面板下方空白，也比纯留白有用）
    {
        const wchar_t* tip = ToolTipText(tool);
        if (tip && *tip) {
            y += Scale(8);
            FrameRectC(dc,
                       RECT{L.panel.left + Scale(14), y - Scale(10),
                            L.panel.right - Scale(14), y - Scale(9)},
                       MixColor(C_LINE, C_PANEL, 0.5));
            y = DrawSectionLabel(dc, L.panel, y, L"说明");
            RECT tr = {L.panel.left + Scale(14), y, L.panel.right - Scale(14),
                       y + Scale(56)};
            TextC(dc, tip, tr, C_DIM, s.fSmall, DT_LEFT | DT_TOP | DT_WORDBREAK);
            y += Scale(64);
        }
    }

    // 面板底部：快捷键提示（只留 3 行，小窗口下不会被裁成半行断字）
    RECT hr = {L.panel.left + Scale(14), L.panel.bottom - Scale(74),
               L.panel.right - Scale(14), L.panel.bottom - Scale(16)};
    TextC(dc, L"按住不放 = 持续施力\n空格 / 中键拖动 = 平移 · 滚轮缩放\n"
              L"Ctrl+Z 撤销上一笔 · [ ] 调笔刷",
          hr, C_DIM, s.fSmall, DT_LEFT | DT_TOP | DT_WORDBREAK);

    SyncParamEdits(win, rowY, L.panel);
    UpdateParamEdits(win);
}

void DrawLiquify(HDC dc, UiWindow& win, const RECT& client) {
    AppState& s = S();
    const std::vector<int> fx = LiquifyEffects();

    UiLayout L = ComputeLayout(client, 300);
    // 顶部不再画效果条：画布上边界直接贴到分类条下沿
    L.canvas.top = L.category.bottom + Scale(12);
    L.effects = {0, 0, 0, 0};

    // 左侧图标列：56px 宽的卡片，按钮 40px 居中
    const int railW = Scale(56);
    const int gap = Scale(12);
    RECT body = {client.left + Scale(16), L.canvas.top, L.canvas.right,
                 L.canvas.bottom};
    RECT rail = {body.left, body.top, body.left + railW, body.bottom};
    win.canvas = {rail.right + gap, body.top, body.right, body.bottom};

    FillRect(dc, &client, s.bgBrush);
    wchar_t head[200], sub[240];
    const ImageBuffer& img = s.core->DisplayImage(BridgeCore::kWinLiquify);
    GrabSubtitleHead(head, 200, img);
    swprintf(sub, 240, L"%s", head);
    DrawWindowChrome(dc, win, L, L"液化", sub, fx, /*showEffects=*/false);
    DrawToolColumn(dc, win, rail);
    DrawCanvas(dc, win, !win.handTool && !win.zoomTool);
    DrawLiquifyPanel(dc, win, L);
    // 悬浮提示画在最后：画布/面板会盖住先画的东西
    if (g_tipValid && !g_tipText.empty()) {
        const int tw = Scale(8) + static_cast<int>(g_tipText.size()) * Scale(13);
        RECT tr = {g_tipAnchor.right + Scale(8),
                   (g_tipAnchor.top + g_tipAnchor.bottom) / 2 - Scale(13),
                   g_tipAnchor.right + Scale(8) + std::max(Scale(64), tw),
                   (g_tipAnchor.top + g_tipAnchor.bottom) / 2 + Scale(13)};
        RoundBox(dc, tr, C_TOOLTIP_BG, MixColor(C_LINE, C_TEXT, 0.18),
                 RadiusSm());
        TextC(dc, g_tipText, tr, C_TOOLTIP_TEXT, s.fSmall,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

bool OnLiquifyHit(UiWindow& win, const UiHit& h) {
    AppState& s = S();
    if (h.kind == HK_TOOL && h.index == kHandToolHit) {
        win.handTool = !win.handTool;
        win.zoomTool = false;
        if (win.handTool) CloseCombo(win);
        InvalidateAll();
        return true;
    }
    if (h.kind == HK_TOOL && h.index == kZoomToolHit) {
        win.zoomTool = !win.zoomTool;
        win.handTool = false;
        InvalidateAll();
        return true;
    }
    if (h.kind == HK_TOOL && h.index >= 0) {
        win.handTool = false; // 选变形工具时退出抓手/缩放模式
        win.zoomTool = false;
        if (!s.core->EffectAvailable(h.index)) {
            win.flash = L"该工具的插件未加载（检查 plugins 目录）";
            return true;
        }
        CloseCombo(win);
        s.core->SetCurrentEffect(h.index, BridgeCore::kWinLiquify);
        win.effectIndex = h.index;
        win.bgraDirty = true;
        return true;
    }
    if (h.kind == HK_CMD && h.index == 900) {
        g_reconstructMode = (g_reconstructMode + 1) % 4;
        return true;
    }
    return false;
}

bool LiquifyMsg(UiWindow& win, UINT msg, WPARAM wp, LPARAM lp) {
    (void)lp;
    if (msg != WM_KEYDOWN) return false;
    switch (wp) {
    case VK_OEM_4: // [
        // Shift+[ ] 调压力（第 2 个参数），默认调笔刷大小（第 1 个参数）
        NudgeParam(win, (GetKeyState(VK_SHIFT) & 0x8000) ? 1 : 0, -0.01f);
        return true;
    case VK_OEM_6: // ]
        NudgeParam(win, (GetKeyState(VK_SHIFT) & 0x8000) ? 1 : 0, 0.01f);
        return true;
    case 'H':
        win.handTool = !win.handTool;
        if (win.handTool) win.zoomTool = false;
        InvalidateRect(win.hwnd, nullptr, FALSE);
        return true;
    case 'Z':
        win.zoomTool = !win.zoomTool;
        if (win.zoomTool) win.handTool = false;
        InvalidateRect(win.hwnd, nullptr, FALSE);
        return true;
    case VK_ESCAPE:
        CloseToolWindow(BridgeCore::kWinLiquify);
        return true;
    default:
        break;
    }
    // Ctrl+Z 由 UiWindow.cpp 的通用键盘分支处理（对所有窗口生效）
    return false;
}

} // namespace

const ToolWindowSpec& LiquifyWindowSpec() {
    static const ToolWindowSpec spec = {
        BridgeCore::kWinLiquify, kClass, L"液化 — SAI2 插件（作用于当前图层）",
        1240, 920, &DrawLiquify, &OnLiquifyHit, &LiquifyMsg};
    return spec;
}

} // namespace ui
} // namespace bridge
} // namespace sai2
