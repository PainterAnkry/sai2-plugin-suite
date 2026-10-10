#include "sai2/host/PluginInstaller.h"
#include "sai2/host/Crypto.h"
#include "sai2/host/Json.h"
#include "sai2/host/Logging.h"
#include "sai2/host/PluginSecurity.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace sai2::host {

// ---------------------------------------------------------------------------
// 工具函数
// ---------------------------------------------------------------------------
namespace {

bool ReadAllBytes(const std::string& path, std::vector<uint8_t>& out,
                  std::string& error) {
    // path 是 UTF-8：先转宽字符再开流（目录含中文时必需，见 PathUtil.h）
    std::ifstream f(PathFromUtf8(path), std::ios::binary);
    if (!f) {
        error = "无法打开文件: " + path;
        return false;
    }
    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    if (size < 0 || size > 512 * 1024 * 1024) {
        error = "文件大小异常: " + path;
        return false;
    }
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    f.read(reinterpret_cast<char*>(out.data()), size);
    return true;
}

bool WriteAllBytes(const fs::path& path, const uint8_t* data, size_t len) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(data),
            static_cast<std::streamsize>(len));
    return true;
}

uint16_t ReadU16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
uint32_t ReadU32(const uint8_t* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24);
}

// CRC32（IEEE 802.3，用于 ZIP 条目完整性校验）
uint32_t Crc32(const uint8_t* data, size_t len) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        init = true;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i)
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

// ZIP 条目名安全检查：拒绝绝对路径与 ..
bool SafeEntryName(const std::string& name) {
    if (name.empty()) return false;
    if (name.front() == '/' || name.front() == '\\') return false;
    if (name.find(':') != std::string::npos) return false;
    std::string norm = name;
    std::replace(norm.begin(), norm.end(), '\\', '/');
    size_t start = 0;
    while (start <= norm.size()) {
        const size_t endPos = norm.find('/', start);
        const std::string part =
            norm.substr(start, endPos == std::string::npos ? std::string::npos
                                                           : endPos - start);
        if (part == "..") return false;
        if (endPos == std::string::npos) break;
        start = endPos + 1;
    }
    return true;
}

void CopyDirRecursive(const fs::path& src, const fs::path& dst,
                      std::string& error) {
    std::error_code ec;
    fs::create_directories(dst, ec);
    for (const auto& item : fs::directory_iterator(src, ec)) {
        const fs::path target = dst / item.path().filename();
        if (item.is_directory()) {
            CopyDirRecursive(item.path(), target, error);
            if (!error.empty()) return;
        } else {
            fs::copy_file(item.path(), target, fs::copy_options::overwrite_existing,
                          ec);
            if (ec) {
                error = "复制文件失败: " + PathToUtf8(item.path());
                return;
            }
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------
// PluginInstaller
// ---------------------------------------------------------------------------
PluginInstaller::PluginInstaller(std::string pluginsRoot,
                                 PluginRegistry& registry,
                                 uint32_t hostApiVersion)
    : pluginsRoot_(std::move(pluginsRoot)),
      registry_(registry),
      hostApiVersion_(hostApiVersion) {}

bool PluginInstaller::InstallFromManifest(const PluginManifest& manifest,
                                          const std::string& srcDir,
                                          std::string& error) {
    // 1. API 兼容性校验
    if (!IsApiVersionCompatible(manifest.apiVersion, hostApiVersion_)) {
        error = "插件 API 版本不兼容: " + manifest.apiVersion;
        return false;
    }
    // 2. 入口文件存在性
    const fs::path entryPath = PathFromUtf8(srcDir) / PathFromUtf8(manifest.entry);
    if (!fs::exists(entryPath)) {
        error = "入口文件不存在: " + PathToUtf8(entryPath);
        return false;
    }
    // 3. SHA-256 完整性校验（manifest 声明了 sha256 时）
    if (!manifest.sha256.empty()) {
        if (!PluginSecurity::VerifyFileSha256(PathToUtf8(entryPath),
                                              manifest.sha256, error)) {
            return false;
        }
    }
    // 3.5 插件签名验签（第十六节：signature 与 signerPubKey 同时存在时强制）
    if (!manifest.signature.empty() || !manifest.signerPubKey.empty()) {
        if (manifest.signature.empty() || manifest.signerPubKey.empty()) {
            error = "签名不完整：signature 与 signerPubKey 必须同时提供";
            return false;
        }
        std::string entrySha;
        if (!PluginSecurity::Sha256File(PathToUtf8(entryPath), entrySha, error))
            return false;
        const std::string msg = PluginSignature::EntryMessage(
            manifest.id, manifest.version, entrySha);
        if (!PluginSignature::Verify(manifest.signerPubKey, manifest.signature,
                                     msg, error)) {
            error = "插件签名验证失败: " + error;
            Logger::Instance().Write(LogLevel::Error,
                                     "签名验证失败: " + manifest.id);
            return false;
        }
        Logger::Instance().Write(LogLevel::Info,
                                 "签名验证通过: " + manifest.id);
    }

    // 4. 复制到 plugins/<id>/ 并注册
    const fs::path destDir =
        PathFromUtf8(pluginsRoot_) / PathFromUtf8(manifest.id);
    std::error_code ec;
    fs::remove_all(destDir, ec);
    CopyDirRecursive(srcDir, destDir, error);
    if (!error.empty()) return false;

    PluginRecord record;
    record.manifest = manifest;
    record.enabled = true;
    record.directory = PathToUtf8(destDir);
    registry_.Add(record);
    registry_.SaveState();

    Logger::Instance().Write(LogLevel::Info,
                             "插件安装成功: " + manifest.id + " v" +
                                 manifest.version);
    return true;
}

bool PluginInstaller::InstallFromDirectory(const std::string& srcDir,
                                           std::string& error) {
    const fs::path manifestPath =
        PathFromUtf8(srcDir) / std::filesystem::path(L"manifest.json");
    if (!fs::exists(manifestPath)) {
        error = "目录中缺少 manifest.json: " + srcDir;
        return false;
    }
    PluginManifest manifest;
    if (!LoadManifestFile(manifestPath, manifest, error)) return false;
    return InstallFromManifest(manifest, srcDir, error);
}

bool PluginInstaller::InstallFromPackage(const std::string& packagePath,
                                         std::string& error) {
    std::vector<uint8_t> zip;
    if (!ReadAllBytes(packagePath, zip, error)) return false;
    if (zip.size() < 22 || zip[0] != 'P' || zip[1] != 'K') {
        error = "不是有效的 ZIP/.sai-plugin 包";
        return false;
    }

    // ---- 定位 End of Central Directory ----
    size_t eocd = std::string::npos;
    const size_t scanStart = zip.size() > 65557 ? zip.size() - 65557 : 0;
    for (size_t i = zip.size() - 22 + 1; i-- > scanStart;) {
        if (zip[i] == 'P' && zip[i + 1] == 'K' && zip[i + 2] == 5 &&
            zip[i + 3] == 6) {
            eocd = i;
            break;
        }
    }
    if (eocd == std::string::npos) {
        error = "ZIP 目录（EOCD）未找到";
        return false;
    }
    const uint16_t entryCount = ReadU16(&zip[eocd + 10]);
    const uint32_t cdOffset = ReadU32(&zip[eocd + 16]);

    // ---- 解压到临时暂存目录 ----
    const fs::path staging =
        PathFromUtf8(pluginsRoot_) /
        std::filesystem::path(L".staging-" +
                              std::to_wstring(std::chrono::steady_clock::now()
                                                  .time_since_epoch()
                                                  .count()));
    std::error_code ec;
    fs::create_directories(staging, ec);

    size_t cd = cdOffset;
    size_t extracted = 0;
    for (uint16_t n = 0; n < entryCount; ++n) {
        if (cd + 46 > zip.size() || zip[cd] != 'P' || zip[cd + 1] != 'K' ||
            zip[cd + 2] != 1 || zip[cd + 3] != 2) {
            error = "ZIP 中央目录损坏";
            fs::remove_all(staging, ec);
            return false;
        }
        const uint16_t method = ReadU16(&zip[cd + 10]);
        const uint32_t crc = ReadU32(&zip[cd + 16]);
        const uint32_t compSize = ReadU32(&zip[cd + 20]);
        const uint32_t uncompSize = ReadU32(&zip[cd + 24]);
        const uint16_t nameLen = ReadU16(&zip[cd + 28]);
        const uint16_t extraLen = ReadU16(&zip[cd + 30]);
        const uint16_t commentLen = ReadU16(&zip[cd + 32]);
        const uint32_t localOffset = ReadU32(&zip[cd + 42]);
        const std::string name(reinterpret_cast<const char*>(&zip[cd + 46]),
                               nameLen);
        cd += 46 + nameLen + extraLen + commentLen;

        if (name.empty()) continue;
        // 目录条目
        if (name.back() == '/') continue;

        if (!SafeEntryName(name)) {
            error = "ZIP 条目路径不安全: " + name;
            fs::remove_all(staging, ec);
            return false;
        }
        if (method != 0) {
            error = "暂不支持 DEFLATE 压缩条目（" + name +
                    "）；请用 STORED 方式打包 .sai-plugin";
            fs::remove_all(staging, ec);
            return false;
        }

        // 读取本地头，确定数据起点
        if (localOffset + 30 > zip.size() || zip[localOffset] != 'P' ||
            zip[localOffset + 1] != 'K' || zip[localOffset + 2] != 3 ||
            zip[localOffset + 3] != 4) {
            error = "ZIP 本地头损坏: " + name;
            fs::remove_all(staging, ec);
            return false;
        }
        const uint16_t lhNameLen = ReadU16(&zip[localOffset + 26]);
        const uint16_t lhExtraLen = ReadU16(&zip[localOffset + 28]);
        const size_t dataOff = localOffset + 30 + lhNameLen + lhExtraLen;
        if (dataOff + compSize > zip.size()) {
            error = "ZIP 条目数据越界: " + name;
            fs::remove_all(staging, ec);
            return false;
        }

        // CRC32 完整性校验
        if (Crc32(&zip[dataOff], compSize) != crc) {
            error = "ZIP 条目 CRC32 校验失败: " + name;
            fs::remove_all(staging, ec);
            return false;
        }

        std::string norm = name;
        std::replace(norm.begin(), norm.end(), '\\', '/');
        if (!WriteAllBytes(staging / fs::path(norm), &zip[dataOff],
                           uncompSize)) {
            error = "写出条目失败: " + name;
            fs::remove_all(staging, ec);
            return false;
        }
        ++extracted;
    }
    if (extracted == 0) {
        error = "包内没有可安装的文件";
        fs::remove_all(staging, ec);
        return false;
    }

    // ---- 读取 manifest 并安装 ----
    const fs::path manifestPath = staging / "manifest.json";
    if (!fs::exists(manifestPath)) {
        error = "包内缺少 manifest.json";
        fs::remove_all(staging, ec);
        return false;
    }
    PluginManifest manifest;
    if (!LoadManifestFile(manifestPath, manifest, error)) {
        fs::remove_all(staging, ec);
        return false;
    }
    const bool ok =
        InstallFromManifest(manifest, PathToUtf8(staging), error);
    fs::remove_all(staging, ec);
    return ok;
}

bool PluginInstaller::Uninstall(const std::string& pluginId,
                                std::string& error) {
    const fs::path dir = PathFromUtf8(pluginsRoot_) / PathFromUtf8(pluginId);
    std::error_code ec;
    if (fs::exists(dir)) {
        fs::remove_all(dir, ec);
        if (ec) {
            error = "删除插件目录失败: " + PathToUtf8(dir);
            return false;
        }
    }
    registry_.Remove(pluginId);
    registry_.SaveState();
    Logger::Instance().Write(LogLevel::Info, "插件卸载: " + pluginId);
    return true;
}

} // namespace sai2::host
