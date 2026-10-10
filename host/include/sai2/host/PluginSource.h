#pragma once
// ============================================================================
// PluginSource.h - 在线插件源（Phase 6，规格书第十五节）
//
// - index.json：公开插件目录（id/name/version/download/description/icon/
//   minApiVersion/sha256）
// - Search：按关键字（id/name/description，大小写不敏感子串）检索
// - InstallOrUpdate：下载 -> SHA-256 校验 -> minApiVersion 检查 ->
//   版本比较（已安装且不比源新则更新）-> PluginInstaller 安装
// - 下载通过 IDownloader 抽象：
//     * HttpDownloader：生产用（WinHTTP）
//     * FileDownloader：测试/离线（本地路径或 file:// 复制）
// ============================================================================

#include "sai2/host/PluginRegistry.h"

#include <string>
#include <vector>

namespace sai2::host {

struct SourceEntry {
    std::string id;
    std::string name;
    std::string version;
    std::string download;      // URL 或本地路径
    std::string description;
    std::string icon;
    std::string sha256;        // 安装包 SHA-256（小写 hex，可空）
    std::string minApiVersion; // 最低插件 API 版本，如 "1.0"

    // ---- 插件签名（第十六节，可选） ----
    // signature 为对 "SAI2-SIGN1|<id>|<version>|<包sha256>" 的
    // RSA PKCS#1 v1.5 签名 hex；signerPubKey 为发布者公钥。
    // 两者同时存在时下载安装流程强制验签。
    std::string signature;
    std::string signerPubKey;
};

// ---------------------------------------------------------------------------
// 下载器抽象（插件默认无网络权限；网络访问仅发生在显式安装/更新时，
// 规格书第十六节）
// ---------------------------------------------------------------------------
class IDownloader {
public:
    virtual ~IDownloader() = default;
    virtual bool Download(const std::string& url, const std::string& destPath,
                          std::string& error) = 0;
};

// 测试/离线实现：支持本地路径与 file:// 前缀，直接复制
class FileDownloader final : public IDownloader {
public:
    bool Download(const std::string& url, const std::string& destPath,
                  std::string& error) override;
};

// 生产实现：WinHTTP 下载 http/https 资源（实现见 net/HttpDownloader.cpp，
// 需要链接 winhttp）
class HttpDownloader final : public IDownloader {
public:
    bool Download(const std::string& url, const std::string& destPath,
                  std::string& error) override;
};

// ---------------------------------------------------------------------------
// PluginSource
// ---------------------------------------------------------------------------
class PluginSource {
public:
    PluginSource(std::string pluginsRoot, PluginRegistry& registry,
                 uint32_t hostApiVersion);

    // 读取并解析 index.json 文件
    bool LoadIndex(const std::string& indexPath, std::string& error);
    // 直接解析 index JSON 文本（便于测试）
    bool LoadIndexFromJson(const std::string& jsonText, std::string& error);

    const std::vector<SourceEntry>& Entries() const { return entries_; }

    // 关键字搜索（匹配 id / name / description，大小写不敏感）
    std::vector<SourceEntry> Search(const std::string& keyword) const;
    const SourceEntry* Find(const std::string& id) const;

    // 更新检查：对已安装插件返回有新版本的源条目
    std::vector<SourceEntry> CheckUpdates() const;

    // 安装 / 更新一个条目：
    //   1. minApiVersion <= 宿主 API 版本
    //   2. 下载到 cacheDir/<id>.sai-plugin
    //   3. sha256 校验（entry.sha256 非空时）
    //   4. 若已安装：源版本必须 > 已装版本才执行更新
    //   5. PluginInstaller::InstallFromPackage 安装并注册
    bool InstallOrUpdate(const SourceEntry& entry, IDownloader& downloader,
                         const std::string& cacheDir, std::string& error);

private:
    std::string pluginsRoot_;
    PluginRegistry& registry_;
    uint32_t hostApiVersion_;
    std::vector<SourceEntry> entries_;
};

} // namespace sai2::host
