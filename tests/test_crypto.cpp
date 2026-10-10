// ============================================================================
// test_crypto.cpp - BigInt / RSA 签名 / Manifest 签名安装集成测试
// ============================================================================

#include "Tests.h"

#include "sai2/PluginAPI.h"
#include "sai2/host/Crypto.h"
#include "sai2/host/Manifest.h"
#include "sai2/host/PluginInstaller.h"
#include "sai2/host/PluginRegistry.h"
#include "sai2/host/PluginSecurity.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace sai2;
using namespace sai2::host;
namespace fs = std::filesystem;

namespace {

void WriteFile(const fs::path& p, const std::string& data) {
    std::ofstream f(p, std::ios::binary);
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
}

} // namespace

namespace sai2test {

bool TestCrypto() {
    // ---- 1. BigInt 基础运算 ----
    {
        const BigInt a = BigInt::FromHex("deadbeefcafebabe");
        const BigInt b = BigInt::FromHex("1234567890abcdef");
        const BigInt sum = BigInt::Add(a, b);
        CHECK(BigInt::Sub(sum, b).ToHex() == a.ToHex());
        CHECK(sum.Compare(b) > 0);

        CHECK(BigInt().IsZero());
        CHECK(BigInt::Mul(a, BigInt()).IsZero());

        // 乘法：123456789 * 987654321 = 121932631112635269
        const BigInt m1 = BigInt::Mul(BigInt(123456789ull), BigInt(987654321ull));
        CHECK(m1.ToHex() == BigInt(121932631112635269ull).ToHex());

        // DivMod 往返
        BigInt q, r;
        BigInt::DivMod(m1, b, q, r);
        const BigInt back = BigInt::Add(BigInt::Mul(q, b), r);
        CHECK(back.ToHex() == m1.ToHex());
        CHECK(r.Compare(b) < 0);

        // 模幂小对拍：3^5 mod 7 = 5
        CHECK(BigInt::ModPow(BigInt(3), BigInt(5), BigInt(7)).ToHex() == "5");

        // hex 往返
        const std::string hexIn = "0123456789abcdef0123456789abcdef";
        CHECK(BigInt::FromHex(hexIn).ToHex() ==
              "123456789abcdef0123456789abcdef");
    }

    // ---- 2. 素性检测 ----
    {
        CHECK(BigInt::IsProbablyPrime(BigInt(2)));
        CHECK(BigInt::IsProbablyPrime(BigInt(97)));
        CHECK(!BigInt::IsProbablyPrime(BigInt(91))); // 7*13
        // 已知素数 2^89-1（Mersenne 素数）
        BigInt m89(1);
        for (int i = 0; i < 89; ++i) m89 = BigInt::Add(m89, m89);
        m89 = BigInt::Sub(m89, BigInt(1));
        CHECK(BigInt::IsProbablyPrime(m89));
    }
    return true;
}

bool TestCryptoSign() {
    // ---- 3. 密钥生成 + 签名/验签往返（512 位，测试速度优先） ----
    RsaKeyPair kp;
    std::string error;
    CHECK_MSG(RsaGenerateKeyPair(512, kp, error), error.c_str());

    const std::string pubText = PluginSignature::EncodePublicKey(kp.n, kp.e);
    BigInt n2, e2;
    CHECK(PluginSignature::DecodePublicKey(pubText, n2, e2, error));
    CHECK(n2.ToHex() == kp.n.ToHex() && e2.ToHex() == kp.e.ToHex());

    const std::string msg =
        PluginSignature::EntryMessage("com.sai2.demo", "1.0.0",
                                      std::string(64, 'a'));
    std::string sig;
    CHECK_MSG(PluginSignature::Sign(kp.n, kp.d, msg, sig, error), error.c_str());
    CHECK_MSG(PluginSignature::Verify(pubText, sig, msg, error),
              error.c_str());

    // ---- 4. 篡改检测 ----
    {
        std::string verr;
        CHECK(!PluginSignature::Verify(pubText, sig, msg + "x", verr));
        std::string bad = sig;
        bad[4] = (bad[4] == 'a') ? 'b' : 'a';
        CHECK(!PluginSignature::Verify(pubText, bad, msg, verr));
        RsaKeyPair kp2;
        CHECK(RsaGenerateKeyPair(512, kp2, error));
        CHECK(!PluginSignature::Verify(
            PluginSignature::EncodePublicKey(kp2.n, kp2.e), sig, msg, verr));
    }

    // ---- 5. 集成：带签名的 manifest 安装 ----
    const uint64_t stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path work = fs::path("..") / ("sai2_sig_test_" + std::to_string(stamp));
    fs::create_directories(work);

    const fs::path srcDir = work / "com.sai2.signed";
    fs::create_directories(srcDir);
    const std::string dllBytes = "fake-dll-binary-content-for-signature-test";
    WriteFile(srcDir / "signed.dll", dllBytes);

    std::string entrySha = PluginSecurity::Sha256Bytes(
        reinterpret_cast<const uint8_t*>(dllBytes.data()), dllBytes.size());
    const std::string signMsg =
        PluginSignature::EntryMessage("com.sai2.signed", "1.0.0", entrySha);
    std::string sig2;
    CHECK_MSG(PluginSignature::Sign(kp.n, kp.d, signMsg, sig2, error),
              error.c_str());
    const std::string pub2 = PluginSignature::EncodePublicKey(kp.n, kp.e);

    const std::string manifestJson =
        "{\n"
        "  \"id\": \"com.sai2.signed\",\n"
        "  \"name\": \"Signed Plugin\",\n"
        "  \"version\": \"1.0.0\",\n"
        "  \"apiVersion\": \"1.0\",\n"
        "  \"entry\": \"signed.dll\",\n"
        "  \"type\": \"image-effect\",\n"
        "  \"sha256\": \"" + entrySha + "\",\n"
        "  \"signature\": \"" + sig2 + "\",\n"
        "  \"signerPubKey\": \"" + pub2 + "\"\n"
        "}\n";
    WriteFile(srcDir / "manifest.json", manifestJson);

    PluginRegistry registry(work.string());
    PluginInstaller installer((work / "plugins").string(), registry,
                              SAI2_PLUGIN_API_VERSION);
    CHECK_MSG(installer.InstallFromDirectory(srcDir.string(), error),
              error.c_str());

    // 篡改签名后重装应失败
    std::string badSig = sig2;
    badSig[10] = (badSig[10] == '0') ? '1' : '0';
    const std::string tamperedJson =
        "{\n"
        "  \"id\": \"com.sai2.signed\",\n"
        "  \"name\": \"Signed Plugin\",\n"
        "  \"version\": \"1.0.0\",\n"
        "  \"apiVersion\": \"1.0\",\n"
        "  \"entry\": \"signed.dll\",\n"
        "  \"type\": \"image-effect\",\n"
        "  \"sha256\": \"" + entrySha + "\",\n"
        "  \"signature\": \"" + badSig + "\",\n"
        "  \"signerPubKey\": \"" + pub2 + "\"\n"
        "}\n";
    PluginManifest tampered;
    CHECK(ParseManifestText(tamperedJson, tampered, error));
    CHECK(!installer.InstallFromManifest(tampered, srcDir.string(), error));
    CHECK(error.find("签名") != std::string::npos);

    // 签名与公钥只给一个 -> 拒绝
    const std::string halfJson =
        "{\n"
        "  \"id\": \"com.sai2.signed\",\n"
        "  \"name\": \"Signed Plugin\",\n"
        "  \"version\": \"1.0.0\",\n"
        "  \"apiVersion\": \"1.0\",\n"
        "  \"entry\": \"signed.dll\",\n"
        "  \"type\": \"image-effect\",\n"
        "  \"signature\": \"" + sig2 + "\"\n"
        "}\n";
    PluginManifest half;
    CHECK(ParseManifestText(halfJson, half, error));
    CHECK(!installer.InstallFromManifest(half, srcDir.string(), error));

    std::error_code ec;
    fs::remove_all(work, ec);
    return true;
}

} // namespace sai2test
