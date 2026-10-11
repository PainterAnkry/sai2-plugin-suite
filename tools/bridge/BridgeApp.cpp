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
int MainW() { return Scale(760); }
int MainH() { return Scale(950); }

RECT RcPreview() { return {Scale(12), Scale(46), Scale(748), Scale(410)}; }
RECT RcCategory() { return {Scale(12), Scale(442), Scale(748), Scale(476)}; }
RECT RcLayerHead() { return {Scale(12), Scale(488), Scale(470), Scale(512)}; }
RECT RcLayerList() { return {Scale(12), Scale(514), Scale(470), Scale(706)}; }
RECT RcLayerBtns() { return {Scale(12), Scale(714), Scale(470), Scale(742)}; }
RECT RcChecks() { return {Scale(486), Scale(488), Scale(748), Scale(610)}; }
RECT RcButtons() { return {Scale(486), Scale(640), Scale(748), Scale(742)}; }
RECT RcStatus() { return {Scale(12), Scale(754), Scale(748), Scale(936)}; }

enum LayerBtnId { LBTN_INJECT = 0, LBTN_REFRESH, LBTN_GRABSEL, LBTN_COUNT };

// 主面板私有命令
enum {
    CMD_LAYER_ROW = 1000,   // +i  点击图层行
    CMD_LAYER_BTN = 2000,   // +i  图层操作按钮
    CMD_WATCH = 910,        // 监听剪贴板
    CMD_AUTOLAYER = 911,    // 点图层行时在 SAI2 选中
    CMD_MERGE = 912         // 合并回原层
};

int g_listScroll = 0;
int g_uiTestSeconds = 0; // --uitest 模式：跳过图层看门狗，保留模拟图层供探针截图

// 图层列表滚动条：把鼠标 y 映射为滚动位置（拖动滑块时用）。返回是否已处理。
bool ListScrollAt(UiWindow& win, int my) {
    AppState& s = S();
    const int total = static_cast<int>(s.layerNames.size());
    if (total == 0 || RectW(win.scrollTrack) <= 0) return false;
    const int rowH = Scale(22);
    const int viewH = RectH(RcLayerList()) - Scale(4);
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

    FillRect(dc, &client, s.bgBrush);

    // ---- 顶栏 ----
    RECT header = {0, 0, client.right, Scale(38)};
    FillRectC(dc, header, C_PANEL2);
    RECT t = {Scale(14), 0, Scale(430), Scale(38)};
    TextC(dc, L"SAI2 插件面板", t, C_TEXT, s.fTitle,
          DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    {
        const ImageBuffer& img = core.DisplayImage(BridgeCore::kWinMain);
        wchar_t sub[200];
        swprintf(sub, 200, L"%s   %d x %d", (core.HasImage() ? L"已抓取图层" : L"等待抓取"),
                 img.IsValid() ? img.Width() : 0,
                 img.IsValid() ? img.Height() : 0);
        RECT rr = {Scale(300), 0, client.right - Scale(140), Scale(38)};
        TextC(dc, sub, rr, core.HasImage() ? C_OK : C_DIM, s.fSmall,
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
    win.canvas = RcPreview();
    DrawCanvas(dc, win, false, true);

    // ---- 功能按钮（4 个分组，各开各的窗口） ----
    {
        RECT title = {Scale(12), Scale(418), Scale(748), Scale(438)};
        TextC(dc, L"功能（点一下 = 抓取当前图层并打开独立功能窗口）", title,
              C_DIM, s.fSmall, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        DrawCategoryStrip(dc, win, RcCategory(), std::wstring());
    }

    // ---- 图层区 ----
    {
        RECT head = RcLayerHead();
        TextC(dc, L"SAI2 图层（注入代理）", head, C_DIM, s.fSmall,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        RECT hint = {head.left + Scale(190), head.top, head.right, head.bottom};
        TextC(dc, s.agentOnline ? L"已连接" : L"未连接 —— 点「连接注入」", hint,
              s.agentOnline ? C_OK : C_WARN, s.fSmall,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        RECT box = RcLayerList();
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
                    const bool sel = (s.selLayer == i);
                    if (sel) {
                        RECT sr = rr;
                        sr.left += 2;
                        sr.right -= 2;
                        FillRectC(dc, sr, C_ACCENT2);
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
        const RECT strip = RcLayerBtns();
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
        RECT box = RcChecks();
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
    }

    // ---- 命令按钮（两行两列：文字放得下，不再省略号截断） ----
    {
        const RECT b = RcButtons();
        const int bh = Scale(34);
        RECT row2 = {b.left, b.bottom - bh, b.right, b.bottom};
        RECT row1 = {b.left, row2.top - Scale(6) - bh, b.right,
                     row2.top - Scale(6)};
        DrawCmdButtons(dc, win, row1, {CMD_GRAB, CMD_APPLY}, CMD_APPLY);
        DrawCmdButtons(dc, win, row2, {CMD_UNDO, CMD_RESET}, CMD_APPLY);
    }

    // ---- 状态与日志 ----
    {
        RECT box = RcStatus();
        FillRectC(dc, box, C_PANEL2);
        FrameRectC(dc, box, C_LINE);
        RECT sr = {box.left + Scale(8), box.top + Scale(3),
                   box.right - Scale(8), box.top + Scale(21)};
        std::wstring txt = U8(core.Status());
        if (!s.flash.empty()) txt = s.flash;
        TextC(dc, txt, sr, C_TEXT, s.fSmall,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        const auto& logs = core.LogLines();
        const size_t maxLines = 12;
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
        case LBTN_GRABSEL: DoSelectLayer(s.selLayer); break;
        default: break;
        }
        return true;
    }
    if (h.kind == HK_CMD) {
        if (h.index >= CMD_LAYER_ROW && h.index < CMD_LAYER_ROW + 256) {
            s.selLayer = h.index - CMD_LAYER_ROW;
            DoSelectLayer(s.selLayer);
            return true;
        }
        switch (h.index) {
        case CMD_WATCH: s.watchClipboard = !s.watchClipboard; return true;
        case CMD_AUTOLAYER: s.autoGrabLayer = !s.autoGrabLayer; return true;
        case CMD_MERGE: s.mergeBack = !s.mergeBack; return true;
        default: break;
        }
    }
    return false;
}

bool MainMsg(UiWindow& win, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_MOUSEWHEEL) {
        const int delta = GET_WHEEL_DELTA_WPARAM(wp);
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(win.hwnd, &pt);
        if (InRect(RcLayerList(), pt.x, pt.y)) {
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
    int panelLeft = 0;
    int rowScreenY = 0;
    DWORD clipSeq = 0;
    ULONGLONG t0 = 0;
    std::wstring message;
};

CaptureJob g_cap;
std::thread g_capThread;

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

bool MenuItemClickImpl(HWND sai, int menuBarOffsetX, const wchar_t* itemPrefix) {
    RECT r;
    GetWindowRect(sai, &r);
    ForceForeground(sai);
    Sleep(120);
    if (GetForegroundWindow() != sai) return false;
    SendEscape();
    Sleep(150);
    const ULONGLONG menuTick = GetTickCount64();
    ClickAtScreenImpl(r.left + menuBarOffsetX, r.top + 33);
    Sleep(480);
    int ix = -1, iy = 0;
    for (int i = 0; i < 8; ++i) {
        if (FindCapturedText(itemPrefix, ix, iy, menuTick - 200)) {
            ClickAtScreenImpl(ix + 40, iy + 6);
            Sleep(350);
            return true;
        }
        Sleep(140);
    }
    SendEscape();
    return false;
}

// 后台线程的阶段推进
void RunCaptureJob() {
    CaptureJob& j = g_cap;
    j.t0 = GetTickCount64();
    j.grabbed = false;
    j.message.clear();
    j.clipSeq = GetClipboardSequenceNumber();

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
        ClickAtScreenImpl(j.panelLeft + 34, j.rowScreenY + 6);
        Sleep(260);
    } else {
        ForceForeground(sai);
        Sleep(180);
    }

    // 2) 快捷键抓取：Ctrl+A 全选 + Ctrl+C 拷贝。
    //    菜单点击链路（截屏找文字+模拟点击）太脆弱：菜单没弹出/前台切换失败/
    //    文字没截到，任何一步失败窗口就永远开不出来。快捷键只要 SAI2 在前台
    //    就一定生效（readme 里手动 Ctrl+C 的流程就是证明）。
    SendCtrlScan(0x1E); // Ctrl+A 全选
    Sleep(350);
    SendCtrlScan(0x2E); // Ctrl+C 拷贝
    for (int i = 0; i < 20; ++i) {
        Sleep(100);
        if (GetClipboardSequenceNumber() != j.clipSeq && ClipboardHasImage()) {
            j.grabbed = true;
            break;
        }
    }
    if (!j.grabbed && ClipboardHasImage()) j.grabbed = true;

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
    if (!MenuItemClickImpl(target, 120, L"全选")) {
        s.flash = L"未定位到 SAI2 的「全选」菜单项";
        return;
    }
    Sleep(200);
    if (MenuItemClickImpl(target, 120, L"粘贴")) {
        s.flash = L"已粘贴回 SAI2（全选对齐，无需手动调整位置）";
    } else {
        s.flash = L"未定位到 SAI2 的「粘贴」菜单项 —— 请手动 Ctrl+V";
    }
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
        DoGrabFromClipboard(s.main);
        s.flash.clear();
        if (want >= 1 && want < BridgeCore::kWinCount) {
            OpenToolWindow(want);
        }
    } else {
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
    if (s.core->GrabFromClipboard(err)) {
        s.flash.clear();
        OnImageAcquired();
    } else {
        s.flash = U8("抓取失败: " + err);
        InvalidateAll();
    }
}

// ---------------------------------------------------------------------------
// 应用 / 撤销 / 复位
// ---------------------------------------------------------------------------
void DoApplyFlow(UiWindow& win) {
    AppState& s = S();
    const BridgeCore::ApplyResult r = s.core->ApplyToClipboard();
    if (r.clipboardWritten) {
        s.selfWriteTick = GetTickCount64();
        (void)win;
        if (s.autoPaste) {
            AutoPasteToSai2();
            if (s.mergeBack) ScheduleMergeAfterPaste(s.mainHwnd);
        }
        s.flash.clear();
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
            s.layerAttrs = agg.attrs;
            s.layerScreenY = agg.screenY;
            s.panelLeft = grp->screenRect.left;
        }    } else {
        if (s.agentOnline) s.core->Log("注入代理失联（SAI2 可能已退出）");
        s.agentOnline = false;
        s.layers.groups.clear();
        s.layers.panelGuess = 0;
    }
}

std::vector<std::wstring> LayerNames() { return S().layerNames; }

// 点击图层行：在 SAI2 里选中该层（不抓取）
void DoSelectLayer(int rowIdx) {
    AppState& s = S();
    if (!s.agentOnline) {
        s.flash = L"未连接注入代理 —— 点「连接注入」";
        return;
    }
    if (rowIdx < 0 || rowIdx >= static_cast<int>(s.layerScreenY.size())) {
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
    ClickAtScreen(static_cast<int>(s.panelLeft) + 34,
                  s.layerScreenY[static_cast<size_t>(rowIdx)] + 6);
    Sleep(220);
    s.flash = L"已在 SAI2 选中图层「" + s.layerNames[static_cast<size_t>(rowIdx)] +
              L"」";
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
    if (!s.mergeBack || !s.mergePending) return;
    s.mergePending = false;
    SetTimer(hwnd, 5, 900, nullptr);
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
    if (MenuItemClickImpl(sai, 292, L"向下转写")) {
        s.flash = L"已合并回原图层（向下转写）";
        s.core->Log("merge-down: merged pasted layer back");
    } else {
        s.flash = L"未在图层菜单里找到「向下转写」";
    }
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
    case WM_CREATE:
        InitFonts();
        CreateParamEdits(win);
        win.hwnd = hwnd;
        win.id = BridgeCore::kWinMain;
        AddClipboardFormatListener(hwnd);
        RegisterHotKey(hwnd, 1, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'G');
        RegisterHotKey(hwnd, 2, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'A');
        RegisterHotKey(hwnd, 3, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'Z');
        RegisterHotKey(hwnd, 4, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'R');
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
            DoGrabFromClipboard(win);
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

// 主面板窗口类名与窗口过程：给 UiWindow.cpp 的 RegisterWindowClasses 用。
// 这里必须是**唯一**注册主面板类的地方 —— 重复注册同名类会让窗口挂上
// 错误的窗口过程（现象：主面板整块空白，这个 bug 已经踩过一次）。
LPCWSTR MainWindowClassName() { return L"SAI2BridgeMain"; }
LRESULT CALLBACK MainWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
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

    // 正常双击/bat 启动（无 CLI 参数）时隐藏宿主控制台窗口：
    // 面板后面挂一个黑色 cmd 既难看，它的 QuickEdit 选择模式还会捕获
    // 鼠标（点住一拖变成选中文字），干扰正常拖拽操作。
    // 带 CLI 参数（--selftest/--uitest/--demo…）时保留控制台，供测试捕获输出。
    if (argc <= 1) {
        if (HWND con = GetConsoleWindow()) ShowWindow(con, SW_HIDE);
    }

    std::string pluginsDir = ExeDir() + "\\plugins";
    std::string logDir = ExeDir() + "\\logs";
    std::string outDir = ExeDir() + "\\selftest-out";
    std::string dumpPath;
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

    if (!dumpPath.empty()) {
        AttachParentConsole();
        ImageBuffer img;
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

    // 启动时若剪贴板里已经有图，直接抓一次，省一次操作
    if (ClipboardHasImage()) DoGrabFromClipboard(s.main);
    RefreshLayers();


    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

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
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CloseAllToolWindows();
    s.core->Shutdown();
    s.core.reset();
    return 0;
}
