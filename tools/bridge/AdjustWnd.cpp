// ============================================================================
// AdjustWnd.cpp - 调整窗口（PS 风格：反相 / 去色 / 亮度对比度 / 色相饱和度 / 色阶）
//
// 与模糊窗口同构：顶部按钮切换调整项，右侧参数面板，画布实时预览。
// ============================================================================

#include "Ui.h"

#include <algorithm>
#include <cstdio>

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace sai2 {
namespace bridge {
namespace ui {

namespace {

const wchar_t* kClass = L"SAI2BridgeAdjustWnd";

const wchar_t* AdjustHint(const std::string& id) {
    if (id == "adjust.invert") return L"反相：每个通道取补（含 alpha 保持）。";
    if (id == "adjust.grayscale") return L"去色：按亮度权重转灰度。";
    if (id == "adjust.brightness-contrast")
        return L"亮度/对比度：以中灰为支点缩放对比度。";
    if (id == "adjust.hue-saturation")
        return L"色相/饱和度/明度（HSL 空间，0 = 不变）。";
    if (id == "adjust.levels")
        return L"色阶：输入黑场/白场 + Gamma，255 级映射。";
    return L"";
}

void DrawAdjust(HDC dc, UiWindow& win, const RECT& client) {
    AppState& s = S();
    const std::vector<int> fx =
        s.core->EffectsInGroup(BridgeCore::kGroupAdjust);

    const UiLayout L = ComputeLayout(client, 268);
    win.canvas = L.canvas;

    FillRect(dc, &client, s.bgBrush);
    const ImageBuffer& img = s.core->DisplayImage(BridgeCore::kWinAdjust);
    const int ci = WindowEffectIndex(win);
    const auto& E = s.core->Effects();
    wchar_t head[200], sub[240];
    GrabSubtitleHead(head, 200, img);
    swprintf(sub, 240, L"%s   预览 %.0f ms", head, s.core->LastPreviewMs());
    DrawWindowChrome(dc, win, L, L"调整（Adjust）", sub, fx);

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

bool OnAdjustHit(UiWindow& win, const UiHit& h) {
    AppState& s = S();
    if (h.kind == HK_TOOL && h.index >= 0) {
        if (!s.core->EffectAvailable(h.index)) {
            win.flash = L"该调整的插件未加载";
            return true;
        }
        s.core->SetCurrentEffect(h.index, BridgeCore::kWinAdjust);
        win.effectIndex = h.index;
        win.bgraDirty = true;
        return true;
    }
    return false;
}

bool AdjustMsg(UiWindow& win, UINT msg, WPARAM wp, LPARAM lp) {
    (void)win;
    (void)lp;
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) {
        CloseToolWindow(BridgeCore::kWinAdjust);
        return true;
    }
    return false;
}

} // namespace

const ToolWindowSpec& AdjustWindowSpec() {
    static const ToolWindowSpec spec = {
        BridgeCore::kWinAdjust, kClass, L"调整 — SAI2 插件（作用于当前图层）",
        1120, 780, &DrawAdjust, &OnAdjustHit, &AdjustMsg};
    return spec;
}

} // namespace ui
} // namespace bridge
} // namespace sai2
