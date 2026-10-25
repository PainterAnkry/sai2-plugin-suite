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
#include <set>
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

// ---- 语义色（不要在窗口代码里再写 RGB 字面量） ---------------------------
extern COLORREF C_INPUT;        // 输入框 / 数值底框
extern COLORREF C_INPUT_LINE;   // 输入框描边（默认态）；聚焦态用 C_ACCENT
extern COLORREF C_TOOLTIP_BG;   // 悬浮提示（反色，两个主题下都是深底浅字）
extern COLORREF C_TOOLTIP_TEXT;
extern COLORREF C_INVERSE_BG;   // 主按钮：反色填充
extern COLORREF C_INVERSE_TEXT;
extern COLORREF C_CURVE_GRID;   // 曲线网格底
extern COLORREF C_HIST;         // 直方图柱
extern COLORREF C_CH_R;         // 曲线通道色（红/绿/蓝）
extern COLORREF C_CH_G;
extern COLORREF C_CH_B;
// 铺在 C_ACCENT 实心块上的文字/图标色。深色主题的强调色是浅蓝，白字对比度
// 只有 3:1 左右，必须用深色前景；浅色主题强调色近黑，反过来用白字。
extern COLORREF C_ON_ACCENT;
// 滑杆已填充段：深色主题用压过的强调色，浅色主题用中性深灰 ——
// 浅色主题的强调色是近黑，整条滑杆铺黑太重，像进度条。
extern COLORREF C_SLIDER_FILL;

// ---- 圆角（设备像素，已按 DPI 缩放） -------------------------------------
// 只有三档：小控件 / 按钮与分段 / 面板。不要再出现 7、10 这种随手值。
int RadiusSm(); // 6  —— 数值底框、缩略图高亮、提示气泡
int RadiusMd(); // 8  —— 按钮、分段控件、滑杆底槽
int RadiusLg(); // 12 —— 面板、工具栏、画布外框

// ---- 间距（4 的倍数） ----------------------------------------------------
// Sp(1)=4 Sp(2)=8 Sp(3)=12 Sp(4)=16 Sp(5)=24 Sp(6)=32
int Sp(int step);
// 画布底部信息条高度（图像区不会再画到这里，保证状态文字有稳定底色）
int CanvasFooterH();

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
// 强制主题（0 跟随 / 1 浅色 / 2 深色）：只影响本次进程，不写注册表。
// 供无人值守截图核对两个主题，避免为了验收去改用户的设置。
void ForceTheme(int mode);

// 圆角矩形（填充+描边），radius 为设备像素
void RoundBox(HDC dc, const RECT& rc, COLORREF fill, COLORREF border,
              int radius);
// 颜色线性混合：t=0 → a，t=1 → b。GDI 没有 alpha，悬浮/选中态用预混色实现，
// 这样一层"8% 白"叠在任意表面上都不会变成脏灰。
COLORREF MixColor(COLORREF a, COLORREF b, double t);
// 面板内的分组小标题（PS 的"画笔工具选项"那种）：返回下一行的 y。
int DrawSectionLabel(HDC dc, const RECT& box, int y, const std::wstring& text);
// PS 风格的滑杆：track + 圆头 + 已填充段。返回滑杆所占的 rect（供命中用）。
RECT DrawSliderTrack(HDC dc, const RECT& box, int y, double t, bool hot);

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
    HK_COMBO,     // 下拉框本体：index = 参数序号（点 = 展开/收起）
    HK_COMBOITEM, // 下拉项：index = 参数序号，sub = 选项序号
    HK_TOGGLE,    // 开关：index = 参数序号
    HK_CAT,       // 滤镜库分类行：index = 分类序号（-1 = 全部）
    HK_CURVE,     // 曲线控制点：index = 点序号（-1 = 曲线区域空白处）
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
    int sub = -1; // 下拉项序号 / 分类序号
};

// 参数数值框的槽位上限（每个功能窗口各自拥有这么多 EDIT 子控件）。
// 只有滑杆参数占框，下拉/开关不占。曲线效果现在正好用满 8 个 —— 也就是
// 零余量：任何效果再加一个滑杆参数，第 9 个会**静默地没有输入框**。
// SelfTest 里有一条断言盯着这件事（超过就 FAIL，而不是等人眼发现）。
constexpr int kMaxParamEdits = 8;

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
    // 液化窗口放大镜工具：左键点 = 放大、Alt+左键 = 缩小（PS 的缩放工具）
    bool zoomTool = false;

    // 液化涂抹
    bool dragStroke = false;
    POINT strokeLast{};
    // 液化笔刷指示圈的**设备坐标包围盒**（DrawCanvas 每帧记录）。
    // 鼠标悬停时只失效"旧圈 ∪ 新圈"这一小块，不必整幅画布重绘 —— 否则
    // 125Hz 的 mousemove 每次都把整张图 StretchDIBits 一遍，CPU 一直烧着。
    RECT brushBox{};
    bool brushBoxValid = false;
    // 屏幕上**真正画出来**的那个圈（圆心 + 半径，设备坐标）。
    // 残留轨迹的根因就在这里：以前用"上一次的重绘区域"当作"圈上次画在哪"，
    // 但重绘被节流丢掉时，圈其实是在新光标处被裁剪着画出来的（只露出与失效
    // 区域相交的一小段弧），那个位置从来没被记下来，于是永远擦不掉。
    // 现在以"实际绘制的几何"为唯一依据计算失效区域。
    POINT brushPaint{};
    int brushPaintR = 0;
    bool brushPaintValid = false;
    // 下拉框展开状态（参数序号，-1 = 收起）与锚点矩形
    int comboOpen = -1;
    RECT comboAnchor{};
    // 曲线编辑：拖动的控制点序号（-1 = 没在拖）
    int dragCurvePoint = -1;
    // 滤镜库折叠树：已展开的分类名（PS 里分类是可折叠的组，可以同时展开多个）
    std::set<std::string> filterOpen;
    // 默认展开只做一次：收起全部分类后不能又被"自动展开"顶回来
    bool filterOpenInit = false;
    // 上次绘制时左栏的树形内容总高（滚轮/命中用它夹取偏移）
    int filterContentH = 0;
    int filterViewH = 0;
    ULONGLONG hoverTick = 0;  // 悬停重绘节流（~33Hz）
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

    // 参数数值输入框（PS 风格）。8 个是因为滤镜库里有 7 个参数的滤镜
    // （粗糙蜡笔/底纹效果），以前只有 4 个 → 后三个参数根本没有输入框，
    // 只能拖滑杆（用户反馈"只能拖动、不能输入"）。
    // editParam[i] 记录第 i 个框对应哪个参数（下拉/开关参数不占框）。
    HWND hEdits[kMaxParamEdits] = {};
    int editParam[kMaxParamEdits] = {-1, -1, -1, -1, -1, -1, -1, -1};
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
    // 注入因权限级别被拒（SAI2 管理员运行 / 面板未提权）时为 true：
    // 面板据此显示"需要管理员权限"的指引和一键提权重启按钮。
    bool needAdmin = false;

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
    // 每行**图层名文字**的屏幕 X（agent 报的绘制原点）。点行选中要用它算
    // 安全落点：面板最左是眼睛图标 / 缩略图，按面板左边距写死偏移会点到
    // 「显示/隐藏」上（用户反馈"选中图层容易自动点到隐藏按钮"）。
    std::vector<int> layerScreenX;
    std::vector<std::wstring> layerAttrs;
    long long panelLeft = 0;
    long long panelRight = 0;
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
    // 数值输入框底刷（EDIT 是子窗口，WM_CTLCOLOREDIT 必须返回一个真刷子；
    // 以前用函数内 static 缓存，切主题后会一直用旧颜色 —— 这里跟主题一起重建）
    HBRUSH editBrush = nullptr;

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
// DPI 缩放。SAI2_UI_SCALE / ui::SetUiScale 可强制倍率（测试 150% 布局用，
// 必须在建第一个窗口/字体之前调用）。
void SetUiScale(double mult);
int Scale(int v);
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
// 滤镜库缩略图墙的滚轮滚动（UiWindow 的 WndProc 在预览缩放之前先问它，
// 否则窗口自己的 Msg 收不到滚轮 → 左栏不能下拉）
bool FilterWheelScroll(UiWindow& win, WPARAM wp, LPARAM lp);

// 画一段参数面板（标签 + 控件），返回下一行的 y。
// Slider：标签 + 数值框 + 滑杆；Choice：标签 + 下拉框；Toggle：标签 + 开关。
// 数值框由 SyncParamEdits 负责摆放（下拉/开关不用子窗口）。
int DrawParamRow(HDC dc, UiWindow& win, const RECT& box, int y,
                 const UiParam& p, float value, int paramIndex);

// 下拉框展开后的浮层：**必须在窗口绘制最后调用**（否则会被后面的面板盖住）。
// 点击落在 HK_COMBOITEM 上时由 HandleComboClick 改值并收起。
void DrawComboPopup(HDC dc, UiWindow& win, const RECT& panel);
bool HandleComboClick(UiWindow& win, const UiHit& h); // 返回 true = 已处理
void CloseCombo(UiWindow& win);
// 参数值改成 val（含显示倍率换算/夹紧），并刷新预览
void SetParamDisplayValue(UiWindow& win, int index, float displayVal);

// 数值输入框：创建（窗口 WM_CREATE 时调用一次）、跟随布局、回写
void CreateParamEdits(UiWindow& win);
void SyncParamEdits(UiWindow& win, const std::vector<int>& rowY,
                    const RECT& box);
// forceSlot >= 0：强制刷新这一格（刚提交完的框，即使它正持有焦点也要
// 写成钳制后的真实值，否则框里会留着越界的原样输入）
void UpdateParamEdits(UiWindow& win, int forceSlot = -1);
// 解析数值框文本（"30%" "-12.5°" "abc" "-" "1.2.3" ...）。
// 返回 false = 非法，调用方应回滚显示、不改参数。纯函数，自检直接测它。
bool ParseParamText(const wchar_t* text, float* out);
void CommitParamEdit(UiWindow& win, int index);
// 数值框的"提交/取消/微调"（Enter / Esc / 上下键 / 滚轮）。
// 这些键在 EDIT 自己的窗口过程里就被吃掉，窗口过程收不到 —— 见 ParamEditProc。
void NudgeParamEdit(UiWindow& win, int index, int dir, float mult);

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
// showEffects = false 时不画"组内效果条"（液化窗口：工具改到左侧图标列，
// 顶部那排文字按钮就是用户要求去掉的"文字选项"）。
void DrawWindowChrome(HDC dc, UiWindow& win, const UiLayout& L,
                      const std::wstring& title, const std::wstring& subtitle,
                      const std::vector<int>& effects,
                      bool showEffects = true);

// 参数面板背景 + 标题（sub = 标题行右端的小字，hint = 底部脚注说明）
void DrawPanelFrame(HDC dc, const UiLayout& L, const std::wstring& title,
                    const std::wstring& hint,
                    const std::wstring& sub = std::wstring());

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
    // 每行图层名文字的屏幕 X（眼睛图标与缩略图的右侧）。点行选中用它当落点。
    std::vector<int> screenX;
};
// 把注入代理捕获的文字行聚合为图层条目（名字 + 混合模式/不透明度 + 屏幕 y/x）
LayerAgg AggregateLayers(const std::vector<sai2bridge::AgentLayerRow>& rows);
// 图层面板所在的那个捕获分组
const sai2bridge::AgentLayerGroup* PanelGroup();

// 点图层行的**逻辑 X**（agent 坐标系，还没做 DPI 换算）。
//
// 为什么不是"面板左边距 + 固定值"：SAI2 图层面板每行最左边是「显示/隐藏」
// 的眼睛图标，再往右才是缩略图和图层名。写死偏移一旦落进眼睛图标，
// 点一下就**把图层藏了** —— 用户反馈的"选中图层容易自动点到隐藏按钮"。
// textX = agent 报的该行图层名文字起点（0 = 没有坐标）。
int LayerRowClickX(long long panelLeft, long long panelRight, int textX);

// 刷新图层列表。budgetMs：agent 管道连接重试预算（毫秒），0 = 按当前
// 已知状态自动选择（在线 3000ms 兜住服务端重建管道名的窗口 / 离线 200ms
// 快速失败）。**离线时绝不能再拿 3s 预算**：定时器每 2.5s 调一次本函数，
// UI 线程会被 Connect 堵满 3 秒 → 面板"切过去就卡死"（用户实录）。
void RefreshLayers(DWORD budgetMs = 0);
std::vector<std::wstring> LayerNames();
void DoInjectAsync(HWND hwnd);
void DoSelectLayer(int rowIdx);
// 以管理员身份重启本面板（UAC 提权）。旧实例退出、新实例带 --takeover 接管。
void RelaunchElevated();

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
