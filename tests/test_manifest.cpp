// ============================================================================
// test_manifest.cpp - Manifest 解析 / 校验 / 版本兼容性
// ============================================================================

#include "Tests.h"
#include "sai2/PluginAPI.h"
#include "sai2/host/Manifest.h"

namespace sai2test {

bool TestManifest() {
    using sai2::host::CompareVersion;
    using sai2::host::IsApiVersionCompatible;
    using sai2::host::ParseManifestText;
    using sai2::host::ParseVersion;
    using sai2::host::PluginManifest;
    using sai2::host::Version;

    // ---- 版本解析与比较 ----
    Version v;
    CHECK(ParseVersion("1.2.3", v));
    CHECK(v.major == 1 && v.minor == 2 && v.patch == 3);
    CHECK(ParseVersion("2.0", v) && v.major == 2 && v.minor == 0);
    CHECK(ParseVersion("7", v) && v.major == 7);
    Version a, b;
    CHECK(ParseVersion("1.0.0", a) && ParseVersion("1.0.1", b));
    CHECK(CompareVersion(a, b) < 0);
    CHECK(CompareVersion(b, a) > 0);
    CHECK(CompareVersion(a, a) == 0);

    // ---- 合法 manifest ----
    const std::string json = R"({
        "id": "com.sai2.liquify",
        "name": "Liquify",
        "version": "1.0.0",
        "apiVersion": "1.0",
        "entry": "plugin.dll",
        "type": "image-effect",
        "author": "SYSTEMAX",
        "description": "液化"
    })";
    PluginManifest m;
    std::string error;
    CHECK(ParseManifestText(json, m, error));
    CHECK(m.id == "com.sai2.liquify");
    CHECK(m.name == "Liquify");
    CHECK(m.version == "1.0.0");
    CHECK(m.apiVersion == "1.0");
    CHECK(m.entry == "plugin.dll");
    CHECK(m.type == "image-effect");
    CHECK(m.description == "液化"); // \uXXXX -> UTF-8
    CHECK(m.valid);

    // ---- 缺字段 ----
    CHECK(!ParseManifestText(R"({"id":"x","name":"n"})", m, error));
    CHECK(!error.empty());

    // ---- 非法 id 字符集 ----
    CHECK(!ParseManifestText(R"({"id":"bad id!","name":"n","version":"1.0.0",)"
                             R"("apiVersion":"1.0","entry":"a.dll","type":"t"})",
                             m, error));

    // ---- API 版本兼容性 ----
    const uint32_t hostApi100 = sai2::MakeApiVersion(1, 0);
    const uint32_t hostApi101 = sai2::MakeApiVersion(1, 1);
    CHECK(IsApiVersionCompatible("1.0", hostApi100));
    CHECK(IsApiVersionCompatible("1.0", hostApi101)); // 旧插件可跑新宿主
    CHECK(!IsApiVersionCompatible("1.1", hostApi100)); // 新插件不可跑旧宿主
    CHECK(!IsApiVersionCompatible("0.9", hostApi100)); // 主版本不匹配
    CHECK(!IsApiVersionCompatible("2.0", hostApi100));
    CHECK(!IsApiVersionCompatible("abc", hostApi100));

    std::cout << "  manifest: 解析/校验/版本兼容 全部通过\n";
    return true;
}

} // namespace sai2test
