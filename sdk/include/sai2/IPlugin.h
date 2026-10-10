#pragma once
// ============================================================================
// IPlugin.h - 每个插件必须实现的核心接口
//
// 插件生命周期：Load -> Initialize(host) -> [Execute / CreateUI]* -> Shutdown -> Unload
//
// 规则（见规格书第四节）：
//   - 插件不得直接修改 SAI2 内部对象，一切通过 IHostContext 操作；
//   - 一次完整的用户操作（Execute）应当成一个 UndoGroup；
//   - 插件内部抛出的异常不允许逃逸出导出函数边界。
// ============================================================================

#include "sai2/PluginAPI.h"

#include <new>

namespace sai2 {

class IHostContext;

struct PluginInfo {
    const char* id          = nullptr; // 如 "com.sai2.helloworld"
    const char* name        = nullptr; // 显示名
    const char* version     = nullptr; // "1.0.0"
    const char* author      = nullptr;
    const char* type        = nullptr; // "image-effect" / "tool" / ...
    const char* description = nullptr;
};

class IPlugin {
public:
    virtual ~IPlugin() = default;

    // 静态插件信息（宿主在加载前后均可读取）
    virtual PluginInfo GetInfo() const = 0;

    // 宿主在加载后调用一次；返回 false 视为初始化失败，宿主将卸载并禁用插件
    virtual bool Initialize(IHostContext* host) = 0;

    // 卸载前调用一次；之后宿主 FreeLibrary
    virtual void Shutdown() = 0;

    // 执行一次完整的插件操作（内部应使用 Host 的 UndoGroup / SnapshotForUndo）
    virtual bool Execute() = 0;

    // 可选：创建插件原生 UI，返回窗口句柄（Win32 HWND / Qt widget 指针）
    virtual void* CreateUI(void* parentWindow) {
        (void)parentWindow;
        return nullptr;
    }
};

} // namespace sai2

// ============================================================================
// 插件实现导出宏：在插件 cpp 文件末尾调用
//   SAI2_DEFINE_PLUGIN(MyPlugin)
// ============================================================================
#define SAI2_DEFINE_PLUGIN(PluginClass)                                        \
    SAI2_PLUGIN_EXPORT sai2::IPlugin* sai2_CreatePlugin() {                    \
        return new (std::nothrow) PluginClass();                               \
    }                                                                          \
    SAI2_PLUGIN_EXPORT uint32_t sai2_GetPluginAPIVersion() {                   \
        return sai2::kPluginApiVersion;                                        \
    }
