// ============================================================================
// BridgeApp.cpp - SAI2 插件面板（主窗口）：抓图 + 图层 + 功能入口
//
// 界面分工（本次重做）：
//   主面板只做"素材与入口"——预览当前抓到的图层、列出 SAI2 图层、
//   四个功能按钮（模糊 / 滤镜 / 调整 / 液化）分别打开**各自独立的功能窗口**；
//   参数调节与涂抹都在对应窗口里完成，不再挤在同一个面板里共用一块小区域。
//
// 与 SAI2 的交互（SAI2 无插件 SDK，走剪贴板 + 注入代理）：
//   1. 在 SAI2 里框选 Ctrl+C，或直接点某个功能按钮 —— 面板会自动选中并抓取
//      SAI2 里的当前图层，抓到后自动打开对应功能窗口（PS 那样的"点液化就开
//      液化窗口"）
//   2. 功能窗口里调参/涂抹 → 应用 → 回写剪贴板 → 自动切回 SAI2 粘贴
//
// 抓取流程放在后台线程（"捕获服务"）里跑：SAI2 的菜单自动化要连续做
// 点击/等待（约 1.2 秒），留在 UI 线程会让面板整段时间不响应。
// ============================================================================

#include "Ui.h"
#include "Diag.h"

#include "BmpIO.h"
#include "ClipboardImage.h"
#include "SelfTest.h"
#include "AgentBridge.h"
#include "Demo.h"

#include <algorithm>
#include <cwchar>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <io.h>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
    #define NOMINMAX
#endif
#ifndef _WIN32_WINNT
    #define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#include <shellapi.h>
#include <windowsx.h>
#include <tlhelp32.h>

namespace sai2 {
namespace bridge {
namespace ui {

namespace {

// ---------------------------------------------------------------------------
// 主面板布局
// ---------------------------------------------------------------------------
// 设计稿 760x950。窗口可自由缩放，布局跟随客户区变化：
//   * 横向：加宽量分给 图层列表(左列) 与 选项/按钮(右列)，预览/状态占满宽；
//   * 纵向：加高量的 30% 给预览、50% 给状态日志区、剩余给图层列表；
//   * 缩小时由 WM_GETMINMAXINFO 限制最小尺寸（Scale(580)xScale(720)），
//     布局内部再用 std::max 兜底，避免极端尺寸下出现负高度矩形。
int MainW() { return Scale(760); }
int MainH() { return Scale(950); }

struct MainLayout {
    RECT preview, category, layerHead, layerList, layerBtns, checks, buttons,
        status;
};

MainLayout MainLayoutOf(const RECT& client) {
    const int cw = std::max(Scale(560),
                            static_cast<int>(client.right) - Scale(24));
    const int ch = std::max(Scale(720), static_cast<int>(client.bottom));
    const int vExtra = ch - Scale(950); // 纵向富余（可为负）

    MainLayout L{};
    // 状态日志区（底部锚定）：吃 50% 纵向富余
    const int statusH = std::max(Scale(110), Scale(182) + vExtra / 2);
    const int statusTop = ch - Scale(14) - statusH;
    L.status = {Scale(12), statusTop, client.right - Scale(12), ch - Scale(14)};

    // 预览（顶部锚定链）：高度吃 30% 纵向富余，最小 140
    const int previewBottom =
        std::max(Scale(46) + Scale(140), Scale(410) + vExtra * 3 / 10);
    L.preview = {Scale(12), Scale(46), client.right - Scale(12), previewBottom};
    // 功能按钮条
    L.category = {Scale(12), previewBottom + Scale(32), client.right - Scale(12),
                  previewBottom + Scale(66)};
    // 左列（图层）与右列（选项/按钮）按设计稿比例分宽（458 : 474 起）
    const int leftColRight = Scale(12) + cw * 458 / 736;
    const int rightColLeft = Scale(12) + cw * 474 / 736;
    // 图层表头/列表/按钮条：列表吃剩余空间，底边贴图层按钮条
    L.layerHead = {Scale(12), L.category.bottom + Scale(12), leftColRight,
                   L.category.bottom + Scale(36)};
    L.layerList = {Scale(12), L.layerHead.bottom + Scale(2), leftColRight,
                   std::max(static_cast<int>(L.layerHead.bottom) + Scale(102),
                            statusTop - Scale(48))};
    L.layerBtns = {Scale(12), statusTop - Scale(40), leftColRight,
                   statusTop - Scale(12)};
    // 右列：复选框在表头右侧；两行命令按钮底部与图层按钮条对齐
    const int row1Top = (statusTop - Scale(12)) - Scale(74);
    L.checks = {rightColLeft, L.layerHead.top, client.right - Scale(12),
                std::min(static_cast<int>(L.layerHead.top) + Scale(150),
                         row1Top - Scale(8))};
    L.buttons = {rightColLeft, L.checks.bottom + Scale(30),
                 client.right - Scale(12), statusTop - Scale(12)};
    return L;
}

enum LayerBtnId { LBTN_INJECT = 0, LBTN_REFRESH, LBTN_GRABSEL, LBTN_COUNT };

// 主面板私有命令
enum {
    CMD_LAYER_ROW = 1000,   // +i  点击图层行
    CMD_LAYER_BTN = 2000,   // +i  图层操作按钮
    CMD_WATCH = 910,        // 监听剪贴板
    CMD_AUTOLAYER = 911,    // 点图层行时在 SAI2 选中
    CMD_MERGE = 912,        // 合并回原层
    CMD_INPLACE = 913       // 精确回贴（补边对齐）
};

int g_listScroll = 0;
int g_uiTestSeconds = 0; // --uitest 模式：跳过图层看门狗，保留模拟图层供探针截图

// 图层列表滚动条：把鼠标 y 映射为滚动位置（拖动滑块时用）。返回是否已处理。
bool ListScrollAt(UiWindow& win, int my) {
    AppState& s = S();
    const int total = static_cast<int>(s.layerNames.size());
    RECT client;
    GetClientRect(win.hwnd, &client);
    const RECT list = MainLayoutOf(client).layerList;
    if (total == 0 || RectH(list) <= 0) return false;
    const int rowH = Scale(22);
    const int viewH = RectH(list) - Scale(4);
    const int maxScroll = std::max(0, (total * rowH - viewH) / std::max(1, rowH));
    if (maxScroll <= 0) return false;
    const RECT& track = win.scrollTrack;
    const int trackH = RectH(track);
    const int thumbH = std::max(Scale(18),
                                std::min(trackH, trackH * viewH / (total * rowH)));
    const int span = std::max(1, trackH - thumbH);
    double t = (double)(my - track.top - thumbH / 2) / span;
    t = std::max(0.0, std::min(1.0, t));
    const int sc = static_cast<int>(t * maxScroll + 0.5);
    if (sc != g_listScroll) {
        g_listScroll = sc;
        InvalidateRect(win.hwnd, nullptr, FALSE);
    }
    return true;
}
const UINT WM_CAPTURE_DONE = WM_APP + 10;

const wchar_t* kWndClass = L"SAI2BridgeMain";

// ---------------------------------------------------------------------------
// 主面板绘制
// ---------------------------------------------------------------------------
void DrawCheck(HDC dc, UiWindow& win, const RECT& rc, const std::wstring& text,
               bool on, int cmd) {
    AppState& s = S();
    RECT boxr = {rc.left, rc.top + Scale(4), rc.left + Scale(14),
                 rc.top + Scale(18)};
    const bool hover = !on && UiHover(win, rc);
    RoundBox(dc, boxr, on ? C_ACCENT : (hover ? C_PANEL : C_PANEL2),
             on ? C_ACCENT : (hover ? C_ACCENT : C_LINE), Scale(3));
    if (on) {
        TextC(dc, L"✓", boxr, RGB(255, 255, 255), s.fSmall,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    RECT tr = {boxr.right + Scale(6), rc.top, rc.right, rc.bottom};
    TextC(dc, text, tr, C_TEXT, s.fSmall,
          DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    UiHit h;
    h.rc = rc;
    h.kind = HK_CMD;
    h.index = cmd;
    win.hits.push_back(h);
}

void DrawMain(HDC dc, UiWindow& win, const RECT& client) {
    AppState& s = S();
    BridgeCore& core = *s.core;
    const MainLayout lay = MainLayoutOf(client);

    FillRect(dc, &client, s.bgBrush);

    // ---- 顶栏 ----
    RECT header = {0, 0, client.right, Scale(38)};
    FillRectC(dc, header, C_PANEL2);
    RECT t = {Scale(14), 0, Scale(430), Scale(38)};
    TextC(dc, L"SAI2 插件面板", t, C_TEXT, s.fTitle,
          DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    {
        const ImageBuffer& img = core.DisplayImage(BridgeCore::kWinMain);
        wchar_t sub[220];
        const int pLoaded = core.PluginLoadedCount();
        const int pTotal = core.PluginTotalCount();
        swprintf(sub, 220, L"%s   %d x %d   插件 %d/%d%s",
                 (core.HasImage() ? L"已抓取图层" : L"等待抓取"),
                 img.IsValid() ? img.Width() : 0,
                 img.IsValid() ? img.Height() : 0, pLoaded, pTotal,
                 (pLoaded < pTotal) ? L"（未全载，见日志）" : L"");
        RECT rr = {Scale(300), 0, client.right - Scale(140), Scale(38)};
        TextC(dc, sub, rr,
              (core.HasImage() && pLoaded >= pTotal) ? C_OK : C_DIM, s.fSmall,
              DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
    // 主题按钮（跟随系统 → 浅色 → 深色 循环）
    {
        RECT tb = {client.right - Scale(130), Scale(6), client.right - Scale(12),
                   Scale(32)};
        const bool hover = UiHover(win, tb);
        RoundBox(dc, tb, hover ? C_PANEL : C_PANEL2, hover ? C_ACCENT : C_LINE,
                 Scale(6));
        wchar_t buf[40];
        swprintf(buf, 40, L"主题：%s", ThemeLabel());
        TextC(dc, buf, tb, C_TEXT, s.fSmall,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        UiHit h;
        h.rc = tb;
        h.kind = HK_CMD;
        h.index = CMD_THEME;
        win.hits.push_back(h);
    }
    FrameRectC(dc, RECT{0, Scale(37), client.right, Scale(38)}, C_LINE);

    // ---- 预览（带缩放控制：滚轮 + 放大镜滑杆 + 100%/适配） ----
    win.canvas = lay.preview;
    DrawCanvas(dc, win, false, true);

    // ---- 功能按钮（4 个分组，各开各的窗口） ----
    {
        RECT title = {Scale(12), lay.category.top - Scale(24),
                      lay.category.right, lay.category.top - Scale(4)};
        TextC(dc, L"功能（点一下 = 抓取当前图层并打开独立功能窗口）", title,
              C_DIM, s.fSmall, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        DrawCategoryStrip(dc, win, lay.category, std::wstring());
    }

    // ---- 图层区 ----
    {
        RECT head = lay.layerHead;
        TextC(dc, L"SAI2 图层（注入代理）", head, C_DIM, s.fSmall,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        RECT hint = {head.left + Scale(190), head.top, head.right, head.bottom};
        TextC(dc, s.agentOnline ? L"已连接" : L"未连接 —— 点「连接注入」", hint,
              s.agentOnline ? C_OK : C_WARN, s.fSmall,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        RECT box = lay.layerList;
        FillRectC(dc, box, C_PANEL);
        FrameRectC(dc, box, C_LINE);

        // 右侧滚动条预留宽度
        const int barW = Scale(10);
        RECT barTrack = {box.right - barW - 3, box.top + 3, box.right - 3,
                         box.bottom - 3};

        const int rowH = Scale(22);
        const int viewTop = box.top + Scale(2);
        const int viewH = RectH(box) - Scale(4);
        const int total = static_cast<int>(s.layerNames.size());
        const int maxScroll = std::max(0, (total * rowH - viewH) / std::max(1, rowH));
        g_listScroll = std::max(0, std::min(g_listScroll, maxScroll));

        if (!s.agentOnline) {
            RECT hr = {box.left + Scale(10), box.top + Scale(8),
                       box.right - Scale(10), box.bottom - Scale(6)};
            TextC(dc, L"未连接注入代理。\n点「连接注入」（SAI2 需已运行），之后"
                      L"这里会列出图层；点行即在 SAI2 里选中该层。",
                  hr, C_WARN, s.fSmall, DT_LEFT | DT_TOP | DT_WORDBREAK);
        } else if (total == 0) {
            RECT hr = {box.left + Scale(10), box.top + Scale(8),
                       box.right - Scale(10), box.bottom - Scale(6)};
            TextC(dc, L"已连接，但还没捕获到图层面板文字 —— 在 SAI2 里点一下图层。",
                  hr, C_DIM, s.fSmall, DT_LEFT | DT_TOP | DT_WORDBREAK);
        } else {
            HRGN clip = CreateRectRgn(box.left + 1, viewTop, box.right - barW - 1,
                                      box.bottom - (maxScroll > 0 ? Scale(20) : 1));
            SelectClipRgn(dc, clip);
            int y = viewTop - g_listScroll * rowH;
            for (int i = 0; i < total; ++i) {
                RECT rr = {box.left + 1, y, box.right - barW - 1, y + rowH};
                if (rr.bottom > viewTop && rr.top < box.bottom) {
                    const bool sel = RowIsSelected(i);
                    const bool primary = (s.selLayer == i);
                    if (sel) {
                        RECT sr = rr;
                        sr.left += 2;
                        sr.right -= 2;
                        // 多选时：主层用亮底，其余选中层用稍暗的底色 + 左侧色条，
                        // 让"编辑的是哪层、还会一起处理哪几层"一眼分清。
                        FillRectC(dc, sr, primary ? C_ACCENT2 : C_DIM);
                        if (!primary) {
                            RECT bar = sr;
                            bar.right = bar.left + Scale(3);
                            FillRectC(dc, bar, C_ACCENT);
                        }
                    }
                    std::wstring line = std::to_wstring(i + 1) + L". " + s.layerNames[i];
                    if (i < static_cast<int>(s.layerAttrs.size()) &&
                        !s.layerAttrs[i].empty())
                        line += L"   (" + s.layerAttrs[i] + L")";
                    RECT tr = rr;
                    tr.left += Scale(10);
                    TextC(dc, line, tr, sel ? RGB(255, 255, 255) : C_TEXT,
                          s.fSmall,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                    UiHit h;
                    h.rc = rr;
                    h.kind = HK_CMD;
                    h.index = CMD_LAYER_ROW + i;
                    win.hits.push_back(h);
                }
                y += rowH;
            }
            SelectClipRgn(dc, nullptr);
            DeleteObject(clip);
            if (maxScroll > 0) {
                // 滚动条：轨道 + 圆角滑块（悬停/拖动时高亮），点轨道即跳转、拖动滚动
                RoundBox(dc, barTrack, C_PANEL2, C_LINE, Scale(4));
                const int trackH = RectH(barTrack);
                const int thumbH =
                    std::max(Scale(18), std::min(trackH,
                                                 trackH * viewH / (total * rowH)));
                const int span = std::max(1, trackH - thumbH);
                const int thumbTop = barTrack.top + static_cast<int>(
                    (g_listScroll / (double)maxScroll) * span);
                RECT thumb = {barTrack.left + 2, thumbTop,
                              barTrack.right - 2, thumbTop + thumbH};
                const bool hot = win.dragList || UiHover(win, thumb);
                RoundBox(dc, thumb, hot ? C_ACCENT : C_DIM,
                         hot ? C_ACCENT : C_DIM, Scale(3));
                UiHit hb;
                hb.rc = barTrack;
                hb.track = barTrack;
                hb.kind = HK_LISTSCROLL;
                win.hits.push_back(hb);

                wchar_t buf[64];
                swprintf(buf, 64, L"共 %d 层  滚轮/拖动翻页", total);
                RECT sr = {box.left + Scale(8), box.bottom - Scale(18),
                           barTrack.left - Scale(4), box.bottom - Scale(2)};
                TextC(dc, buf, sr, C_DIM, s.fSmall,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            }
        }

        static const wchar_t* labels[LBTN_COUNT] = {L"连接注入", L"刷新图层",
                                                    L"抓取选中图层"};
        static const bool primary[LBTN_COUNT] = {false, false, true};
        const RECT strip = lay.layerBtns;
        const int gap = Scale(6);
        const int bw = (RectW(strip) - gap * (LBTN_COUNT - 1)) / LBTN_COUNT;
        for (int i = 0; i < LBTN_COUNT; ++i) {
            RECT br = {strip.left + i * (bw + gap), strip.top,
                       strip.left + i * (bw + gap) + bw, strip.bottom};
            FillRectC(dc, br, primary[i] ? C_ACCENT2 : C_PANEL);
            FrameRectC(dc, br, primary[i] ? C_ACCENT : C_LINE);
            TextC(dc, labels[i], br, primary[i] ? RGB(255, 255, 255) : C_TEXT,
                  s.fBody, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            UiHit h;
            h.rc = br;
            h.kind = HK_TOOL;
            h.index = CMD_LAYER_BTN + i;
            win.hits.push_back(h);
        }
    }

    // ---- 选项 ----
    {
        RECT box = lay.checks;
        const int rowH = Scale(24);
        int y = box.top;
        DrawCheck(dc, win, RECT{box.left, y, box.right, y + rowH},
                  L"应用后自动粘贴回 SAI2", s.autoPaste, CMD_AUTOPASTE);
        y += rowH;
        DrawCheck(dc, win, RECT{box.left, y, box.right, y + rowH},
                  L"监听剪贴板（自动抓图）", s.watchClipboard, CMD_WATCH);
        y += rowH;
        DrawCheck(dc, win, RECT{box.left, y, box.right, y + rowH},
                  L"点图层行时在 SAI2 里选中", s.autoGrabLayer, CMD_AUTOLAYER);
        y += rowH;
        DrawCheck(dc, win, RECT{box.left, y, box.right, y + rowH},
                  L"叠加处理（在当前结果上继续）", core.Chain(), CMD_CHAIN);
        y += rowH;
        DrawCheck(dc, win, RECT{box.left, y, box.right, y + rowH},
                  L"应用后合并回原图层（向下转写）", s.mergeBack, CMD_MERGE);
        y += rowH;
        DrawCheck(dc, win, RECT{box.left, y, box.right, y + rowH},
                  L"精确回贴（补边对齐，推荐）", s.pasteInPlace, CMD_INPLACE);
    }

    // ---- 命令按钮（两行两列：文字放得下，不再省略号截断） ----
    {
        const RECT b = lay.buttons;
        const int bh = Scale(34);
        RECT row2 = {b.left, b.bottom - bh, b.right, b.bottom};
        RECT row1 = {b.left, row2.top - Scale(6) - bh, b.right,
                     row2.top - Scale(6)};
        DrawCmdButtons(dc, win, row1, {CMD_GRAB, CMD_APPLY}, CMD_APPLY);
        DrawCmdButtons(dc, win, row2, {CMD_UNDO, CMD_RESET}, CMD_APPLY);
    }

    // ---- 状态与日志 ----
    {
        RECT box = lay.status;
        FillRectC(dc, box, C_PANEL2);
        FrameRectC(dc, box, C_LINE);
        RECT sr = {box.left + Scale(8), box.top + Scale(3),
                   box.right - Scale(8), box.top + Scale(21)};
        std::wstring txt = U8(core.Status());
        if (!s.flash.empty()) txt = s.flash;
        TextC(dc, txt, sr, C_TEXT, s.fSmall,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        const auto& logs = core.LogLines();
        const size_t maxLines = std::max<size_t>(
            4, static_cast<size_t>(std::max(1, (RectH(box) - Scale(24)) / Scale(14))));
        const size_t start = logs.size() > maxLines ? logs.size() - maxLines : 0;
        int y = box.top + Scale(22);
        const int lh = Scale(14);
        for (size_t i = start; i < logs.size(); ++i) {
            RECT lr = {box.left + Scale(8), y, box.right - Scale(8), y + lh};
            TextC(dc, U8(logs[i]), lr, C_DIM, s.fSmall,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            y += lh;
        }
    }

    SyncParamEdits(win, {}, RECT{0, 0, 0, 0});
}

bool OnMainHit(UiWindow& win, const UiHit& h) {
    AppState& s = S();
    if (h.kind == HK_TOOL && h.index >= CMD_LAYER_BTN &&
        h.index < CMD_LAYER_BTN + LBTN_COUNT) {
        switch (h.index - CMD_LAYER_BTN) {
        case LBTN_INJECT: DoInjectAsync(win.hwnd); break;
        case LBTN_REFRESH: RefreshLayers(); break;
        case LBTN_GRABSEL: SyncSelectionToSai2(); GrabSelectedLayer(win.hwnd, s.selLayer, -1); break;
        default: break;
        }
        return true;
    }
    if (h.kind == HK_CMD) {
        if (h.index >= CMD_LAYER_ROW && h.index < CMD_LAYER_ROW + 256) {
            const int row = h.index - CMD_LAYER_ROW;
            const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            SetRowSelection(row, ctrl, shift);
            SyncSelectionToSai2();
            InvalidateAll();
            return true;
        }
        switch (h.index) {
        case CMD_WATCH: s.watchClipboard = !s.watchClipboard; return true;
        case CMD_AUTOLAYER: s.autoGrabLayer = !s.autoGrabLayer; return true;
        case CMD_MERGE: s.mergeBack = !s.mergeBack; return true;
        case CMD_INPLACE: s.pasteInPlace = !s.pasteInPlace; return true;
        default: break;
        }
    }
    return false;
}

bool MainMsg(UiWindow& win, UINT msg, WPARAM wp, LPARAM lp) {
    RECT client;
    GetClientRect(win.hwnd, &client);
    const MainLayout lay = MainLayoutOf(client);
    if (msg == WM_MOUSEWHEEL) {
        const int delta = GET_WHEEL_DELTA_WPARAM(wp);
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(win.hwnd, &pt);
        if (InRect(lay.layerList, pt.x, pt.y)) {
            g_listScroll -= delta / WHEEL_DELTA * 2;
            if (g_listScroll < 0) g_listScroll = 0;
            InvalidateRect(win.hwnd, nullptr, FALSE);
            return true;
        }
        // 预览画布：滚轮自由缩放（以光标为锚点）
        if (InRect(win.canvas, pt.x, pt.y) && S().core->HasImage()) {
            CanvasView v;
            if (CanvasViewOf(win, v)) {
                const double cur = (win.zoom > 0.0) ? win.zoom : v.fit;
                const double factor = (delta > 0) ? 1.25 : 1.0 / 1.25;
                CanvasZoomAt(win, cur * factor, pt.x, pt.y);
                InvalidateRect(win.hwnd, nullptr, FALSE);
            }
            return true;
        }
        return false;
    }
    if (msg == WM_MOUSEMOVE) {
        const int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);
        if (win.dragZoom) {
            OnZoomSliderDrag(win, mx);
            InvalidateRect(win.hwnd, nullptr, FALSE);
            return true;
        }
        if (win.dragList) {
            ListScrollAt(win, my);
            return true;
        }
        UiTrackMouse(win, win.hwnd, mx, my);
        return false;
    }
    if (msg == WM_MOUSELEAVE) {
        UiMouseLeave(win, win.hwnd);
        return false;
    }
    if (msg == WM_LBUTTONUP && (win.dragZoom || win.dragList)) {
        win.dragZoom = false;
        win.dragList = false;
        ReleaseCapture();
        InvalidateRect(win.hwnd, nullptr, FALSE);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// SAI2 窗口与输入
// ---------------------------------------------------------------------------
DWORD FindProcessIdByName(const wchar_t* exeName) {
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, exeName) == 0) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

struct EnumCtx {
    DWORD pid = 0;
    HWND found = nullptr;
};

BOOL CALLBACK EnumWindowsProc(HWND h, LPARAM lp) {
    EnumCtx* c = reinterpret_cast<EnumCtx*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != c->pid) return TRUE;
    if (!IsWindowVisible(h)) return TRUE;
    if (GetWindow(h, GW_OWNER) != nullptr) return TRUE;
    if (GetWindowTextLengthW(h) == 0) return TRUE;
    wchar_t cls[64] = {0};
    GetClassNameW(h, cls, 64);
    // 排除注入代理的浮窗
    if (wcsstr(cls, L"SAI2PluginBar") != nullptr) return TRUE;
    RECT r;
    if (!GetWindowRect(h, &r)) return TRUE;
    const long area = (r.right - r.left) * (r.bottom - r.top);
    if (c->found) {
        RECT pr;
        if (GetWindowRect(c->found, &pr) &&
            (pr.right - pr.left) * (pr.bottom - pr.top) >= area)
            return TRUE;
    }
    c->found = h;
    return FALSE;
}

// ---------------------------------------------------------------------------
// 剪贴板 / 输入自动化
// ---------------------------------------------------------------------------
void SendKeyScan(WORD scan, bool up) {
    INPUT in = {};
    in.type = INPUT_KEYBOARD;
    in.ki.wScan = scan;
    in.ki.dwFlags = KEYEVENTF_SCANCODE | (up ? KEYEVENTF_KEYUP : 0);
    SendInput(1, &in, sizeof(INPUT));
}

void SendCtrlScan(WORD scan) {
    SendKeyScan(0x1D, false);
    SendKeyScan(scan, false);
    Sleep(30);
    SendKeyScan(scan, true);
    SendKeyScan(0x1D, true);
}

// Ctrl+V：SAI2 新建式粘贴，图像按画布 (0,0) 对齐 —— 1.19 起精确回贴
// 不再用 Ctrl+Shift+V（很多 SAI2 版本没有该快捷键），而是靠补边成画布
// 尺寸让 (0,0) 对齐恰好等于原位置。

void SendEscape() {
    INPUT in[2] = {};
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wVk = VK_ESCAPE;
    in[1].type = INPUT_KEYBOARD;
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

// 用户正按着鼠标（拖拽中/选区中）时绝不能注入合成输入：
// 此时插入 LEFTDOWN/UP 或按键会污染系统输入状态机，最坏把 Explorer
// 的拖拽状态留在"按钮未释放"，表现为文件/图标拖拽全部失效，直到重启。
bool UserHoldingMouse() {
    return (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 ||
           (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
}

// 最多等 timeoutMs 毫秒，让用户先松开鼠标（正在拖拽时返回 false）
void WaitMouseReleased(int timeoutMs) {
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(timeoutMs);
    while (UserHoldingMouse() && GetTickCount64() < deadline) Sleep(60);
}

// 保险丝：把自动化用到的所有键/鼠标强制补一次 KEYUP。
// 正常流程它们都是成对发送的；一旦某条失败路径把 down 留在系统里
// （逻辑上卡住的 Ctrl/Alt/Esc/左键会让拖拽/点选全面异常），这里兜底。
// 对本来已松开的键补 KEYUP 无副作用。
void ReleaseSyntheticKeys() {
    INPUT in[6] = {};
    for (int i = 0; i < 6; ++i) in[i].type = INPUT_KEYBOARD;
    in[0].ki.wScan = 0x1D; // Ctrl
    in[0].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
    in[1].ki.wVk = VK_MENU; // Alt
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    in[2].ki.wVk = VK_ESCAPE;
    in[2].ki.dwFlags = KEYEVENTF_KEYUP;
    in[3].ki.wVk = VK_SHIFT;
    in[3].ki.dwFlags = KEYEVENTF_KEYUP;
    in[4].ki.wScan = 0x1D;
    in[4].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
    in[5].ki.wVk = VK_CONTROL;
    in[5].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(6, in, sizeof(INPUT));
    INPUT m[2] = {};
    m[0].type = INPUT_MOUSE;
    m[0].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    m[1].type = INPUT_MOUSE;
    m[1].mi.dwFlags = MOUSEEVENTF_RIGHTUP;
    SendInput(2, m, sizeof(INPUT));
}

// ---------------------------------------------------------------------------
// 抓取流程（后台线程里按阶段推进）
// ---------------------------------------------------------------------------
enum CapPhase {
    CAP_IDLE = 0,
    CAP_SELECT_LAYER,
    CAP_WAIT_LAYER,
    CAP_SELECT_ALL,
    CAP_WAIT_ALL,
    CAP_COPY,
    CAP_WAIT_CLIP,
    CAP_CLIP_RETRY,
    CAP_DONE,
    CAP_FAIL
};

struct CaptureJob {
    std::atomic<int> phase{CAP_IDLE};
    std::atomic<bool> busy{false};
    std::atomic<bool> hasJob{false};
    std::atomic<int> wantWindow{-1};
    bool needImage = true;
    std::atomic<bool> grabbed{false};
    std::atomic<int> selectedRow{-1};
    int row = -1;
    // 多选：本次抓取要**一起处理**的全部图层行（主层在前，其余按面板顺序）。
    // 长度 > 1 时，应用阶段会对每一层施加同一形变、并各自合并回原层。
    std::vector<int> selRows;
    int panelLeft = 0;
    int rowScreenY = 0;
    DWORD clipSeq = 0;
    ULONGLONG t0 = 0;
    std::wstring message;
};

CaptureJob g_cap;
std::thread g_capThread;

// 抓取线程刚设置好的画布上下文要跨过 OnCaptureFinished →
// DoGrabFromClipboard（剪贴板读图）保留下来；手动"从剪贴板抓取"则清除
bool g_grabKeepCtx = false;

// 浮窗心跳：独立于 UI 线程的管道 ping（每 1 秒）。此前心跳只靠 UI
// 定时器的 400ms 轮询（take.request），液化窗口最小化/连片重绘等
// UI 线程繁忙场景会把心跳拖过阈值，SAI2 内浮动按钮条被误判
// "桥已挂死"而自动隐藏（用户反馈：最小化液化窗口浮窗会掉）。
std::atomic<bool> g_hbStop{false};
void HeartbeatLoop() {
    while (!g_hbStop.load(std::memory_order_relaxed)) {
        if (S().agentOnline) {
            long long pid = 0;
            sai2bridge::AgentPing(pid); // 结果不重要，让 agent 知道桥还活着
        }
        for (int i = 0; i < 10 && !g_hbStop.load(std::memory_order_relaxed);
             ++i)
            Sleep(100);
    }
}
std::thread g_hbThread;

// 单击屏幕某点（用户正拖拽时先等松开，最多 1 秒）
void ClickAtScreenImpl(int x, int y) {
    WaitMouseReleased(1000);
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    INPUT in[3] = {};
    in[0].type = INPUT_MOUSE;
    in[0].mi.dwFlags =
        MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    in[0].mi.dx = static_cast<LONG>((x - vx) * 65536 / std::max(1, vw));
    in[0].mi.dy = static_cast<LONG>((y - vy) * 65536 / std::max(1, vh));
    in[1].type = INPUT_MOUSE;
    in[1].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    in[2].type = INPUT_MOUSE;
    in[2].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(3, in, sizeof(INPUT));
}

// ---------------------------------------------------------------------------
// DPI 坐标换算：agent 在 SAI2 内部拿到的屏幕坐标可能是虚拟化坐标（SAI2 非
// DPI 感知、显示器缩放 ≠100% 时），而桥是 DPI 感知的，SendInput 用物理
// 坐标 —— 不换算会把点击打到标题栏等错误位置（已实测踩坑）。
// ---------------------------------------------------------------------------

// agent 坐标 → 物理坐标（比例由 RefreshLayers 用「桥看到的窗口物理矩形 /
// agent 报告的同窗口矩形」实时校准，SAI2 感知 DPI 时为 1:1）
POINT AgentToScreen(double x, double y) {
    AppState& st = S();
    const double sx = std::min(4.0, std::max(0.5, st.agentScaleX));
    const double sy = std::min(4.0, std::max(0.5, st.agentScaleY));
    POINT p{static_cast<LONG>(x * sx + 0.5), static_cast<LONG>(y * sy + 0.5)};
    return p;
}

// SAI2 逻辑像素 → 物理像素的缩放（用于菜单栏偏移这类"从窗口边框量出来"
// 的固定逻辑偏移）。SAI2 自己感知 DPI 时取它的 DPI；不感知时取所在显示器
// 的 DPI（DWM 会把不感知的窗口整体拉伸）。
double SaiDpiScale(HWND sai) {
    UINT dpi = 96;
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    static GetDpiForWindowFn f1 = []() -> GetDpiForWindowFn {
        HMODULE m = GetModuleHandleW(L"user32.dll");
        return m ? reinterpret_cast<GetDpiForWindowFn>(
                       GetProcAddress(m, "GetDpiForWindow"))
                 : nullptr;
    }();
    if (f1 && sai) dpi = f1(sai);
    if (dpi <= 96) {
        using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
        static GetDpiForMonitorFn f2 = []() -> GetDpiForMonitorFn {
            HMODULE m = LoadLibraryW(L"Shcore.dll");
            return m ? reinterpret_cast<GetDpiForMonitorFn>(
                           GetProcAddress(m, "GetDpiForMonitor"))
                     : nullptr;
        }();
        UINT mx = 96, my = 96;
        if (f2 && sai) {
            const HMONITOR mon =
                MonitorFromWindow(sai, MONITOR_DEFAULTTONEAREST);
            if (mon && SUCCEEDED(f2(mon, 0 /*MDT_EFFECTIVE_DPI*/, &mx, &my)))
                dpi = my;
        }
        if (dpi <= 96) {
            if (HDC dc = GetDC(nullptr)) {
                dpi = static_cast<UINT>(GetDeviceCaps(dc, LOGPIXELSY));
                ReleaseDC(nullptr, dc);
            }
        }
    }
    return dpi > 96 ? dpi / 96.0 : 1.0;
}

bool MenuItemClickImpl(HWND sai, int menuBarOffsetX, const wchar_t* itemPrefix) {
    RECT r;
    GetWindowRect(sai, &r);
    ForceForeground(sai);
    Sleep(120);
    if (GetForegroundWindow() != sai) return false;
    SendEscape();
    Sleep(150);
    const ULONGLONG menuTick = GetTickCount64();
    // 菜单栏偏移是 SAI2 逻辑像素：显示器缩放 >100% 时必须先放大，
    // 否则点在标题栏上（菜单根本弹不出来）
    const double ms = SaiDpiScale(sai);
    ClickAtScreenImpl(r.left + static_cast<int>(menuBarOffsetX * ms + 0.5),
                      r.top + static_cast<int>(33 * ms + 0.5));
    Sleep(480);
    int ix = -1, iy = 0;
    for (int i = 0; i < 8; ++i) {
        if (FindCapturedText(itemPrefix, ix, iy, menuTick - 200)) {
            // agent 报的坐标可能是虚拟化坐标，换算成物理坐标再点
            const POINT p = AgentToScreen(ix + 40, iy + 6);
            ClickAtScreenImpl(p.x, p.y);
            Sleep(350);
            return true;
        }
        Sleep(140);
    }
    SendEscape();
    return false;
}

// ---------------------------------------------------------------------------
// 画布上下文（精确回贴）
// SAI2 的 Ctrl+C 只拷贝图层不透明部分包围盒（透明区域被裁掉），Ctrl+V 又按
// 画布 (0,0) 对齐 —— 这是错位的根源。整层抓取时：agent 把 SAI2 的绘制
// 表面快照落盘（实测是整个文档视图的后台缓冲：画布按当前缩放渲染在其中
// 一块子矩形里），桥读回后做**多尺度**模板匹配求出缩放比例 s 和内容位置，
// 再检测内容外围的"画布纸"颜色环带得到纸矩形 —— 纸尺寸/s = 画布尺寸、
// 内容相对纸的偏移/s = 原始位置。处理结果补边成画布尺寸再 Ctrl+V 就能
// 精确落回原位，不再依赖 Ctrl+Shift+V（很多 SAI2 版本没有该快捷键）。
// ---------------------------------------------------------------------------

struct CanvasCtxResult {
    bool ok = false;
    int cw = 0, ch = 0, x0 = 0, y0 = 0;
    double score = 1e9;
};

// 纯数学的画布上下文定位（实现在 AcquireCanvasContext 之后）
bool LocateCanvasCtx(const ImageBuffer& view, const ImageBuffer& tmpl,
                     CanvasCtxResult& out, std::string& diag, int docW = 0,
                     int docH = 0);

// ---------------------------------------------------------------------------
// 精确画布尺寸：读当前 .sai2 的文件头
//
// 为什么需要：画布尺寸原先只能从快照推算（纸矩形 / 视图缩放），实测波动
// 1%~3.5%，落位残差就是这么来的。而 .sai2 头部有精确宽高（offset 20/24，
// uint32 LE，magic "SAI2-CANV"），实测 7/7 个真实文件（1920x1080、2160x3280、
// 3736x2102 …）都吻合。读出它就能把尺度钉死 → 落位误差降到 1~2 px。
// 这是**读一个普通文件**，不需要注入更多东西，也不碰 SAI2 内存。
// ---------------------------------------------------------------------------
inline uint32_t ReadU32LE(const unsigned char* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

bool Sai2DocPathFromTitle(const std::wstring& title, std::wstring& out);

// 成功时填 w/h 并返回 true；任何异常（文件不在/不是 .sai2/尺寸离谱）都返回
// false，调用方静默退回"推算"路径。
bool ReadSai2CanvasSize(const std::wstring& path, int& w, int& h) {
    if (path.empty()) return false;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    unsigned char buf[32] = {0};
    DWORD got = 0;
    const bool read = ReadFile(f, buf, sizeof(buf), &got, nullptr) != 0;
    CloseHandle(f);
    if (!read || got < 28) return false;
    // magic 实测是 "SAI-CANVAS-TYPE0"（16 字节）。
    // ⚠ 踩过：早前从 d[:8] 的十六进制转储里**误读**成 "SAI2-CANV"（因为文件叫
    //   .sai2，想当然以为 magic 以 SAI2 开头），于是 magic 校验永远失败 →
    //   这个功能静默退回推算路径，现象上"完全看不出它没生效"。
    //   与 alpha 阈值那次是同一类错误：一个常量写错就让整条路静默失效。
    //   这里只比前缀 "SAI-CANVAS"，对 TYPE1/TYPE2 等后续变体也成立。
    static const char kMagic[9] = {'S', 'A', 'I', '-', 'C', 'A', 'N', 'V', 'A'};
    for (int i = 0; i < 9; ++i)
        if (buf[i] != static_cast<unsigned char>(kMagic[i])) return false;
    const uint32_t cw = ReadU32LE(buf + 20);
    const uint32_t ch = ReadU32LE(buf + 24);
    if (cw < 16 || ch < 16 || cw > 100000 || ch > 100000) return false;
    w = static_cast<int>(cw);
    h = static_cast<int>(ch);
    return true;
}

// 便利封装：SAI2 主窗口标题 → 当前 .sai2 → 精确画布尺寸。
// 任何一步失败都返回 false（未保存文档 / 标题格式变化 / 文件读不到），
// 调用方静默退回"从快照推算"，行为与加这个功能之前完全一致。
namespace {
struct TitleCtx { DWORD pid; std::vector<std::wstring>* out; };
BOOL CALLBACK TitleEnumProc(HWND h, LPARAM lp) {
    TitleCtx* c = reinterpret_cast<TitleCtx*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != c->pid) return TRUE;
    wchar_t buf[512] = {0};
    if (GetWindowTextW(h, buf, 512) <= 0) return TRUE;
    const std::wstring s(buf);
    for (const std::wstring& e : *c->out)
        if (e == s) return TRUE;
    c->out->push_back(s);
    return TRUE;
}
} // namespace

void CollectWindowTitles(DWORD pid, std::vector<std::wstring>& titles) {
    TitleCtx ctx{pid, &titles};
    EnumWindows(&TitleEnumProc, reinterpret_cast<LPARAM>(&ctx));
}
bool Sai2DocCanvasSize(int& w, int& h) {
    w = 0;
    h = 0;
    std::vector<std::wstring> titles;
    wchar_t title[512] = {0};
    HWND sai = FindSai2Window();
    if (sai) {
        GetWindowTextW(sai, title, 512);
        DWORD pid = 0;
        GetWindowThreadProcessId(sai, &pid);
        // ★ 只读"主窗口标题"是不够的：实测拿到的是 "SAI2 插件入口" ✗
        //   （不是文档标题）→ 解析不出 .sai2 路径 → 拿不到精确画布尺寸 ✗。
        //   这里把该进程**所有**顶层窗口的标题都收一遍，取第一个能解析出
        //   文档路径的 —— 精确尺寸（2160x3280 之类）从此不再依赖窗口标题格式。
        if (pid) CollectWindowTitles(pid, titles);
    }
    if (title[0]) titles.insert(titles.begin(), std::wstring(title));
    for (const std::wstring& tt : titles) {
        std::wstring docPath;
        if (Sai2DocPathFromTitle(tt, docPath) && ReadSai2CanvasSize(docPath, w, h))
            return true;
    }
    // ★ 回退：标题里**没有盘符**时（实测 `… - 桌面 / 博丽灵梦.sai2 (*)`），
    //   把尾部相对路径拼到常见用户目录下再试。
    //   这是"拿不到精确画布尺寸"最常见的原因（SAI2 的标题只给文件夹名）。
    const wchar_t* kRoots[] = {L"Desktop", L"Documents", L"Downloads", L"Pictures",
                               L"OneDrive\\Desktop", L"OneDrive\\Documents"};
    for (const std::wstring& tt : titles) {
        const size_t dash = tt.rfind(L" - ");
        if (dash == std::wstring::npos) continue;
        std::wstring tail = tt.substr(dash + 3);
        // 去掉未保存标记 " (*)" / " *"
        for (;;) {
            if (tail.size() >= 3 && tail.compare(tail.size() - 3, 3, L" (*)") == 0) {
                tail.erase(tail.size() - 3);
                continue;
            }
            if (!tail.empty() && (tail.back() == L'*' || tail.back() == L' ')) {
                tail.pop_back();
                continue;
            }
            break;
        }
        std::wstring rel = tail;
        for (size_t i = 0; i + 2 < rel.size(); ++i)
            if (rel[i] == L' ' && rel[i + 1] == L'/' && rel[i + 2] == L' ') {
                rel[i] = L'\\';
                rel[i + 1] = L' ';
                rel[i + 2] = L'\\';
            }
        // 本地化文件夹名 → 真实目录名
        struct Map { const wchar_t* from; const wchar_t* to; };
        const Map kNames[] = {{L"桌面", L"Desktop"}, {L"文档", L"Documents"},
                              {L"下载", L"Downloads"}, {L"图片", L"Pictures"}};
        std::wstring tailNoFolder = rel;
        for (const Map& m : kNames) {
            if (rel.rfind(m.from, 0) == 0) {
                tailNoFolder = std::wstring(m.to) + rel.substr(std::wcslen(m.from));
                break;
            }
        }
        wchar_t prof[512] = {0};
        if (!GetEnvironmentVariableW(L"USERPROFILE", prof, 512)) break;
        std::vector<std::wstring> cands;
        for (const wchar_t* root : kRoots) {
            cands.push_back(std::wstring(prof) + L"\\" + root + L"\\" + tailNoFolder);
            cands.push_back(std::wstring(prof) + L"\\" + root + L"\\" + rel);
        }
        for (const std::wstring& c : cands)
            if (ReadSai2CanvasSize(c, w, h)) return true;
    }
    return false;
}

// SAI2 主窗口标题 → 当前文档完整路径。
// 标题实测形如：
//   PaintTool SAI Ver.2 (64bit) Preview.2023.04.05 - Video (F:) / sai2test / align_test.sai2 (*)
// 尾部 " (*)" / " *" 表示有未保存修改；盘符以"卷标 (X:)"形式给出。
// 解析不出来（未保存的新文档、标题格式变了）就返回 false，调用方退回避推算。
bool Sai2DocPathFromTitle(const std::wstring& title, std::wstring& out) {
    out.clear();
    const std::wstring dash = L" - ";
    const size_t d = title.find(dash);
    if (d == std::wstring::npos) return false;
    std::wstring tail = title.substr(d + dash.size());

    // 去掉未保存标记
    while (!tail.empty() && (tail.back() == L' ' || tail.back() == L'*'))
        tail.pop_back();
    if (!tail.empty() && tail.back() == L')') {
        const size_t p = tail.rfind(L" (");
        if (p != std::wstring::npos) tail.erase(p);
    }
    if (tail.find(L".sai2") == std::wstring::npos) return false;

    // 盘符：卷标 (X:)
    const size_t lp = tail.find(L'(');
    const size_t rp = tail.find(L")", lp == std::wstring::npos ? 0 : lp);
    if (lp == std::wstring::npos || rp == std::wstring::npos || rp < lp + 2)
        return false;
    const std::wstring inParen = tail.substr(lp + 1, rp - lp - 1);
    if (inParen.size() < 2 || inParen[1] != L':') return false;
    const wchar_t drive = inParen[0];
    if (!((drive >= L'A' && drive <= L'Z') ||
          (drive >= L'a' && drive <= L'z')))
        return false;

    // "(X:)" 之后按 " / " 分段拼路径
    std::wstring rest = tail.substr(rp + 1);
    std::wstring path;
    wchar_t dl = drive;
    if (dl >= L'a' && dl <= L'z') dl = static_cast<wchar_t>(dl - 32);
    path += dl;
    path += L":";
    const std::wstring sep = L" / ";
    size_t pos = rest.find(sep);
    if (pos == std::wstring::npos) return false;
    rest = rest.substr(pos + sep.size());
    for (;;) {
        const size_t s = rest.find(sep);
        const std::wstring seg =
            (s == std::wstring::npos) ? rest : rest.substr(0, s);
        if (!seg.empty()) {
            path += L"\\";
            path += seg;
        }
        if (s == std::wstring::npos) break;
        rest = rest.substr(s + sep.size());
    }
    if (path.size() < 5) return false;
    out = path;
    return true;
}

// 盒式降采样：RGB 取平均、alpha 取最大（保留内容掩码）
ImageBuffer DownscaleRGBA(const ImageBuffer& src, int step) {
    const int w = std::max(1, src.Width() / step);
    const int h = std::max(1, src.Height() / step);
    ImageBuffer out(w, h, PixelFormat::RGBA8);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            long r = 0, g = 0, b = 0;
            int a = 0, n = 0;
            for (int dy = 0; dy < step; ++dy) {
                for (int dx = 0; dx < step; ++dx) {
                    RGBA p;
                    if (!src.GetPixel(x * step + dx, y * step + dy, p))
                        continue;
                    r += p.r; g += p.g; b += p.b; ++n;
                    if (p.a > a) a = p.a;
                }
            }
            if (n == 0) continue;
            RGBA o;
            o.r = static_cast<uint16_t>(r / n);
            o.g = static_cast<uint16_t>(g / n);
            o.b = static_cast<uint16_t>(b / n);
            o.a = static_cast<uint16_t>(a);
            out.SetPixel(x, y, o);
        }
    }
    return out;
}

// 最近邻重采样（模板按缩放比例重采样后与视图内容比对）
ImageBuffer ResampleNearest(const ImageBuffer& src, int w, int h) {
    ImageBuffer out(std::max(1, w), std::max(1, h), PixelFormat::RGBA8);
    const int sw = src.Width(), sh = src.Height();
    for (int y = 0; y < out.Height(); ++y) {
        const int sy = std::min(sh - 1, static_cast<int>(
            static_cast<long long>(y) * sh / out.Height()));
        for (int x = 0; x < out.Width(); ++x) {
            const int sx = std::min(sw - 1, static_cast<int>(
                static_cast<long long>(x) * sw / out.Width()));
            RGBA p;
            if (src.GetPixel(sx, sy, p)) out.SetPixel(x, y, p);
        }
    }
    return out;
}

// 通道值统一到 8bit：RGBA8 原样，RGBA16 取高字节。
// 关键：ImageBuffer::GetPixel 对 RGBA8 返回的就是 **0..255**（不会放大到 16bit），
// 而 MaskedSearch 是按字节直接读 canvas 缓冲比较的，所以样本必须存 8bit 值。
inline uint8_t Ch8(uint16_t v, PixelFormat f) {
    return f == PixelFormat::RGBA16 ? static_cast<uint8_t>(v >> 8)
                                    : static_cast<uint8_t>(v);
}

// 把 8bit 阈值换算到该像素格式的通道量纲
inline int ChThresh(int v8, PixelFormat f) {
    return f == PixelFormat::RGBA16 ? (v8 << 8) : v8;
}

// 模板样本（只在 alpha >= 250 的像素上比对；数量封顶 cap）
struct TplSample { int tx, ty; uint8_t r, g, b; };

std::vector<TplSample> BuildSamples(const ImageBuffer& img, int cap) {
    const int w = img.Width(), h = img.Height();
    const PixelFormat fmt = img.Format();
    const int aMin = ChThresh(250, fmt);
    std::vector<TplSample> all;
    // 只对**小模板**做"剔除贴边样本"：大模板（心形等）边缘占比小、原本就能
    // 过阈值，实测加腐蚀反而会让它选错尺度（回归）。
    // 【已停用】本想让粗搜避开"贴边样本"（DownscaleRGBA 对 alpha 取 max、
    // RGB 取平均，导致模板边缘单元与 view 的渲染方式不一致）。实测：即使只对
    // 小降采样单元启用，也会改变粗搜选出的尺度（心形从 0.5 变 0.31）→
    // 连带 paperEqFrame 误判 → 三组夹具全部 ambiguous。真机小圆的失败根源
    // 也不在这里（模板 alpha 正常、合成同类数据完美匹配），故整体停用。
    const bool erode = false;
    const int stride = std::max(
        1, static_cast<int>(std::sqrt(
               static_cast<double>(w) * h / static_cast<double>(cap))));
    for (int y = 0; y < h; y += stride) {
        for (int x = 0; x < w; x += stride) {
            RGBA p;
            if (!img.GetPixel(x, y, p) || p.a < aMin) continue;
            // ★ 只取**内部**单元：把紧贴透明区的一圈样本剔掉。
            //
            // 原因（实测踩坑）：粗搜用的降采样是 DownscaleRGBA —— 它对 alpha 取
            // max、对 RGB 取**平均**。模板边缘那个 cell 里混着"透明"(RGB 通常是
            // 黑的)，平均出来是**暗绿**；而 view 里同一位置是"内容+白纸"平均成
            // **亮绿**，两者永远对不上。
            //   - 大模板（心形 970x593）边缘占比小 → SAD 只涨到 0.5~1.7，能过阈值；
            //   - 小模板（281x281 的实心圆）边缘占比大 → SAD 爆掉 → 粗搜报
            //     "coarse no scale"，整个精确回贴静默退化成普通粘贴。
            // 剔掉边缘样本后，内部单元两边都是纯内容色，SAD 在正确位置接近 0；
            // 而"尺度过小时 SAD 也接近 0"的平台特性不受影响（仍取最大可通过尺度）。
            bool interior = true;   // 只有 erode 为真时才做邻域检查
            for (int dy = -stride; erode && dy <= stride && interior; dy += stride) {
                for (int dx = -stride; dx <= stride; dx += stride) {
                    if (dx == 0 && dy == 0) continue;
                    const int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                    RGBA q;
                    if (!img.GetPixel(nx, ny, q) || q.a < aMin) {
                        interior = false;   // 邻居是透明的 → 本样本贴边
                        break;
                    }
                }
            }
            if (!interior) continue;
            TplSample s;
            s.tx = x; s.ty = y;
            s.r = Ch8(p.r, fmt);
            s.g = Ch8(p.g, fmt);
            s.b = Ch8(p.b, fmt);
            all.push_back(s);
        }
    }
    return all;
}

// 在 [x0..x1]×[y0..y1] 内找模板最佳位置（掩码 SAD，RGBA8 直查）
void MaskedSearch(const ImageBuffer& canvas, const std::vector<TplSample>& smp,
                  int tw, int th, int x0, int y0, int x1, int y1, int posStep,
                  int& bx, int& by, double& bs) {
    if (smp.empty()) return;
    const int cw = canvas.Width(), ch = canvas.Height();
    const uint8_t* cd = canvas.Data();
    for (int py = y0; py <= y1; py += posStep) {
        if (py < 0 || py + th > ch) continue;
        for (int px = x0; px <= x1; px += posStep) {
            if (px < 0 || px + tw > cw) continue;
            long long acc = 0;
            for (const TplSample& s : smp) {
                const uint8_t* cp = cd +
                    (static_cast<size_t>(py + s.ty) * cw +
                     static_cast<size_t>(px + s.tx)) * 4;
                acc += std::abs(s.r - cp[0]) + std::abs(s.g - cp[1]) +
                       std::abs(s.b - cp[2]);
            }
            const double score =
                static_cast<double>(acc) / static_cast<double>(smp.size());
            if (score < bs) { bs = score; bx = px; by = py; }
        }
    }
}

// ---------------------------------------------------------------------------
// 轮廓样本 + 轮廓匹配（消除"实心色块"导致的尺度过估）
//
// 为什么需要它：只取 alpha>=250 的样本时，整块实心色在"模板缩放到略大于
// 真值"的位置仍然落在视图内容内部，SAD 形成一段平台，取"最大可通过尺度"
// 就会过估（实测 0.519 vs 真值 0.500，回贴偏约 (38,21) px）。
// 把模板按 alpha 合成到纸色上、并让**透明外框**一起参与比对，尺度一旦超过
// 真值，外框就会压到内容上，误差立刻上升 —— 评分曲线在真值处形成尖峰最小。
// 中间过渡带（8<=alpha<128）跳过：SAI2 自己的合成与最近邻重采样差异较大。
// ---------------------------------------------------------------------------
struct SilSample { int tx, ty; uint8_t r, g, b; int w; };

// --ctx-test 时打印 Pass C 的尺度评分曲线（排查尺度过/欠估用）
bool g_ctxDebugCurve = false;

// 构建时间：**直接读本 exe 的文件修改时间**。
// 不能用 __DATE__/__TIME__ —— 它们按**翻译单元**在编译时展开，而 zig 的编译
// 缓存会让"未改动的源文件"复用旧对象：改了 BridgeCore.cpp 后，BridgeApp.cpp 里
// 的时间戳仍是上一次编译它时的值，于是"版本看起来没变"，反而误导判断（实测踩过）。
// 读 exe 自身的文件时间没有这个问题，且天然就是构建完成时刻。
std::string BuildStamp() {
    wchar_t path[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "unknown";
    FILETIME ft{}, local{};
    const bool got = GetFileTime(h, nullptr, nullptr, &ft) != 0;
    CloseHandle(h);
    if (!got) return "unknown";
    FileTimeToLocalFileTime(&ft, &local);
    SYSTEMTIME st{};
    FileTimeToSystemTime(&local, &st);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                  st.wSecond);
    return buf;
}

void BuildSilhouetteSamples(const ImageBuffer& t, const RGBA& paper,
                            std::vector<SilSample>& out, int cap) {
    const int w = t.Width(), h = t.Height();
    if (w <= 0 || h <= 0) return;
    const int stride = std::max(
        1, static_cast<int>(std::sqrt(
               static_cast<double>(w) * h / static_cast<double>(cap))));
    const int pr = paper.r, pg = paper.g, pb = paper.b;
    for (int y = 0; y < h; y += stride) {
        for (int x = 0; x < w; x += stride) {
            RGBA p;
            if (!t.GetPixel(x, y, p)) continue;
            const int a = p.a; // RGBA8：0..255
            SilSample s;
            s.tx = x; s.ty = y;
            if (a >= 128) {
                s.r = static_cast<uint8_t>((p.r * a + pr * (255 - a)) / 255);
                s.g = static_cast<uint8_t>((p.g * a + pg * (255 - a)) / 255);
                s.b = static_cast<uint8_t>((p.b * a + pb * (255 - a)) / 255);
                s.w = 10;
            } else if (a < 8) {
                s.r = static_cast<uint8_t>(pr);
                s.g = static_cast<uint8_t>(pg);
                s.b = static_cast<uint8_t>(pb);
                s.w = 6;
            } else {
                continue;
            }
            out.push_back(s);
        }
    }
}

void SilhouetteSearch(const ImageBuffer& canvas,
                      const std::vector<SilSample>& smp, int tw, int th,
                      int cx, int cy, int radius, int& bx, int& by,
                      double& bs, int posStep = 1) {
    bx = -1; by = -1; bs = 1e18;
    if (smp.empty()) return;
    if (posStep < 1) posStep = 1;
    long long wsum = 0;
    for (const SilSample& s : smp) wsum += s.w;
    if (wsum == 0) return;
    const int cw = canvas.Width(), ch = canvas.Height();
    const uint8_t* cd = canvas.Data();
    for (int py = cy - radius; py <= cy + radius; py += posStep) {
        if (py < 0 || py + th > ch) continue;
        for (int px = cx - radius; px <= cx + radius; px += posStep) {
            if (px < 0 || px + tw > cw) continue;
            long long acc = 0;
            for (const SilSample& s : smp) {
                const uint8_t* cp =
                    cd + (static_cast<size_t>(py + s.ty) * cw +
                          static_cast<size_t>(px + s.tx)) * 4;
                acc += static_cast<long long>(s.w) *
                       (std::abs(s.r - cp[0]) + std::abs(s.g - cp[1]) +
                        std::abs(s.b - cp[2]));
            }
            const double score =
                static_cast<double>(acc) / static_cast<double>(wsum);
            if (score < bs) { bs = score; bx = px; by = py; }
        }
    }
}

namespace canvasctx {

// 颜色统计桶（RGB 各 5bit）+ 桶内平均求精确模态色
struct ColorKey { int r, g, b; bool operator==(const ColorKey& o) const {
    return r == o.r && g == o.g && b == o.b; } };
struct ColorKeyHash {
    size_t operator()(const ColorKey& k) const {
        return (static_cast<size_t>(k.r) << 10) |
               (static_cast<size_t>(k.g) << 5) | static_cast<size_t>(k.b);
    }
};

// 统计区域内 RGB 模态色（返回是否达到 minFrac 占比）。
// hole* 提供"挖洞"矩形（如内容矩形），用于只统计外围环带。
bool ModalColor(const ImageBuffer& img, int x0, int y0, int x1, int y1,
                int step, double minFrac, RGBA& modal, double& frac,
                int hx0 = -1, int hy0 = -1, int hx1 = -1, int hy1 = -1) {
    // 5bit 分桶：RGBA16 用 >>11，RGBA8 用 >>3（否则 8bit 值 >>11 恒为 0，
    // 所有像素落进同一个桶，"模态色"退化成整块区域的平均色 → 纸色判错）
    const int shift = (img.Format() == PixelFormat::RGBA16) ? 11 : 3;
    std::unordered_map<ColorKey, long long, ColorKeyHash> hist;
    long long total = 0;
    x1 = std::min(x1, img.Width() - 1);
    y1 = std::min(y1, img.Height() - 1);
    for (int y = y0; y <= y1; y += step) {
        if (y < 0 || y >= img.Height()) continue;
        for (int x = x0; x <= x1; x += step) {
            if (x < 0 || x >= img.Width()) continue;
            if (hx0 >= 0 && x >= hx0 && x <= hx1 && y >= hy0 && y <= hy1)
                continue;
            RGBA p;
            if (!img.GetPixel(x, y, p)) continue;
            ++hist[ColorKey{p.r >> shift, p.g >> shift, p.b >> shift}];
            ++total;
        }
    }
    if (total == 0) return false;
    ColorKey best{}; long long bestN = 0;
    for (const auto& kv : hist)
        if (kv.second > bestN) { bestN = kv.second; best = kv.first; }
    if (bestN < total * minFrac) return false;
    // 桶内精确平均（容差内像素），得到比 8 级量化更准的模态色
    long long r = 0, g = 0, b = 0, n = 0;
    for (int y = y0; y <= y1; y += step) {
        if (y < 0 || y >= img.Height()) continue;
        for (int x = x0; x <= x1; x += step) {
            if (x < 0 || x >= img.Width()) continue;
            if (hx0 >= 0 && x >= hx0 && x <= hx1 && y >= hy0 && y <= hy1)
                continue;
            RGBA p;
            if (!img.GetPixel(x, y, p)) continue;
            if ((p.r >> shift) != best.r || (p.g >> shift) != best.g ||
                (p.b >> shift) != best.b)
                continue;
            r += p.r; g += p.g; b += p.b; ++n;
        }
    }
    if (n == 0) return false;
    modal.r = static_cast<uint16_t>(r / n);
    modal.g = static_cast<uint16_t>(g / n);
    modal.b = static_cast<uint16_t>(b / n);
    modal.a = 0;
    frac = static_cast<double>(bestN) / static_cast<double>(total);
    return true;
}

bool ColorNear(const ImageBuffer& img, int x, int y, const RGBA& c,
               int tol) {
    RGBA p;
    if (!img.GetPixel(x, y, p)) return false;
    return std::abs(static_cast<int>(p.r) - static_cast<int>(c.r)) <= tol &&
           std::abs(static_cast<int>(p.g) - static_cast<int>(c.g)) <= tol &&
           std::abs(static_cast<int>(p.b) - static_cast<int>(c.b)) <= tol;
}

} // namespace canvasctx

bool AcquireCanvasContext(CanvasCtxResult& out, std::string& diag) {
    // 等 agent 完成画布快照（canvas.arm 在抓取开始前已设置；抓取期间的
    // 前台切换与 Ctrl+A 通常已触发画布重绘）。第 5 轮还没抓到就让宿主
    // 补一次强制重绘，避免"没有重绘 → 永远抓不到"的死等。
    std::string resp;
    bool captured = false;
    for (int i = 0; i < 22; ++i) {
        Sleep(100);
        resp.clear();
        if (!sai2agent::AgentClient::Query("{\"cmd\":\"canvas.state\"}",
                                           resp))
            continue;
        if (resp.find("unknown cmd") != std::string::npos) {
            diag = "agent too old";
            return false; // 宿主里的 agent 还是旧版
        }
        if (resp.find("\"captured\":true") != std::string::npos) {
            captured = true;
            break;
        }
        if (i == 5) {
            std::string r2;
            sai2agent::AgentClient::Query("{\"cmd\":\"canvas.redraw\"}", r2);
        }
    }
    if (!captured) {
        diag = "no snapshot";
        return false;
    }

    // 取快照。这里必须能容忍"快照在 state 与 save 之间被清掉"：
    // canvas.arm 会清空快照，而面板的其它链路（或上一轮残留的 arm）可能
    // 恰好插在中间 —— 实测 3 次里有 2 次 save 返回 no canvas snapshot。
    // 兜底：重新 arm → 强制重绘 → 等新快照 → 再 save，最多重试 2 轮。
    std::string saveResp;
    std::string saveReason;
    bool saved = false;
    for (int attempt = 0; attempt < 3 && !saved; ++attempt) {
        saveResp.clear();
        // 用显式实例而不是 Query()，失败时才能把 lastErr 报出来
        // ——"连不上"和"服务端回了 not-ok"是两件完全不同的事，
        //   以前都只说一句 pipe busy，白查了好几轮。
        sai2agent::AgentClient c;
        if (c.Connect() && c.Send("{\"cmd\":\"canvas.save\"}", saveResp)) {
            if (saveResp.find("\"ok\":true") != std::string::npos) {
                saved = true;
                break;
            }
            saveReason = "agent 返回: " + saveResp;
        } else {
            saveReason = "管道连接/收发失败 (Win32=" +
                         std::to_string(c.lastErr) + ")";
        }
        if (attempt == 2) break;
        Sleep(150);   // 给可能正在用管道的另一条线程让路（重试之间必须有间隔）
        std::string r2;
        sai2agent::AgentClient::Query("{\"cmd\":\"canvas.arm\",\"ms\":9000}", r2);
        sai2agent::AgentClient::Query("{\"cmd\":\"canvas.redraw\"}", r2);
        for (int i = 0; i < 25; ++i) {
            Sleep(100);
            r2.clear();
            if (!sai2agent::AgentClient::Query("{\"cmd\":\"canvas.state\"}", r2))
                continue;
            if (r2.find("\"captured\":true") != std::string::npos) break;
            if (i == 6) {
                std::string r3;
                sai2agent::AgentClient::Query("{\"cmd\":\"canvas.redraw\"}", r3);
            }
        }
    }
    if (!saved) {
        diag = "save failed: " + saveReason;
        return false;
    }
    resp = saveResp;
    std::string file8;
    long long w = 0, h = 0;
    sai2agent::JsonFindString(resp, "file", file8);
    sai2agent::JsonFindInt(resp, "w", w);
    sai2agent::JsonFindInt(resp, "h", h);
    if (file8.empty() || w < 256 || h < 256) {
        diag = "bad snapshot size";
        return false;
    }

    // 快照 = SAI2 的绘制表面。实测它通常是**整个文档视图的后台缓冲**：
    // 画布按当前缩放比例渲染在其中一块子矩形里，四周是视图边距（灰）和
    // 未用区（黑），DIB 的 alpha 通道是垃圾值不能信。所以不能当 1:1 画布
    // 用 —— 必须先做多尺度模板匹配求出缩放 s 和内容位置，再检测"画布纸"
    // 矩形，纸尺寸/s = 画布尺寸、内容相对纸的偏移/s = 原始位置。
    ImageBuffer view;
    std::string err;
    if (!ReadBmp32(file8, view, err) || !view.IsValid()) {
        diag = "read bmp failed";
        return false;
    }

    // 模板 = 刚抓到剪贴板里的图层内容（1:1 画布像素，alpha 作掩码）
    ImageBuffer tmpl;
    if (!ReadClipboardImage(tmpl, err) || !tmpl.IsValid()) {
        diag = "read clipboard failed";
        return false;
    }

    // ★ 精确画布尺寸：从 SAI2 窗口标题拿到当前 .sai2 路径，读它的文件头。
    // 画布尺寸本来只能从快照**推算**（纸矩形 / 缩放），实测有 1%~3.5% 的波动，
    // 落位残差就是这么来的。而 .sai2 头部里有精确的宽高，读出来即可把尺度
    // 钉死 —— 不依赖任何逆向/内存读取，只是读一个普通文件。
    int docW = 0, docH = 0;
    Sai2DocCanvasSize(docW, docH);
    // ★ 先把快照裁到"非黑内容区"再定位。
    //   agent 抓的画布快照里，画布之外是一大片**纯黑未用区**（实测整幅 69.9% 是黑）。
    //   它会让"纸色"被识别成黑（[paper] rgb=(0,0,0)），并把粗搜尺度带偏
    //   （实测 s=0.18，真值≈0.333）→ 定位失败 → 应用时退化成普通粘贴 →
    //   整张画布尺寸的结果被**错位**贴上（用户看到"一大片淡黄糊住画面"）。
    int cropX = 0, cropY = 0;
    {
        int x0 = view.Width(), y0 = view.Height(), x1 = -1, y1 = -1;
        for (int y = 0; y < view.Height(); ++y)
            for (int x = 0; x < view.Width(); ++x) {
                RGBA c;
                if (!view.GetPixel(x, y, c)) continue;
                // 近黑 = 未用区（真实画布/纸几乎不会是纯黑）
                if (c.r < 24 && c.g < 24 && c.b < 24) continue;
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
            }
        if (x1 > x0 && y1 > y0 &&
            (x1 - x0 + 1) < view.Width()) {   // 确实有黑边才裁
            ImageBuffer cr(x1 - x0 + 1, y1 - y0 + 1, view.Format());
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x) {
                    RGBA c;
                    if (view.GetPixel(x, y, c)) cr.SetPixel(x - x0, y - y0, c);
                }
            const bool ok = LocateCanvasCtx(cr, tmpl, out, diag, docW, docH);
            if (ok) {
                out.x0 += x0;   // 偏移换算回原快照坐标
                out.y0 += y0;
            }
            return ok;
        }
    }
    (void)cropX;
    (void)cropY;
    return LocateCanvasCtx(view, tmpl, out, diag, docW, docH);
}

// 定位"画布上下文"：纯数学部分，输入只有两张 RGBA8 图 ——
//   view = SAI2 绘制表面快照     tmpl = 剪贴板里的图层（1:1 文档像素）
//   docW/docH = .sai2 头部读到的精确画布尺寸（0 = 未知，则退回推算）
// 与 agent/剪贴板完全解耦，因此可以用 --match-test 离线复跑（两条链路抢
// 同一个管道时无法稳定复现问题，这个测试缝就是为此留的）。
bool LocateCanvasCtx(const ImageBuffer& view, const ImageBuffer& tmpl,
                     CanvasCtxResult& out, std::string& diag, int docW,
                     int docH) {
    if (!view.IsValid() || view.Width() < 256 || view.Height() < 256) {
        diag = "bad snapshot size";
        return false;
    }
    if (!tmpl.IsValid() || tmpl.Width() < 8 || tmpl.Height() < 8) {
        diag = "template too small";
        return false;
    }
    const int vw = view.Width(), vh = view.Height();
    const int tw = tmpl.Width(), th = tmpl.Height();

    // ---- Pass A：尺度网格粗搜（视图降采样 8 倍，位置步长 2）----
    // 关键：纯色内容在「模板缩小到完全落入内容内部」的所有尺度上 SAD≈0
    // （退化匹配），不能取全局最小 SAD。SAD 在真实尺度以下是平台、超过
    // 真实尺度急剧上升 —— 拐点即真实尺度，所以取 **SAD≤阈值的最大尺度**。
    static const double kScales[] = {
        1.0, 0.9, 0.8, 0.7, 0.6, 0.5, 0.45, 0.4, 0.35, 0.3, 0.25, 0.22,
        0.2, 0.18, 0.16, 0.15, 0.14, 0.13, 0.12, 0.11, 0.1,
        1.15, 1.3, 1.5, 1.75, 2.0, 2.3, 2.6, 3.0, 3.5, 4.0};
    constexpr double kSadEps = 40.0;   // 粗搜阈值（原 15 对"稀疏/软边"图层过严：实测头发层报 coarse no scale）
    const ImageBuffer v8 = DownscaleRGBA(view, 8);
    double bestS = 0.0;
    int bestX8 = -1, bestY8 = -1;
    double bestSad = 1e9;
    // ★ 加**密集**尺度扫描：kScales 的步长太粗（0.35 与 0.3 之间漏掉真实的
    //   0.333），而"稀疏 + 软边"的图层在 ±5% 尺度误差下 SAD 就会超标 →
    //   实测报 "coarse no scale"（头发层 1930x1391，SAI2 缩放 33.3%）。
    //   加 0.08~0.70 步长 0.01 的一圈，真实尺度必被命中（误差 ≤0.5%）。
    std::vector<double> scales;
    scales.reserve(static_cast<size_t>(64));
    for (double s : kScales) scales.push_back(s);
    for (double s = 0.08; s <= 0.70 + 1e-9; s += 0.01) scales.push_back(s);
    for (double s : scales) {
        const int rw = static_cast<int>(std::lround(tw * s));
        const int rh = static_cast<int>(std::lround(th * s));
        if (rw < 16 || rh < 16 || rw > vw || rh > vh) continue;
        const ImageBuffer t = ResampleNearest(tmpl, rw, rh);
        const ImageBuffer t8 = DownscaleRGBA(t, 8);
        if (t8.Width() < 2 || t8.Height() < 2) continue;
        const std::vector<TplSample> smp = BuildSamples(t8, 220);
        int bx = -1, by = -1;
        double bs = 1e9;
        // 位置步长要随模板大小自适应：粗搜在 8 倍降采样空间里进行，
        // 步长 2 单元 = 16 视图像素。对大模板（心形降采样后 61x37）这没问题；
        // 但小模板（281px 的圆在 33% 缩放下只有 94 视图像素 → 降采样后仅
        // 12x12，几乎全是边缘单元）半个单元错位就足以让 SAD 爆掉 →
        // 整个精确回贴会报 "coarse no scale" 静默退化成普通粘贴。
        const int posStep = (t8.Width() < 24 || t8.Height() < 24) ? 1 : 2;
        MaskedSearch(v8, smp, t8.Width(), t8.Height(), 0, 0,
                     v8.Width() - t8.Width(), v8.Height() - t8.Height(), posStep,
                     bx, by, bs);
        if (bx >= 0 && bs <= kSadEps &&
            (bestX8 < 0 || s > bestS)) {
            bestS = s; bestSad = bs; bestX8 = bx; bestY8 = by;
        }
    }
    if (bestX8 < 0) {
        diag = "coarse no scale with sad<=" + std::to_string(kSadEps);
        return false;
    }

    // ---- Pass A2：拐点细扫（0.25% 步长向上，直到 SAD 越过阈值）----
    for (int i = 1; i <= 60; ++i) {
        const double s = bestS * std::pow(1.0025, i);
        if (s > bestS * 1.15) break;
        const int rw = static_cast<int>(std::lround(tw * s));
        const int rh = static_cast<int>(std::lround(th * s));
        if (rw < 8 || rh < 8 || rw > vw || rh > vh) break;
        const ImageBuffer t = ResampleNearest(tmpl, rw, rh);
        const ImageBuffer t8 = DownscaleRGBA(t, 8);
        if (t8.Width() < 2 || t8.Height() < 2) break;
        const std::vector<TplSample> smp = BuildSamples(t8, 220);
        int bx = -1, by = -1;
        double bs = 1e9;
        MaskedSearch(v8, smp, t8.Width(), t8.Height(),
                     bestX8 - 3, bestY8 - 3, bestX8 + 3, bestY8 + 3, 1,
                     bx, by, bs);
        if (bx < 0 || bs > kSadEps) break;
        bestS = s; bestSad = bs; bestX8 = bx; bestY8 = by;
    }

    // ---- Pass B：2 倍降采样位置细化 ----
    const ImageBuffer v2 = DownscaleRGBA(view, 2);
    int fx = bestX8 * 4, fy = bestY8 * 4;
    {
        const int rw = static_cast<int>(std::lround(tw * bestS));
        const int rh = static_cast<int>(std::lround(th * bestS));
        if (rw >= 8 && rh >= 8 && rw <= vw && rh <= vh) {
            const ImageBuffer t = ResampleNearest(tmpl, rw, rh);
            const ImageBuffer t2 = DownscaleRGBA(t, 2);
            if (t2.Width() >= 2 && t2.Height() >= 2) {
                const std::vector<TplSample> smp = BuildSamples(t2, 700);
                const int cx = bestX8 * 4, cy = bestY8 * 4; // ds8 → ds2 = ×4
                int bx = -1, by = -1;
                double bs = 1e9;
                MaskedSearch(v2, smp, t2.Width(), t2.Height(),
                             std::max(0, cx - 14), std::max(0, cy - 14),
                             std::min(v2.Width() - t2.Width(), cx + 14),
                             std::min(v2.Height() - t2.Height(), cy + 14), 1,
                             bx, by, bs);
                if (bx >= 0) { fx = bx * 2; fy = by * 2; bestSad = bs; }
            }
        }
    }

    // ---- 位置精修：全分辨率固定尺度 ±3 ----
    {
        const int rw = static_cast<int>(std::lround(tw * bestS));
        const int rh = static_cast<int>(std::lround(th * bestS));
        if (rw >= 8 && rh >= 8 && rw <= vw && rh <= vh) {
            const ImageBuffer t = ResampleNearest(tmpl, rw, rh);
            const std::vector<TplSample> smp = BuildSamples(t, 1500);
            int bx = -1, by = -1;
            double bs = 1e9;
            MaskedSearch(view, smp, rw, rh,
                         std::max(0, fx - 3), std::max(0, fy - 3),
                         std::min(vw - rw, fx + 3),
                         std::min(vh - rh, fy + 3), 1, bx, by, bs);
            if (bx >= 0) { fx = bx; fy = by; bestSad = bs; }
        }
    }
    if (bestSad > 25.0) {
        diag = "final sad=" + std::to_string(bestSad) +
               " s=" + std::to_string(bestS);
        return false;
    }

    // ---- Pass C：轮廓尺度精修（见 BuildSilhouetteSamples 的说明）----
    // 这一步专门修"实心色块"造成的尺度过估。
    // 关键细节：候选尺度的预测位置必须以**纸原点**为锚点伸缩，而不是以视图
    // 原点。内容相对纸的位置随尺度缩放、纸本身不动；若按视图原点伸缩，尺度
    // 一旦偏离初值预测位置就跑偏，搜索窗覆盖不到，最小点被拉回初值附近
    // （实测只能从 0.519 修到 0.514，而真值是 0.500）。
    {
        RGBA paperC{};
        double paperFrac = 0.0;
        const int cm = std::max(8, static_cast<int>(std::lround(12 * bestS)));
        const int cbw = static_cast<int>(std::lround(tw * bestS));
        const int cbh = static_cast<int>(std::lround(th * bestS));
        if (canvasctx::ModalColor(view, fx - cm - 12, fy - cm - 12,
                                  fx + cbw + cm + 12, fy + cbh + cm + 12, 2,
                                  0.5, paperC, paperFrac,
                                  fx - cm, fy - cm, fx + cbw + cm,
                                  fy + cbh + cm)) {
            // 纸矩形（与后面正式检测同一套判据）：从粗内容矩形出发向纸色扩散
            int px0 = fx, py0 = fy, px1 = fx + cbw - 1, py1 = fy + cbh - 1;
            for (int y = 0; y < vh; y += 2) {
                for (int x = 0; x < vw; x += 2) {
                    if (!canvasctx::ColorNear(view, x, y, paperC, 3)) continue;
                    if (x < px0) px0 = x;
                    if (x > px1) px1 = x;
                    if (y < py0) py0 = y;
                    if (y > py1) py1 = y;
                }
            }

            const ImageBuffer v2c = DownscaleRGBA(view, 2);
            const int v2w = v2c.Width(), v2h = v2c.Height();
            double bestScore = 1e18, scoreAtCoarse = 1e18;
            double bestSc = bestS;
            int bestPx = fx / 2, bestPy = fy / 2;
            for (int k = -20; k <= 20; ++k) {
                const double s = bestS * (1.0 + k * 0.005);
                const int rw2 = static_cast<int>(std::lround(tw * s / 2.0));
                const int rh2 = static_cast<int>(std::lround(th * s / 2.0));
                if (rw2 < 4 || rh2 < 4 || rw2 > v2w || rh2 > v2h) continue;
                const ImageBuffer t2 = ResampleNearest(tmpl, rw2, rh2);
                std::vector<SilSample> smp;
                BuildSilhouetteSamples(t2, paperC, smp, 2000);
                if (smp.empty()) continue;
                // 以纸原点为锚点，把内容相对纸的偏移按 s 伸缩。
                // 注意：粗尺度过估时它给出的 fx 本身就偏，锚点预测只能当近似，
                // 所以这里用**大窗口 + 位置步长 2**兜住不确定性。
                const double relx = (fx - px0) * s / bestS;
                const double rely = (fy - py0) * s / bestS;
                const int pxc = static_cast<int>(
                    std::lround((px0 + relx) / 2.0));
                const int pyc = static_cast<int>(
                    std::lround((py0 + rely) / 2.0));
                int bpx = -1, bpy = -1;
                double sc = 1e18;
                SilhouetteSearch(v2c, smp, rw2, rh2, pxc, pyc, 24, bpx, bpy, sc,
                                 2);
                // 粗搜用步长 2（快），但相邻尺度的分数差只有 ~0.3，位置量化
                // 噪声同量级 → 曲线会被噪声主导（实测全局最小被拉到 0.5087，
                // 而真值 0.5009 处只是个局部最小）。这里对每个尺度再做一次
                // 步长 1 的局部精修，让各尺度的分数可比。
                if (bpx >= 0) {
                    int rx = -1, ry = -1;
                    double rsc = 1e18;
                    SilhouetteSearch(v2c, smp, rw2, rh2, bpx, bpy, 2, rx, ry,
                                     rsc, 1);
                    if (rx >= 0) { bpx = rx; bpy = ry; sc = rsc; }
                }
                if (bpx < 0) continue;
                if (k == 0) scoreAtCoarse = sc;
                if (g_ctxDebugCurve)
                    std::printf("  [sil] s=%.5f n=%zu score=%.2f pos=(%d,%d)\n",
                                s, smp.size(), sc, bpx, bpy);
                if (sc < bestScore) {
                    bestScore = sc; bestSc = s; bestPx = bpx; bestPy = bpy;
                }
            }
            if (bestPx >= 0 && bestScore < scoreAtCoarse * 0.99) {
                bestS = bestSc;
                fx = bestPx * 2;   // v2 坐标 → 全分辨率坐标
                fy = bestPy * 2;
                // 用同一套不透明样本复核尺度，供下面的阈值判断
                const int rw = static_cast<int>(std::lround(tw * bestS));
                const int rh = static_cast<int>(std::lround(th * bestS));
                if (rw >= 8 && rh >= 8 && rw <= vw && rh <= vh) {
                    const ImageBuffer t = ResampleNearest(tmpl, rw, rh);
                    const std::vector<TplSample> smp = BuildSamples(t, 1500);
                    int bx = -1, by = -1;
                    double bs = 1e9;
                    MaskedSearch(view, smp, rw, rh, std::max(0, fx - 3),
                                 std::max(0, fy - 3),
                                 std::min(vw - rw, fx + 3),
                                 std::min(vh - rh, fy + 3), 1, bx, by, bs);
                    if (bx >= 0) { fx = bx; fy = by; bestSad = bs; }
                }
            }
        }
    }

    const double s = bestS;
    const int bx = fx, by = fy;
    const int bw = static_cast<int>(std::lround(tw * s));
    const int bh = static_cast<int>(std::lround(th * s));

    // ---- 画布纸检测（不依赖粗搜内容框是否准确）----------------------------
    // 旧做法：取"内容框外围环带"的模态色当纸色。但内容框来自粗搜，小模板 /
    // 高对比内容时它可能不可靠 —— 环带会取到**内容自己的颜色**当纸色，于是纸
    // 矩形、画布尺寸、偏移一起崩（实测：960px 的纸被测成 425px，随后被精确画布
    // 尺寸的自洽校验以 dS=54% 否决）。
    // 新做法：全视图统计**大面积均匀色块**，按下面的优先级挑纸：
    //   ① 必须包容内容框（内容框只当"内容在哪儿"的提示，不要求它准确）；
    //   ② 填充率够高（矩形块应填满自己的 bbox，散落同色像素被挡掉）；
    //   ③ 排除"几乎铺满整个视图"的候选 —— 那是视口灰底，不是纸；
    //   ④ 剩下的取**面积最大**者（纸比内容里的平坦色块大）。
    RGBA paper{};
    double paperFrac = 0.0;
    bool paperFound = false;
    // 主路径：内容外围环带模态色（一直在用的做法，心形等大内容上已验证）
    {
        RGBA p1{};
        double f1 = 0.0;
        const int m1 = std::max(8, static_cast<int>(std::lround(12 * s)));
        if (canvasctx::ModalColor(view, bx - m1 - 12, by - m1 - 12,
                                  bx + bw + m1 + 12, by + bh + m1 + 12, 2, 0.5,
                                  p1, f1, bx - m1, by - m1, bx + bw + m1,
                                  by + bh + m1)) {
            paper = p1;
            paperFrac = f1;
            paperFound = true;
        }
    }
    // ★ 纸色不可能是纯黑：agent 抓的画布快照里，**画布之外的未用区常被填成黑**。
    //   环带一旦落在那里，modal 色就是黑 → 后续全错。实测（博丽灵梦 2160x3280）：
    //   [paper] rgb=(0,0,0) → ambiguous paper color → 定位失败 → 应用时普通粘贴
    //   → 整张画布尺寸的结果被错位贴上（用户看到"一大片淡黄糊住画面"）。
    //   把"黑纸色"作废，交给下面的"最大均匀亮区"检测兜底。
    if (paperFound && paper.r < 40 && paper.g < 40 && paper.b < 40)
        paperFound = false;
    if (!paperFound) {
        struct Region {
            RGBA c{};
            long long count = 0;
            int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1;
        };
        const int step = std::max(1, std::min(vw, vh) / 400);
        std::unordered_map<int, Region> regs;
        regs.reserve(8192);
        for (int y = 0; y < vh; y += step) {
            for (int x = 0; x < vw; x += step) {
                RGBA p;
                if (!view.GetPixel(x, y, p)) continue;
                const int key = (static_cast<int>(p.r >> 4) << 8) |
                                (static_cast<int>(p.g >> 4) << 4) |
                                static_cast<int>(p.b >> 4);
                Region& r = regs[key];
                if (r.count == 0) r.c = p;
                ++r.count;
                if (x < r.x0) r.x0 = x;
                if (x > r.x1) r.x1 = x;
                if (y < r.y0) r.y0 = y;
                if (y > r.y1) r.y1 = y;
            }
        }
        const int cx0 = bx, cy0 = by, cx1 = bx + bw - 1, cy1 = by + bh - 1;
        const double viewArea = static_cast<double>(vw) * vh;
        long long bestArea = -1;
        for (std::unordered_map<int, Region>::const_iterator it = regs.begin();
             it != regs.end(); ++it) {
            const Region& r = it->second;
            if (r.count < 200) continue;          // 太小的块不可能是纸
            const long long rw = r.x1 - r.x0 + 1, rh = r.y1 - r.y0 + 1;
            if (rw < 64 || rh < 64) continue;     // 纸不可能这么小
            if (r.x0 > cx0 || r.y0 > cy0 || r.x1 < cx1 || r.y1 < cy1)
                continue;                         // ① 不包容内容
            const long long area = rw * rh;
            const double fill = static_cast<double>(r.count) *
                                static_cast<double>(step) * step /
                                static_cast<double>(area);
            if (fill < 0.45) continue;            // ② 不成矩形
            if (static_cast<double>(area) >= viewArea * 0.95)
                continue;                         // ③ 这是视口底/边距
            if (area <= bestArea) continue;       // ④ 取面积最大者
            bestArea = area;
            paper = r.c;
            paperFrac = fill;
            paperFound = true;
        }
    }
    if (false) {
        // 【已停用】这一步原本想用较宽容差复核纸矩形，但实测有害：把 paper
        // 拉向"大面积均值"（含视口灰底）→ paper 与 frame 同色 → 触发
        // ambiguous paper color，三组夹具（心形/合成小圆/真机小圆）全部失败。
        // 保留代码备查，但不再执行。
        int x0 = vw, y0 = vh, x1 = -1, y1 = -1;
        long long in = 0, tot = 0;
        for (int y = 0; y < vh; y += 2) {
            for (int x = 0; x < vw; x += 2) {
                ++tot;
                if (!canvasctx::ColorNear(view, x, y, paper, 8)) continue;
                ++in;
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
            }
        }
        if (x1 > x0 && y1 > y0) {
            paperFrac = static_cast<double>(in) /
                        static_cast<double>(std::max<long long>(1, tot));
            // 修正代表色为该范围内像素的均值，减少量化偏差
            long long sr = 0, sg = 0, sb = 0, n = 0;
            for (int y = y0; y <= y1; y += 4) {
                for (int x = x0; x <= x1; x += 4) {
                    RGBA p;
                    if (!view.GetPixel(x, y, p)) continue;
                    if (!canvasctx::ColorNear(view, x, y, paper, 8)) continue;
                    sr += p.r;
                    sg += p.g;
                    sb += p.b;
                    ++n;
                }
            }
            if (n > 0) {
                RGBA m{};
                m.r = static_cast<uint16_t>(sr / n);
                m.g = static_cast<uint16_t>(sg / n);
                m.b = static_cast<uint16_t>(sb / n);
                paper = m;
            }
        }
    }
    if (!paperFound) {
        // 兜底：退回旧的环带法（视图被严重裁剪、纸区不完整时）
        RGBA p2{};
        double f2 = 0.0;
        const int m2 = std::max(8, static_cast<int>(std::lround(12 * s)));
        if (canvasctx::ModalColor(view, bx - m2 - 12, by - m2 - 12,
                                  bx + bw + m2 + 12, by + bh + m2 + 12, 2, 0.5,
                                  p2, f2, bx - m2, by - m2, bx + bw + m2,
                                  by + bh + m2)) {
            paper = p2;
            paperFrac = f2;
            paperFound = true;
        }
    }
    if (!paperFound) {
        diag = "paper region not found s=" + std::to_string(s);
        return false;
    }
    if (g_ctxDebugCurve)
        std::printf("[paper] rgb=(%d,%d,%d)\n", static_cast<int>(paper.r),
                    static_cast<int>(paper.g), static_cast<int>(paper.b));
    // 视图最外框模态色（边距/未用区）
    RGBA frame{};
    double frameFrac = 0.0;
    canvasctx::ModalColor(view, 0, 0, vw - 1, vh - 1, 3, 0.0, frame,
                          frameFrac, 4, 4, vw - 5, vh - 5);

    const bool paperEqFrame =
        std::abs(static_cast<int>(paper.r) - static_cast<int>(frame.r)) <= 4 &&
        std::abs(static_cast<int>(paper.g) - static_cast<int>(frame.g)) <= 4 &&
        std::abs(static_cast<int>(paper.b) - static_cast<int>(frame.b)) <= 4;

    int cw = 0, ch = 0, cx0 = 0, cy0 = 0;
    // 纸矩形（视图像素）。提到外面：下面要用它 + .sai2 的精确画布尺寸反推尺度。
    int px0 = 0, py0 = 0, pw = 0, ph = 0;
    if (paperEqFrame) {
        // 纸色与外框同色 → 无法靠颜色区分纸和边距。若内容与视图边框之间
        // 几乎全是"纸色"，说明快照本身就是画布 DIB（1:1，无视图边距）。
        //
        // ★ 但已知精确画布尺寸（.sai2 文件头）时必须先做一致性检查：
        // 实测 art.sai2 文件头 bg=#ff808080 与 SAI2 视口留白同色 → paperEqFrame
        // 成立 → 原来直接认定"快照 = 1:1 画布"，得出画布 = 视图尺寸 2592x1472
        // （真值 2160x3280）、偏移全错。画布底色与视口同色（或被视图裁切）时
        // 本来就没有可用的纸矩形，此时应当**明确拒绝并给出可操作提示**。
        if (docW > 0 && docH > 0 &&
            (std::abs(vw - docW) > 8 || std::abs(vh - docH) > 8)) {
            diag = "canvas not locatable: view " + std::to_string(vw) + "x" +
                   std::to_string(vh) + " vs doc " + std::to_string(docW) + "x" +
                   std::to_string(docH) +
                   " (canvas bg == viewport colour / view cropped"
                   " -> please zoom out until the whole canvas is visible)";
            return false;
        }
        long long in = 0, tot = 0;
        for (int y = 0; y < vh; y += 8) {
            for (int x = 0; x < vw; x += 8) {
                const bool insideBlob =
                    x >= bx - 4 && x < bx + bw + 4 && y >= by - 4 &&
                    y < by + bh + 4;
                if (insideBlob) continue;
                ++tot;
                if (canvasctx::ColorNear(view, x, y, paper, 4)) ++in;
            }
        }
        if (tot == 0 || in < tot * 0.9) {
            diag = "ambiguous paper color s=" + std::to_string(s);
            return false;
        }
        cw = vw; ch = vh; cx0 = bx; cy0 = by;
    } else {
        // 纸矩形 = **与内容相连的那块近纸色连通分量**，而不是"所有近纸色
        // 像素的包围盒"。
        // 踩坑（实测 ml2.sai2）：SAI2 的画布视图 DIB 里还画着**两条水平滚动
        // 条**，它们也是白的。用包围盒会把纸和上下两条滚动条并成一块
        // 639x819 的区域（纸真值 645x367），画布高被算成 2.3 倍，进而触发
        // "与 .sai2 尺寸矛盾"的拒绝。滚动条与纸之间隔着灰底、**并不连通**，
        // 所以从内容旁边洪泛一次即可精确取到纸。
        std::vector<uint8_t> mask(static_cast<size_t>(vw) * vh, 0);
        for (int y = 0; y < vh; ++y)
            for (int x = 0; x < vw; ++x)
                if (canvasctx::ColorNear(view, x, y, paper, 8))
                    mask[static_cast<size_t>(y) * vw + x] = 1;
        // 种子：内容框外扩一圈里第一个近纸色像素（上下左右四个方向试）
        int sx = -1, sy = -1;
        for (int d = 1; d <= 64 && sx < 0; ++d) {
            const int cand[4][2] = {{bx - d, by + bh / 2},
                                    {bx + bw - 1 + d, by + bh / 2},
                                    {bx + bw / 2, by - d},
                                    {bx + bw / 2, by + bh - 1 + d}};
            for (int k = 0; k < 4 && sx < 0; ++k) {
                const int x = cand[k][0], y = cand[k][1];
                if (x < 0 || y < 0 || x >= vw || y >= vh) continue;
                if (mask[static_cast<size_t>(y) * vw + x] == 1) {
                    sx = x;
                    sy = y;
                }
            }
        }
        if (sx < 0) {
            diag = "no paper seed near content s=" + std::to_string(s);
            return false;
        }
        int rx0 = sx, ry0 = sy, rx1 = sx, ry1 = sy;
        std::vector<int> stack;
        stack.reserve(1 << 16);
        mask[static_cast<size_t>(sy) * vw + sx] = 2;
        stack.push_back(sy * vw + sx);
        while (!stack.empty()) {
            const int idx = stack.back();
            stack.pop_back();
            const int x = idx % vw, y = idx / vw;
            if (x < rx0) rx0 = x;
            if (x > rx1) rx1 = x;
            if (y < ry0) ry0 = y;
            if (y > ry1) ry1 = y;
            const int nb[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
            for (int k = 0; k < 4; ++k) {
                const int nx = nb[k][0], ny = nb[k][1];
                if (nx < 0 || ny < 0 || nx >= vw || ny >= vh) continue;
                const size_t ni = static_cast<size_t>(ny) * vw + nx;
                if (mask[ni] != 1) continue;
                mask[ni] = 2;
                stack.push_back(ny * vw + nx);
            }
        }
        px0 = rx0; py0 = ry0; pw = rx1 - rx0 + 1; ph = ry1 - ry0 + 1;
        if (pw < 16 || ph < 16) {
            diag = "paper rect too small (cc) s=" + std::to_string(s);
            return false;
        }
        cw = static_cast<int>(std::lround(pw / s));
        ch = static_cast<int>(std::lround(ph / s));
        cx0 = static_cast<int>(std::lround((bx - px0) / s));
        cy0 = static_cast<int>(std::lround((by - py0) / s));
    }

    // ---- 用 .sai2 文件里的精确画布尺寸把尺度钉死（有则用，无则保持推算）----
    // 推算的画布尺寸实测有 1%~3.5% 波动 → 落位残差 3~15 px，全都来自这里。
    // 已知精确画布 (docW,docH) 与纸矩形 (pw,ph) 时，真实缩放 sFile = pw/docW；
    // 再要求高度方向独立自洽（ph/(sFile*docH) ≈ 1）作为交叉校验，两者都过
    // 才采用 —— 这样即使纸矩形被选区蚂蚁线/画笔光标啃掉一两像素，也不会
    // 把错的尺度当成"精确"用。
    std::string exactNote;
    if (docW > 0 && docH > 0 && pw > 0 && ph > 0 && s > 0.0) {
        const double sFile = static_cast<double>(pw) / docW;
        const double sRel = std::fabs(sFile - s) / s;
        const double hRatio = static_cast<double>(ph) / (sFile * docH);
        if (sRel <= 0.08 && std::fabs(hRatio - 1.0) <= 0.02) {
            cw = docW;
            ch = docH;
            cx0 = static_cast<int>(std::lround((bx - px0) / sFile));
            cy0 = static_cast<int>(std::lround((by - py0) / sFile));
            exactNote = " [exact " + std::to_string(docW) + "x" +
                        std::to_string(docH) + " from .sai2, sFile=" +
                        std::to_string(sFile) + " dS=" +
                        std::to_string(sRel * 100.0) + "%]";
        } else {
            // 与已知事实矛盾 → **拒绝**，不要拿这个估计值去准备粘贴。
            // 原因：纸矩形量错时，估计出的画布/偏移可能离谱（实测 ml2.sai2：
            // 宽度差 0.66% 但高度差 127.9%），若继续用就会把内容贴到错误位置。
            // 宁可明确失败并让上层提示用户，也好过"看起来成功了但贴歪了"。
            diag = "paper rect contradicts .sai2 size: est canvas=" +
                   std::to_string(cw) + "x" + std::to_string(ch) + " vs doc " +
                   std::to_string(docW) + "x" + std::to_string(docH) + " (dS=" +
                   std::to_string(sRel * 100.0) + "% dH=" +
                   std::to_string((hRatio - 1.0) * 100.0) + "%)";
            return false;
        }
    }

    // ---- 一致性检查 + 边缘贴合微调（±3px 内贴齐画布边/内容边）----
    if (cw < 16 || ch < 16 || cw > 16384 || ch > 16384 ||
        cx0 < -3 || cy0 < -3 || cx0 + bw > cw + 3 || cy0 + bh > ch + 3) {
        diag = "inconsistent ctx s=" + std::to_string(s) +
               " canvas=" + std::to_string(cw) + "x" + std::to_string(ch) +
               " off=" + std::to_string(cx0) + "," + std::to_string(cy0);
        return false;
    }
    if (cx0 < 0) cx0 = 0;
    if (cy0 < 0) cy0 = 0;
    if (cx0 + bw > cw) cx0 = cw - bw;
    if (cy0 + bh > ch) cy0 = ch - bh;
    if (cx0 <= 3) cx0 = 0;   // 贴左缘
    if (cy0 <= 3) cy0 = 0;
    if (bw >= cw - 3) cx0 = 0;
    if (bh >= ch - 3) cy0 = 0;

    out.cw = cw;
    out.ch = ch;
    out.x0 = cx0;
    out.y0 = cy0;
    out.score = bestSad;
    out.ok = true;
    diag = "s=" + std::to_string(s) + " sad=" + std::to_string(bestSad) +
           exactNote;
    return true;
}

// 后台线程的阶段推进
void RunCaptureJob() {
    CaptureJob& j = g_cap;
    j.t0 = GetTickCount64();
    j.grabbed = false;
    j.message.clear();
    j.clipSeq = GetClipboardSequenceNumber();
    AppState& st = S();

    // 0) 画布上下文预备：先 arm 再抓 —— 抓取期间的前台切换/Ctrl+A 会触发
    //    画布重绘，钩子才能拿到画布 DIB（= 画布尺寸）。
    const bool ctxArmed =
        st.agentOnline && st.pasteInPlace && [&]() {
            std::string resp;
            return sai2agent::AgentClient::Query(
                       "{\"cmd\":\"canvas.arm\",\"ms\":9000}",
                       resp) &&
                   resp.find("\"ok\":true") != std::string::npos;
        }();
    if (!ctxArmed) st.core->SetCanvasContext(false, 0, 0, 0, 0);

    // 用户正在拖拽/按住鼠标时推迟注入：拖拽中途插入合成输入会污染
    // 系统输入状态机（拖拽失效的根源之一），等他松手再动。
    WaitMouseReleased(3000);

    HWND sai = FindSai2Window();
    if (!sai) {
        j.message = L"未找到 SAI2 窗口（sai2.exe 未运行？）";
        j.phase = CAP_FAIL;
        ReleaseSyntheticKeys();
        PostMessageW(S().mainHwnd, WM_CAPTURE_DONE, 0, 0);
        return;
    }

    // 1) 点图层行（若有选中行）
    if (j.row >= 0 && j.panelLeft > 0 && j.rowScreenY > 0) {
        ForceForeground(sai);
        Sleep(140);
        const POINT p = AgentToScreen(j.panelLeft + 34, j.rowScreenY + 6);
        ClickAtScreenImpl(p.x, p.y);
        Sleep(260);
    } else {
        ForceForeground(sai);
        Sleep(180);
    }

    // 2) 快捷键抓取：Ctrl+A 全选 + Ctrl+C 拷贝。
    //    菜单点击链路（截屏找文字+模拟点击）太脆弱：菜单没弹出/前台切换失败/
    //    文字没截到，任何一步失败窗口就永远开不出来。快捷键只要 SAI2 在前台
    //    就一定生效（readme 里手动 Ctrl+C 的流程就是证明）。
    //    整段重试一次：前台切换偶发失败/首个按键被吞是「浮窗点了没反应」
    //    的主因，用户看到的就是点了按钮什么都没发生。
    for (int attempt = 0; attempt < 2 && !j.grabbed; ++attempt) {
        ForceForeground(sai);
        Sleep(attempt > 0 ? 260 : 100);
        if (GetForegroundWindow() != sai) continue;
        SendCtrlScan(0x1E); // Ctrl+A 全选
        Sleep(attempt > 0 ? 450 : 350);
        SendCtrlScan(0x2E); // Ctrl+C 拷贝
        for (int i = 0; i < 20; ++i) {
            Sleep(100);
            if (GetClipboardSequenceNumber() != j.clipSeq &&
                ClipboardHasImage()) {
                j.grabbed = true;
                break;
            }
        }
        if (!j.grabbed && ClipboardHasImage()) j.grabbed = true;
    }

    // 3) 兜底：快捷键没拿到图再退回旧菜单点击链路
    if (!j.grabbed) {
        if (!MenuItemClickImpl(sai, 120, L"全选")) {
            j.message = L"抓取失败：Ctrl+A/Ctrl+C 未生效，也未定位到「全选」菜单项";
            j.phase = CAP_FAIL;
            ReleaseSyntheticKeys();
            PostMessageW(S().mainHwnd, WM_CAPTURE_DONE, 0, 0);
            return;
        }
        Sleep(200);

        if (!MenuItemClickImpl(sai, 120, L"拷贝")) {
            j.message = L"抓取失败：未定位到 SAI2 的「拷贝」菜单项";
            j.phase = CAP_FAIL;
            ReleaseSyntheticKeys();
            PostMessageW(S().mainHwnd, WM_CAPTURE_DONE, 0, 0);
            return;
        }

        for (int i = 0; i < 30; ++i) {
            Sleep(100);
            if (GetClipboardSequenceNumber() != j.clipSeq && ClipboardHasImage()) {
                j.grabbed = true;
                break;
            }
        }
        if (!j.grabbed && ClipboardHasImage()) j.grabbed = true;
        if (!j.grabbed) j.message = L"SAI2 没有把图像放进剪贴板（拷贝失败？）";
    }
    // 4) 画布上下文（精确回贴）：读画布快照 + 模板匹配求内容偏移
    if (j.grabbed && ctxArmed) {
        CanvasCtxResult ctx;
        std::string diag;
        if (AcquireCanvasContext(ctx, diag)) {
            st.core->SetCanvasContext(true, ctx.cw, ctx.ch, ctx.x0, ctx.y0);
        } else {
            st.core->SetCanvasContext(false, 0, 0, 0, 0);
            st.core->Log("canvas ctx: locate failed (" + diag +
                         "), fallback to plain paste");
            // 定位失败 → 不补边 → 粘贴必然错位。必须明确告诉用户，
            // 否则现象只是"粘回去偏了"，看不出是哪一步没生效。
            if (j.message.empty())
                j.message = L"⚠ 画布定位失败（" +
                            std::wstring(diag.begin(), diag.end()) +
                            L"）→ 本次粘贴会错位，请把该提示反馈给开发者";
        }
    } else if (j.grabbed && !ctxArmed) {
        // 没有画布上下文 = 不会补边 = 粘贴必然错位。
        // 这是最容易被误判成"修复没生效"的分支，必须显式报警并说明原因。
        const wchar_t* why = !st.agentOnline
                                 ? L"未连接注入代理（请先点「连接注入」）"
                                 : !st.pasteInPlace
                                       ? L"未勾选「精确回贴（补边对齐）」"
                                       : L"画布快照 arm 失败";
        j.message = std::wstring(L"⚠ 无法精确回贴：") + why +
                    L" → 本次粘贴会错位";
        st.core->Log("WARN 无法精确回贴（ctx 未获取）: " +
                     std::string(!st.agentOnline ? "agent 未连接"
                                 : !st.pasteInPlace ? "未勾选精确回贴"
                                                    : "arm 失败"));
    }

    j.phase = j.grabbed ? CAP_DONE : CAP_FAIL;
    // 保险丝：确保 Ctrl/Alt/Esc/鼠标键没有逻辑上残留在按下状态
    ReleaseSyntheticKeys();
    PostMessageW(S().mainHwnd, WM_CAPTURE_DONE, 0, 0);
}

} // namespace

// ---------------------------------------------------------------------------
// 公共 API
// ---------------------------------------------------------------------------
HWND FindSai2Window() {
    const DWORD pid = FindProcessIdByName(L"sai2.exe");
    if (!pid) return nullptr;
    EnumCtx c;
    c.pid = pid;
    EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&c));
    return c.found;
}

void ForceForeground(HWND target) {
    if (!target) return;
    // 最小化/图标化的窗口：SetForegroundWindow 不会还原它，后续所有
    // 合成鼠标点击都会落到 (-32000,-32000) 的假坐标上，而且画布不会重绘
    // （精确回贴依赖画布快照）。实测遇到过 SAI2 被最小化时整条链路静默错位。
    if (IsIconic(target)) {
        ShowWindow(target, SW_RESTORE);
        Sleep(200);
    }
    HWND fore = GetForegroundWindow();
    if (fore == target) return;
    const DWORD foreThread = GetWindowThreadProcessId(fore, nullptr);
    const DWORD ourThread = GetCurrentThreadId();
    AttachThreadInput(ourThread, foreThread, TRUE);
    BringWindowToTop(target);
    SetForegroundWindow(target);
    SetFocus(target);
    AttachThreadInput(ourThread, foreThread, FALSE);
    if (GetForegroundWindow() != target) {
        keybd_event(VK_MENU, 0, 0, 0);
        SetForegroundWindow(target);
        keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
    }
}

void ClickAtScreen(int x, int y) { ClickAtScreenImpl(x, y); }

// Ctrl+点击：多选图层用。坐标换算与 ClickAtScreenImpl 完全一致，只是在
// 移动/按下/抬起的前后按住并松开 Ctrl —— SAI2 读的是点击瞬间的修饰键状态，
// 所以必须让 Ctrl 与点击在**同一个 SendInput 批次**里按顺序下发。
void CtrlClickAtScreenImpl(int x, int y) {
    WaitMouseReleased(1000);
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    INPUT in[5] = {};
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wVk = VK_CONTROL;
    in[1].type = INPUT_MOUSE;
    in[1].mi.dwFlags =
        MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    in[1].mi.dx = static_cast<LONG>((x - vx) * 65536 / std::max(1, vw));
    in[1].mi.dy = static_cast<LONG>((y - vy) * 65536 / std::max(1, vh));
    in[2].type = INPUT_MOUSE;
    in[2].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    in[3].type = INPUT_MOUSE;
    in[3].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    in[4].type = INPUT_KEYBOARD;
    in[4].ki.wVk = VK_CONTROL;
    in[4].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(5, in, sizeof(INPUT));
}

bool FindCapturedText(const wchar_t* text, int& outX, int& outY,
                      ULONGLONG minTick) {
    std::string resp;
    if (!sai2agent::AgentClient::Query("{\"cmd\":\"text.recent\",\"n\":200}",
                                       resp))
        return false;
    size_t pos = resp.find("\"items\":[");
    if (pos == std::string::npos) return false;
    size_t cur = pos + 9;
    bool found = false;
    int fx = 0, fy = 0;
    size_t objBegin;
    while (sai2bridge::detail::NextObject(resp, cur, resp.size(), &objBegin,
                                          &cur)) {
        const std::string obj = resp.substr(objBegin, cur - objBegin);
        std::string t8;
        if (!sai2agent::JsonFindString(obj, "text", t8)) continue;
        const std::wstring t = sai2bridge::detail::Utf8ToWide(t8);
        if (t.rfind(text, 0) != 0) continue;
        long long sx = 0, sy = 0, tick = 0;
        if (!sai2agent::JsonFindInt(obj, "screenX", sx) ||
            !sai2agent::JsonFindInt(obj, "screenY", sy) ||
            !sai2agent::JsonFindInt(obj, "tick", tick))
            continue;
        if (tick < static_cast<long long>(minTick)) continue;
        if (obj.find("\"hasScreen\":true") == std::string::npos) continue;
        if (!found || sx < fx) {
            fx = static_cast<int>(sx);
            fy = static_cast<int>(sy);
            found = true;
        }
    }
    if (found) {
        outX = fx;
        outY = fy;
    }
    return found;
}

bool MenuItemClick(HWND sai, int menuBarOffsetX, const wchar_t* itemPrefix) {
    return MenuItemClickImpl(sai, menuBarOffsetX, itemPrefix);
}

void AutoPasteToSai2() {
    AppState& s = S();
    HWND target = FindSai2Window();
    if (!target) {
        s.flash = L"剪贴板已就绪，但未找到 SAI2 窗口";
        return;
    }
    // 用快捷键而不是菜单点击：不依赖 agent 管道（菜单文字钩子），
    // 也不依赖菜单栏坐标换算 —— 只要 SAI2 在前台就一定生效。
    // 对齐原理（1.19）：SAI2 的 Ctrl+C 只拷贝图层不透明部分包围盒（透明
    // 区域被裁掉），Ctrl+V 又按画布 (0,0) 对齐。整层抓取时桥已记录画布
    // 尺寸与内容偏移，写剪贴板前把结果补边成画布尺寸 —— 图=画布尺寸，
    // (0,0) 对齐就是精确回贴，不依赖 Ctrl+Shift+V。
    WaitMouseReleased(1000);
    ForceForeground(target);
    Sleep(160);
    if (GetForegroundWindow() != target) {
        s.flash = L"无法把 SAI2 切到前台 —— 请手动 Ctrl+V";
        return;
    }
    SendEscape();
    Sleep(200);
    if (s.pasteInPlace && s.core->HasCanvasContext()) {
        SendCtrlScan(0x2F); // Ctrl+V（新建图层，左上角对齐 = 原位）
        ReleaseSyntheticKeys();
        s.flash = L"已精确回贴到画布原位置（新建图层，可按 F 向下转写）";
        return;
    }
    // 无画布上下文（外部剪贴板抓取/定位失败/未勾选）：退回全选+粘贴
    SendCtrlScan(0x1E); // Ctrl+A 全选
    Sleep(400);
    SendCtrlScan(0x2F); // Ctrl+V 粘贴（新建图层）
    ReleaseSyntheticKeys();
    s.flash = s.pasteInPlace
                  ? L"已粘贴回 SAI2（未能定位图层原位置，请核对结果）"
                  : L"已粘贴回 SAI2（新建图层）";
}

// ---------------------------------------------------------------------------
// 抓取入口
// ---------------------------------------------------------------------------
namespace {

void OnCaptureFinished() {
    AppState& s = S();
    const bool grabbed = g_cap.grabbed.load();
    const int want = g_cap.wantWindow.load();
    std::wstring msg = g_cap.message;
    g_cap.phase = CAP_IDLE;
    g_cap.busy = false;
    g_cap.hasJob = false;
    g_cap.wantWindow = -1;

    if (grabbed) {
        g_grabKeepCtx = true;
        DoGrabFromClipboard(s.main);
        g_grabKeepCtx = false;
        s.flash.clear();
        if (want >= 1 && want < BridgeCore::kWinCount) {
            OpenToolWindow(want);
        }
    } else {
        // 抓取失败：之前留下的画布上下文对应的是新一次抓取，作废
        s.core->SetCanvasContext(false, 0, 0, 0, 0);
        if (!msg.empty()) s.flash = msg;
        // 兜底：自动抓取失败，但桥里已经有图像 → 照样打开目标窗口，
        // 绝不能让「点液化没反应」。失败原因保留在状态栏提示里。
        if (want >= 1 && want < BridgeCore::kWinCount && s.core->HasImage()) {
            if (!s.flash.empty()) s.flash += L" —— 已用桥内现有图像打开窗口";
            OpenToolWindow(want);
        }
    }
    InvalidateAll();
}

} // namespace

void RequestToolWindow(HWND hwnd, int winIdx, bool needImage) {
    AppState& s = S();
    const int idx = (winIdx >= 1 && winIdx < BridgeCore::kWinCount) ? winIdx : 0;
    if (idx == 0) return;

    // 已经在抓了：记录下来，抓到后自动开这个窗口
    if (g_cap.busy.load()) {
        if (needImage) g_cap.wantWindow = idx;
        else OpenToolWindow(idx);
        return;
    }

    // 没图就必须先抓；有图时也重新抓一次（保证"针对 SAI2 当前图层"）
    if (needImage && s.agentOnline) {
        RefreshLayers();
        g_cap.row = s.selLayer;
        g_cap.panelLeft = static_cast<int>(s.panelLeft);
        g_cap.rowScreenY =
            (s.selLayer >= 0 && s.selLayer < static_cast<int>(s.layerScreenY.size()))
                ? s.layerScreenY[static_cast<size_t>(s.selLayer)]
                : 0;
        g_cap.wantWindow = idx;
        g_cap.busy = true;
        g_cap.hasJob = true;
        g_cap.phase = CAP_SELECT_LAYER;
        s.flash = L"正在选中并抓取 SAI2 当前图层…";
        InvalidateAll();
        if (g_capThread.joinable()) g_capThread.join();
        g_capThread = std::thread(RunCaptureJob);
        return;
    }

    if (s.core->HasImage()) {
        OpenToolWindow(idx);
    } else {
        s.flash = L"还没有图像：请在 SAI2 里框选后 Ctrl+C，或先点「连接注入」";
        InvalidateAll();
    }
}

bool CaptureInProgress() { return g_cap.busy.load(); }

void OnImageAcquired() {
    AppState& s = S();
    // 图层行信息可能在抓图期间变了，刷新一次
    RefreshLayers();
    InvalidateAll();
    (void)s;
}

void DoGrabFromClipboard(UiWindow& win) {
    (void)win;
    AppState& s = S();
    std::string err;
    // 默认**保留**画布上下文：只走「监听剪贴板 / 从剪贴板抓取」的用户不会经过
    // 抓取路径的第 4 步重新定位，一旦这里清空，窗口就再也拿不到"整幅画布"显示。
    // 偏差由 SetCanvasContext 的"偏移变化 → 平移纠正"和"应用前就地补定位"兜住。
    const bool keepCtx = true;
    g_grabKeepCtx = false;
    g_grabKeepCtx = false;
    if (!keepCtx) s.core->SetCanvasContext(false, 0, 0, 0, 0);
    if (s.core->GrabFromClipboard(err)) {
        // ★ 这里**不能**再去"补定位"：AcquireCanvasContext 会给 SAI2 再发一次
        //   Ctrl+A/Ctrl+C，而它定位的是 SAI2 **当前选中**的层 —— 如果用户复制
        //   的那一层不是当前层，就会拿到错误的画布偏移 → 补边位置错 → 又错位
        //   （实测踩坑）。定位只在两条安全路径做：
        //     1) 抓取流程第 4 步（它本来就会重新选中并复制该层）
        //     2) 应用前（DoApplyFlow，同样会重新选中该层）
        //   所以这里只保留"如果已经有画布上下文就补边"（由 SetImage→
        //   PadBaseToCanvas 完成），不再主动发输入。
        s.flash.clear();
        OnImageAcquired();
    } else {
        s.core->SetCanvasContext(false, 0, 0, 0, 0);
        s.flash = U8("抓取失败: " + err);
        InvalidateAll();
    }
}

// 主面板「抓取 SAI2 图层」按钮 / 全局抓取热键：走完整抓取链路。
// 历史 bug：这个按钮此前经 RunCmd → RequestToolWindow(hwnd, kWinMain)
// 而 winIdx=0 直接 return，是个空操作 —— 用户实际靠"监控剪贴板"抓到
// 的都是自己在 SAI2 里 Ctrl+C 的内容（局部选区/内容包围盒），应用后
// 按 SAI2"画布中心对齐"粘贴就会出现大幅错位。
void DoGrabWholeLayer(HWND hwnd) {
    (void)hwnd;
    AppState& s = S();
    if (s.agentOnline && !g_cap.busy.load()) {
        g_cap.row = -1;        // 不点图层行：直接抓当前激活图层整层
        g_cap.panelLeft = 0;
        g_cap.rowScreenY = 0;
        g_cap.wantWindow = -1; // 不自动打开功能窗口
        g_cap.busy = true;
        g_cap.hasJob = true;
        g_cap.phase = CAP_SELECT_LAYER;
        s.flash = L"正在抓取 SAI2 当前图层（整层）…";
        InvalidateAll();
        if (g_capThread.joinable()) g_capThread.join();
        g_capThread = std::thread(RunCaptureJob);
        return;
    }
    // 没有注入代理时，这里以前**静默退化**为"抓剪贴板当前图像"。
    // 用户点的是「抓取选中图层」，却拿到了剪贴板里任意一张图（甚至是他刚截的
    // 屏），而且因为没有画布上下文，粘回去必然错位 —— 现象只有一个"偏了"，
    // 完全看不出根因。现在显式说明退化原因。
    s.flash = s.agentOnline
                  ? L"抓取任务忙，已改为抓取剪贴板当前图像"
                  : L"未连接注入代理 → 无法抓 SAI2 图层，已退化为抓取剪贴板"
                    L"当前图像（请先点「连接注入」）";
    S().core->Log(s.agentOnline ? "grab: 任务忙 → 退化为抓剪贴板"
                                : "WARN 未连接注入代理 → 退化为抓剪贴板");
    DoGrabFromClipboard(s.main);
}

// ---------------------------------------------------------------------------
// 应用 / 撤销 / 复位
// ---------------------------------------------------------------------------
void DoApplyFlow(UiWindow& win) {
    AppState& s = S();
    // ★ 应用前若缺"画布上下文"，就地补一次定位。
    //
    // 根因（用户实测："液化不错位了，但滤镜/调整/模糊还错位"）：
    //   液化窗口是点「液化」按钮打开的 —— 那条路会先抓图层并
    //   `SetCanvasContext(true, …)`，所以应用时补边正确；
    //   而滤镜/调整/模糊窗口是用**已经抓到的图**直接打开的，若用户是先在
    //   SAI2 里 Ctrl+C（面板的剪贴板监控 → DoGrabFromClipboard）拿的图，
    //   那条路会 `SetCanvasContext(false, …)` **把上下文清空**，
    //   于是应用时补边退化成原图 → 粘贴落到 (0,0) → 大幅错位。
    //
    // 所以这里不依赖"上下文恰好还在"：缺了就现测一次（与「液化」按钮同一套
    // 定位：agent 画布快照 + 模板匹配 + .sai2 精确尺寸）。
    if (s.core->HasImage() && !s.core->HasCanvasContext()) {
        CanvasCtxResult ctx;
        std::string diag;
        if (AcquireCanvasContext(ctx, diag) && ctx.ok) {
            s.core->SetCanvasContext(true, ctx.cw, ctx.ch, ctx.x0, ctx.y0);
        } else {
            s.flash = U8("没有画布尺寸，粘贴会错位：" + diag);
        }
    }
    const BridgeCore::ApplyResult r = s.core->ApplyToClipboard();
    if (r.clipboardWritten) {
        s.selfWriteTick = GetTickCount64();
        (void)win;
        if (s.autoPaste) {
            AutoPasteToSai2();
            // 1.19 起所有粘贴路径都是新建图层（Ctrl+V）：勾了「合并回原
            // 图层」就发 F（向下转写）把效果并入原图层。
            if (s.mergeBack) ScheduleMergeAfterPaste(s.mainHwnd);
        }
        s.flash.clear();
        // 多选了多层 → 主层已由上面处理完，其余层逐层补做（后台线程）。
        // 放在这里而不是抓取阶段：只有"应用"才知道用户最终定下的形变。
        if (s.selLayers.size() > 1) {
            const HWND h = win.hwnd;
            if (g_cap.busy.exchange(true)) return;
            std::thread([h]() {
                RunMultiLayerApply(h);
                g_cap.busy = false;
            }).detach();
        }
    } else if (!r.error.empty()) {
        s.flash = U8(r.error);
    }
    InvalidateAll();
}

void DoApply(UiWindow& win) { DoApplyFlow(win); }

void DoUndo(UiWindow& win) {
    (void)win;
    AppState& s = S();
    std::string err;
    s.core->Undo(err);
    s.selfWriteTick = GetTickCount64();
    InvalidateAll();
}

void DoReset(UiWindow& win) {
    (void)win;
    AppState& s = S();
    std::string err;
    s.core->ResetToOriginal(err);
    s.selfWriteTick = GetTickCount64();
    InvalidateAll();
}

// ---------------------------------------------------------------------------
// 图层
// ---------------------------------------------------------------------------
// 用「桥看到的图层窗口物理矩形 / agent 在 SAI2 内报告的同窗口矩形」校准
// agent 坐标 → 物理屏幕坐标的比例（显示器缩放 ≠100% 且 SAI2 非 DPI 感知
// 时，agent 坐标是虚拟化坐标，不换算会把自动点击打偏 —— 实测踩坑）。
void UpdateAgentScale(const sai2bridge::AgentLayerGroup& grp) {
    AppState& s = S();
    s.agentScaleX = 1.0;
    s.agentScaleY = 1.0;
    const RECT ar = grp.screenRect;
    if (grp.hwnd == 0 || ar.right - ar.left < 50 || ar.bottom - ar.top < 50)
        return;
    HWND h = reinterpret_cast<HWND>(static_cast<uintptr_t>(grp.hwnd));
    RECT phys{};
    if (!GetWindowRect(h, &phys)) return;
    const int pw = phys.right - phys.left, ph = phys.bottom - phys.top;
    if (pw < 50 || ph < 50) return;
    s.agentScaleX = std::min(4.0, std::max(0.5, pw / double(ar.right - ar.left)));
    s.agentScaleY = std::min(4.0, std::max(0.5, ph / double(ar.bottom - ar.top)));
}

void RefreshLayers() {
    AppState& s = S();
    // uitest 模式：不查代理，保留注入的模拟图层（供界面探针截图覆盖滚动条路径）
    if (g_uiTestSeconds > 0) return;
    sai2bridge::AgentLayers l;
    if (sai2bridge::QueryLayers(l)) {
        const bool wasOnline = s.agentOnline;
        s.layers = std::move(l);
        s.agentOnline = true;
        s.agentHint.clear();
        if (!wasOnline) s.agentPushed = false;

        const sai2bridge::AgentLayerGroup* grp = PanelGroup();
        if (grp) {
            const LayerAgg agg = AggregateLayers(grp->rows);
            s.layerNames = agg.names;
    // 图层集合变了（合并/新建/删除都会动行号）→ 把越界的选中项剔掉，
    // 否则后面的"逐层处理"会点到不存在的行上
    {
        const int t2 = static_cast<int>(s.layerNames.size());
        std::vector<int> keep;
        for (size_t i = 0; i < s.selLayers.size(); ++i)
            if (s.selLayers[i] >= 0 && s.selLayers[i] < t2)
                keep.push_back(s.selLayers[i]);
        s.selLayers.swap(keep);
        if (s.selLayers.empty() && t2 > 0) {
            s.selLayer = std::min(std::max(0, s.selLayer), t2 - 1);
            s.selLayers.assign(1, s.selLayer);
        }
        if (s.selLayer >= t2) s.selLayer = t2 - 1;
    }
            s.layerAttrs = agg.attrs;
            s.layerScreenY = agg.screenY;
            s.panelLeft = grp->screenRect.left;
            UpdateAgentScale(*grp);
        }    } else {
        if (s.agentOnline) s.core->Log("注入代理失联（SAI2 可能已退出）");
        s.agentOnline = false;
        s.layers.groups.clear();
        s.layers.panelGuess = 0;
    }
}

std::vector<std::wstring> LayerNames() { return S().layerNames; }

// 点击图层行：在 SAI2 里选中该层（不抓取）
// ---------------------------------------------------------------------------
// 图层多选（Ctrl+点切换 / Shift+点范围 / 普通点单选）
//
// 面板**不去猜** SAI2 里选中了哪几层 —— 图层面板的高亮不经过我们挂钩的那几个
// GDI 调用，没有可用的数据源（实测 agent 抓到的位图里只有界面各部件的 DIB）。
// 反过来做：面板是权威，选中集由自己维护，再用 ClickAtScreen/Ctrl+点击把
// 同一组行"点"给 SAI2，两边因此始终一致。
// ---------------------------------------------------------------------------

bool RowIsSelected(int row) {
    const AppState& s = S();
    for (size_t i = 0; i < s.selLayers.size(); ++i)
        if (s.selLayers[i] == row) return true;
    return false;
}

void SetRowSelection(int row, bool additive, bool range) {
    AppState& s = S();
    const int total = static_cast<int>(s.layerNames.size());
    if (row < 0 || row >= total) return;

    if (range && s.selAnchor >= 0 && s.selAnchor < total) {
        const int a = std::min(s.selAnchor, row);
        const int b = std::max(s.selAnchor, row);
        s.selLayers.clear();
        for (int i = a; i <= b; ++i) s.selLayers.push_back(i);
    } else if (additive) {
        std::vector<int>::iterator it =
            std::find(s.selLayers.begin(), s.selLayers.end(), row);
        if (it != s.selLayers.end())
            s.selLayers.erase(it);
        else
            s.selLayers.push_back(row);
        s.selAnchor = row;
    } else {
        s.selLayers.assign(1, row);
        s.selAnchor = row;
    }

    // 主层 = 刚点的这行（若它已被 Ctrl 取消，就退回集合里的第一行）。
    // 单选语义下 selLayers 恰好只含一行，行为与改动前一致。
    if (!s.selLayers.empty()) {
        s.selLayer = RowIsSelected(row) ? row : s.selLayers.front();
        std::sort(s.selLayers.begin(), s.selLayers.end());
    } else {
        s.selLayers.assign(1, row);
        s.selLayer = row;
        s.selAnchor = row;
    }

    if (s.selLayers.size() > 1) {
        s.flash = L"已选中 " + std::to_wstring(s.selLayers.size()) +
                  L" 个图层（一起液化：逐层施加同一形变）";
    } else {
        s.flash = L"已选中图层「" +
                  s.layerNames[static_cast<size_t>(s.selLayer)] + L"」";
    }
}

// 把面板的选中集同步到 SAI2：先普通点一行，再 Ctrl+点其余行。
void SyncSelectionToSai2() {
    AppState& s = S();
    if (!s.agentOnline) {
        s.flash = L"未连接注入代理 —— 点「连接注入」";
        return;
    }
    if (s.layerScreenY.empty() || s.panelLeft <= 0) {
        s.flash = L"没有可用的图层行";
        return;
    }
    HWND sai = FindSai2Window();
    if (!sai) {
        s.flash = L"未找到 SAI2 窗口";
        return;
    }
    ForceForeground(sai);
    Sleep(140);
    bool first = true;
    for (size_t i = 0; i < s.selLayers.size(); ++i) {
        const int row = s.selLayers[i];
        if (row < 0 || row >= static_cast<int>(s.layerScreenY.size())) continue;
        const POINT p = AgentToScreen(
            s.panelLeft + 34, s.layerScreenY[static_cast<size_t>(row)] + 6);
        if (first) {
            ClickAtScreen(p.x, p.y);
            first = false;
        } else {
            CtrlClickAtScreenImpl(p.x, p.y);
        }
        Sleep(130);
    }
}

void DoSelectLayer(int rowIdx) {
    AppState& s = S();
    // 单选入口（按钮/旧路径）：先归一成"只选这一行"，再整组同步
    if (rowIdx >= 0 && rowIdx < static_cast<int>(s.layerNames.size())) {
        s.selLayers.assign(1, rowIdx);
        s.selAnchor = rowIdx;
        s.selLayer = rowIdx;
    }
    SyncSelectionToSai2();
}

// 选中该层并抓取（点行 → 全选 → 拷贝），走后台线程，抓完自动开窗口
void GrabSelectedLayer(HWND hwnd, int rowIdx, int wantWindow) {
    AppState& s = S();
    if (!s.agentOnline) {
        s.flash = L"未连接注入代理 —— 点「连接注入」";
        InvalidateAll();
        return;
    }
    if (g_cap.busy.load()) return;
    g_cap.row = rowIdx;
    // 把面板的整个选中集带进任务（主层在前）：应用阶段据此逐层施加同一形变
    g_cap.selRows.clear();
    const int capTotal = static_cast<int>(s.layerNames.size());
    if (rowIdx >= 0 && rowIdx < capTotal) g_cap.selRows.push_back(rowIdx);
    for (size_t i = 0; i < s.selLayers.size(); ++i) {
        const int v = s.selLayers[i];
        if (v == rowIdx || v < 0 || v >= capTotal) continue;
        g_cap.selRows.push_back(v);
    }
    g_cap.panelLeft = static_cast<int>(s.panelLeft);
    g_cap.rowScreenY =
        (rowIdx >= 0 && rowIdx < static_cast<int>(s.layerScreenY.size()))
            ? s.layerScreenY[static_cast<size_t>(rowIdx)]
            : 0;
    g_cap.wantWindow = wantWindow;
    g_cap.busy = true;
    g_cap.hasJob = true;
    g_cap.phase = CAP_SELECT_LAYER;
    s.flash = L"正在抓取 SAI2 图层…";
    InvalidateAll();
    if (g_capThread.joinable()) g_capThread.join();
    g_capThread = std::thread(RunCaptureJob);
    (void)hwnd;
}

// 在若干候选位置里找 sai2_injector.exe / sai2_agent.dll。
// 场景：解压时多套了一层目录（外层只有 sai2_bridge.exe），或者用户把
// injector/agent 放在了子目录里 —— 这时按"只看自己所在目录"就会误报
// "找不到 sai2_injector.exe"，所以这里把常见位置都找一遍，
// 找到了就把两个路径显式传给注入器（--dll / --out）。
bool FileExistsW(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring DirOf(const std::wstring& path) {
    const size_t p = path.find_last_of(L"\\/");
    return p == std::wstring::npos ? std::wstring() : path.substr(0, p);
}

// 宽字符路径 → UTF-8（写日志用）
std::string Wide2Utf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
                                      static_cast<int>(w.size()), nullptr, 0,
                                      nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                        out.data(), n, nullptr, nullptr);
    return out;
}

struct InjectorPaths {
    std::wstring injector;
    std::wstring agent;
    std::vector<std::wstring> tried;
};

InjectorPaths FindInjectorTools(const std::wstring& exeDir) {
    InjectorPaths r;
    std::vector<std::wstring> dirs;
    auto push = [&dirs](const std::wstring& d) {
        if (!d.empty() &&
            std::find(dirs.begin(), dirs.end(), d) == dirs.end())
            dirs.push_back(d);
    };
    push(exeDir);
    push(DirOf(exeDir)); // 上一层（解压多套一层时的常见情况）

    // 当前目录 + 一/二级子目录（子目录名不限，覆盖 sai2-plugin-suite /
    // 中文包名 / bin 等各种解压结果）
    wchar_t cwd[MAX_PATH] = {0};
    if (GetCurrentDirectoryW(MAX_PATH, cwd) > 0) push(cwd);
    std::vector<std::wstring> bases = dirs;
    for (const std::wstring& base : bases) {
        WIN32_FIND_DATAW fd{};
        const std::wstring pat = base + L"\\*";
        HANDLE h = FindFirstFileW(pat.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (wcscmp(fd.cFileName, L".") == 0 ||
                wcscmp(fd.cFileName, L"..") == 0)
                continue;
            const std::wstring sub = base + L"\\" + fd.cFileName;
            push(sub);
            // 再下一级
            WIN32_FIND_DATAW fd2{};
            HANDLE h2 = FindFirstFileW((sub + L"\\*").c_str(), &fd2);
            if (h2 == INVALID_HANDLE_VALUE) continue;
            do {
                if (!(fd2.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                    continue;
                if (wcscmp(fd2.cFileName, L".") == 0 ||
                    wcscmp(fd2.cFileName, L"..") == 0)
                    continue;
                push(sub + L"\\" + fd2.cFileName);
            } while (FindNextFileW(h2, &fd2));
            FindClose(h2);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    for (const std::wstring& d : dirs) {
        const std::wstring inj = d + L"\\sai2_injector.exe";
        r.tried.push_back(inj);
        if (!FileExistsW(inj)) continue;
        r.injector = inj;
        const std::wstring ag = d + L"\\sai2_agent.dll";
        if (FileExistsW(ag)) r.agent = ag;
        break;
    }
    // agent 没跟 injector 同目录时，再单独找一遍
    if (r.injector.empty() || r.agent.empty()) {
        for (const std::wstring& d : dirs) {
            const std::wstring ag = d + L"\\sai2_agent.dll";
            if (FileExistsW(ag)) {
                r.agent = ag;
                break;
            }
        }
    }
    return r;
}

void DoInjectAsync(HWND hwnd) {
    AppState& s = S();
    wchar_t buf[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    const std::wstring exeDir = DirOf(std::wstring(buf));
    const InjectorPaths paths = FindInjectorTools(exeDir);

    if (paths.injector.empty()) {
        std::string msg = "找不到 sai2_injector.exe，已找过这些位置：";
        for (const std::wstring& t : paths.tried) {
            const int n = WideCharToMultiByte(CP_UTF8, 0, t.c_str(), -1, nullptr,
                                              0, nullptr, nullptr);
            if (n > 1) {
                std::string u8(static_cast<size_t>(n - 1), '\0');
                WideCharToMultiByte(CP_UTF8, 0, t.c_str(), -1, u8.data(), n,
                                    nullptr, nullptr);
                msg += "\n  " + u8;
            }
        }
        S().core->Log("injector: " + msg);
        s.flash = L"找不到 sai2_injector.exe —— 它要和 sai2_bridge.exe 放在一起"
                  L"（详见日志）";
        InvalidateAll();
        return;
    }
    S().core->Log("injector: 使用 " + Wide2Utf8(paths.injector));

    const std::wstring injector = paths.injector;
    const std::wstring agent = paths.agent;

    s.flash = L"正在注入 SAI2 代理…";
    InvalidateAll();

    std::thread([hwnd, injector, agent, exeDir]() {
        SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
        HANDLE readEnd = nullptr, writeEnd = nullptr;
        std::string out;
        if (CreatePipe(&readEnd, &writeEnd, &sa, 0)) {
            SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
            STARTUPINFOW si = {};
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;
            si.hStdOutput = writeEnd;
            si.hStdError = writeEnd;
            PROCESS_INFORMATION pi = {};
            // 显式把 agent 路径喂给注入器；同时用注入器所在目录当工作目录
            // （它默认会找同目录的 sai2_agent.dll）
            std::wstring cmdline = L"\"" + injector + L"\" inject";
            if (!agent.empty())
                cmdline += L" --dll \"" + agent + L"\"";
            std::vector<wchar_t> cmdbuf(cmdline.begin(), cmdline.end());
            cmdbuf.push_back(L'\0');
            const std::wstring workDir = DirOf(injector);
            if (CreateProcessW(injector.c_str(), cmdbuf.data(), nullptr, nullptr,
                               TRUE, CREATE_NO_WINDOW, nullptr,
                               workDir.empty() ? nullptr : workDir.c_str(), &si,
                               &pi)) {
                CloseHandle(writeEnd);
                WaitForSingleObject(pi.hProcess, 25000);
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
                char tmp[2048] = {0};
                DWORD n = 0;
                ReadFile(readEnd, tmp, sizeof(tmp) - 1, &n, nullptr);
                if (n > 0) {
                    tmp[std::min<size_t>(n, sizeof(tmp) - 1)] = '\0';
                    out = tmp;
                }
            } else {
                char e[128];
                std::snprintf(e, sizeof(e),
                              "启动注入器失败 err=%lu（路径见日志）",
                              static_cast<unsigned long>(GetLastError()));
                out = e;
            }
            CloseHandle(readEnd);
        }
        S().core->Log("injector: " + out);
        PostMessageW(hwnd, WM_APP + 2, 0, 0);
    }).detach();
}

void ScheduleMergeAfterPaste(HWND hwnd) {
    AppState& s = S();
    if (!s.mergeBack) return;
    // 注意：这里曾经要求 s.mergePending（从未有代码把它置 true），
    // 结果勾选了「合并回原图层」也永远不会合并——1.15 修复。
    // 粘贴（含 SAI2 新建图层）需要一点时间，大画布更久，给足 1.4 秒。
    SetTimer(hwnd, 5, 1400, nullptr);
}

// 测试钩子：--ml-sel 预设的图层多选（无人值守验证「一起液化」用）
std::vector<int> g_mlSelRows;

// ---------------------------------------------------------------------------
// 多图层一起液化（方案 B）：主层走既有应用流程，其余层在这里逐层补做。
//
// 每一层：选中该行 → Ctrl+A/Ctrl+C → 求该层的画布偏移 → 用**同一个置换场**
// 按偏移渲染（RenderLiquifyLayerToCanvas）→ 补边 → 写剪贴板 → Ctrl+V →
// 向下合并（F）回该层。全程串行：SAI2 的图层操作不能并发，而且 agent 的
// 命名管道是单连接同步服务（g_cap.busy 期间 UI 定时器不轮询）。
//
// 为什么"同一形变、分层不丢"：置换场是在主层局部坐标里建的，其余层只需把场
// 按 (本层偏移 − 主层偏移) 采样，几何就完全一致；每层各自补边、各自粘贴并
// 合并回自己，所以图层结构保持不变。
// ---------------------------------------------------------------------------
void RunMultiLayerApply(HWND hwnd) {
    AppState& s = S();
    // 用**应用时刻**的选中集，而不是抓取时留下的副本：demo/浮窗等路径不一定
    // 经过 GrabSelectedLayer，用副本会漏掉层（实测只处理了 1 层）。
    // 基准层 = 主层 selLayer（它的结果已由既有应用流程贴回），其余层在这里补做。
    std::vector<int> rows;
    rows.push_back(s.selLayer);
    for (size_t k = 0; k < s.selLayers.size(); ++k)
        if (s.selLayers[k] != s.selLayer) rows.push_back(s.selLayers[k]);
    if (!s.core || rows.size() <= 1) return;
    if (!s.core->HasCanvasContext()) {
        s.core->Log("multilayer: skip (no canvas ctx)");
        return;
    }
    const int cw = s.core->CanvasW();
    const int ch = s.core->CanvasH();
    const int baseX = s.core->CanvasX0();
    const int baseY = s.core->CanvasY0();
    const size_t total = rows.size();
    s.core->Log("multilayer: start, layers=" + std::to_string(total) +
                " canvas=" + std::to_string(cw) + "x" + std::to_string(ch));

    int done = 0;
    for (size_t i = 1; i < total; ++i) {
        const int row = rows[i];
        if (row < 0 || row >= static_cast<int>(s.layerNames.size())) continue;
        s.flash = L"一起液化：正在处理第 " + std::to_wstring(i + 1) + L"/" +
                  std::to_wstring(total) + L" 层…";
        InvalidateAll();

        // 1) 选中该行（每轮重新取一次坐标：上一轮的合并可能动了行/滚动）
        {
            HWND sai = FindSai2Window();
            if (!sai) break;
            // 必须**确认 SAI2 真的在前台**：否则后面的 Ctrl+A/C 会发给别的窗口，
            // 剪贴板仍是上一层的图 —— 那样"第二层"渲染出来的其实是同一张图，
            // 现象是 shift 恒为 (0,0)（实测踩坑）。ForceForeground 用
            // AttachThreadInput 也未必成功，这里再用 TOPMOST 三步 + 校验。
            SetWindowPos(sai, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE);
            SetForegroundWindow(sai);
            SetWindowPos(sai, HWND_NOTOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE);
            Sleep(200);
            if (GetForegroundWindow() != sai) {
                s.core->Log("multilayer: cannot bring SAI2 to foreground, skip");
                continue;
            }
            if (s.panelLeft > 0 &&
                row < static_cast<int>(s.layerScreenY.size())) {
                const POINT p = AgentToScreen(
                    s.panelLeft + 34,
                    s.layerScreenY[static_cast<size_t>(row)] + 6);
                ClickAtScreen(p.x, p.y);
                Sleep(260);
            }
        }
        // 2) 全选 + 拷贝（与整层抓取同一套输入），并**确认剪贴板真的换了** ——
        //    这是"键有没有送到 SAI2"最直接的证据，避免拿上一层的内容重复处理。
        const DWORD clipSeq0 = GetClipboardSequenceNumber();
        SendCtrlScan(0x1E);   // A
        Sleep(140);
        SendCtrlScan(0x2E);   // C
        for (int kw = 0; kw < 20; ++kw) {
            Sleep(80);
            if (GetClipboardSequenceNumber() != clipSeq0) break;
        }
        if (GetClipboardSequenceNumber() == clipSeq0) {
            s.core->Log("multilayer: clipboard unchanged (keys not delivered?) "
                        "-> skip layer " + std::to_string(row));
            continue;
        }

        // 3) 求该层的画布偏移（与整层抓取完全相同的定位链路）
        CanvasCtxResult ctx;
        std::string diag;
        if (!AcquireCanvasContext(ctx, diag)) {
            s.core->Log("multilayer: layer " + std::to_string(row) +
                        " ctx failed: " + diag);
            continue;
        }
        // 4) 取该层图像
        ImageBuffer layer;
        std::string err;
        if (!ReadClipboardImage(layer, err) || !layer.IsValid()) {
            s.core->Log("multilayer: layer " + std::to_string(row) +
                        " clipboard read failed: " + err);
            continue;
        }
        // 5) 同一个置换场 + 按偏移采样 + 按本层偏移补边
        const ImageBuffer out = s.core->RenderLiquifyLayerToCanvas(
            layer, "ml" + std::to_string(row), ctx.cw, ctx.ch, ctx.x0, ctx.y0,
            ctx.x0 - baseX, ctx.y0 - baseY);
        if (!out.IsValid()) {
            s.core->Log("multilayer: layer " + std::to_string(row) +
                        " render failed");
            continue;
        }
        // 6) 写剪贴板 → 粘贴 → 合并回该层
        if (!s.core->WriteImageToClipboard(out, err)) {
            s.core->Log("multilayer: layer " + std::to_string(row) +
                        " clipboard write failed: " + err);
            continue;
        }
        s.selfWriteTick = GetTickCount64();
        AutoPasteToSai2();
        Sleep(1500);                     // 等 SAI2 把粘贴层建出来
        if (s.mergeBack) {
            DoMergeDown(hwnd);           // F：粘贴层转写进它下面的那一层
            Sleep(700);
        }
        ++done;
        s.core->Log("multilayer: layer " + std::to_string(row) + " done place=(" +
                    std::to_string(ctx.x0) + "," + std::to_string(ctx.y0) +
                    ") shift=(" + std::to_string(ctx.x0 - baseX) + "," +
                    std::to_string(ctx.y0 - baseY) + ")");

        RefreshLayers();                 // 合并可能改了行号/滚动位置 → 重定位
        Sleep(400);
    }
    s.flash = L"一起液化完成：共处理 " + std::to_wstring(done + 1) + L" 层";
    s.core->Log("multilayer: finished, extra layers done=" +
                std::to_string(done));
    InvalidateAll();
}

// ---------------------------------------------------------------------------
// 代理请求（SAI2 内浮窗点击 → 打开对应功能窗口）
// ---------------------------------------------------------------------------
void PollAgentRequest(HWND hwnd) {
    int idx = -1;
    if (!sai2bridge::TakeCategoryRequest(idx)) return;
    if (idx < 1 || idx >= BridgeCore::kWinCount) return;
    RequestToolWindow(hwnd, idx, true);
}

void PushCategoriesToAgent() {
    AppState& s = S();
    std::vector<sai2bridge::AgentCategoryItem> items;
    for (int i = 1; i < BridgeCore::kWinCount; ++i) {
        sai2bridge::AgentCategoryItem it;
        it.id = i;
        it.name = BridgeCore::kGroupNames[i - 1];
        items.push_back(it);
    }
    s.agentPushed = sai2bridge::PushCategories(items);
    s.core->Log(s.agentPushed ? "功能清单已推送到 SAI2 插件入口"
                              : "功能清单推送失败");
}

// ---------------------------------------------------------------------------
// 主窗口过程
// ---------------------------------------------------------------------------
void DoMergeDown(HWND hwnd) {
    AppState& s = S();
    HWND sai = FindSai2Window();
    if (!sai) {
        s.flash = L"未找到 SAI2 窗口，无法合并回图层";
        return;
    }
    // 用 SAI2 原生快捷键 F（向下转写：当前层内容并入下层、本层清空）。
    // 此前走「图层菜单 → 向下转写」的菜单点击链路：依赖 agent 管道的
    // 文字钩子 + 菜单栏坐标换算 + 屏幕找字，任何一环失败就悄悄不合并
    //（用户反馈：勾了合并回原图层，结果还在新建图层上）。
    // 粘贴层刚被 SAI2 选中且直接位于原图层之上，F 正好把它转写下去。
    WaitMouseReleased(1000);
    ForceForeground(sai);
    Sleep(160);
    if (GetForegroundWindow() != sai) {
        s.flash = L"无法把 SAI2 切到前台 —— 请在 SAI2 里手动按 F（向下转写）";
        return;
    }
    SendKeyScan(0x21, false); // F = 向下转写
    Sleep(40);
    SendKeyScan(0x21, true);
    ReleaseSyntheticKeys();
    s.flash = L"已发送「向下转写」（F）：效果已并入原图层";
    s.core->Log("merge-down: sent F (transfer down)");
    InvalidateRect(hwnd, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// 主窗口过程（窗口类名与过程由 UiWindow.cpp 的 RegisterWindowClasses 统一注册，
// 这里通过 MainWindowClassName() / MainWndProc 暴露给公共层）
// ---------------------------------------------------------------------------
LRESULT CALLBACK MainWindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    AppState& s = S();
    UiWindow& win = s.main;

    switch (msg) {
    case WM_GETMINMAXINFO: {
        // 允许自由缩放，但限制最小尺寸，避免布局挤爆
        MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        mmi->ptMinTrackSize.x = Scale(580);
        mmi->ptMinTrackSize.y = Scale(720);
        return 0;
    }

    case WM_CREATE:
        InitFonts();
        CreateParamEdits(win);
        win.hwnd = hwnd;
        win.id = BridgeCore::kWinMain;
        AddClipboardFormatListener(hwnd);
        // 注册结果必须记录：热键被别的程序占用时 RegisterHotKey 会失败，
        // 之后按下它**什么都不会发生且毫无提示**（实测踩过：Ctrl+Alt+G 能用、
        // Ctrl+Alt+A 静默无效，排查时完全没有线索）。
        {
            struct HotKeyDef { int id; UINT vk; const char* name; };
            const HotKeyDef hks[] = {
                {1, 'G', "Ctrl+Alt+G 抓取图层"},
                {2, 'A', "Ctrl+Alt+A 应用"},
                {3, 'Z', "Ctrl+Alt+Z 撤销"},
                {4, 'R', "Ctrl+Alt+R 复位"},
            };
            for (const HotKeyDef& k : hks) {
                if (RegisterHotKey(hwnd, k.id,
                                   MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, k.vk))
                    s.core->Log(std::string("hotkey OK: ") + k.name);
                else
                    s.core->Log(
                        std::string("hotkey FAILED (被其它程序占用?): ") +
                        k.name + " err=" + std::to_string(GetLastError()));
            }
        }
        SetTimer(hwnd, 1, 2500, nullptr); // 图层周期刷新
        SetTimer(hwnd, 2, 400, nullptr);  // SAI2 浮窗请求轮询
        return 0;

    case WM_DESTROY:
        RemoveClipboardFormatListener(hwnd);
        UnregisterHotKey(hwnd, 1);
        UnregisterHotKey(hwnd, 2);
        UnregisterHotKey(hwnd, 3);
        UnregisterHotKey(hwnd, 4);
        KillTimer(hwnd, 1);
        KillTimer(hwnd, 2);
        KillTimer(hwnd, 5);
        PostQuitMessage(0);
        return 0;

    case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(wp);
        SetTextColor(dc, C_TEXT);
        SetBkColor(dc, ThemeIsDark() ? RGB(26, 27, 29) : RGB(255, 255, 255));
        static HBRUSH editBgDark = CreateSolidBrush(RGB(26, 27, 29));
        static HBRUSH editBgLight = CreateSolidBrush(RGB(255, 255, 255));
        return reinterpret_cast<LRESULT>(ThemeIsDark() ? editBgDark
                                                       : editBgLight);
    }

    case WM_APP + 2:
        RefreshLayers();
        InvalidateAll();
        return 0;

    case WM_CAPTURE_DONE:
        OnCaptureFinished();
        return 0;

    case WM_APP + 11:
        QuitApp();
        return 0;

    case WM_COMMAND: {
        const int id = LOWORD(wp);
        if (HIWORD(wp) == EN_KILLFOCUS && id >= 100 && id < 104) {
            CommitParamEdit(win, id - 100);
            win.bgraDirty = true;
        }
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);
        win.hits.clear();
        try {
            DrawMain(mem, win, rc);
        } catch (const std::exception& e) {
            // 绘制异常不该让整个面板变白：明确报出来（同时写诊断日志）
            Diag(std::string("DrawMain 抛异常: ") + e.what());
            FillRectC(mem, rc, C_BG);
            TextC(mem, U8(std::string("界面绘制异常: ") + e.what()),
                  RECT{20, 60, rc.right - 20, 120}, C_WARN, S().fBody,
                  DT_LEFT | DT_TOP | DT_WORDBREAK);
        } catch (...) {
            Diag("DrawMain 抛未知异常");
            FillRectC(mem, rc, C_BG);
            TextC(mem, L"界面绘制异常（设 SAI2_BRIDGE_DIAG=1 后重开可看日志）",
                  RECT{20, 60, rc.right - 20, 120}, C_WARN, S().fBody,
                  DT_LEFT | DT_TOP | DT_WORDBREAK);
        }
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        const int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);
        for (const UiHit& h : win.hits) {
            if (!InRect(h.rc, mx, my)) continue;
            if (h.kind == HK_ZSLIDER) {
                SetCapture(hwnd);
                win.dragZoom = true;
                win.zoomTrack = h.track;
                OnZoomSliderDrag(win, mx);
                InvalidateAll();
                return 0;
            }
            if (h.kind == HK_LISTSCROLL) {
                SetCapture(hwnd);
                win.dragList = true;
                win.scrollTrack = h.track;
                ListScrollAt(win, my);
                InvalidateAll();
                return 0;
            }
            if (HandleZoomHit(win, h)) {
                InvalidateAll();
                return 0;
            }
            if (h.kind == HK_TOOL && h.index < 0) {
                const int idx = -h.index;
                if (idx >= 1 && idx < BridgeCore::kWinCount)
                    RequestToolWindow(hwnd, idx, true);
                return 0;
            }
            if (OnMainHit(win, h)) {
                InvalidateAll();
                return 0;
            }
            if (h.kind == HK_CMD) {
                RunCmd(win, h.index);
                return 0;
            }
            if (h.kind == HK_PREVIEW) {
                // 主面板预览不涂抹：点一下直接打开当前效果所属窗口
                return 0;
            }
            return 0;
        }
        return 0;
    }

    case WM_TIMER:
        if (wp == 1) {
            if (!g_uiTestSeconds) RefreshLayers();
            InvalidateAll();
        } else if (wp == 2) {
            if (!s.agentOnline) {
                s.agentPushed = false;
            } else if (g_cap.busy.load()) {
                // 抓取线程正在用管道（canvas.arm/state/save）。agent 管道是
                // 单实例同步服务，这里再插进去就会把"请求↔响应"配对打乱，
                // 抓取线程的 canvas.save 拿不到自己的回复 → save failed。
                // 抓取期间让路；抓完自然恢复（busy 清掉后下一拍继续）。
            } else {
                if (!s.agentPushed && !g_uiTestSeconds) PushCategoriesToAgent();
                PollAgentRequest(hwnd);
            }
        } else if (wp == 5) {
            KillTimer(hwnd, 5);
            DoMergeDown(hwnd);
        }
        return 0;

    case WM_HOTKEY:
        if (wp == 1) {
            DoGrabWholeLayer(win.hwnd);
        } else if (wp == 2) {
            if (s.core->HasImage()) DoApplyFlow(win);
        } else if (wp == 3) {
            DoUndo(win);
        } else if (wp == 4) {
            DoReset(win);
        }
        return 0;

    case WM_CLIPBOARDUPDATE:
        if (!s.watchClipboard) return 0;
        if (GetTickCount64() - s.selfWriteTick < 1500) return 0;
        if (CaptureInProgress()) return 0;
        if (!ClipboardHasImage()) return 0;
        DoGrabFromClipboard(win);
        return 0;

    default:
        break;
    }
    if (MainMsg(win, msg, wp, lp)) return 0;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// 入口辅助
// ---------------------------------------------------------------------------
// GUI 子系统下把 CLI 输出接回去：
//   1) 从真控制台启动（交互式）→ AttachConsole 父控制台；
//   2) 被重定向/管道（> file、Git Bash 捕获）→ AttachConsole 会失败，
//      但进程仍继承了 stdout/stderr 句柄，用 _open_osfhandle 接回 CRT。
// 没有这两条，--selftest 在 GUI 子系统下输出会静默丢失。
void ReopenStdHandle(DWORD stdId, const char* mode, FILE* stream) {
    HANDLE h = GetStdHandle(stdId);
    if (!h || h == INVALID_HANDLE_VALUE) return;
    int fd = _open_osfhandle(reinterpret_cast<intptr_t>(h),
                             _O_WRONLY | _O_TEXT);
    if (fd < 0) return;
    FILE* f = _fdopen(fd, mode);
    if (!f) {
        _close(fd);
        return;
    }
    *stream = *f;
}

void AttachParentConsole() {
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
        return;
    }
    ReopenStdHandle(STD_OUTPUT_HANDLE, "w", stdout);
    ReopenStdHandle(STD_ERROR_HANDLE, "w", stderr);
}

std::string ExeDir() {
    wchar_t buf[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring s(buf);
    const size_t p = s.find_last_of(L"\\/");
    if (p != std::wstring::npos) s = s.substr(0, p);
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0,
                                      nullptr, nullptr);
    std::string out(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
    if (n > 1)
        WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, out.data(), n, nullptr,
                            nullptr);
    return out;
}

// ---------------------------------------------------------------------------
// --ctx-test：无人值守验证"精确回贴"的定位链路（画布上下文）。
// 只做 抓图层 + 求画布尺寸/内容偏移，**绝不粘贴、绝不改动 SAI2 文档**。
// 之所以要有这个模式：错位问题全部发生在这一段，而它的失败模式在界面上
// 只表现为"粘回去偏了"，没有可观测的中间值。这里把中间值直接打出来。
// ---------------------------------------------------------------------------
int RunCanvasCtxTest() {
    AttachParentConsole();
    g_ctxDebugCurve = true;
    std::printf("build: %s\n", BuildStamp().c_str());

    std::string resp;
    if (!sai2agent::AgentClient::Query("{\"cmd\":\"ping\"}", resp)) {
        std::printf("FAIL 注入代理不在线（先跑 sai2_injector.exe inject）\n");
        return 2;
    }
    std::printf("agent: %s\n", resp.c_str());

    if (!sai2agent::AgentClient::Query("{\"cmd\":\"canvas.arm\",\"ms\":9000}",
                                       resp) ||
        resp.find("\"ok\":true") == std::string::npos) {
        std::printf("FAIL canvas.arm 失败: %s\n", resp.c_str());
        return 2;
    }

    HWND sai = FindSai2Window();
    if (!sai) {
        std::printf("FAIL 未找到 SAI2 主窗口\n");
        return 2;
    }
    ForceForeground(sai);          // 内含最小化还原
    WaitMouseReleased(3000);
    Sleep(200);

    const DWORD seq = GetClipboardSequenceNumber();
    SendCtrlScan(0x1E);            // Ctrl+A 全选
    Sleep(400);
    SendCtrlScan(0x2E);            // Ctrl+C 拷贝
    for (int i = 0; i < 25; ++i) {
        Sleep(100);
        if (GetClipboardSequenceNumber() != seq && ClipboardHasImage()) break;
    }
    ReleaseSyntheticKeys();

    ImageBuffer probe;
    std::string perr, fmt;
    if (!ReadClipboardImage(probe, perr, &fmt)) {
        std::printf("FAIL 剪贴板没有图像: %s\n", perr.c_str());
        return 1;
    }
    std::printf("剪贴板: %s %dx%d（SAI2 的 Ctrl+C 只给图层不透明包围盒）\n",
                fmt.c_str(), probe.Width(), probe.Height());

    CanvasCtxResult ctx;
    std::string diag;
    if (AcquireCanvasContext(ctx, diag)) {
        std::printf("OK   画布=%dx%d  内容偏移=(%d,%d)  %s\n", ctx.cw, ctx.ch,
                    ctx.x0, ctx.y0, diag.c_str());
        std::printf("     => 回贴时会补边成 %dx%d 并在 (%d,%d) 落位\n",
                    ctx.cw, ctx.ch, ctx.x0, ctx.y0);
        return 0;
    }
    std::printf("FAIL 画布上下文定位失败: %s\n", diag.c_str());
    return 1;
}

// --multilayer-test <A.bmp> <B.bmp> <cw> <ch> <ax> <ay> <bx> <by> <outPrefix>
//
// 离线验证「多图层一起液化」的**桥侧全链路**（置换场 → 按层偏移渲染 → 按层补边），
// 完全不依赖 SAI2：A 是基准层、B 是另一层，两者是**同一画布的不同子矩形**。
// 在 A 上刷一笔建立置换场，再用**同一个场**把 B 渲染出来。
// 期望：两张补边结果在画布空间的重叠区里逐像素一致（同一画布点 = 同一内容）。
// 这条正是"每层被同一套形变处理，而分层不丢"的可判定形式。
int RunMultiLayerTest(const std::string& pa, const std::string& pb, int cw,
                      int ch, int ax, int ay, int bx, int by,
                      const std::string& outPrefix,
                      const std::string& pluginsDir,
                      const std::string& logDir) {
    AttachParentConsole();
    std::printf("build: %s\n", BuildStamp().c_str());
    ImageBuffer A, B;
    std::string err;
    if (!ReadBmp32(pa, A, err) || !A.IsValid()) {
        std::printf("FAIL 读基准层: %s (%s)\n", err.c_str(), pa.c_str());
        return 2;
    }
    if (!ReadBmp32(pb, B, err) || !B.IsValid()) {
        std::printf("FAIL 读第二层: %s (%s)\n", err.c_str(), pb.c_str());
        return 2;
    }
    std::printf("基准层 %dx%d@(%d,%d)  第二层 %dx%d@(%d,%d)  画布 %dx%d\n",
                A.Width(), A.Height(), ax, ay, B.Width(), B.Height(), bx, by,
                cw, ch);

    BridgeCore core(pluginsDir, logDir);
    if (!core.Startup(err)) {
        std::printf("FAIL 启动核心: %s\n", err.c_str());
        return 2;
    }
    int idx = -1;
    const std::vector<EffectDef>& E = core.Effects();
    for (size_t i = 0; i < E.size(); ++i)
        if (E[i].id == "liquify.twirl-cw") {
            idx = static_cast<int>(i);
            break;
        }
    if (idx < 0) {
        std::printf("FAIL 没有找到 liquify.twirl-cw 效果\n");
        return 2;
    }
    core.SetCurrentEffect(idx, BridgeCore::kWinLiquify);
    core.SetImage(A, "layerA");
    core.SetCanvasContext(true, cw, ch, ax, ay);

    // 在基准层上刷一笔 → 建立置换场。
    // 用 twirl（旋转场）：它是**非均匀**的，如果偏移算错，重叠区立刻对不上；
    // 用均匀场则可能"错了也看不出来"。
    core.StrokeBegin(0.35f, 0.35f, BridgeCore::kWinLiquify);
    core.StrokeMove(0.60f, 0.60f, BridgeCore::kWinLiquify);
    core.StrokeEnd(BridgeCore::kWinLiquify);

    // 走产品应用路径（全分辨率渲染现有场 + 补边 + 写剪贴板）
    const BridgeCore::ApplyResult r = core.ApplyToClipboard();
    if (!r.clipboardWritten) {
        std::printf("FAIL 基准层应用失败: %s\n", r.error.c_str());
        return 1;
    }
    const ImageBuffer apad = core.PaddedApplied();
    if (!apad.IsValid() || !WriteBmp32(apad, outPrefix + "_A.bmp", err)) {
        std::printf("FAIL 写基准层补边图: %s\n", err.c_str());
        return 1;
    }

    // 第二层：同一个置换场、换源图、按偏移采样、按自己的画布偏移补边
    const ImageBuffer bpad = core.RenderLiquifyLayerToCanvas(
        B, "layerB", cw, ch, bx, by, bx - ax, by - ay);
    if (!bpad.IsValid() || !WriteBmp32(bpad, outPrefix + "_B.bmp", err)) {
        std::printf("FAIL 第二层渲染/写出失败: %s\n", err.c_str());
        return 1;
    }
    std::printf("OK 基准层补边 %dx%d → %s_A.bmp ；第二层 shift=(%d,%d) 补边 "
                "%dx%d → %s_B.bmp\n",
                apad.Width(), apad.Height(), outPrefix.c_str(), bx - ax, by - ay,
                bpad.Width(), bpad.Height(), outPrefix.c_str());
    core.Shutdown();
    return 0;
}

// --doc-test：只走"SAI2 窗口标题 → .sai2 路径 → 精确画布尺寸"这条链路并逐步
// 打印结果。加它的原因：精确画布尺寸没生效时，光看 --ctx-test 的 diag 分不清
// 是**标题没解析出来**、**文件读不到**，还是**被自洽性校验否决了**。
int RunDocTest() {
    AttachParentConsole();
    // 不要用 std::wprintf：控制台代码页不匹配时中文甚至数字会被整段丢掉
    // （实测只剩 "[" "]"），排查反而被它误导。统一转 UTF-8 窄串走 printf。
    auto W2U8 = [](const std::wstring& ws) {
        if (ws.empty()) return std::string();
        const int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(),
                                          static_cast<int>(ws.size()), nullptr,
                                          0, nullptr, nullptr);
        std::string out(static_cast<size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, ws.c_str(),
                            static_cast<int>(ws.size()), out.data(), n, nullptr,
                            nullptr);
        return out;
    };
    std::printf("build: %s\n", BuildStamp().c_str());
    HWND sai = FindSai2Window();
    std::printf("SAI2 主窗口: %s\n", sai ? "找到" : "未找到");
    if (!sai) return 1;
    wchar_t title[512] = {0};
    GetWindowTextW(sai, title, 512);
    std::printf("窗口标题: %s\n", W2U8(title).c_str());

    std::wstring path;
    const bool parsed = Sai2DocPathFromTitle(title, path);
    std::printf("[1] 标题解析: %s\n", parsed ? "OK" : "失败");
    if (parsed) std::printf("    -> %s\n", W2U8(path).c_str());
    if (!parsed) return 1;

    const DWORD attr = GetFileAttributesW(path.c_str());
    std::printf("[2] 文件存在: %s (attr=0x%08lX)\n",
                (attr != INVALID_FILE_ATTRIBUTES) ? "是" : "否",
                static_cast<unsigned long>(attr));

    // 手工走一遍打开/读取，把每一步的原始结果都打出来（生产函数只返回 bool，
    // 分不清是打不开、读不够、还是 magic/尺寸校验否决）。
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    std::printf("[3] CreateFileW: %s  GetLastError=%lu\n",
                (f != INVALID_HANDLE_VALUE) ? "OK" : "失败", GetLastError());
    if (f == INVALID_HANDLE_VALUE) return 1;
    unsigned char buf[32] = {0};
    DWORD got = 0;
    const BOOL rok = ReadFile(f, buf, sizeof(buf), &got, nullptr);
    const DWORD rerr = GetLastError();
    CloseHandle(f);
    std::printf("[4] ReadFile: %s  got=%lu  err=%lu\n", rok ? "OK" : "失败",
                static_cast<unsigned long>(got), static_cast<unsigned long>(rerr));
    printf("[5] 前 16 字节: ");
    for (int i = 0; i < 16; ++i) std::printf("%02x ", buf[i]);
    std::printf("\n    作为文本: ");
    for (int i = 0; i < 16; ++i)
        std::printf("%c", (buf[i] >= 32 && buf[i] < 127) ? buf[i] : '.');
    std::printf("\n[6] offset20/24 作为 uint32: %u x %u\n", ReadU32LE(buf + 20),
                ReadU32LE(buf + 24));

    int w = 0, h = 0;
    const bool sized = ReadSai2CanvasSize(path, w, h);
    std::printf("[7] ReadSai2CanvasSize: %s  画布=%dx%d\n",
                sized ? "OK" : "失败", w, h);
    return sized ? 0 : 1;
}

// 不碰 agent、不碰剪贴板、不改 SAI2 —— 用已经落盘的两张图确定性复现匹配，
// 便于对照 Python 原型定位尺度偏差。
// --match-test <snapshot.bmp> <template.bmp>：离线复跑画布上下文定位。
int RunMatchTest(const std::string& snapPath, const std::string& tmplPath) {
    AttachParentConsole();
    g_ctxDebugCurve = true;
    std::printf("build: %s\n", BuildStamp().c_str());
    ImageBuffer view, tmpl;
    std::string err;
    if (!ReadBmp32(snapPath, view, err)) {
        std::printf("FAIL 读快照: %s (%s)\n", err.c_str(), snapPath.c_str());
        return 2;
    }
    if (!ReadBmp32(tmplPath, tmpl, err)) {
        std::printf("FAIL 读模板: %s (%s)\n", err.c_str(), tmplPath.c_str());
        return 2;
    }
    std::printf("快照 %dx%d   模板 %dx%d\n", view.Width(), view.Height(),
                tmpl.Width(), tmpl.Height());
    CanvasCtxResult ctx;
    std::string diag;
    if (LocateCanvasCtx(view, tmpl, ctx, diag)) {
        std::printf("OK   画布=%dx%d  内容偏移=(%d,%d)  %s\n", ctx.cw, ctx.ch,
                    ctx.x0, ctx.y0, diag.c_str());
        return 0;
    }
    std::printf("FAIL %s\n", diag.c_str());
    return 1;
}

// --pad-clipboard <snapshot.bmp> <template.bmp>
// 复跑产品"精确回贴"的**写入路径**：定位画布上下文 → 把模板补边成画布尺寸
// → 写剪贴板（走 BridgeCore::WriteAppliedToClipboard，与产品同一段代码）。
// 只动剪贴板、不碰 SAI2 文档；之后 Ctrl+V 即可验证落位准不准。
int RunPadClipboard(const std::string& snapPath, const std::string& tmplPath,
                    const std::string& pluginsDir, const std::string& logDir) {
    AttachParentConsole();
    g_ctxDebugCurve = true;
    std::printf("build: %s\n", BuildStamp().c_str());
    ImageBuffer view, tmpl;
    std::string err;
    if (!ReadBmp32(snapPath, view, err)) {
        std::printf("FAIL 读快照: %s (%s)\n", err.c_str(), snapPath.c_str());
        return 2;
    }
    if (!ReadBmp32(tmplPath, tmpl, err)) {
        std::printf("FAIL 读模板: %s (%s)\n", err.c_str(), tmplPath.c_str());
        return 2;
    }
    CanvasCtxResult ctx;
    std::string diag;
    // 与产品一致：能用 .sai2 的精确画布尺寸就用（SAI2 没开则自动退回推算）
    int exactW = 0, exactH = 0;
    if (Sai2DocCanvasSize(exactW, exactH))
        std::printf("SAI2 当前文档画布尺寸（读 .sai2 头部）= %dx%d\n", exactW,
                    exactH);
    else
        std::printf("未取到 .sai2 精确画布尺寸 → 走快照推算\n");
    if (!LocateCanvasCtx(view, tmpl, ctx, diag, exactW, exactH)) {
        std::printf("FAIL 定位失败: %s\n", diag.c_str());
        return 1;
    }
    std::printf("定位: 画布=%dx%d 内容偏移=(%d,%d) %s\n", ctx.cw, ctx.ch,
                ctx.x0, ctx.y0, diag.c_str());

    BridgeCore core(pluginsDir, logDir);
    if (!core.Startup(err)) {
        std::printf("FAIL 启动核心: %s\n", err.c_str());
        return 2;
    }
    core.SetImage(std::move(tmpl), "pad-clipboard");
    core.SetCanvasContext(true, ctx.cw, ctx.ch, ctx.x0, ctx.y0);
    const bool ok = core.WriteAppliedToClipboard(err);
    core.Shutdown();
    if (!ok) {
        std::printf("FAIL 写剪贴板: %s\n", err.c_str());
        return 1;
    }
    std::printf("OK 剪贴板已写入补边结果：%dx%d，内容落在 (%d,%d)\n", ctx.cw,
                ctx.ch, ctx.x0, ctx.y0);
    return 0;
}

// 主面板窗口类名与窗口过程：给 UiWindow.cpp 的 RegisterWindowClasses 用。
// 这里必须是**唯一**注册主面板类的地方 —— 重复注册同名类会让窗口挂上
// 错误的窗口过程（现象：主面板整块空白，这个 bug 已经踩过一次）。
LPCWSTR MainWindowClassName() { return L"SAI2BridgeMain"; }LRESULT CALLBACK MainWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    return MainWindowProc(h, m, w, l);
}

} // namespace ui
} // namespace bridge
} // namespace sai2

// ---------------------------------------------------------------------------
// 程序入口
// ---------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    using namespace sai2;
    using namespace sai2::bridge;
    using namespace sai2::bridge::ui;

    SetProcessDPIAware();
    InstallCrashDiagnostics(ExeDir());

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    // 正常双击/bat 启动（无 CLI 参数）时脱离宿主控制台窗口：
    // 面板后面挂一个黑色 cmd 既难看，它的 QuickEdit 选择模式还会捕获
    // 鼠标（点住一拖变成选中文字），干扰正常拖拽操作。
    // 用 FreeConsole 而不是 ShowWindow(SW_HIDE)：Win11 默认终端是
    // Windows Terminal 时控制台窗口归 WT 所有，SW_HIDE 藏不住（非管理员
    // 启动必现黑窗口）；FreeConsole 对 conhost / WT 都有效。
    // 带 CLI 参数（--selftest/--uitest/--demo…）时保留控制台，供测试捕获输出。
    if (argc <= 1) {
        FreeConsole();
    }

    std::string pluginsDir = ExeDir() + "\\plugins";
    std::string logDir = ExeDir() + "\\logs";
    std::string outDir = ExeDir() + "\\selftest-out";
    std::string dumpPath;
    bool ctxTest = false;
    bool docTest = false;
    std::string mlA, mlB, mlOut;
    std::string clipPadIn, clipPadCw, clipPadCh, clipPadX, clipPadY;
    std::string mlCW, mlCH, mlAx, mlAy, mlBx, mlBy;
    bool versionOnly = false;
    std::string matchSnap, matchTmpl;
    bool padClipboard = false;
    bool selfTest = false;
    bool findTools = false; // 只报告"注入器/agent 找到在哪"，不真的注入
    int uiTestSeconds = 0; // >0: 打开全部功能窗口，跑几秒后自动退出（UI 冒烟测试）
    int demoSeconds = 0;   // >0: 功能演示模式（自动编排 + 逐帧导出 BMP 到 outDir）
    bool brushBench = false; // 笔刷流畅度自检（真实消息队列灌拖动 + 屏抓统计刷新）
    int brushW = 2480, brushH = 3508; // 自检用测试图尺寸（默认 A4@300dpi）
    int sai2DemoSeconds = 0; // >0: 真机 SAI2 演示（抓注入图层 → 液化涂抹 → 应用）

    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        auto next = [&](std::string& dst) {
            if (i + 1 < argc) {
                const std::wstring w = argv[++i];
                const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1,
                                                  nullptr, 0, nullptr, nullptr);
                dst.assign(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
                if (n > 1)
                    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, dst.data(), n,
                                        nullptr, nullptr);
            }
        };
        if (a == L"--selftest") selfTest = true;
        else if (a == L"--uitest") uiTestSeconds = 6;
        else if (a == L"--find-tools") findTools = true;
        else if (a == L"--demo") {
            demoSeconds = 30;
            // 可选跟一个秒数：--demo 20
            if (i + 1 < argc) {
                const int v = _wtoi(argv[i + 1]);
                if (v > 0 && v <= 300) {
                    demoSeconds = v;
                    ++i;
                }
            }
        }
        else if (a == L"--plugins") next(pluginsDir);
        else if (a == L"--logs") next(logDir);
        else if (a == L"--out") next(outDir);
        else if (a == L"--brushbench") {
            brushBench = true;
            // 可选跟一个尺寸：--brushbench 2000x2500
            if (i + 1 < argc) {
                const std::wstring v = argv[i + 1];
                const size_t xp = v.find(L'x');
                if (xp != std::wstring::npos) {
                    const int w = _wtoi(v.substr(0, xp).c_str());
                    const int h = _wtoi(v.substr(xp + 1).c_str());
                    if (w >= 64 && h >= 64 && w <= 8000 && h <= 8000) {
                        brushW = w;
                        brushH = h;
                        ++i;
                    }
                }
            }
        }
        else if (a == L"--sai2demo") {
            sai2DemoSeconds = 6;
            if (i + 1 < argc) {
                const int v = _wtoi(argv[i + 1]);
                if (v > 0 && v <= 120) {
                    sai2DemoSeconds = v;
                    ++i;
                }
            }
        }
        else if (a == L"--dump-clipboard") next(dumpPath);
        else if (a == L"--clip-pad") {
            next(clipPadIn); next(clipPadCw); next(clipPadCh);
            next(clipPadX); next(clipPadY);
        }
        else if (a == L"--ctx-test") ctxTest = true;
        else if (a == L"--doc-test") docTest = true;
        else if (a == L"--ml-sel") {
            // 测试钩子：预设图层多选（如 "0,1"），供无人值守验证"一起液化"
            std::string v;
            next(v);
            g_mlSelRows.clear();
            size_t p0 = 0;
            for (;;) {
                const size_t c = v.find(',', p0);
                const std::string one =
                    (c == std::string::npos) ? v.substr(p0)
                                             : v.substr(p0, c - p0);
                if (!one.empty()) g_mlSelRows.push_back(std::atoi(one.c_str()));
                if (c == std::string::npos) break;
                p0 = c + 1;
            }
        }
        else if (a == L"--multilayer-test") {
            next(mlA); next(mlB); next(mlCW); next(mlCH);
            next(mlAx); next(mlAy); next(mlBx); next(mlBy); next(mlOut);
        }
        else if (a == L"--version") versionOnly = true;
        else if (a == L"--match-test") {
            next(matchSnap);
            next(matchTmpl);
        }
        else if (a == L"--pad-clipboard") {
            next(matchSnap);
            next(matchTmpl);
            padClipboard = true;
        }
    }
    if (argv) LocalFree(argv);
    g_uiTestSeconds = uiTestSeconds;

    // 只报告注入器/agent 找在哪（不入界面、不注入），用于排查"找不到
    // sai2_injector.exe"这类问题：卡片会被解压成几层目录时特别有用。
    if (findTools) {
        AttachParentConsole();
        wchar_t buf[MAX_PATH] = {0};
        GetModuleFileNameW(nullptr, buf, MAX_PATH);
        const std::wstring exeDir = DirOf(std::wstring(buf));
        const InjectorPaths p = FindInjectorTools(exeDir);
        std::printf("本程序目录: %s\n", Wide2Utf8(exeDir).c_str());
        std::printf("injector  : %s\n",
                    p.injector.empty() ? "(未找到)" : Wide2Utf8(p.injector).c_str());
        std::printf("agent dll : %s\n",
                    p.agent.empty() ? "(未找到)" : Wide2Utf8(p.agent).c_str());
        std::printf("已查找 %d 个位置:\n", static_cast<int>(p.tried.size()));
        for (const std::wstring& t : p.tried)
            std::printf("  %s\n", Wide2Utf8(t).c_str());
        return p.injector.empty() ? 1 : 0;
    }

    // --clip-pad <in.bmp> <cw> <ch> <x> <y>
    // 把 in.bmp 摆到 (x,y) 并补边成 cw x ch 后写进剪贴板。
    // 用途：造多层测试文档时，把某层内容复制一份到画布别处，
    //       这样两个图层的画布偏移不同，"同一形变"才有可判别性。
    if (!clipPadIn.empty()) {
        AttachParentConsole();
        ImageBuffer in;
        std::string err;
        if (!ReadBmp32(clipPadIn, in, err) || !in.IsValid()) {
            std::printf("FAIL 读入: %s (%s)\n", err.c_str(), clipPadIn.c_str());
            return 2;
        }
        const int cw = std::atoi(clipPadCw.c_str());
        const int ch = std::atoi(clipPadCh.c_str());
        const int px = std::atoi(clipPadX.c_str());
        const int py = std::atoi(clipPadY.c_str());
        ImageBuffer out(cw, ch, PixelFormat::RGBA8);
        out.Clear(RGBA{0, 0, 0, 0});
        for (int y = 0; y < in.Height(); ++y) {
            const int dy = py + y;
            if (dy < 0 || dy >= ch) continue;
            for (int x = 0; x < in.Width(); ++x) {
                const int dx = px + x;
                if (dx < 0 || dx >= cw) continue;
                RGBA c;
                if (in.GetPixel(x, y, c)) out.SetPixel(dx, dy, c);
            }
        }
        std::string werr;
        if (!WriteClipboardImage(out, werr)) {
            std::printf("FAIL 写剪贴板: %s\n", werr.c_str());
            return 1;
        }
        std::printf("OK 已写入剪贴板: %dx%d，内容落在 (%d,%d)\n", cw, ch, px, py);
        return 0;
    }

    if (!dumpPath.empty()) {
        AttachParentConsole();        ImageBuffer img;
        std::string err, fmt;
        if (!ReadClipboardImage(img, err, &fmt)) {
            std::printf("FAIL 读取剪贴板: %s\n", err.c_str());
            return 1;
        }
        std::string werr;
        if (!WriteBmp32(img, dumpPath, werr)) {
            std::printf("FAIL 导出 BMP: %s\n", werr.c_str());
            return 1;
        }
        std::printf("OK format=%s size=%dx%d -> %s\n", fmt.c_str(), img.Width(),
                    img.Height(), dumpPath.c_str());
        return 0;
    }

    if (selfTest) {
        AttachParentConsole();
        const int failures = sai2::bridge::RunSelfTest(outDir, pluginsDir, logDir);
        std::printf("\n失败项: %d\n", failures);
        return failures;
    }

    // 只读诊断：抓图层 + 求画布上下文，不改动 SAI2 文档
    if (versionOnly) {
        AttachParentConsole();
        std::printf("sai2_bridge build: %s\n", BuildStamp().c_str());
        std::printf("（与日志里的 \"bridge started ... build=...\" 对照，\n"
                    "  即可确认正在跑的是不是这个构建）\n");
        return 0;
    }
    if (!mlA.empty())
        return RunMultiLayerTest(mlA, mlB, std::atoi(mlCW.c_str()),
                                 std::atoi(mlCH.c_str()),
                                 std::atoi(mlAx.c_str()),
                                 std::atoi(mlAy.c_str()),
                                 std::atoi(mlBx.c_str()),
                                 std::atoi(mlBy.c_str()), mlOut,
                                 pluginsDir, logDir);
    if (docTest) return RunDocTest();
    if (ctxTest) return RunCanvasCtxTest();
    if (!matchSnap.empty() && !matchTmpl.empty())
        return padClipboard ? RunPadClipboard(matchSnap, matchTmpl, pluginsDir,
                                              logDir)
                            : RunMatchTest(matchSnap, matchTmpl);

    AppState& s = S();
    s.core = std::make_unique<BridgeCore>(pluginsDir, logDir);
    std::string err;
    if (!s.core->Startup(err)) {
        MessageBoxA(nullptr,
                    ("插件管理器启动失败:\n" + err + "\n\n插件目录: " + pluginsDir)
                        .c_str(),
                    "SAI2 Bridge", MB_ICONERROR | MB_OK);
        return 1;
    }
    ApplyTheme(); // 主题（跟随系统/浅色/深色）必须在首帧前应用

    // 把构建时间戳写进日志：以后再出现"改了却没生效"，先看这一行就能判断
    // 跑的是不是新构建（曾踩过：面板进程启动时间早于修复时间）。
    s.core->Log(std::string("bridge build: ") + BuildStamp());

    if (!RegisterWindowClasses(hInst)) {
        MessageBoxA(nullptr, "窗口类注册失败", "SAI2 Bridge",
                    MB_ICONERROR | MB_OK);
        return 1;
    }

    s.main.hwnd = nullptr;
    s.main.id = BridgeCore::kWinMain;

    RECT rc = {0, 0, MainW(), MainH()};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(
        0, MainWindowClassName(),
        L"SAI2 插件面板 — 抓取图层后点功能按钮开独立窗口",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left,
        rc.bottom - rc.top, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;
    s.mainHwnd = hwnd;
    EnableRoundedWindow(hwnd); // Win11 圆角窗口

    // ★ 顺序很关键：**先让窗口显示**，再做启动时的抓取/图层刷新。
    // 原来 ShowWindow 在这两步之后，而 RefreshLayers() 里的 agent 查询
    // 每次连接预算 3 秒（AgentClient::Connect 的默认值）：agent 还没注入时会
    // 连续白等 → 表现为"启动慢，而且起来后一段时间不响应"。
    // 现在：窗口先出来（首帧立刻呈现），随后用 400ms 的**启动预算**做首刷 ——
    // 已注入时第一次 CreateFileW 就成功（等于零等待），未注入则快速失败并
    // 显示未连接，用户点「连接注入」即可；最后恢复 3 秒的正常预算。
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    sai2agent::AgentClient::SetDefaultConnectTimeout(400);
    if (ClipboardHasImage()) DoGrabFromClipboard(s.main);
    RefreshLayers();
    sai2agent::AgentClient::SetDefaultConnectTimeout(3000);

    // 功能演示模式：自动编排操作流程 + 逐帧导出 BMP（合成视频用）
    if (demoSeconds > 0) {
        g_uiTestSeconds = demoSeconds; // 跳过图层看门狗
        if (outDir == ExeDir() + "\\selftest-out")
            outDir = ExeDir() + "\\demo-frames"; // 默认输出目录
        CreateDirectoryW(U8(outDir).c_str(), nullptr);
        RunDemo(demoSeconds, outDir);
    }
    // 笔刷流畅度自检：真实消息队列灌拖动 + 屏抓统计（量化"涂抹时画面不动"）
    else if (brushBench) {
        g_uiTestSeconds = 60;
        if (outDir == ExeDir() + "\\selftest-out")
            outDir = ExeDir() + "\\brushbench-out";
        CreateDirectoryW(U8(outDir).c_str(), nullptr);
        RunBrushBench(brushW, brushH, outDir);
    }
    // 真机 SAI2 演示：抓注入图层 → 液化真涂抹 → 应用回写（录素材用）
    else if (sai2DemoSeconds > 0) {
        g_uiTestSeconds = sai2DemoSeconds + 60; // 跳过图层看门狗
        RunSai2Demo(sai2DemoSeconds, outDir);
    }
    // UI 冒烟测试：把 4 个功能窗口全部打开（每个都完整绘制一遍布局），
    // 停留 uiTestSeconds 秒后自动退出。用于无人值守地验证窗口创建/绘制。
    else if (uiTestSeconds > 0) {
        if (!s.core->HasImage()) {
            // 没有剪贴板图像时造一张测试图，保证画布/笔刷/缩略图代码都跑一遍
            ImageBuffer probe;
            probe.Create(640, 480, PixelFormat::RGBA8);
            for (int y = 0; y < probe.Height(); ++y)
                for (int x = 0; x < probe.Width(); ++x) {
                    RGBA c;
                    c.r = static_cast<uint16_t>(x * 255 / probe.Width());
                    c.g = static_cast<uint16_t>(y * 255 / probe.Height());
                    c.b = static_cast<uint16_t>(((x / 16 + y / 16) % 2) ? 200 : 60);
                    c.a = 255;
                    probe.SetPixel(x, y, c);
                }
            s.core->SetImage(std::move(probe), "uitest");
        }
        // 模拟一堆图层：覆盖图层列表滚动条（轨道+滑块）的绘制路径
        for (int i = 1; i <= 30; ++i)
            s.layerNames.push_back(L"图层 " + std::to_wstring(i) +
                                   (i % 5 == 0 ? L"（文件夹）" : L""));
        s.layerAttrs.assign(30, L"");
        s.agentOnline = true;
        for (int i = 1; i < BridgeCore::kWinCount; ++i) OpenToolWindow(i);
        s.core->Log("uitest: all tool windows opened");
        // 用后台线程而不是 SetTimer：无人值守时更确定（定时器依赖窗口过程分发）
        std::thread([uiTestSeconds]() {
            Sleep(static_cast<DWORD>(uiTestSeconds) * 1000);
            S().core->Log("uitest: done, quitting");
            // 先请求正常退出，再兜底强制结束：保证无人值守一定跑得完
            PostMessageW(S().mainHwnd, WM_APP + 11, 0, 0);
            Sleep(1500);
            ExitProcess(0);
        }).detach();
    }

    MSG msg;
    g_hbThread = std::thread(HeartbeatLoop);
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_hbStop = true;
    if (g_hbThread.joinable()) g_hbThread.join();
    CloseAllToolWindows();
    s.core->Shutdown();
    s.core.reset();
    return 0;
}
