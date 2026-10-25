// ============================================================================
// FilterWnd.cpp - 滤镜窗口（分类 + 缩略图 + 参数面板，PS「滤镜库」风格）
//
//   * 左栏上：**分类列表**（艺术效果 / 画笔描边 / 扭曲 / 素描 / 风格化 /
//     纹理 / 其它），对应 Photoshop 滤镜库左边那棵分类树；
//   * 左栏下：所选分类的缩略图墙（惰性生成：只在进入可见区域时才跑一次
//     插件，避免开窗时一次性跑几十个滤镜造成长时间卡顿）；
//   * 中间：画布实时预览；
//   * 右侧：当前滤镜的参数（滑杆 / 下拉框 / 开关）；
//   * 底部：应用 → 剪贴板 / 撤销 / 复位 / 关闭。
// ============================================================================

#include "Ui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windowsx.h>

namespace sai2 {
namespace bridge {
namespace ui {

namespace {

const wchar_t* kClass = L"SAI2BridgeFilterWnd";
int g_scroll = 0;     // 缩略图墙滚动（像素）
int g_catScroll = 0;  // 分类列表滚动（像素）

int ThumbDim() { return Scale(84); }
int RowH() { return ThumbDim() + Scale(26); }

std::vector<int> FilterEffects() {
    return S().core->EffectsInGroup(BridgeCore::kGroupFilter);
}

// 把效果按分类归组（分类顺序 = 效果定义顺序 = PS 的滤镜库顺序）
struct CategoryList {
    std::vector<std::string> names;
    std::vector<std::vector<int>> items;
    int indexOf(const std::string& name) const {
        for (size_t i = 0; i < names.size(); ++i)
            if (names[i] == name) return static_cast<int>(i);
        return -1;
    }
};

CategoryList BuildCategories(const std::vector<int>& fx) {
    CategoryList out;
    const auto& E = S().core->Effects();
    for (int e : fx) {
        if (e < 0 || e >= static_cast<int>(E.size())) continue;
        std::string cat = E[static_cast<size_t>(e)].category;
        if (cat.empty()) cat = "其它";
        int at = out.indexOf(cat);
        if (at < 0) {
            out.names.push_back(cat);
            out.items.push_back({});
            at = static_cast<int>(out.names.size()) - 1;
        }
        out.items[static_cast<size_t>(at)].push_back(e);
    }
    return out;
}

// 首次打开时把"当前效果所在分类"默认展开，避免用户面对一棵全折叠的树。
// 只做一次：用户把全部分类都收起后，不能又被这条逻辑顶回来（踩过 ——
// 点一下唯一的展开分类，收起后立刻自动展开，看着像"点了没反应"）。
void EnsureDefaultOpen(UiWindow& win, const CategoryList& cats) {
    if (win.filterOpenInit) return;
    win.filterOpenInit = true;
    const auto& E = S().core->Effects();
    const int ci = WindowEffectIndex(win);
    std::string cat;
    if (ci >= 0 && ci < static_cast<int>(E.size()))
        cat = E[static_cast<size_t>(ci)].category;
    int at = cats.indexOf(cat);
    if (at < 0 && !cats.names.empty()) at = 0;
    if (at >= 0) win.filterOpen.insert(cats.names[static_cast<size_t>(at)]);
}

void DrawThumb(HDC dc, UiWindow& win, const RECT& img, int effect,
               bool avail);

void DrawGallery(HDC dc, UiWindow& win, const RECT& box,
                 const std::vector<int>& fx) {
    AppState& s = S();
    RoundBox(dc, box, C_PANEL, C_LINE, RadiusLg());
    const CategoryList cats = BuildCategories(fx);

    EnsureDefaultOpen(win, cats);

    // ---- 标题 ----
    RECT tr = {box.left + Scale(14), box.top + Scale(6), box.right - Scale(14),
               box.top + Scale(34)};
    TextC(dc, L"滤镜库", tr, C_DIM, s.fSmall,
          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);

    // ---- 折叠树：分类行 + 紧跟其后的缩略图（PS 滤镜库的内联展开） ----
    const int rowH = Scale(26);
    const int viewTop = box.top + Scale(36);
    const int viewBottom = box.bottom - Scale(6);
    const int viewH = std::max(Scale(40), viewBottom - viewTop);

    const int cols = 2;
    const int indent = Scale(18);   // 缩略图相对分类行缩进
    const int cellW = (RectW(box) - Scale(16) - indent) / cols;
    const int thumb = ThumbDim();
    const int cellH = RowH();

    // 内容总高：先算一遍，滚动范围与滚动条都以它为准
    int contentH = Scale(4);
    for (size_t i = 0; i < cats.names.size(); ++i) {
        contentH += rowH;
        if (!win.filterOpen.count(cats.names[i])) continue;
        const int n = static_cast<int>(cats.items[i].size());
        contentH += (n + cols - 1) / cols * cellH + Scale(4);
    }
    const int maxScroll = std::max(0, contentH - viewH);
    g_scroll = std::max(0, std::min(g_scroll, maxScroll));
    win.filterContentH = contentH;
    win.filterViewH = viewH;

    const int savedDc = SaveDC(dc);
    IntersectClipRect(dc, box.left + 1, viewTop, box.right - Scale(2),
                      viewBottom);
    // 滚动区必须先清底：面板底色是在 clip 之外填的，滚动后上一帧的缩略图
    // 边框和文字会残留在没被新内容覆盖的地方（滚一下就看见一层淡线）。
    FillRectC(dc, RECT{box.left + 1, viewTop, box.right - Scale(2), viewBottom},
              C_PANEL);
    const int ci = WindowEffectIndex(win);
    int y = viewTop - g_scroll;
    for (size_t i = 0; i < cats.names.size(); ++i) {
        const bool open = win.filterOpen.count(cats.names[i]) > 0;
        const bool holdsCur =
            (ci >= 0 && std::find(cats.items[i].begin(), cats.items[i].end(),
                                  ci) != cats.items[i].end());
        RECT rc = {box.left + Scale(6), y, box.right - Scale(10), y + rowH};
        const bool hover = UiHover(win, rc);
        if (open) {
            RoundBox(dc, rc, MixColor(C_PANEL, C_TEXT, 0.09),
                     MixColor(C_PANEL, C_TEXT, 0.05), RadiusSm());
        } else if (hover) {
            RoundBox(dc, rc, MixColor(C_PANEL, C_TEXT, 0.05),
                     MixColor(C_PANEL, C_TEXT, 0.05), Scale(6));
        }
        // 展开三角：展开 = 朝下，折叠 = 朝右
        const int ax = rc.left + Scale(10), ay = (rc.top + rc.bottom) / 2;
        HPEN pen = CreatePen(PS_SOLID, std::max(1, Scale(1)), C_DIM);
        HGDIOBJ op = SelectObject(dc, pen);
        if (open) {
            MoveToEx(dc, ax - Scale(2), ay - Scale(2), nullptr);
            LineTo(dc, ax + Scale(3), ay + Scale(2));
            LineTo(dc, ax + Scale(6), ay - Scale(2));
        } else {
            MoveToEx(dc, ax, ay - Scale(4), nullptr);
            LineTo(dc, ax + Scale(4), ay);
            LineTo(dc, ax, ay + Scale(4));
        }
        SelectObject(dc, op);
        DeleteObject(pen);

        RECT nr = {rc.left + Scale(22), rc.top, rc.right - Scale(34), rc.bottom};
        wchar_t cnt[32];
        swprintf(cnt, 32, L"%d", static_cast<int>(cats.items[i].size()));
        TextC(dc, U8(cats.names[i]), nr,
              (open || holdsCur) ? C_TEXT : C_DIM,
              (open || holdsCur) ? s.fBold : s.fBody,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        RECT cr = {rc.right - Scale(34), rc.top, rc.right - Scale(8), rc.bottom};
        TextC(dc, cnt, cr, C_DIM, s.fSmall,
              DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

        UiHit h;
        h.rc = rc;
        h.kind = HK_CAT;
        h.index = static_cast<int>(i);
        win.hits.push_back(h);
        y += rowH;
        if (!open) continue;

        const std::vector<int>& items = cats.items[i];
        // 缩略图区左边界压一条竖线，明确"这些属于上面那个分类"
        const int railX = box.left + Scale(14);
        const int gridTop = y;
        const int gridH =
            (static_cast<int>(items.size()) + cols - 1) / cols * cellH;
        FrameRectC(dc, RECT{railX, gridTop + Scale(2), railX + 1,
                            gridTop + gridH - Scale(2)},
                   MixColor(C_LINE, C_PANEL, 0.4));

        for (size_t k = 0; k < items.size(); ++k) {
            const int col = static_cast<int>(k) % cols;
            const int row = static_cast<int>(k) / cols;
            const int x = box.left + Scale(8) + indent + col * cellW;
            const int yy = y + row * cellH;
            if (yy + cellH < viewTop || yy > viewBottom) continue; // 不可见

            const int e = items[k];
            const bool isSel = (e == ci);
            RECT img = {x + Scale(2), yy, x + Scale(2) + cellW - Scale(8),
                        yy + thumb};
            // 保持正方形缩略图（沿用 PS 的观感）
            const int w = std::min(RectW(img), thumb);
            img.right = img.left + w;
            RECT lbl = {img.left - Scale(2), img.bottom + Scale(2), img.right,
                        img.bottom + Scale(22)};
            if (isSel) {
                RECT hi = {img.left - Scale(3), img.top - Scale(3),
                           img.right + Scale(3), lbl.bottom + Scale(2)};
                RoundBox(dc, hi, MixColor(C_PANEL, C_TEXT, 0.10),
                         MixColor(C_LINE, C_TEXT, 0.25), RadiusSm());
            }
            DrawThumb(dc, win, img, e, S().core->EffectAvailable(e));
            if (isSel) FrameRectC(dc, img, C_ACCENT);

            std::wstring nm = U8(S().core->Effects()[static_cast<size_t>(e)].name);
            const size_t dot = nm.find(L" · ");
            if (dot != std::wstring::npos) nm = nm.substr(0, dot);
            TextC(dc, nm, lbl,
                  S().core->EffectAvailable(e) ? C_TEXT : C_DIM,
                  isSel ? S().fBold : S().fSmall,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            UiHit th;
            th.rc = RECT{x, yy, x + cellW, yy + cellH};
            th.kind = HK_THUMB;
            th.index = e;
            win.hits.push_back(th);
        }
        y += gridH + Scale(4);
    }
    RestoreDC(dc, savedDc);

    // 滚动条：整个折叠树共用一条。
    // 以前轨道用 C_PANEL2、滑块用 C_LINE —— 两个都是"比面板深一点点"的颜色，
    // 深色主题下几乎看不见（用户反馈"不知道这里能滚"）。改成
    // 轨道淡、滑块明显，并把宽度从 4px 加到 6px。
    if (maxScroll > 0) {
        RECT sb = {box.right - Scale(9), viewTop + Scale(2),
                   box.right - Scale(3), viewBottom - Scale(2)};
        RoundBox(dc, sb, MixColor(C_PANEL, C_TEXT, 0.05),
                 MixColor(C_PANEL, C_TEXT, 0.05), Scale(3));
        const int barH = std::max(Scale(24), viewH * viewH /
                                                 std::max(1, contentH));
        const int barY = sb.top + (RectH(sb) - barH) * g_scroll /
                                      std::max(1, maxScroll);
        RECT bar = {sb.left, barY, sb.right, barY + barH};
        RoundBox(dc, bar, MixColor(C_PANEL, C_TEXT, 0.26),
                 MixColor(C_PANEL, C_TEXT, 0.26), Scale(3));
    }
}

// 缩略图（含棋盘格合成，透明区域才不会变成黑块）
void DrawThumb(HDC dc, UiWindow& win, const RECT& img, int effect,
               bool avail) {
    AppState& s = S();
    FillRectC(dc, img, C_CANVAS);
    // 选中态必须是"看得见的一档"：浅色主题下原来只有 C_CANVAS 底，
    // 选中和未选几乎没有区别（用户反馈"不知道现在选的是哪个"）。
    // 现在选中 = 强调色描边 + 两像素外环；悬停 = 强调色细边；其余 = 发丝线。
    const bool selected = (effect == WindowEffectIndex(win));
    if (selected) {
        RECT outer = img;
        InflateRect(&outer, Scale(2), Scale(2));
        RoundBox(dc, outer, MixColor(C_CANVAS, C_ACCENT, 0.12), C_ACCENT,
                 RadiusSm());
        FillRectC(dc, img, C_CANVAS);
    }
    FrameRectC(dc, img, selected ? C_ACCENT
                                 : (UiHover(win, img) ? C_ACCENT : C_LINE));
    const int dim = ThumbDim();
    const ImageBuffer* th = avail ? s.core->Thumbnail(effect, dim) : nullptr;
    if (!th || !th->IsValid()) {
        TextC(dc, avail ? L"…" : L"未加载", img, avail ? C_DIM : C_WARN,
              s.fSmall, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    ImageBuffer small = BridgeCore::MakeThumbnail(*th, dim);
    const int w = std::min(RectW(img) - 2, small.Width());
    const int h = std::min(RectH(img) - 2, small.Height());
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = small.Width();
    bi.bmiHeader.biHeight = -small.Height();
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    std::vector<uint8_t> px(static_cast<size_t>(small.Width()) *
                                small.Height() * 4,
                            0);
    const uint32_t bpc = small.BytesPerChannel();
    const int cell = 6;             // 棋盘格边长
    const int c1 = 0xFF, c2 = 0xCC; // 两种浅灰
    const int bw = small.Width(), bh = small.Height();
    for (int yy = 0; yy < bh; ++yy) {
        const uint8_t* src =
            small.Data() + static_cast<size_t>(yy) * small.Stride();
        uint8_t* row = px.data() + static_cast<size_t>(yy) * bw * 4;
        for (int xx = 0; xx < bw; ++xx) {
            uint8_t r, g, b, a;
            if (bpc == 1) {
                r = src[xx * 4 + 0];
                g = src[xx * 4 + 1];
                b = src[xx * 4 + 2];
                a = src[xx * 4 + 3];
            } else {
                r = src[xx * 8 + 1];
                g = src[xx * 8 + 3];
                b = src[xx * 8 + 5];
                a = src[xx * 8 + 7];
            }
            // 透明底用**棋盘格**合成后再贴：StretchDIBits + SRCCOPY 会忽略
            // alpha，透明处的 RGB（通常是 0）会变成黑色，缩略图就看不清了。
            const int bg = (((xx / cell) + (yy / cell)) & 1) ? c1 : c2;
            const int o = xx * 4;
            row[o + 0] = static_cast<uint8_t>((b * a + bg * (255 - a)) / 255);
            row[o + 1] = static_cast<uint8_t>((g * a + bg * (255 - a)) / 255);
            row[o + 2] = static_cast<uint8_t>((r * a + bg * (255 - a)) / 255);
            row[o + 3] = 255;
        }
    }
    const int ox = img.left + 1 + (RectW(img) - 2 - w) / 2;
    const int oy = img.top + 1 + (RectH(img) - 2 - h) / 2;
    SetStretchBltMode(dc, HALFTONE);
    StretchDIBits(dc, ox, oy, w, h, 0, 0, small.Width(), small.Height(),
                  px.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
}

void DrawFilter(HDC dc, UiWindow& win, const RECT& client) {
    AppState& s = S();
    const std::vector<int> fx =
        s.core->EffectsInGroup(BridgeCore::kGroupFilter);

    // 布局：顶部只留分组按钮条（滤镜用缩略图墙代替横排效果条），
    // 下方三栏 = 分类+缩略图 | 画布 | 参数面板
    const int m = Scale(12);
    const int gap = Scale(10);
    const int headerH = Scale(34);
    const int stripH = Scale(30);
    const int cmdH = Scale(34);
    const int galleryW = Scale(268);
    const int panelW = Scale(276);

    UiLayout L;
    L.header = {0, 0, client.right, headerH};
    L.category = {m, headerH + gap, client.right - m, headerH + gap + stripH};
    L.effects = {0, 0, 0, 0};
    L.commands = {m, client.bottom - m - cmdH, client.right - m,
                  client.bottom - m};
    RECT gallery = {m, L.category.bottom + gap, m + galleryW,
                    L.commands.top - gap};
    L.panel = {client.right - m - panelW, gallery.top, client.right - m,
               gallery.bottom};
    L.canvas = {gallery.right + gap, gallery.top, L.panel.left - gap,
                gallery.bottom};
    win.canvas = L.canvas;

    FillRect(dc, &client, s.bgBrush);
    const ImageBuffer& img = s.core->DisplayImage(BridgeCore::kWinFilter);
    const int ci = WindowEffectIndex(win);
    const auto& E = s.core->Effects();
    wchar_t head[200], sub[240];
    GrabSubtitleHead(head, 200, img);
    swprintf(sub, 240, L"%s   预览 %.0f ms", head, s.core->LastPreviewMs());
    DrawWindowChrome(dc, win, L, L"滤镜库（Filter Gallery）", sub, fx);
    DrawGallery(dc, win, gallery, fx);

    const std::wstring name =
        (ci >= 0 && ci < static_cast<int>(E.size())) ? U8(E[ci].name) : L"-";
    const std::wstring cat =
        (ci >= 0 && ci < static_cast<int>(E.size())) ? U8(E[ci].category)
                                                     : std::wstring();
    // 分类放在标题行右端（小字），不再孤零零钉在面板底部
    DrawPanelFrame(dc, L, L"参数 — " + name, L"",
                   cat.empty() ? std::wstring() : (L"分类：" + cat));

    std::vector<int> rowY;
    int y = L.panel.top + Scale(52);
    const auto vals = s.core->ParamValues(win.id);
    if (ci >= 0 && ci < static_cast<int>(E.size())) {
        const std::vector<UiParam>& params = E[ci].params;
        int lastGroup = -1;
        for (size_t i = 0; i < params.size() && i < vals.size(); ++i) {
            rowY.push_back(y);
            (void)lastGroup;
            y = DrawParamRow(dc, win, L.panel, y, params[i], vals[i],
                             static_cast<int>(i));
        }
        if (params.empty()) {
            RECT hr = {L.panel.left + Scale(14), y, L.panel.right - Scale(14),
                       y + Scale(40)};
            TextC(dc, L"该滤镜没有可调参数（点「应用」即生效）", hr, C_DIM,
                  s.fSmall, DT_LEFT | DT_TOP | DT_WORDBREAK);
        }
    }
    SyncParamEdits(win, rowY, L.panel);
    UpdateParamEdits(win);

    DrawCanvas(dc, win, false);
}

bool OnFilterHit(UiWindow& win, const UiHit& h) {
    AppState& s = S();
    if (h.kind == HK_CAT) {
        const auto fx = s.core->EffectsInGroup(BridgeCore::kGroupFilter);
        const CategoryList cats = BuildCategories(fx);
        if (h.index >= 0 && h.index < static_cast<int>(cats.names.size())) {
            // 折叠树：点一下展开 / 再点收起。展开的内容就长在这个分类行
            // 下面（以前是"分类列表 + 底下一个大缩略图墙"，点谁都一样，
            // 展开的东西永远在最下面，用户看不出点的是哪个文件夹）。
            const std::string& name = cats.names[static_cast<size_t>(h.index)];
            if (win.filterOpen.count(name)) {
                win.filterOpen.erase(name);
            } else {
                win.filterOpen.insert(name);
                // 展开后保证这个分类的名字在可视区内（别展开了却看不见）
                if (win.filterContentH > win.filterViewH) {
                    const int perRow = RowH();
                    int top = 0;
                    for (int i = 0; i < h.index; ++i) {
                        top += Scale(26);
                        if (win.filterOpen.count(
                                cats.names[static_cast<size_t>(i)]))
                            top += (static_cast<int>(cats.items[static_cast<size_t>(i)].size()) + 1) /
                                       2 * perRow + Scale(4);
                    }
                    if (top < g_scroll) g_scroll = top;
                }
            }
            InvalidateRect(win.hwnd, nullptr, FALSE);
        }
        return true;
    }
    if (h.kind == HK_THUMB && h.index >= 0) {
        if (!s.core->EffectAvailable(h.index)) {
            win.flash = L"该滤镜的插件未加载";
            return true;
        }
        s.core->SetCurrentEffect(h.index, BridgeCore::kWinFilter);
        win.effectIndex = h.index;
        win.bgraDirty = true;
        return true;
    }
    return false;
}

bool FilterMsg(UiWindow& win, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_MOUSEWHEEL) return FilterWheelScroll(win, wp, lp);
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) {
        CloseToolWindow(BridgeCore::kWinFilter);
        return true;
    }
    return false;
}

} // namespace

// 缩略图墙的滚轮滚动。抽成外部可见的函数是因为 UiWindow 的 WndProc 会先
// 自己处理 WM_MOUSEWHEEL（预览缩放）并无条件 return —— 窗口自己的 Msg
// 根本收不到滚轮，导致"滤镜库左边栏不能下拉"（用户反馈）。现在 WndProc
// 在缩放之前先调用本函数，光标在左栏时交给它滚动列表。
bool FilterWheelScroll(UiWindow& win, WPARAM wp, LPARAM lp) {
    POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    ScreenToClient(win.hwnd, &pt);
    RECT client;
    GetClientRect(win.hwnd, &client);
    const int galleryW = Scale(268) + Scale(24);
    if (pt.x > galleryW) return false;   // 不在左栏上
    const int delta = GET_WHEEL_DELTA_WPARAM(wp);
    g_scroll -= delta / WHEEL_DELTA * RowH();
    const int maxScroll = std::max(0, win.filterContentH - win.filterViewH);
    if (g_scroll < 0) g_scroll = 0;
    if (g_scroll > maxScroll) g_scroll = maxScroll;
    InvalidateRect(win.hwnd, nullptr, FALSE);
    return true;
}
void FilterThumbsArrived() { g_scroll = g_scroll; }

const ToolWindowSpec& FilterWindowSpec() {
    static const ToolWindowSpec spec = {
        BridgeCore::kWinFilter, kClass, L"滤镜库 — SAI2 插件（作用于当前图层）",
        1240, 860, &DrawFilter, &OnFilterHit, &FilterMsg};
    return spec;
}

} // namespace ui
} // namespace bridge
} // namespace sai2
