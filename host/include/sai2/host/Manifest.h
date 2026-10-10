#pragma once
// ============================================================================
// Manifest.h - 插件 manifest.json（规格书第三节）
//
// 必填字段：id / name / version / apiVersion / entry / type
// 可选字段：author / description / minHostVersion / sha256
// 示例：
//   {
//     "id": "com.sai2.liquify",
//     "name": "Liquify",
//     "version": "1.0.0",
//     "apiVersion": "1.0",
//     "entry": "liquify.dll",
//     "type": "image-effect"
//   }
// ============================================================================

#include "sai2/host/PathUtil.h"

#include <filesystem>
#include <string>

namespace sai2::host {

struct Version {
    uint32_t major = 0;
    uint32_t minor = 0;
    uint32_t patch = 0;
};

// "1.2.3" / "1.2" / "1" -> Version；失败返回 false
bool ParseVersion(const std::string& s, Version& out);

// -1 / 0 / 1
int CompareVersion(const Version& a, const Version& b);

struct PluginManifest {
    std::string id;
    std::string name;
    std::string version;        // 插件自身版本
    std::string apiVersion;     // 依赖的插件 API 版本，如 "1.0"
    std::string entry;          // 入口动态库文件名，如 "helloworld.dll"
    std::string type;           // image-effect / tool / ...
    std::string author;
    std::string description;
    std::string minHostVersion; // 最低宿主版本（可选）
    std::string sha256;         // 入口 DLL 校验值（可选）

    // ---- 插件签名（第十六节，可选） ----
    // signature 为对 "SAI2-SIGN1|<id>|<version>|<sha256>" 的
    // RSA PKCS#1 v1.5 (SHA-256) 签名 hex；signerPubKey 为发布者公钥。
    // 两者同时存在时安装器强制验签。
    std::string signature;
    std::string signerPubKey;

    bool valid = false;
    std::string error;          // 校验失败原因
};

bool ParseManifestText(const std::string& text, PluginManifest& out,
                       std::string& error);
// path 为文件系统路径（含非 ASCII 时先 PathFromUtf8，见 PathUtil.h）
bool LoadManifestFile(const std::filesystem::path& path, PluginManifest& out,
                      std::string& error);

// 结构校验：必填字段、id 字符集（[a-zA-Z0-9._-]）、版本可解析
bool ValidateManifest(PluginManifest& m);

// 插件声明的 apiVersion 是否与宿主兼容（主版本必须相等，次版本 <= 宿主次版本）
bool IsApiVersionCompatible(const std::string& pluginApiVersion,
                            uint32_t hostApiVersion);

} // namespace sai2::host
