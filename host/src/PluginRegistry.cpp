#include "sai2/host/PluginRegistry.h"
#include "sai2/host/Json.h"
#include "sai2/host/Logging.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace sai2::host {

PluginRegistry::PluginRegistry(std::string pluginsRoot)
    : pluginsRoot_(std::move(pluginsRoot)) {}

std::string PluginRegistry::StatePath() const {
    // 返回 UTF-8 字符串（日志/持久化用）；真正访问文件系统时再转宽字符
    return PathToUtf8(PathFromUtf8(pluginsRoot_) /
                      std::filesystem::path(L"registry.json"));
}

void PluginRegistry::Scan() {
    records_.clear();
    // 插件根目录可能是 UTF-8 非 ASCII 路径（例如「SAI2插件测试包」），
    // 必须先转宽字符再交给 filesystem，否则在中文 Windows 上直接扫不到
    const fs::path root = PathFromUtf8(pluginsRoot_);
    std::error_code ec;
    if (!fs::exists(root, ec)) {
        Logger::Instance().Write(
            LogLevel::Warning,
            "插件目录不存在: " + pluginsRoot_);
        return;
    }

    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (!entry.is_directory()) continue;
        const fs::path manifestPath = entry.path() / L"manifest.json";
        if (!fs::exists(manifestPath)) continue;

        PluginRecord record;
        std::string error;
        if (!LoadManifestFile(manifestPath, record.manifest, error)) {
            Logger::Instance().Write(
                LogLevel::Warning,
                "跳过无效插件目录 " + PathToUtf8(entry.path()) + ": " + error);
            continue;
        }
        record.directory = PathToUtf8(entry.path());

        // 合并已持久化的启用状态
        if (const auto* prev = Find(record.manifest.id))
            record.enabled = prev->enabled;

        records_.push_back(std::move(record));
    }
}

PluginRecord* PluginRegistry::Find(const std::string& id) {
    for (auto& r : records_)
        if (r.manifest.id == id) return &r;
    return nullptr;
}

void PluginRegistry::Add(PluginRecord record) {
    Remove(record.manifest.id);
    records_.push_back(std::move(record));
}

void PluginRegistry::Remove(const std::string& id) {
    for (auto it = records_.begin(); it != records_.end(); ++it) {
        if (it->manifest.id == id) {
            records_.erase(it);
            return;
        }
    }
}

bool PluginRegistry::SetEnabled(const std::string& id, bool enabled) {
    PluginRecord* r = Find(id);
    if (!r) return false;
    r->enabled = enabled;
    return true;
}

void PluginRegistry::SaveState() {
    std::ostringstream os;
    os << "{\n  \"plugins\": {\n";
    bool first = true;
    for (const auto& r : records_) {
        if (!first) os << ",\n";
        first = false;
        os << "    " << JsonEscape(r.manifest.id) << ": {\"enabled\": "
           << (r.enabled ? "true" : "false") << ", \"version\": "
           << JsonEscape(r.manifest.version) << "}";
    }
    os << "\n  }\n}\n";
    std::error_code ec;
    const fs::path root = PathFromUtf8(pluginsRoot_);
    fs::create_directories(root, ec);
    std::ofstream f(root / L"registry.json",
                    std::ios::binary | std::ios::trunc);
    if (f) f << os.str();
}

void PluginRegistry::LoadState() {
    const fs::path path =
        PathFromUtf8(pluginsRoot_) / std::filesystem::path(L"registry.json");
    std::ifstream f(path, std::ios::binary);
    if (!f) return;
    std::ostringstream ss;
    ss << f.rdbuf();

    JsonValue root;
    std::string error;
    if (!JsonParser::Parse(ss.str(), root, error)) {
        Logger::Instance().Write(LogLevel::Warning,
                                 "registry.json 解析失败: " + error);
        return;
    }
    const JsonValue* plugins = root.Find("plugins");
    if (!plugins || !plugins->IsObject()) return;
    for (const auto& [id, val] : plugins->object) {
        if (PluginRecord* r = Find(id)) r->enabled = val.Find("enabled") ? val.Find("enabled")->AsBool(true) : true;
    }
}

} // namespace sai2::host
