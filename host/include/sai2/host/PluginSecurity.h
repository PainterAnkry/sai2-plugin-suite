#pragma once
// ============================================================================
// PluginSecurity.h - 插件安全校验（规格书第十六节）
//
// - SHA-256 文件校验（入口 DLL / 安装包）
// - API Version 校验（见 Manifest.h）
// 后续扩展：插件签名、权限声明。
// ============================================================================

#include <cstdint>
#include <string>

namespace sai2::host {

class PluginSecurity {
public:
    // 计算文件 SHA-256，输出小写 hex
    static bool Sha256File(const std::string& path, std::string& hexOut,
                           std::string& error);
    // 计算内存数据 SHA-256
    static std::string Sha256Bytes(const uint8_t* data, size_t len);

    // 校验文件 SHA-256 是否等于期望值（大小写不敏感）
    static bool VerifyFileSha256(const std::string& path,
                                 const std::string& expectedHex,
                                 std::string& error);
};

} // namespace sai2::host
