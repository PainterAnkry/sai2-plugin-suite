#include "sai2/host/PluginManager.h"
#include "sai2/host/Logging.h"
#include "sai2/host/Manifest.h"

namespace sai2::host {

PluginManager::PluginManager(std::string pluginsRoot, std::string logDir,
                             uint32_t hostApiVersion, IHostContext* hostContext)
    : pluginsRoot_(std::move(pluginsRoot)),
      logDir_(std::move(logDir)),
      hostApiVersion_(hostApiVersion),
      host_(hostContext),
      registry_(pluginsRoot_),
      installer_(pluginsRoot_, registry_, hostApiVersion) {}

PluginManager::~PluginManager() { Shutdown(); }

bool PluginManager::Startup(std::string& error) {
    if (started_) return true;
    // logDir_ 是 UTF-8，可能含非 ASCII 字符 → 必须转宽字符（PathUtil.h）
    Logger::Instance().Init(PathFromUtf8(logDir_));
    Logger::Instance().Write(LogLevel::Info, "PluginManager 启动");

    registry_.Scan();
    registry_.LoadState();

    for (auto& record : registry_.All()) {
        // 不跳过 enabled=false 的插件：禁用标记只来自"上次启动失败"的自动
        // 隔离（UI 没有手动启用入口）。若在这里永久跳过，首次失败（典型：
        // 杀毒软件锁定/拦截了新解压的 DLL）之后插件永远不会恢复 —— 用户
        // 现象就是"抓取正常但液化/滤镜全部不可用"。改为每次启动都重试，
        // 失败才再次禁用；环境恢复后（如杀软放行）重启即自愈。
        Instance inst;
        inst.record = record;
        if (!StartInstance(inst, error)) {
            Logger::Instance().Write(LogLevel::Error,
                                     "插件启动失败 " + record.manifest.id +
                                         ": " + error);
            // 启动失败的插件标记为禁用（隔离故障）
            registry_.SetEnabled(record.manifest.id, false);
            registry_.SaveState();
            continue;
        }
        if (!record.enabled) {
            // 上次失败、本次成功 → 清除禁用标记
            Logger::Instance().Write(LogLevel::Info,
                                     "插件恢复加载 " + record.manifest.id);
            registry_.SetEnabled(record.manifest.id, true);
            registry_.SaveState();
        }
        instances_.push_back(std::move(inst));
    }
    started_ = true;
    return true;
}

void PluginManager::Shutdown() {
    if (!started_) return;
    for (auto& inst : instances_) StopInstance(inst);
    instances_.clear();
    registry_.SaveState();
    started_ = false;
    Logger::Instance().Write(LogLevel::Info, "PluginManager 停止");
}

std::vector<PluginListItem> PluginManager::ListPlugins() const {
    std::vector<PluginListItem> items;
    for (const auto& record : registry_.All()) {
        PluginListItem item;
        item.id = record.manifest.id;
        item.name = record.manifest.name;
        item.version = record.manifest.version;
        item.type = record.manifest.type;
        item.installed = true;
        item.enabled = record.enabled;
        for (const auto& inst : instances_) {
            if (inst.record.manifest.id == record.manifest.id) {
                item.loaded = inst.running;
                break;
            }
        }
        items.push_back(std::move(item));
    }
    return items;
}

PluginManager::Instance* PluginManager::FindInstance(const std::string& id) {
    for (auto& inst : instances_)
        if (inst.record.manifest.id == id) return &inst;
    return nullptr;
}

bool PluginManager::StartInstance(Instance& inst, std::string& error) {
    if (inst.running) return true;

    auto loaded = std::make_unique<PluginLoader::Loaded>();
    if (!loader_.Load(inst.record, *loaded, error)) return false;

    // Initialize 必须隔离异常：插件崩溃 != 宿主崩溃
    try {
        if (!loaded->plugin->Initialize(host_)) {
            error = "插件 Initialize 返回 false";
            loader_.Unload(*loaded);
            return false;
        }
    } catch (const std::exception& e) {
        error = std::string("插件 Initialize 抛出异常: ") + e.what();
        DisableAndLogCrash(inst.record.manifest.id, error);
        loader_.Unload(*loaded);
        return false;
    } catch (...) {
        error = "插件 Initialize 抛出未知异常";
        DisableAndLogCrash(inst.record.manifest.id, error);
        loader_.Unload(*loaded);
        return false;
    }

    inst.loaded = std::move(loaded);
    inst.running = true;
    return true;
}

void PluginManager::StopInstance(Instance& inst) {
    if (!inst.running) return;
    if (inst.loaded && inst.loaded->plugin) {
        try {
            inst.loaded->plugin->Shutdown();
        } catch (const std::exception& e) {
            Logger::Instance().WriteCrash(
                "插件 Shutdown 异常 " + inst.record.manifest.id + ": " + e.what());
        } catch (...) {
            Logger::Instance().WriteCrash(
                "插件 Shutdown 未知异常 " + inst.record.manifest.id);
        }
    }
    if (inst.loaded) loader_.Unload(*inst.loaded);
    inst.loaded.reset();
    inst.running = false;
}

void PluginManager::DisableAndLogCrash(const std::string& id,
                                       const std::string& reason) {
    Logger::Instance().WriteCrash("插件异常已隔离: " + id + " - " + reason);
    registry_.SetEnabled(id, false);
    registry_.SaveState();
}

bool PluginManager::EnablePlugin(const std::string& id, std::string& error) {
    PluginRecord* record = registry_.Find(id);
    if (!record) {
        error = "插件未安装: " + id;
        return false;
    }
    Instance* inst = FindInstance(id);
    if (inst && inst->running) {
        // 已在运行
        registry_.SetEnabled(id, true);
        registry_.SaveState();
        return true;
    }
    if (inst) {
        // 实例存在但已停止（之前被禁用）：重新启动
        if (!StartInstance(*inst, error)) return false;
    } else {
        Instance fresh;
        fresh.record = *record;
        if (!StartInstance(fresh, error)) return false;
        instances_.push_back(std::move(fresh));
    }
    registry_.SetEnabled(id, true);
    registry_.SaveState();
    return true;
}

bool PluginManager::DisablePlugin(const std::string& id, std::string& error) {
    Instance* inst = FindInstance(id);
    if (inst) StopInstance(*inst);
    if (!registry_.SetEnabled(id, false)) {
        error = "插件未安装: " + id;
        return false;
    }
    registry_.SaveState();
    return true;
}

bool PluginManager::ExecutePlugin(const std::string& id, std::string& error) {
    Instance* inst = FindInstance(id);
    if (!inst || !inst->running) {
        error = "插件未加载: " + id;
        return false;
    }
    try {
        if (!inst->loaded->plugin->Execute()) {
            error = "插件 Execute 返回 false";
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        error = std::string("插件 Execute 异常: ") + e.what();
        StopInstance(*inst);
        DisableAndLogCrash(id, error);
        return false;
    } catch (...) {
        error = "插件 Execute 未知异常";
        StopInstance(*inst);
        DisableAndLogCrash(id, error);
        return false;
    }
}

bool PluginManager::InstallFromPackage(const std::string& packagePath,
                                       std::string& error) {
    if (!installer_.InstallFromPackage(packagePath, error)) return false;
    registry_.Scan();
    return true;
}

bool PluginManager::InstallFromDirectory(const std::string& dir,
                                         std::string& error) {
    if (!installer_.InstallFromDirectory(dir, error)) return false;
    registry_.Scan();
    return true;
}

bool PluginManager::UninstallPlugin(const std::string& id, std::string& error) {
    if (Instance* inst = FindInstance(id)) {
        StopInstance(*inst);
        for (auto it = instances_.begin(); it != instances_.end(); ++it) {
            if (&(*it) == inst) {
                instances_.erase(it);
                break;
            }
        }
    }
    return installer_.Uninstall(id, error);
}

} // namespace sai2::host
