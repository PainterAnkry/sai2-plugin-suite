#pragma once
// ============================================================================
// Ui.h - 功能窗口公共层（Win32 自绘，零第三方依赖）
//
// 每个"功能"= 一个独立窗口（PS 风格）：
//   液化窗口 / 滤镜窗口 / 模糊窗口 / 调整窗口，各自有画布、工具条、参数面板，
//   互不干扰；主面板只负责抓图、图层与"打开哪个功能窗口"。
//
// 本头文件集中声明：窗口结构体、绘制/交互原语、画布绘制、滑杆与数值框、
// 各类窗口的创建/刷新入口、剪贴板监听与 SAI2 图层抓取。
// ============================================================================

#include "AgentBridge.h"
#include "BridgeCore.h"

#include <memory>
#include <string>
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

namespace sai2 {
namespace bridge {
namespace ui {

using namespace sai2;

// ---------------------------------------------------------------------------
// 配色（主题化：跟随系统 / 浅色 / 深色，见 ApplyTheme/CycleTheme）
// 这些是全局变量，ApplyTheme() 会按当前主题整体切换。
// ---------------------------------------------------------------------------
extern COLORREF C_BG;
extern COLORREF C_PANEL;
extern COLORREF C_PANEL2;
extern COLORREF C_LINE;
extern COLORREF C_TEXT;
extern COLORREF C_DIM;
extern COLORREF C_ACCENT;
extern COLORREF C_ACCENT2;
extern COLORREF C_WARN;
extern COLORREF C_OK;
extern COLORREF C_CANVAS;

// ---------------------------------------------------------------------------
// 主题
// ---------------------------------------------------------------------------
enum ThemeMode {
    THEME_FOLLOW = 0, // 跟随 Windows 系统应用主题
    THEME_LIGHT = 1,
    THEME_DARK = 2
};
// 读取设置（注册表）+ 系统主题，切换全部配色并重建背景刷（启动时调用一次）
void ApplyTheme();
ThemeMode ThemeSetting();              // 用户设置（未初始化时 = 跟随系统）
void CycleTheme(HWND hwnd);            // 循环切换：跟随系统 → 浅色 → 深色
const wchar_t* ThemeLabel();           // 当前设置文字（按钮用）
bool ThemeIsDark();                    // 当前生效的是深色吗

// 圆角矩形（填充+描边），radius 为设备像素
void RoundBox(HDC dc, const RECT& rc, COLORREF fill, COLORREF border,
              int radius);
// Win11+ 顶层窗口圆角（旧系统静默跳过）
void EnableRoundedWindow(HWND hwnd);

// 画布缩放范围（连续缩放；win.zoom == 0 表示"适配窗口"）
const double kZoomMin = 0.02;
const double kZoomMax = 32.0;

// ---------------------------------------------------------------------------
// 命中区域
// ---------------------------------------------------------------------------
enum HitKind {
    HK_NONE = 0,
    HK_PREVIEW,   // 画布区域（液化涂抹 / 右键平移）
    HK_ZIN,
    HK_ZOUT,
    HK_ZFIT,
    HK_Z100,      // 100%（1:1 像素）
    HK_ZSLIDER,   // 放大镜滑杆：左拖缩小、右拖放大（track = 滑杆轨道）
    HK_LISTSCROLL, // 图层列表滚动条（track = 滚动条轨道，拖动滚动）
    HK_SLIDER,    // index = 参数序号
    HK_TOOL,      // index = 该分组内的效果下标
    HK_CMD,       // index = 命令枚举（见 CmdId）
    HK_THUMB,     // 滤镜缩略图：index = 效果下标
    HK_CLOSE
};

enum CmdId {
    CMD_APPLY = 0,   // 应用 → 剪贴板（并粘贴回 SAI2）
    CMD_UNDO,
    CMD_RESET,
    CMD_CLOSE,
    CMD_CHAIN,       // 叠加处理
    CMD_AUTOPASTE,
    CMD_GRAB,        // 重新抓取 SAI2 当前图层
    CMD_THEME,       // 切换主题（跟随系统/浅色/深色）
    CMD_COUNT
};

struct UiHit {
    RECT rc{};
    RECT track{}; // 滑杆轨道
    int kind = HK_NONE;
    int index = -1;
};

// ---------------------------------------------------------------------------
// 单个功能窗口的运行时状态
// ---------------------------------------------------------------------------
struct UiWindow {
    int id = BridgeCore::kWinMain;
    HWND hwnd = nullptr;

    // 画布矩形（写像素的地方）
    RECT canvas{};

    std::vector<UiHit> hits;

    // 缩放/平移（相对画布）
    // zoom = 每图像像素对应的设备像素数；0 = 适配（等比缩放到窗口并居中）。
    // panX/panY = 视口左上角对应的图像坐标（图像大于视口时有效，自动钳制）。
    double zoom = 0.0;
    double panX = 0, panY = 0;

    // 鼠标悬停（UiTrackMouse 更新，绘制控件用 UiHover 查询）
    POINT mouse{};
    bool mouseValid = false;
    bool panning = false;
    POINT panLast{};
    // 液化窗口抓手工具（UI 层状态）：激活时左键拖动 = 平移画布而非涂抹。
    // 临时平移：任何功能窗口按住空格 + 左键拖、或中键拖（见 UiWindow.cpp）。
    bool handTool = false;

    // 液化涂抹
    bool dragStroke = false;
    POINT strokeLast{};
    // 累积已进置换场、等一个显示帧渲染（kRenderMsg 驱动）
    bool strokeDirty = false;
    // 已经投递了一条渲染请求（避免拖动时把消息队列灌满渲染请求）
    bool renderPosted = false;

    // 滑杆拖动
    int dragSlider = -1;
    int sliderPendX = 0;
    bool sliderPending = false;

    // 画布缩放滑杆（放大镜）拖动
    bool dragZoom = false;
    RECT zoomTrack{};

    // 图层列表滚动条拖动
    bool dragList = false;
    RECT scrollTrack{};

    // 参数数值输入框（PS 风格）
    HWND hEdits[4] = {};
    int editCount = 0;

    // 缩略图选择（滤镜窗口）
    int thumbScroll = 0;

    // 正在生成缩略图
    bool thumbsBuilding = false;

    // 该窗口当前显示的效果（= core->CurrentEffect() 的镜像，便于快速比对）
    int effectIndex = -1;

    // 画布位图缓存
    std::vector<uint8_t> bgra;
    int bgraW = 0, bgraH = 0;
    bool bgraDirty = true;

    // 状态栏临时提示
    std::wstring flash;
    ULONGLONG flashTick = 0;
};

// ---------------------------------------------------------------------------
// 全局状态（BridgeApp.cpp 定义）
// ---------------------------------------------------------------------------
struct AppState {
    std::unique_ptr<BridgeCore> core;
    HWND mainHwnd = nullptr;

    UiWindow main;                            // 主面板
    UiWindow toolWindows[BridgeCore::kWinCount]; // 1..4 功能窗口

    // 主面板
    int listScroll = 0;
    bool autoPaste = true;
    bool watchClipboard = true;
    bool autoGrabLayer = true; // 点图层行时自动到 SAI2 选中
    bool mergeBack = false;    // 应用后自动"向下转写"合并回原图层
    // 精确回贴（1.19）：整层抓取时记录画布尺寸与内容偏移，应用后把结果
    // 补边成画布尺寸再 Ctrl+V —— SAI2 按画布 (0,0) 对齐，内容正好落回
    // 原位置。不依赖 Ctrl+Shift+V（很多 SAI2 版本没有该快捷键）。
    // 关闭时退回 Ctrl+A+Ctrl+V 居中粘贴 + 向下转写。
    bool pasteInPlace = true;
    bool agentPushed = false;  // 效果清单已推给注入代理
    bool mergePending = false;
    ULONGLONG selfWriteTick = 0;

    // 注入代理（图层识别）
    sai2bridge::AgentLayers layers;
    bool agentOnline = false;
    std::wstring agentHint;
    int selLayer = -1;
    // 多选（"一起液化"要一次处理多层）：
    //   Ctrl+点 = 切换单行、Shift+点 = 选范围、普通点 = 单选。
    // selLayers 升序去重；selLayer 是**主层**（最后点的那行），抓取窗口里编辑
    // 的就是它，应用时对其余层施加**同一形变**并各自合并回原层。
    std::vector<int> selLayers;
    int selAnchor = -1;   // Shift 范围选择的锚点行
    std::vector<std::wstring> layerNames;
    std::vector<int> layerScreenY;
    std::vector<std::wstring> layerAttrs;
    long long panelLeft = 0;
    int layersScroll = 0;
    // Agent 屏幕坐标 → 物理屏幕坐标的比例（默认 1:1）。
    // SAI2 若非 DPI 感知（或显示器缩放 ≠100%），agent 在 SAI2 内部取到的
    // ClientToScreen/GetWindowRect 是虚拟化坐标，而桥是 DPI 感知的，
    // SendInput 用物理坐标 —— 不换算会把点击打到标题栏等错误位置。
    double agentScaleX = 1.0;
    double agentScaleY = 1.0;

    // 字体
    HFONT fTitle = nullptr, fBody = nullptr, fSmall = nullptr, fBold = nullptr;
    HBRUSH bgBrush = nullptr;

    std::wstring flash;
};

AppState& S();

// ---------------------------------------------------------------------------
// 悬停状态：绘制控件时用 UiHover() 决定是否加高亮；
// WM_MOUSEMOVE 里调 UiTrackMouse()，WM_MOUSELEAVE 里调 UiMouseLeave()。
// ---------------------------------------------------------------------------
bool UiHover(const UiWindow& win, const RECT& rc);
void UiTrackMouse(UiWindow& win, HWND hwnd, int mx, int my);
void UiMouseLeave(UiWindow& win, HWND hwnd);

// 某个窗口当前选中的效果下标（工具窗口 = 它自己选的效果；主面板 = 主面板选的）。
// 参数面板、滑杆、数值框都必须用这个，而不是 core->CurrentEffect() ——
// 后者是"引擎里最后碰过的那个效果"，多窗口同时开着时会张冠李戴。
int WindowEffectIndex(const UiWindow& win);

// 功能窗口副标题前半段："已抓取图层 W x H → 画布 CW x CH（整幅画布）"。
// 抓取到的是 SAI2 给的**图层包围盒**，补边到画布尺寸发生在"应用"时；
// 两者并排显示可以避免"画布大小和截取大小不一致 = bug"的误解；
// 拿不到画布尺寸时直接给"粘贴会错位"的警告。
void GrabSubtitleHead(wchar_t* out, size_t n, const ImageBuffer& img);

// ---------------------------------------------------------------------------
// 绘制原语
// ---------------------------------------------------------------------------
std::wstring U8(const std::string& s);
void FillRectC(HDC dc, const RECT& rc, COLORREF c);
void FrameRectC(HDC dc, const RECT& rc, COLORREF c);
void RoundedPanel(HDC dc, const RECT& rc, COLORREF fill, COLORREF border);
void TextC(HDC dc, const std::wstring& s, RECT rc, COLORREF col, HFONT f,
           UINT flags);
int RectW(const RECT& r);
int RectH(const RECT& r);
bool InRect(const RECT& r, int x, int y);
int Scale(int v); // DPI 缩放
void InitFonts();

// 画布视图：把图像等比映射到 win.canvas 内（信箱式留边，不拉伸变形）。
struct CanvasView {
    bool valid = false;
    double zoom = 1.0;  // 设备像素 / 图像像素
    double ox = 0, oy = 0; // 图像 (0,0) 相对 inner 左上角的偏移（可为负=裁切）
    int dx = 0, dy = 0, dw = 0, dh = 0; // 图像目标矩形（设备像素，取整后）
    double fit = 1.0;   // 当前"适配"倍率
    RECT inner{};       // 画布可用区（canvas 内缩 1px）
};
// 计算窗口当前画布视图（顺带把 win.panX/panY 钳制到合法范围）。
bool CanvasViewOf(UiWindow& win, CanvasView& v);
// 以客户区点 (mx,my) 为锚点缩放到 newZoom（保持光标下的图像点不动）。
void CanvasZoomAt(UiWindow& win, double newZoom, int mx, int my);
// 放大镜滑杆 ↔ 倍率：对数映射 [kZoomMin, kZoomMax]，左端缩小、右端放大。
double CanvasZoomFromSlider(double t);   // t∈[0,1] → 倍率
double CanvasSliderFromZoom(double zoom); // 倍率 → t∈[0,1]
// 拖动缩放滑杆（mx = 客户区 x）。返回 true = 已处理。
bool OnZoomSliderDrag(UiWindow& win, int mx);

// 画布绘制：把 core 的显示图按窗口缩放/平移画到 win.canvas 里，
// 并登记 HK_PREVIEW / HK_ZIN / HK_ZOUT / HK_Z100 / HK_ZFIT / 液化笔刷圈。
void DrawCanvas(HDC dc, UiWindow& win, bool allowStroke, bool zoomButtons = true);

// 画一段参数面板（标签 + 滑杆 + 数值），返回下一行的 y。
// 数值框由 SyncParamEdits 负责摆放。
int DrawParamRow(HDC dc, UiWindow& win, const RECT& box, int y,
                 const UiParam& p, float value, int paramIndex);

// 数值输入框：创建（窗口 WM_CREATE 时调用一次）、跟随布局、回写
void CreateParamEdits(UiWindow& win);
void SyncParamEdits(UiWindow& win, const std::vector<int>& rowY,
                    const RECT& box);
void UpdateParamEdits(UiWindow& win);
void CommitParamEdit(UiWindow& win, int index);

// 分类按钮条（模糊/滤镜/调整/液化 四选一，点 = 换窗口）
void DrawCategoryStrip(HDC dc, UiWindow& win, const RECT& strip,
                       const std::wstring& activeGroup);
// 该分组内的效果按钮条（横排）
void DrawEffectStrip(HDC dc, UiWindow& win, const RECT& strip,
                     const std::vector<int>& effects);
// 命令按钮（应用/撤销/复位/关闭 …）
void DrawCmdButtons(HDC dc, UiWindow& win, const RECT& strip,
                    const std::vector<int>& cmds, int primaryCmd);
// 命令按钮文字
const wchar_t* CmdLabel(int cmd);

// 命中分发公共部分（分类按钮 / 命令按钮 / 缩放按钮 / 关闭）。
// 返回 true = 已处理（调用方不必再处理）。
bool HandleCommonHit(UiWindow& win, const UiHit& h);
bool HandleZoomHit(UiWindow& win, const UiHit& h);
// 命令按钮执行（应用/撤销/复位/关闭/切换选项）
void RunCmd(UiWindow& win, int cmd);

// 画布坐标换算（屏幕/客户区 → 归一化图像坐标）
bool CanvasPointToNorm(UiWindow& win, int mx, int my, float& nx, float& ny);

// 滑杆拖动（返回 true = 该 hit 是滑杆并已处理）
bool OnSliderDrag(UiWindow& win, int mx);

// 窗口位置记忆
void RestoreWindowPlacement(HWND hwnd, int defX, int defY, int w, int h);
void SaveWindowPlacement(HWND hwnd, const wchar_t* key);

// ---------------------------------------------------------------------------
// 功能窗口
// ---------------------------------------------------------------------------
// 主面板窗口类名 / 窗口过程（定义在 BridgeApp.cpp）。
// 注意：主面板与功能窗口**共用唯一的注册入口** RegisterWindowClasses()，
// 同一个类名被注册两次时第二次必然失败，窗口会挂上错误的窗口过程
// （现象：主面板整块空白）。
extern "C" const wchar_t* MainWindowClassName();
LRESULT CALLBACK MainWndProc(HWND, UINT, WPARAM, LPARAM);

// 注册窗口类（主面板 + 4 个功能窗口；wWinMain 里调用一次）
bool RegisterWindowClasses(HINSTANCE hInst);
// 打开/关闭某功能窗口（winIdx: kWinBlur / kWinFilter / kWinAdjust / kWinLiquify）
void OpenToolWindow(int winIdx);
void CloseToolWindow(int winIdx);
void CloseAllToolWindows();
bool ToolWindowOpen(int winIdx);
// 全部窗口重绘（图像变化后调用）
void InvalidateAll();
// 关闭所有窗口并退出消息循环
void QuitApp();

// 功能窗口的标准布局（所有分组窗口共用一套骨架）
struct UiLayout {
    RECT header{};
    RECT category{};
    RECT effects{};
    RECT canvas{};
    RECT panel{};
    RECT commands{};
};
UiLayout ComputeLayout(const RECT& client, int panelW);

// 画窗口骨架（标题 + 状态行 + 分组按钮条 + 组内效果条 + 命令条）
void DrawWindowChrome(HDC dc, UiWindow& win, const UiLayout& L,
                      const std::wstring& title, const std::wstring& subtitle,
                      const std::vector<int>& effects);

// 参数面板背景 + 标题
void DrawPanelFrame(HDC dc, const UiLayout& L, const std::wstring& title,
                    const std::wstring& hint);

// 功能窗口的注册规格（窗口类 / 标题 / 默认尺寸 / 绘制与点击回调）
struct ToolWindowSpec {
    int id = 0;
    const wchar_t* cls = L"";
    const wchar_t* title = L"";
    int w = 1000, h = 700;
    void (*draw)(HDC, UiWindow&, const RECT&) = nullptr;
    bool (*hit)(UiWindow&, const UiHit&) = nullptr;
    // 可选的窗口级消息钩子（返回 true = 已处理），用于滤镜缩略图的后台完成通知等
    bool (*msg)(UiWindow&, UINT, WPARAM, LPARAM) = nullptr;
};
const ToolWindowSpec& BlurWindowSpec();
const ToolWindowSpec& FilterWindowSpec();
const ToolWindowSpec& AdjustWindowSpec();
const ToolWindowSpec& LiquifyWindowSpec();

// 后台缩略图生成完成后调用（滤镜窗口）
void FilterThumbsArrived();

// 命令按钮动作
void RunCmd(UiWindow& win, int cmd);

// ---------------------------------------------------------------------------
// SAI2 交互
// ---------------------------------------------------------------------------
HWND FindSai2Window();
void ForceForeground(HWND target);
void ClickAtScreen(int x, int y);
bool FindCapturedText(const wchar_t* text, int& outX, int& outY,
                      ULONGLONG minTick);
bool MenuItemClick(HWND sai, int menuBarOffsetX, const wchar_t* itemPrefix);
void AutoPasteToSai2();
void DoApply(UiWindow& win);
void DoUndo(UiWindow& win);
void DoReset(UiWindow& win);
void DoGrabFromClipboard(UiWindow& win);
// 主面板「抓取 SAI2 图层」按钮 / 全局抓取热键：走完整抓取链路
// （切前台 → Esc → Ctrl+A 全选 → Ctrl+C），保证拿到的是整层
// （画布尺寸）内容，回贴才能按画布中心精确对齐。
// 代理不在线时退回 DoGrabFromClipboard（读当前剪贴板）。
void DoGrabWholeLayer(HWND hwnd);
// 图层：连接注入代理 / 刷新 / 选中并抓取某一层
struct LayerAgg {
    std::vector<std::wstring> names;
    std::vector<std::wstring> attrs;
    std::vector<int> screenY;
};
// 把注入代理捕获的文字行聚合为图层条目（名字 + 混合模式/不透明度 + 屏幕 y）
LayerAgg AggregateLayers(const std::vector<sai2bridge::AgentLayerRow>& rows);
// 图层面板所在的那个捕获分组
const sai2bridge::AgentLayerGroup* PanelGroup();

void RefreshLayers();
std::vector<std::wstring> LayerNames();
void DoInjectAsync(HWND hwnd);
void DoSelectLayer(int rowIdx);

// ---- 图层多选（Ctrl+点切换 / Shift+点范围）--------------------------------
// 面板是选中集的权威，再用真实点击（含 Ctrl+点）把它同步给 SAI2 ——
// 反过来"读 SAI2 选中了哪几层"没有数据源：图层面板的高亮不经过我们挂钩的
// 那几个 GDI 调用（实测 agent 抓到的位图里只有界面各部件的 DIB）。
bool RowIsSelected(int row);
void SetRowSelection(int row, bool additive, bool range);
void SyncSelectionToSai2();
// 测试钩子：--ml-sel 0,1 预设图层多选（无人值守验证「一起液化」用）
extern std::vector<int> g_mlSelRows;
// 主层应用完成后，对其余选中层逐一施加同一形变并各自合并回原层
void RunMultiLayerApply(HWND hwnd);
// 选中该层并抓取（后台线程；抓完可选打开 wantWindow 功能窗口）
void GrabSelectedLayer(HWND hwnd, int rowIdx, int wantWindow);
void DoMergeDown(HWND hwnd);
void ScheduleMergeAfterPaste(HWND hwnd);
void PushCategoriesToAgent();
void PollAgentRequest(HWND hwnd);

// 打开功能窗口前先确保拿到 SAI2 当前图层：
//   needImage = true 且当前没有图像/图层已变 → 自动"选中并抓取该图层"，抓完回调开窗
void RequestToolWindow(HWND hwnd, int winIdx, bool needImage);
// 抓取流程是否正在跑（跑的时候按钮显示"抓取中…"）
bool CaptureInProgress();
// 主面板拿到新图后，把结果同步给已打开的功能窗口
void OnImageAcquired();

// 应用 → 回写剪贴板 → 自动粘贴（+可选合并回层）
void DoApplyFlow(UiWindow& win);

} // namespace ui
} // namespace bridge
} // namespace sai2
