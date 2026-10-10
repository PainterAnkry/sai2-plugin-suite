#pragma once
// ============================================================================
// PluginLoader.h - 动态库加载器（Phase 1）
//
// Windows: LoadLibraryW + GetProcAddress("sai2_CreatePlugin")
// 校验插件导出的 sai2_GetPluginAPIVersion 与宿主 API 版本兼容。
// ============================================================================

#include "sai2/host/PluginRegistry.h"
#include "sai2/IPlugin.h"

#include <string>

namespace sai2::host {

class PluginLoader {
public:
    struct Loaded {
        std::string id;
        void* handle = nullptr;   // HMODULE / dlopen 句柄
        IPlugin* plugin = nullptr;
    };

    // 加载一个插件；失败时 error 给出原因
    bool Load(const PluginRecord& record, Loaded& out, std::string& error);
    // 卸载（内部先 Shutdown 由 PluginManager 负责）
    void Unload(Loaded& loaded);
};

} // namespace sai2::host
