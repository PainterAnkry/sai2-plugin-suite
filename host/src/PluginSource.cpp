// ============================================================================
// PluginSource.cpp - 在线插件源实现（Phase 6）
// ============================================================================

#include "sai2/host/PluginSource.h"
#include "sai2/host/PathUtil.h"

#include "sai2/host/Json.h"
#include "sai2/host/Crypto.h"
#include "sai2/host/Manifest.h"
#include "sai2/host/PluginInstaller.h"
#include "sai2/host/PluginSecurity.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace sai2::host {

namespace {

std::string ToLower(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool ReadTextFile(const std::string& path, std::string& out,
                  std::string& error) {
    // path 是 UTF-8：转宽字符再开流
    std::ifstream f(PathFromUtf8(path), std::ios::binary);
    if (!f) {
        error = "无法打开文件: " + path;
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// FileDownloader
// ---------------------------------------------------------------------------
bool FileDownloader::Download(const std::string& url,
                              const std::string& destPath,
                              std::string& error) {
    std::string src = url;
    if (src.rfind("file://", 0) == 0) src = src.substr(7);
    std::string text;
    if (!ReadTextFile(src, text, error)) return false;
    std::ofstream out(PathFromUtf8(destPath), std::ios::binary);
    if (!out) {
        error = "无法写入下载目标: " + destPath;
        return false;
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return true;
}

// ---------------------------------------------------------------------------
// PluginSource
// ---------------------------------------------------------------------------
PluginSource::PluginSource(std::string pluginsRoot, PluginRegistry& registry,
                           uint32_t hostApiVersion)
    : pluginsRoot_(std::move(pluginsRoot)), registry_(registry),
      hostApiVersion_(hostApiVersion) {}

bool PluginSource::LoadIndex(const std::string& indexPath,
                             std::string& error) {
    std::string text;
    if (!ReadTextFile(indexPath, text, error)) return false;
    return LoadIndexFromJson(text, error);
}

bool PluginSource::LoadIndexFromJson(const std::string& jsonText,
                                     std::string& error) {
    JsonValue root;
    if (!JsonParser::Parse(jsonText, root, error) || !root.IsObject()) {
        error = "index.json 解析失败: " + error;
        return false;
    }
    const JsonValue* arr = root.Find("plugins");
    if (!arr || !arr->IsArray()) {
        error = "index.json 缺少 plugins 数组";
        return false;
    }
    entries_.clear();
    size_t valid = 0;
    for (const JsonValue& item : arr->array) {
        if (!item.IsObject()) continue;
        SourceEntry e;
        e.id            = item.Find("id") ? item.Find("id")->AsString() : "";
        e.name          = item.Find("name") ? item.Find("name")->AsString() : "";
        e.version       = item.Find("version") ? item.Find("version")->AsString() : "";
        e.download      = item.Find("download") ? item.Find("download")->AsString() : "";
        e.description   = item.Find("description") ? item.Find("description")->AsString() : "";
        e.icon          = item.Find("icon") ? item.Find("icon")->AsString() : "";
        e.sha256        = item.Find("sha256") ? item.Find("sha256")->AsString() : "";
        e.minApiVersion = item.Find("minApiVersion") ? item.Find("minApiVersion")->AsString() : "";
        e.signature     = item.Find("signature") ? item.Find("signature")->AsString() : "";
        e.signerPubKey  = item.Find("signerPubKey") ? item.Find("signerPubKey")->AsString() : "";

        // 基本校验：id / version / download 必填，版本可解析；
        // 无效条目跳过（索引来自网络，单个坏条目不应影响整个源）
        Version v;
        if (e.id.empty() || e.download.empty() || !ParseVersion(e.version, v))
            continue;
        ++valid;
        entries_.push_back(std::move(e));
    }
    if (valid == 0) {
        error = "index.json 中没有有效的插件条目";
        return false;
    }
    return true;
}

std::vector<SourceEntry> PluginSource::Search(
    const std::string& keyword) const {
    const std::string kw = ToLower(keyword);
    std::vector<SourceEntry> out;
    for (const SourceEntry& e : entries_) {
        if (kw.empty() ||
            ToLower(e.id).find(kw) != std::string::npos ||
            ToLower(e.name).find(kw) != std::string::npos ||
            ToLower(e.description).find(kw) != std::string::npos) {
            out.push_back(e);
        }
    }
    return out;
}

const SourceEntry* PluginSource::Find(const std::string& id) const {
    for (const SourceEntry& e : entries_)
        if (e.id == id) return &e;
    return nullptr;
}

std::vector<SourceEntry> PluginSource::CheckUpdates() const {
    std::vector<SourceEntry> out;
    for (const SourceEntry& e : entries_) {
        const PluginRecord* rec = registry_.Find(e.id);
        if (!rec) continue; // 未安装
        Version srcV, curV;
        if (!ParseVersion(e.version, srcV) ||
            !ParseVersion(rec->manifest.version, curV))
            continue;
        if (CompareVersion(srcV, curV) > 0) out.push_back(e);
    }
    return out;
}

bool PluginSource::InstallOrUpdate(const SourceEntry& entry,
                                   IDownloader& downloader,
                                   const std::string& cacheDir,
                                   std::string& error) {
    error.clear();
    // 1. minApiVersion 检查（与 Manifest 约定一致：主版本相等，次版本 <= 宿主）
    if (!entry.minApiVersion.empty()) {
        Version mv;
        if (ParseVersion(entry.minApiVersion, mv)) {
            const uint32_t hostMajor = hostApiVersion_ >> 16;
            const uint32_t hostMinor = hostApiVersion_ & 0xFFFF;
            if (mv.major != hostMajor || mv.minor > hostMinor) {
                error = "插件要求 API " + entry.minApiVersion +
                        "，宿主当前 API 不满足";
                return false;
            }
        }
    }

    // 2. 更新场景：源版本必须比已安装版本新
    const PluginRecord* installed = registry_.Find(entry.id);
    if (installed) {
        Version srcV, curV;
        if (ParseVersion(entry.version, srcV) &&
            ParseVersion(installed->manifest.version, curV) &&
            CompareVersion(srcV, curV) <= 0) {
            error = "已安装相同或更高版本 (" + installed->manifest.version + ")";
            return false;
        }
    }

    // 3. 下载
    std::error_code ec;
    fs::create_directories(PathFromUtf8(cacheDir), ec);
    const fs::path pkgPath =
        PathFromUtf8(cacheDir) / PathFromUtf8(entry.id + ".sai-plugin");
    if (!downloader.Download(entry.download, PathToUtf8(pkgPath), error)) {
        error = "下载失败: " + error;
        return false;
    }

    // 4. SHA-256 完整性校验
    if (!entry.sha256.empty()) {
        std::string actual;
        if (!PluginSecurity::Sha256File(PathToUtf8(pkgPath), actual, error)) {
            error = "无法计算校验值: " + error;
            return false;
        }
        std::string expect = ToLower(entry.sha256);
        if (actual != expect) {
            error = "SHA-256 校验失败: 期望 " + expect + "，实际 " + actual;
            fs::remove(pkgPath, ec);
            return false;
        }
    }

    // 4.5 插件签名验签（第十六节：signature 与 signerPubKey 同时存在时强制）
    if (!entry.signature.empty() || !entry.signerPubKey.empty()) {
        if (entry.signature.empty() || entry.signerPubKey.empty()) {
            error = "签名不完整：signature 与 signerPubKey 必须同时提供";
            return false;
        }
        if (entry.sha256.empty()) {
            error = "签名条目必须同时提供 sha256";
            return false;
        }
        const std::string msg = PluginSignature::EntryMessage(
            entry.id, entry.version, ToLower(entry.sha256));
        std::string verr;
        if (!PluginSignature::Verify(entry.signerPubKey, entry.signature, msg,
                                     verr)) {
            error = "插件源签名验证失败: " + verr;
            fs::remove(pkgPath, ec);
            return false;
        }
    }

    // 5. 安装（PluginInstaller 内部完成 Manifest 校验 + API 版本校验）
    PluginInstaller installer(pluginsRoot_, registry_, hostApiVersion_);
    if (!installer.InstallFromPackage(PathToUtf8(pkgPath), error)) return false;

    // 安装成功后清理缓存包
    fs::remove(pkgPath, ec);
    return true;
}

} // namespace sai2::host
