// ============================================================================
// LiquifyWnd.cpp - 液化窗口（PS 风格独立窗口）
//
// 与 PS 的液化对话框对应：
//   * 左侧竖排 7 个工具：向前变形 / 膨胀 / 收缩 / 顺时针旋转 / 逆时针旋转 /
//     平滑 / 重建
//   * 右侧参数面板：笔刷大小、强度/角度（随工具而变）、重建模式
//   * 中间画布：按住拖动即可涂抹（逐笔累积），抓手工具/空格+拖/中键拖/右键拖
//     平移，滚轮缩放，H 切换抓手，[ ] 调笔刷大小
//   * 底部：应用 → 剪贴板（自动粘贴回 SAI2）/ 撤销 / 复位 / 关闭
//
// 关键实现：笔刷几何按归一化坐标下发，插件按"归一化 × 图像尺寸"求半径，
// 因此置换场始终在**全分辨率坐标**下累积 —— 窗口里看到的形变与提交结果一致。
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

// UI 层的"抓手（平移）"工具：不属于插件效果，用特殊命中下标区分
constexpr int kHandToolHit = -2;

std::vector<int> LiquifyEffects() {
    return S().core->EffectsInGroup(BridgeCore::kGroupLiquify);
}

// 当前工具在分组里的序号（-1 = 没找到）
int CurrentToolSlot(const std::vector<int>& fx) {
    const int ci = S().core->CurrentEffect();
    for (size_t i = 0; i < fx.size(); ++i)
        if (fx[i] == ci) return static_cast<int>(i);
    return -1;
}

const wchar_t* ReconstructLabel(int mode) {
    switch (mode) {
    case 0: return L"恢复全部";
    case 1: return L"恢复平滑";
    case 2: return L"恢复锐利";
    default: return L"恢复边界";
    }
}

// 把参数值按 step 调整并刷新
void NudgeParam(int index, float step) {
    BridgeCore& core = *S().core;
    const int ci = core.CurrentEffect();
    const auto& fx = core.Effects();
    if (ci < 0 || ci >= static_cast<int>(fx.size())) return;
    const auto& ps = fx[static_cast<size_t>(ci)].params;
    if (index < 0 || index >= static_cast<int>(ps.size())) return;
    const auto& vals = core.ParamValues();
    if (index >= static_cast<int>(vals.size())) return;
    float v = vals[static_cast<size_t>(index)] + step;
    core.SetParamValue(static_cast<size_t>(index), v);
    core.ParamChanged(BridgeCore::kWinLiquify);
    for (int i = 1; i < BridgeCore::kWinCount; ++i)
        S().toolWindows[i].bgraDirty = true;
    InvalidateAll();
}

// 画左侧竖排工具条
void DrawToolColumn(HDC dc, UiWindow& win, const RECT& strip,
                    const std::vector<int>& fx) {
    AppState& s = S();
    FillRectC(dc, strip, C_PANEL);
    FrameRectC(dc, strip, C_LINE);
    RECT tr = {strip.left + 8, strip.top + 4, strip.right - 8, strip.top + 24};
    TextC(dc, L"工具", tr, C_DIM, s.fSmall,
          DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    const int ci = WindowEffectIndex(win);
    const int y0 = strip.top + 26;
    const int h = Scale(30);
    const int gap = Scale(4);
    int y = y0;
    for (size_t i = 0; i < fx.size(); ++i) {
        const int e = fx[i];
        RECT rc = {strip.left + 4, y, strip.right - 4, y + h};
        const bool active = (e == ci);
        const bool hover = !active && UiHover(win, rc);
        RoundBox(dc, rc, active ? C_ACCENT2 : (hover ? C_PANEL : C_PANEL2),
                 active ? C_ACCENT : (hover ? C_ACCENT : C_LINE), Scale(6));
        RECT tx = rc;
        tx.left += 8;
        TextC(dc, U8(s.core->Effects()[static_cast<size_t>(e)].name), tx,
              active ? RGB(255, 255, 255) : C_TEXT,
              active ? s.fBold : s.fBody,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        UiHit hit;
        hit.rc = rc;
        hit.kind = HK_TOOL;
        hit.index = e;
        win.hits.push_back(hit);
        y += h + gap;
    }

    // 抓手（平移）工具：激活后左键拖动 = 平移画布（不影响涂抹工具的选择，
    // 点任意变形工具即退出抓手模式；H 键也能切换）
    {
        RECT rc = {strip.left + 4, y, strip.right - 4, y + h};
        const bool active = win.handTool;
        const bool hover = !active && UiHover(win, rc);
        RoundBox(dc, rc, active ? C_ACCENT2 : (hover ? C_PANEL : C_PANEL2),
                 active ? C_ACCENT : (hover ? C_ACCENT : C_LINE), Scale(6));
        RECT tx = rc;
        tx.left += 8;
        TextC(dc, L"抓手（平移）  H", tx,
              active ? RGB(255, 255, 255) : C_TEXT,
              active ? s.fBold : s.fBody,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        UiHit hit;
        hit.rc = rc;
        hit.kind = HK_TOOL;
        hit.index = kHandToolHit;
        win.hits.push_back(hit);
    }

    // 底部提示
    RECT hr = {strip.left + 6, strip.bottom - Scale(74), strip.right - 6,
               strip.bottom - 6};
    TextC(dc, L"画布内按住拖动涂抹\n抓手/空格+拖/中键拖 = 平移\n滚轮缩放 · [ ] 调笔刷\nCtrl+Z 撤销上一笔",
          hr, C_DIM, s.fSmall, DT_LEFT | DT_TOP | DT_WORDBREAK);
}

// 画右侧参数面板
void DrawLiquifyPanel(HDC dc, UiWindow& win, const UiLayout& L,
                      const std::vector<int>& fx) {
    AppState& s = S();
    const int slot = CurrentToolSlot(fx);
    const int ci = WindowEffectIndex(win);
    const auto& E = s.core->Effects();
    std::wstring toolName = win.handTool
                                ? L"抓手"
                                : ((ci >= 0 && ci < static_cast<int>(E.size()))
                                       ? U8(E[ci].name)
                                       : L"-");
    DrawPanelFrame(dc, L, L"液化 — " + toolName,
                   slot >= 0 ? L"在画布上涂抹即实时变形；\n「应用」后自动粘贴回 SAI2。"
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
    }

    // 重建模式（仅"重建"工具）
    if (slot >= 0 && ci >= 0 && ci < static_cast<int>(E.size()) &&
        E[ci].id == "liquify.reconstruct") {
        RECT lr = {L.panel.left + 12, y, L.panel.right - 12, y + 18};
        TextC(dc, L"重建模式", lr, C_DIM, s.fSmall,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        RECT br = {L.panel.left + 12, y + 22, L.panel.right - 12, y + 48};
        const bool hoverRec = UiHover(win, br);
        RoundBox(dc, br, hoverRec ? C_PANEL : C_PANEL2, C_LINE, Scale(5));
        TextC(dc, ReconstructLabel(g_reconstructMode), br, C_TEXT, s.fBody,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        UiHit hit;
        hit.rc = br;
        hit.kind = HK_CMD;
        hit.index = 900; // 私有命令：切换重建模式
        win.hits.push_back(hit);
        y += 56;
    }

    // 参数之下：只保留笔刷尺寸文字。
    // （原来的笔刷大小示意圈已去掉 —— 大图上圈的半径会超出面板宽度，画出框外。）
    {
        const float rnorm = s.core->StrokeRadiusNorm();
        wchar_t buf[96];
        swprintf(buf, 96, L"笔刷 半径 %.1f%%（图像最短边）", rnorm * 100.0f);
        RECT cr = {L.panel.left + 10, L.panel.bottom - Scale(76),
                   L.panel.right - 10, L.panel.bottom - Scale(58)};
        TextC(dc, buf, cr, C_DIM, s.fSmall,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    SyncParamEdits(win, rowY, L.panel);
    UpdateParamEdits(win);
}

void DrawLiquify(HDC dc, UiWindow& win, const RECT& client) {
    AppState& s = S();
    const std::vector<int> fx = LiquifyEffects();

    const UiLayout L = ComputeLayout(client, 264);
    // 液化：左侧另开一列工具
    const int toolW = Scale(132);
    const int gap = Scale(10);
    RECT body = {client.left + Scale(12), L.canvas.top, L.canvas.right,
                 L.canvas.bottom};
    RECT toolCol = {body.left, body.top, body.left + toolW, body.bottom};
    win.canvas = {toolCol.right + gap, body.top, body.right, body.bottom};

    FillRect(dc, &client, s.bgBrush);
    wchar_t head[200], sub[240];
    const ImageBuffer& img = s.core->DisplayImage(BridgeCore::kWinLiquify);
    GrabSubtitleHead(head, 200, img);
    swprintf(sub, 240, L"%s   液化工具 %d 个", head, static_cast<int>(fx.size()));
    DrawWindowChrome(dc, win, L, L"液化（Liquify）", sub, fx);
    DrawToolColumn(dc, win, toolCol, fx);
    DrawCanvas(dc, win, true);
    DrawLiquifyPanel(dc, win, L, fx);
}

bool OnLiquifyHit(UiWindow& win, const UiHit& h) {
    AppState& s = S();
    if (h.kind == HK_TOOL && h.index == kHandToolHit) {
        win.handTool = !win.handTool;
        InvalidateAll();
        return true;
    }
    if (h.kind == HK_TOOL && h.index >= 0) {
        win.handTool = false; // 选变形工具时退出抓手模式
        if (!s.core->EffectAvailable(h.index)) {
            win.flash = L"该工具的插件未加载（检查 plugins 目录）";
            return true;
        }
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
        // Shift+[ ] 调强度（第 2 个参数），默认调笔刷大小（第 1 个参数）
        NudgeParam((GetKeyState(VK_SHIFT) & 0x8000) ? 1 : 0, -0.01f);
        return true;
    case VK_OEM_6: // ]
        NudgeParam((GetKeyState(VK_SHIFT) & 0x8000) ? 1 : 0, 0.01f);
        return true;
    case 'H':
        // 抓手工具切换（H = Hand）
        win.handTool = !win.handTool;
        InvalidateRect(win.hwnd, nullptr, FALSE);
        return true;
    case VK_ESCAPE:
        CloseToolWindow(BridgeCore::kWinLiquify);
        return true;
    case 'Z':
        if (GetKeyState(VK_CONTROL) & 0x8000) {
            // 液化窗口里 Ctrl+Z = 撤销上一笔（置换场快照回退）
            S().core->UndoStroke(BridgeCore::kWinLiquify);
            return true;
        }
        break;
    default:
        break;
    }
    return false;
}

} // namespace

const ToolWindowSpec& LiquifyWindowSpec() {
    static const ToolWindowSpec spec = {
        BridgeCore::kWinLiquify, kClass, L"液化 — SAI2 插件（作用于当前图层）",
        1200, 900, &DrawLiquify, &OnLiquifyHit, &LiquifyMsg};
    return spec;
}

} // namespace ui
} // namespace bridge
} // namespace sai2
