// ============================================================================
// FilterWnd.cpp - 滤镜窗口（缩略图墙 + 参数面板，PS「滤镜库」风格）
//
//   * 左侧：滤镜缩略图（惰性生成：只在进入可见区域时才跑一次插件，
//     避免开窗时一次性跑 10 个滤镜造成长时间卡顿）
//   * 中间：画布实时预览
//   * 右侧：当前滤镜的参数面板
//   * 底部：应用 → 剪贴板 / 撤销 / 复位 / 关闭
// ============================================================================

#include "Ui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

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
const int kThumbDim = 88;
const int kThumbCols = 2;
int g_scroll = 0;          // 缩略图墙滚动（行）
int g_builtCount = 0;      // 已生成的缩略图数（仅用于状态显示）

std::vector<int> FilterEffects() {
    return S().core->EffectsInGroup(BridgeCore::kGroupFilter);
}

int GalleryRowH() { return Scale(kThumbDim) + Scale(24); }

void DrawGallery(HDC dc, UiWindow& win, const RECT& box,
                 const std::vector<int>& fx) {
    AppState& s = S();
    RoundBox(dc, box, C_PANEL, C_LINE, Scale(10));
    RECT tr = {box.left + 8, box.top + 4, box.right - 8, box.top + 24};
    TextC(dc, L"滤镜库", tr, C_DIM, s.fSmall,
          DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    const int ci = WindowEffectIndex(win);
    const int cols = kThumbCols;
    const int cellW = (RectW(box) - Scale(12)) / cols;
    const int rowH = GalleryRowH();
    const int viewTop = box.top + Scale(26);
    const int viewH = box.bottom - viewTop - Scale(4);
    const int rows = (static_cast<int>(fx.size()) + cols - 1) / cols;
    const int maxScroll = std::max(0, rows * rowH - viewH);
    g_scroll = std::max(0, std::min(g_scroll, maxScroll));

    HRGN clip = CreateRectRgn(box.left + 1, viewTop, box.right - 1,
                              box.bottom - 1);
    SelectClipRgn(dc, clip);

    for (size_t i = 0; i < fx.size(); ++i) {
        const int col = static_cast<int>(i) % cols;
        const int row = static_cast<int>(i) / cols;
        const int x = box.left + Scale(6) + col * cellW;
        const int y = viewTop + row * rowH - g_scroll;
        if (y + rowH < viewTop || y > box.bottom) continue; // 不在可见区

        const int e = fx[i];
        RECT img = {x + Scale(4), y + Scale(2), x + cellW - Scale(4),
                    y + Scale(2) + Scale(kThumbDim)};
        RECT lbl = {x + Scale(2), img.bottom, x + cellW - Scale(2),
                    img.bottom + Scale(20)};

        const bool sel = (e == ci);
        const bool avail = s.core->EffectAvailable(e);
        // 缩略图本身保持方角（位图会覆盖圆角内部），仅选中/悬停描边高亮
        FillRectC(dc, img, C_CANVAS);
        FrameRectC(dc, img, sel ? C_ACCENT : (UiHover(win, img) ? C_ACCENT : C_LINE));

        const ImageBuffer* th = avail ? s.core->Thumbnail(e, kThumbDim) : nullptr;
        if (th && th->IsValid()) {
            ImageBuffer small =
                BridgeCore::MakeThumbnail(*th, kThumbDim);
            const int w = std::min(RectW(img) - 2, small.Width());
            const int h = std::min(RectH(img) - 2, small.Height());
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = small.Width();
            bi.bmiHeader.biHeight = -small.Height();
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            // BGRA 转换（缩略图很小，直接内联转换）
            std::vector<uint8_t> px(static_cast<size_t>(small.Width()) *
                                        small.Height() * 4,
                                    0);
            const uint32_t bpc = small.BytesPerChannel();
            for (int yy = 0; yy < small.Height(); ++yy) {
                const uint8_t* src =
                    small.Data() + static_cast<size_t>(yy) * small.Stride();
                uint8_t* dst = px.data() +
                               static_cast<size_t>(yy) * small.Width() * 4;
                for (int xx = 0; xx < small.Width(); ++xx) {
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
                    dst[xx * 4 + 0] = b;
                    dst[xx * 4 + 1] = g;
                    dst[xx * 4 + 2] = r;
                    dst[xx * 4 + 3] = a;
                }
            }
            // ★ 透明底用**棋盘格**合成后再贴。
            //   StretchDIBits + SRCCOPY 会忽略 alpha：透明处的 RGB（通常是 0）
            //   会显示成黑色 → 缩略图看不清（用户反馈"黑底看不清"）。
            //   注意遍历的是**缓冲区尺寸** small.Width()/Height()，不是下面
            //   用于贴图的绘制尺寸 w/h（w/h 可能小于缓冲区）。
            {
                const int cell = 6;             // 棋盘格边长（缩略图小，6px 够）
                const int c1 = 0xFF, c2 = 0xCC; // 两种浅灰
                const int bw = small.Width(), bh = small.Height();
                for (int yy = 0; yy < bh; ++yy) {
                    uint8_t* row =
                        px.data() + static_cast<size_t>(yy) * bw * 4;
                    for (int xx = 0; xx < bw; ++xx) {
                        const int o = xx * 4;   // 缓冲区布局：BGRA
                        const int bg =
                            (((xx / cell) + (yy / cell)) & 1) ? c1 : c2;
                        const int a8 = row[o + 3];
                        for (int k = 0; k < 3; ++k) {
                            const int v = row[o + k];
                            row[o + k] = static_cast<uint8_t>(
                                (v * a8 + bg * (255 - a8)) / 255);
                        }
                        row[o + 3] = 255;
                    }
                }
            }
            const int ox = img.left + 1 + (RectW(img) - 2 - w) / 2;
            const int oy = img.top + 1 + (RectH(img) - 2 - h) / 2;
            SetStretchBltMode(dc, HALFTONE);
            StretchDIBits(dc, ox, oy, w, h, 0, 0, small.Width(), small.Height(),
                          px.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
        } else {
            TextC(dc, avail ? L"…" : L"未加载", img,
                  avail ? C_DIM : C_WARN, s.fSmall,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        TextC(dc, U8(s.core->Effects()[static_cast<size_t>(e)].name), lbl,
              sel ? RGB(255, 255, 255) : (avail ? C_TEXT : C_DIM),
              sel ? s.fBold : s.fSmall,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        UiHit h;
        h.rc = RECT{x, y, x + cellW, y + rowH};
        h.kind = HK_THUMB;
        h.index = e;
        win.hits.push_back(h);
    }
    SelectClipRgn(dc, nullptr);
    DeleteObject(clip);

    // 滚动条
    if (maxScroll > 0) {
        RECT sb = {box.right - Scale(6), viewTop, box.right - 2, box.bottom - 2};
        FillRectC(dc, sb, C_PANEL2);
        const int barH = std::max(Scale(24),
                                  viewH * viewH / std::max(1, rows * rowH));
        const int barY = viewTop + (viewH - barH) * g_scroll /
                                      std::max(1, maxScroll);
        RECT bar = {sb.left, barY, sb.right, barY + barH};
        FillRectC(dc, bar, C_LINE);
    }
}

void DrawFilter(HDC dc, UiWindow& win, const RECT& client) {
    AppState& s = S();
    const std::vector<int> fx =
        s.core->EffectsInGroup(BridgeCore::kGroupFilter);

    // 布局：顶部只留分组按钮条（滤镜用缩略图墙代替横排效果条），
    // 下方三栏 = 缩略图墙 | 画布 | 参数面板
    const int m = Scale(12);
    const int gap = Scale(10);
    const int headerH = Scale(34);
    const int stripH = Scale(30);
    const int cmdH = Scale(34);
    const int galleryW = Scale(232);
    const int panelW = Scale(264);

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
    DrawPanelFrame(dc, L, L"参数 — " + name,
                   L"缩略图按可见区域惰性生成；\n点缩略图切换滤镜，拖动滑块调参。");

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
    }
    SyncParamEdits(win, rowY, L.panel);
    UpdateParamEdits(win);

    DrawCanvas(dc, win, false);
}

bool OnFilterHit(UiWindow& win, const UiHit& h) {
    AppState& s = S();
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
    if (h.kind == HK_PREVIEW || h.kind == HK_NONE) return false;
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
    if (pt.x >= client.right / 3) return false;   // 不在缩略图墙上
    const int delta = GET_WHEEL_DELTA_WPARAM(wp);
    g_scroll -= delta / WHEEL_DELTA * GalleryRowH();
    if (g_scroll < 0) g_scroll = 0;
    InvalidateRect(win.hwnd, nullptr, FALSE);
    return true;
}
void FilterThumbsArrived() { g_builtCount = 0; }

const ToolWindowSpec& FilterWindowSpec() {
    static const ToolWindowSpec spec = {
        BridgeCore::kWinFilter, kClass, L"滤镜库 — SAI2 插件（作用于当前图层）",
        1180, 860, &DrawFilter, &OnFilterHit, &FilterMsg};
    return spec;
}

} // namespace ui
} // namespace bridge
} // namespace sai2
