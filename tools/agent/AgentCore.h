#pragma once
// ============================================================================
// AgentCore.h - Agent 的事件记录、图层行重建与命令处理
// ============================================================================

#include "AgentShared.h"

#include <string>
#include <vector>

namespace sai2agent {

class AgentCore {
public:
    // 初始化：安装 IAT 钩子并开始记录。outDir 为日志/缩略图目录（空 = DLL 目录）
    bool Startup(const std::wstring& outDir, std::wstring& error);
    // 卸载全部钩子并释放（DllMain DETACH 时调用）
    void Shutdown();

    // 管道命令分发（输入一行 JSON，输出一行 JSON；无对象返回值便于 SEH 包装）
    void HandleCommand(const std::string& line, std::string& out);

    const std::wstring& LogPath() const { return logPath_; }

    // 退出标志：DllMain 写、工作线程读
    void RequestExit() { exitFlag_ = true; }
    bool ExitRequested() const { return exitFlag_; }

    // Hook 实现需要直接访问记录器
    struct Impl;
    Impl* ImplPtr() const { return impl_; }

    // ---- 插件入口（SAI2 内浮动按钮条） ------------------------------
    // 每个条目 = 一个"功能"（模糊/滤镜/调整/液化），点击后桥打开对应功能窗口
    struct PluginItem {
        int index = 0;          // 功能窗口索引（1..4，由桥定义）
        std::wstring group;     // 分组名（= 功能名）
        std::wstring name;      // 显示名
    };
    // 桥推送功能清单（覆盖旧值）
    void SetPluginItems(const std::vector<PluginItem>& items);
    // 工具栏线程读取（用于绘制按钮条）
    std::vector<PluginItem> CopyPluginItems();
    // 用户在 SAI2 里点了某功能 → 记录待处理请求
    void SetPendingRequest(int winIdx);
    // 桥轮询取走待处理请求（无则返回 false）
    bool TakePendingRequest(int& winIdx);

    // 启动 SAI2 内浮动「插件」入口（独立 UI 线程；SAI2_AGENT_NOBAR=1 关闭）
    void StartToolbar();
    // 卸载前等待工具栏线程退出（FreeLibrary 安全的前提）
    void StopToolbarAndWait();
    // 工具栏线程写日志用
    void LogText(const char* line);

private:
    Impl* impl_ = nullptr;
    volatile bool exitFlag_ = false;
    std::wstring logPath_;
};

} // namespace sai2agent
