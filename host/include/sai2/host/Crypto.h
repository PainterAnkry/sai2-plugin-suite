#pragma once
// ============================================================================
// Crypto.h - 轻量 RSA 签名（规格书第十六节：插件签名）
//
// 自带实现（零第三方依赖）：
//   - BigInt：uint32 小端 limb 大整数（加减乘除、模幂、Miller-Rabin 素性检测）
//   - RSA PKCS#1 v1.5 签名/验签（EMSA-PKCS1-v1_5 + SHA-256 DigestInfo）
//   - 公钥序列化格式："SAI2RSA1.<modulusHex>.<exponentHex>"
//
// 安全性说明：
//   - 生产环境建议 2048 位密钥；验签开销（e=65537）毫秒级可接受。
//   - 密钥生成/签名建议离线完成（signing 工具），运行时只做验签。
//   - 本实现用于 MVP；正式发布可替换为 CNG / OpenSSL 等标准库，
//     接口（SignMessage / VerifyMessage）保持不变。
// ============================================================================

#include <cstdint>
#include <string>
#include <vector>

namespace sai2::host {

// ---------- 大整数（小端 uint32 limb） ----------
class BigInt {
public:
    std::vector<uint32_t> limb; // little-endian，无前导零（空 = 0）

    BigInt() = default;
    explicit BigInt(uint64_t v);

    bool IsZero() const;
    size_t BitLength() const;
    int Compare(const BigInt& o) const; // -1/0/1

    static BigInt Add(const BigInt& a, const BigInt& b);
    static BigInt Sub(const BigInt& a, const BigInt& b); // 要求 a >= b
    static BigInt Mul(const BigInt& a, const BigInt& b);
    // 二进制长除法：返回商与余数
    static void DivMod(const BigInt& a, const BigInt& b, BigInt& q, BigInt& r);
    static BigInt Mod(const BigInt& a, const BigInt& b);
    static BigInt ModPow(const BigInt& base, const BigInt& exp,
                         const BigInt& mod);

    void Trim();

    // hex（大端书写，便于人读与持久化）
    static BigInt FromHex(const std::string& hex);
    std::string ToHex() const;

    // Miller-Rabin（确定性轮数足够用于测试与工程）
    static bool IsProbablyPrime(const BigInt& n, int rounds = 24);
};

struct RsaKeyPair {
    BigInt n; // 模数
    BigInt e; // 公钥指数
    BigInt d; // 私钥指数
    BigInt p, q;
};

// 生成 bits 位 RSA 密钥（bits >= 512；e 固定 65537）
bool RsaGenerateKeyPair(int bits, RsaKeyPair& out, std::string& error);

// ---------- 插件签名（PKCS#1 v1.5 / SHA-256） ----------
class PluginSignature {
public:
    // 被签名消息的标准拼接格式
    static std::string EntryMessage(const std::string& pluginId,
                                    const std::string& version,
                                    const std::string& sha256Hex);

    // 签名：返回签名 hex（模数等长）
    static bool Sign(const BigInt& n, const BigInt& d,
                     const std::string& message, std::string& sigHexOut,
                     std::string& error);

    // 验签：pubKeyText = "SAI2RSA1.<modulusHex>.<exponentHex>"
    static bool Verify(const std::string& pubKeyText,
                       const std::string& sigHex,
                       const std::string& message, std::string& error);

    // 公钥序列化 / 解析
    static std::string EncodePublicKey(const BigInt& n, const BigInt& e);
    static bool DecodePublicKey(const std::string& text, BigInt& n, BigInt& e,
                                std::string& error);
};

} // namespace sai2::host
