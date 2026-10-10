// ============================================================================
// test_plugin_source.cpp - Phase 6 在线插件源测试
//
// 覆盖：index.json 解析与校验、搜索、CheckUpdates、安装全流程
//      （下载 -> SHA-256 校验 -> minApiVersion 检查 -> 版本比较 -> 安装）、
//      SHA-256 不匹配拒绝、API 版本不满足拒绝、重复安装拒绝。
//
// 测试用 FileDownloader（本地路径复制），安装包为测试内生成的
// STORED（无压缩）ZIP，含 manifest.json + demo.dll。
// ============================================================================

#include "Tests.h"

#include "sai2/PluginAPI.h"
#include "sai2/host/PluginRegistry.h"
#include "sai2/host/PluginSecurity.h"
#include "sai2/host/PluginSource.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// ---- 最小 CRC32（用于构造 ZIP）----
uint32_t Crc32(const uint8_t* p, size_t n) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i)
        crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

void Put16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8));
}
void Put32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i)));
}

// JSON 字符串转义（至少处理反斜杠与引号）
std::string JqPath(const fs::path& p) {
    std::string s = p.string(), out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '\\' || c == '"') out += '\\';
        out += c;
    }
    return out;
}

// 构造 STORED（无压缩）ZIP：manifest.json 在包根目录
std::vector<uint8_t> MakeStoredZip(
    const std::vector<std::pair<std::string, std::string>>& files) {
    struct Entry { std::string name; uint32_t crc, size; size_t offset; };
    std::vector<uint8_t> zip;
    std::vector<Entry> entries;

    for (const auto& [name, content] : files) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(content.data());
        const uint32_t crc = Crc32(p, content.size());
        const uint32_t sz = static_cast<uint32_t>(content.size());
        const size_t localOffset = zip.size();

        // local file header（30 字节）
        Put32(zip, 0x04034b50);
        Put16(zip, 20);       // version needed
        Put16(zip, 0);        // flags
        Put16(zip, 0);        // method = STORED
        Put16(zip, 0); Put16(zip, 0); // time/date
        Put32(zip, crc);
        Put32(zip, sz);       // csize
        Put32(zip, sz);       // usize
        Put16(zip, static_cast<uint16_t>(name.size()));
        Put16(zip, 0);        // extra len
        zip.insert(zip.end(), name.begin(), name.end());
        zip.insert(zip.end(), p, p + content.size());

        entries.push_back({name, crc, sz, localOffset});
    }

    const uint32_t cdOffset = static_cast<uint32_t>(zip.size());
    for (const Entry& e : entries) {
        // central directory entry（46 字节）
        Put32(zip, 0x02014b50);
        Put16(zip, 20); Put16(zip, 20);
        Put16(zip, 0);  Put16(zip, 0);
        Put16(zip, 0); Put16(zip, 0);
        Put32(zip, e.crc);
        Put32(zip, e.size);
        Put32(zip, e.size);
        Put16(zip, static_cast<uint16_t>(e.name.size()));
        Put16(zip, 0); // extra len
        Put16(zip, 0); // comment len
        Put16(zip, 0); // disk start
        Put16(zip, 0); // internal attrs
        Put32(zip, 0); // external attrs
        Put32(zip, static_cast<uint32_t>(e.offset));
        zip.insert(zip.end(), e.name.begin(), e.name.end());
    }
    const uint32_t cdSize = static_cast<uint32_t>(zip.size()) - cdOffset;

    // EOCD（22 字节）
    Put32(zip, 0x06054b50);
    Put16(zip, 0); Put16(zip, 0);
    Put16(zip, static_cast<uint16_t>(entries.size()));
    Put16(zip, static_cast<uint16_t>(entries.size()));
    Put32(zip, cdSize);
    Put32(zip, cdOffset);
    Put16(zip, 0);
    return zip;
}

std::string ManifestJson(const char* id, const char* version) {
    return std::string("{\"id\":\"") + id + "\",\"name\":\"Demo\","
           "\"version\":\"" + version + "\",\"apiVersion\":\"1.0\","
           "\"entry\":\"demo.dll\",\"type\":\"image-effect\"}";
}

bool WriteBytes(const fs::path& p, const std::vector<uint8_t>& data) {
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(f);
}

std::vector<uint8_t> ReadBytes(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
}

} // namespace

namespace sai2test {

bool TestPluginSource() {
    using namespace sai2::host;

    // ---- 测试工作区（放在当前目录下）----
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path workDir =
        fs::current_path() / ("sai2_src_test_" + std::to_string(stamp));
    const fs::path repoDir = workDir / "repo";   // 模拟插件源仓库
    const fs::path pluginsDir = workDir / "plugins";
    const fs::path cacheDir = workDir / "cache";
    std::error_code ec;
    fs::create_directories(repoDir, ec);
    CHECK(!ec);

    // ---- 生成两个版本的测试安装包 ----
    const std::vector<uint8_t> demoDll(512, 0x42); // 假 DLL（不加载）
    auto makePackage = [&](const char* version) {
        return MakeStoredZip({
            {"manifest.json", ManifestJson("com.sai2.demo", version)},
            {"demo.dll",
             std::string(demoDll.begin(), demoDll.end())},
        });
    };
    const std::vector<uint8_t> pkgV1 = makePackage("1.0.0");
    const std::vector<uint8_t> pkgV2 = makePackage("1.1.0");
    CHECK(WriteBytes(repoDir / "demo-1.0.0.sai-plugin", pkgV1));
    CHECK(WriteBytes(repoDir / "demo-1.1.0.sai-plugin", pkgV2));

    const std::string shaV1 = PluginSecurity::Sha256Bytes(pkgV1.data(), pkgV1.size());
    const std::string shaV2 = PluginSecurity::Sha256Bytes(pkgV2.data(), pkgV2.size());
    CHECK(shaV1.size() == 64 && shaV2.size() == 64);

    // JSON 字符串转义（Windows 路径反斜杠必须转义）
    const std::string pkg1Path = JqPath(repoDir / "demo-1.0.0.sai-plugin");
    const std::string pkg2Path = JqPath(repoDir / "demo-1.1.0.sai-plugin");

    // ---- index.json v1 ----
    const std::string indexJsonV1 =
        "{ \"plugins\": ["
        "{ \"id\": \"com.sai2.demo\", \"name\": \"Demo Plugin\", "
        "\"version\": \"1.0.0\", \"download\": \"" + pkg1Path + "\", "
        "\"description\": \"A demo plugin for testing\", "
        "\"icon\": \"demo.png\", \"sha256\": \"" + shaV1 + "\", "
        "\"minApiVersion\": \"1.0\" },"
        "{ \"id\": \"com.sai2.tool\", \"name\": \"Tool X\", "
        "\"version\": \"2.0.0\", \"download\": \"nowhere.bin\", "
        "\"description\": \"Another tool with keyword search target\" },"
        "{ \"id\": \"com.sai2.future\", \"name\": \"Future API\", "
        "\"version\": \"1.0.0\", \"download\": \"future.sai-plugin\", "
        "\"minApiVersion\": \"99.0\" },"
        "{ \"id\": \"com.sai2.corrupt\", \"name\": \"Corrupt Pkg\", "
        "\"version\": \"1.0.0\", \"download\": \"" + pkg2Path + "\", "
        "\"sha256\": \"00000000000000000000000000000000"
        "00000000000000000000000000000000\" },"
        "{ \"id\": \"bad-entry\", \"name\": \"No Version\" }"
        "] }";

    // ---- 初始化注册表与插件源 ----
    PluginRegistry registry(pluginsDir.string());
    registry.Scan();
    PluginSource source(pluginsDir.string(), registry, sai2::kPluginApiVersion);

    std::string error;

    // 无效 index 拒绝
    CHECK(!source.LoadIndexFromJson("{not json", error));
    CHECK(!source.LoadIndexFromJson("{\"plugins\": [{\"id\":\"x\"}]}", error));

    CHECK(source.LoadIndexFromJson(indexJsonV1, error));
    // 5 个条目中 bad-entry（无版本号）被跳过
    CHECK(source.Entries().size() == 4);

    // ---- 搜索 ----
    CHECK(source.Search("demo").size() == 1);
    CHECK(source.Search("TOOL").size() == 1);       // 大小写不敏感
    CHECK(source.Search("testing").size() == 1);    // 命中 description
    CHECK(source.Search("").size() == 4);           // 空关键字 = 全部
    CHECK(source.Search("no-such-keyword").empty());
    CHECK(source.Find("com.sai2.demo") != nullptr);
    CHECK(source.Find("missing") == nullptr);

    // ---- CheckUpdates：未安装 -> 空 ----
    CHECK(source.CheckUpdates().empty());

    // ---- 安装 happy path ----
    const SourceEntry* demo = source.Find("com.sai2.demo");
    CHECK(demo != nullptr);
    FileDownloader dl;
    const bool okInstall = source.InstallOrUpdate(*demo, dl, cacheDir.string(), error);
    if (!okInstall)
        std::cout << "install error: " << error << "\n";
    CHECK(okInstall);
    CHECK(registry.Find("com.sai2.demo") != nullptr);
    CHECK(registry.Find("com.sai2.demo")->manifest.version == "1.0.0");
    // 安装包缓存已清理
    CHECK(!fs::exists(cacheDir / "com.sai2.demo.sai-plugin"));
    // 插件目录落盘
    CHECK(fs::exists(pluginsDir / "com.sai2.demo" / "manifest.json"));
    CHECK(fs::exists(pluginsDir / "com.sai2.demo" / "demo.dll"));

    // ---- 重复安装同版本：拒绝 ----
    CHECK(!source.InstallOrUpdate(*demo, dl, cacheDir.string(), error));
    CHECK(error.find("已安装") != std::string::npos);

    // ---- SHA-256 不匹配：拒绝且不落盘 ----
    const SourceEntry* corrupt = source.Find("com.sai2.corrupt");
    CHECK(corrupt != nullptr);
    CHECK(!source.InstallOrUpdate(*corrupt, dl, cacheDir.string(), error));
    CHECK(error.find("SHA-256") != std::string::npos);
    CHECK(registry.Find("com.sai2.corrupt") == nullptr);

    // ---- minApiVersion 不满足：拒绝 ----
    const SourceEntry* future = source.Find("com.sai2.future");
    CHECK(future != nullptr);
    CHECK(!source.InstallOrUpdate(*future, dl, cacheDir.string(), error));
    CHECK(error.find("API") != std::string::npos);

    // ---- 更新检查：源有 1.1.0，已装 1.0.0 ----
    const std::string indexJsonV2 =
        "{ \"plugins\": ["
        "{ \"id\": \"com.sai2.demo\", \"name\": \"Demo Plugin\", "
        "\"version\": \"1.1.0\", \"download\": \"" + pkg2Path + "\", "
        "\"sha256\": \"" + shaV2 + "\" }"
        "] }";
    CHECK(source.LoadIndexFromJson(indexJsonV2, error));
    std::vector<SourceEntry> updates = source.CheckUpdates();
    CHECK(updates.size() == 1 && updates[0].id == "com.sai2.demo");

    // ---- 更新 happy path ----
    const SourceEntry* demoV2 = source.Find("com.sai2.demo");
    CHECK(source.InstallOrUpdate(*demoV2, dl, cacheDir.string(), error));
    registry.Scan();
    const PluginRecord* rec = registry.Find("com.sai2.demo");
    CHECK(rec != nullptr && rec->manifest.version == "1.1.0");

    // 更新后 CheckUpdates 为空
    CHECK(source.CheckUpdates().empty());

    // ---- 降级/同版本更新：拒绝（注意：重新 LoadIndex 后需重新 Find，
    //      旧指针会因 entries_ 重建而悬垂）----
    const SourceEntry* demoCurrent = source.Find("com.sai2.demo");
    CHECK(demoCurrent != nullptr && demoCurrent->version == "1.1.0");
    CHECK(!source.InstallOrUpdate(*demoCurrent, dl, cacheDir.string(), error));

    // ---- 清理 ----
    fs::remove_all(workDir, ec);
    return true;
}

} // namespace sai2test
