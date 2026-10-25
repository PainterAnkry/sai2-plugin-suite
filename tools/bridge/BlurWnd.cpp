// ============================================================================
// BlurWnd.cpp - 模糊窗口（独立窗口，4 种模糊各自一组参数）
//
// 高斯 / 方框 / 动感 / 径向：点顶部按钮切换，参数面板随效果变化，
// 画布实时预览（降采样 + 像素半径按倍率缩放，拖动流畅）。
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

const wchar_t* kClass = L"SAI2BridgeBlurWnd";

const wchar_t* BlurHint(const std::string& id) {
    if (id == "blur.gaussian") return L"可分离高斯卷积，预览走 DX11 计算着色器。";
    if (id == "blur.box") return L"方框模糊（均值滤波），边缘比高斯更硬。";
    if (id == "blur.motion") return L"动感模糊：按角度方向线性拖影。";
    if (id == "blur.radial") return L"径向模糊：从中心向外缩放拖影。";
    return L"";
}

void DrawBlur(HDC dc, UiWindow& win, const RECT& client) {
    AppState& s = S();
    const std::vector<int> fx = s.core->EffectsInGroup(BridgeCore::kGroupBlur);

    const UiLayout L = ComputeLayout(client, 268);
    win.canvas = L.canvas;

    FillRect(dc, &client, s.bgBrush);
    const ImageBuffer& img = s.core->DisplayImage(BridgeCore::kWinBlur);
    const int ci = WindowEffectIndex(win);
    if (getenv("SAI2_UI_DEBUG"))
        s.core->Log("drawblur ci=" + std::to_string(ci) + " win.effectIndex=" +
                    std::to_string(win.effectIndex) + " core.current=" +
                    std::to_string(s.core->CurrentEffect()));
    const auto& E = s.core->Effects();
    wchar_t head[200], sub[240];
    GrabSubtitleHead(head, 200, img);
    swprintf(sub, 240, L"%s   预览 %.0f ms", head, s.core->LastPreviewMs());
    DrawWindowChrome(dc, win, L, L"模糊（Blur）", sub, fx);

    const std::wstring name =
        (ci >= 0 && ci < static_cast<int>(E.size())) ? U8(E[ci].name) : L"-";
    DrawPanelFrame(dc, L, L"参数 — " + name,
                   (ci >= 0 && ci < static_cast<int>(E.size()))
                       ? BlurHint(E[ci].id)
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
            TextC(dc, L"该效果没有可调参数", hr, C_DIM, s.fSmall,
                  DT_LEFT | DT_TOP | DT_WORDBREAK);
        }
    }
    SyncParamEdits(win, rowY, L.panel);
    UpdateParamEdits(win);

    DrawCanvas(dc, win, false);
}

bool OnBlurHit(UiWindow& win, const UiHit& h) {
    AppState& s = S();
    if (h.kind == HK_TOOL && h.index >= 0) {
        if (!s.core->EffectAvailable(h.index)) {
            win.flash = L"该效果的插件未加载";
            return true;
        }
        s.core->SetCurrentEffect(h.index, BridgeCore::kWinBlur);
        win.effectIndex = h.index;
        win.bgraDirty = true;
        return true;
    }
    return false;
}

bool BlurMsg(UiWindow& win, UINT msg, WPARAM wp, LPARAM lp) {
    (void)win;
    (void)lp;
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) {
        CloseToolWindow(BridgeCore::kWinBlur);
        return true;
    }
    return false;
}

} // namespace

const ToolWindowSpec& BlurWindowSpec() {
    static const ToolWindowSpec spec = {
        BridgeCore::kWinBlur, kClass, L"模糊 — SAI2 插件（作用于当前图层）",
        1080, 780, &DrawBlur, &OnBlurHit, &BlurMsg};
    return spec;
}

} // namespace ui
} // namespace bridge
} // namespace sai2
