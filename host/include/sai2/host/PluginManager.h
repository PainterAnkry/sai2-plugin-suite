#pragma once
// ============================================================================
// PluginManager.h - 插件管理器（Phase 1 核心）
//
// 职责：
//   - 启动时扫描注册表并加载所有启用插件
//   - 启用/禁用（禁用 = Shutdown + Unload，保留安装）
//   - 执行插件（异常隔离：插件崩溃 != SAI2 崩溃，自动禁用 + crash.log）
//   - 卸载（调 Installer 删除文件）
// ============================================================================

#include "sai2/host/PluginInstaller.h"
#include "sai2/host/PluginLoader.h"
#include "sai2/host/PluginRegistry.h"
#include "sai2/IHostContext.h"

#include <memory>
#include <string>
#include <vector>

namespace sai2::host {

struct PluginListItem {
    std::string id;
    std::string name;
    std::string version;
    std::string type;
    bool installed = false; // 已安装（注册表内）
    bool loaded = false;    // 已加载运行
    bool enabled = true;
};

class PluginManager {
public:
    // hostContext: 由宿主（或测试/Mock 宿主）提供的 IHostContext 实现
    PluginManager(std::string pluginsRoot, std::string logDir,
                  uint32_t hostApiVersion, IHostContext* hostContext);
    ~PluginManager();

    // 初始化日志、扫描注册表、加载所有 enabled 插件
    bool Startup(std::string& error);
    // 停止并卸载全部插件
    void Shutdown();

    std::vector<PluginListItem> ListPlugins() const;

    bool EnablePlugin(const std::string& id, std::string& error);
    bool DisablePlugin(const std::string& id, std::string& error);

    // 执行一次插件操作；内部 catch 所有异常，失败自动禁用并记录 crash.log
    bool ExecutePlugin(const std::string& id, std::string& error);

    // 安装（转发给 Installer），成功后刷新注册表
    bool InstallFromPackage(const std::string& packagePath, std::string& error);
    bool InstallFromDirectory(const std::string& dir, std::string& error);

    // 卸载：先停止再删文件
    bool UninstallPlugin(const std::string& id, std::string& error);

    PluginRegistry& GetRegistry() { return registry_; }

private:
    struct Instance {
        PluginRecord record;
        std::unique_ptr<PluginLoader::Loaded> loaded;
        bool running = false;
    };

    Instance* FindInstance(const std::string& id);
    bool StartInstance(Instance& inst, std::string& error);
    void StopInstance(Instance& inst);
    void DisableAndLogCrash(const std::string& id, const std::string& reason);

    std::string pluginsRoot_;
    std::string logDir_;
    uint32_t hostApiVersion_;
    IHostContext* host_ = nullptr;

    PluginRegistry registry_;
    PluginInstaller installer_;
    PluginLoader loader_;
    std::vector<Instance> instances_;
    bool started_ = false;
};

} // namespace sai2::host
