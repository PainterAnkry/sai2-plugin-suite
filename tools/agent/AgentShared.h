#pragma once
// ============================================================================
// AgentShared.h - 注入式 Agent 的共享定义
//
// 背景：SAI2 没有任何官方插件接口（见 docs/SAI2Integration.md），要拿到
// "图层面板里有什么"只能进到 SAI2 进程里看它自己怎么画。
//
// 本 Agent 的思路（相对硬编码内存偏移的内存解析更抗版本变化）：
//   SAI2 导入了 ExtTextOutW / DrawTextW（GDI 文字绘制）与
//   SetDIBitsToDevice / BitBlt（位块传输）。图层面板里的图层名就是
//   用 ExtTextOutW 画出来的，缩略图就是小尺寸位块传输。
//   Agent 修改 sai2.exe 的 IAT（导入地址表），在这些函数被调用时
//   把"画了什么字、画在哪（含屏幕坐标）、画了什么小图"记录下来，
//   重建出图层行；宿主进程外的工具通过命名管道查询。
//
// 安全性：只读观察（不改参数、不改返回值、不改内存数据）；
//   每个 Hook 体都有 SEH 兜底，异常时直接走原函数，宁可丢数据不弄崩 SAI2。
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <windows.h>

#include <string>
#include <vector>

namespace sai2agent {

// 默认命名管道名（可用环境变量 SAI2_AGENT_PIPE 覆盖）
inline const wchar_t* kDefaultPipeName = L"\\\\.\\pipe\\sai2agent";

// 捕获的函数种类
enum FnId {
    FN_EXTTXTOUT = 0,  // gdi32!ExtTextOutW
    FN_DRAWTEXT  = 1,  // user32!DrawTextW
    FN_SETDIBITS = 2,  // gdi32!SetDIBitsToDevice（缩略图/位块）
    FN_BITBLT    = 3,  // gdi32!BitBlt（缩略图/位块）
};

// 一条文字捕获记录
struct TextCapture {
    ULONGLONG tick = 0;       // GetTickCount64
    int fn = FN_EXTTXTOUT;
    HWND hwnd = nullptr;      // 绘制目标窗口（WindowFromDC；内存 DC 为 null）
    wchar_t cls[96] = {};     // 该窗口的类名
    int x = 0, y = 0;         // 文字参考点（DC 逻辑/设备坐标）
    int w = 0, h = 0;         // 文字尺寸估算（像素）
    bool hasScreen = false;   // screenX/screenY 是否有效（屏幕 DC 才有效）
    int screenX = 0, screenY = 0; // 参考点对应的屏幕坐标（GetDCOrgEx 推得）
    UINT eto = 0;             // ExtTextOut 的选项（诊断用，如 ETO_GLYPH_INDEX）
    std::wstring text;
};

// 一条位块捕获记录（缩略图候选）
struct BlitCapture {
    ULONGLONG tick = 0;
    int fn = FN_SETDIBITS;
    HWND hwnd = nullptr;
    wchar_t cls[96] = {};
    bool hasScreen = false;       // screenX/screenY 是否有效
    int screenX = 0, screenY = 0; // 目标矩形左上角的屏幕坐标
    int w = 0, h = 0;
    int srcX = 0, srcY = 0;       // 源矩形（内存表面坐标，用于文字映射）
    bool captured = false;        // 是否已把像素存成 BMP
    char file[MAX_PATH] = {};     // 存放路径（captured 时有效）
};

// 图层面板重建出的"行"（同一窗口里按纵向位置聚合的文字）
struct RowInfo {
    int y = 0;                  // 行文字参考点（客户区坐标）
    int x = 0;
    int w = 0;
    bool hasScreen = false;
    int screenX = 0, screenY = 0;
    ULONGLONG tick = 0;         // 最近一次绘制时间
    int count = 0;              // 被绘制过的次数
    std::wstring text;
};

// 按窗口聚合的行集合
struct GroupInfo {
    HWND hwnd = nullptr;
    wchar_t cls[96] = {};
    RECT screenRect = {};       // 查询时的窗口屏幕矩形
    bool isChild = false;
    std::vector<RowInfo> rows;  // 按 y 升序
};

// 位块捕获经 BitBlt/EnumChildWindows 刷新后的窗口矩形等由查询端补齐，
// Agent 只负责如实上报捕获内容。

} // namespace sai2agent
