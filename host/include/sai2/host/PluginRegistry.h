#pragma once
// ============================================================================
// PluginRegistry.h - 已安装插件注册表
//
// - 扫描 plugins 根目录下每个子目录的 manifest.json
// - enabled 状态持久化到 plugins/registry.json
// ============================================================================

#include "sai2/host/Manifest.h"

#include <map>
#include <string>
#include <vector>

namespace sai2::host {

struct PluginRecord {
    PluginManifest manifest;
    bool enabled = true;
    std::string directory; // 插件目录绝对/相对路径
};

class PluginRegistry {
public:
    explicit PluginRegistry(std::string pluginsRoot);

    const std::string& PluginsRoot() const { return pluginsRoot_; }

    // 扫描磁盘上的插件目录，并合并 registry.json 中的启用状态
    void Scan();

    const std::vector<PluginRecord>& All() const { return records_; }
    PluginRecord* Find(const std::string& id);

    // 新安装后登记（不立即写盘，调用 SaveState）
    void Add(PluginRecord record);
    // 卸载后移除
    void Remove(const std::string& id);

    bool SetEnabled(const std::string& id, bool enabled);

    // 状态持久化：plugins/registry.json
    void SaveState();
    void LoadState();

private:
    std::string pluginsRoot_;
    std::string StatePath() const;
    std::vector<PluginRecord> records_;
};

} // namespace sai2::host
