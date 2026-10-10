#include "sai2/host/Manifest.h"
#include "sai2/host/Json.h"
#include "sai2/PluginAPI.h"

#include <fstream>
#include <sstream>

namespace sai2::host {

bool ParseVersion(const std::string& s, Version& out) {
    out = Version{};
    if (s.empty()) return false;
    uint32_t parts[3] = {0, 0, 0};
    int idx = 0;
    for (const char c : s) {
        if (c == '.') {
            if (++idx > 2) return false;
        } else if (c >= '0' && c <= '9') {
            parts[idx] = parts[idx] * 10 + static_cast<uint32_t>(c - '0');
            if (parts[idx] > 65535u) return false;
        } else {
            return false;
        }
    }
    out.major = parts[0];
    out.minor = parts[1];
    out.patch = parts[2];
    return true;
}

int CompareVersion(const Version& a, const Version& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    return 0;
}

namespace {

std::string ReadFileAll(const std::filesystem::path& path, std::string& error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = "无法打开文件: " + PathToUtf8(path);
        return {};
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace

bool ValidateManifest(PluginManifest& m) {
    m.error.clear();
    if (m.id.empty()) {
        m.error = "缺少必填字段 id";
    } else if (m.name.empty()) {
        m.error = "缺少必填字段 name";
    } else if (m.version.empty()) {
        m.error = "缺少必填字段 version";
    } else if (m.apiVersion.empty()) {
        m.error = "缺少必填字段 apiVersion";
    } else if (m.entry.empty()) {
        m.error = "缺少必填字段 entry";
    } else if (m.type.empty()) {
        m.error = "缺少必填字段 type";
    }
    if (!m.error.empty()) return false;

    // id 字符集
    for (const char c : m.id) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '-' ||
                        c == '_';
        if (!ok) {
            m.error = "id 含非法字符（允许 a-zA-Z0-9._-）";
            return false;
        }
    }
    Version v;
    if (!ParseVersion(m.version, v)) {
        m.error = "version 无法解析: " + m.version;
        return false;
    }
    Version av;
    if (!ParseVersion(m.apiVersion, av)) {
        m.error = "apiVersion 无法解析: " + m.apiVersion;
        return false;
    }
    if (!m.minHostVersion.empty()) {
        Version hv;
        if (!ParseVersion(m.minHostVersion, hv)) {
            m.error = "minHostVersion 无法解析: " + m.minHostVersion;
            return false;
        }
    }
    m.valid = true;
    return true;
}

bool ParseManifestText(const std::string& text, PluginManifest& out,
                       std::string& error) {
    out = PluginManifest{};
    JsonValue root;
    if (!JsonParser::Parse(text, root, error)) return false;
    if (!root.IsObject()) {
        error = "manifest 根节点必须是 JSON 对象";
        return false;
    }
    if (const JsonValue* v = root.Find("id")) out.id = v->AsString();
    if (const JsonValue* v = root.Find("name")) out.name = v->AsString();
    if (const JsonValue* v = root.Find("version")) out.version = v->AsString();
    if (const JsonValue* v = root.Find("apiVersion"))
        out.apiVersion = v->AsString();
    if (const JsonValue* v = root.Find("entry")) out.entry = v->AsString();
    if (const JsonValue* v = root.Find("type")) out.type = v->AsString();
    if (const JsonValue* v = root.Find("author")) out.author = v->AsString();
    if (const JsonValue* v = root.Find("description"))
        out.description = v->AsString();
    if (const JsonValue* v = root.Find("minHostVersion"))
        out.minHostVersion = v->AsString();
    if (const JsonValue* v = root.Find("sha256")) out.sha256 = v->AsString();
    if (const JsonValue* v = root.Find("signature"))
        out.signature = v->AsString();
    if (const JsonValue* v = root.Find("signerPubKey"))
        out.signerPubKey = v->AsString();

    if (!ValidateManifest(out)) {
        error = out.error;
        return false;
    }
    return true;
}

bool LoadManifestFile(const std::filesystem::path& path, PluginManifest& out,
                      std::string& error) {
    const std::string text = ReadFileAll(path, error);
    if (text.empty() && !error.empty()) return false;
    return ParseManifestText(text, out, error);
}

bool IsApiVersionCompatible(const std::string& pluginApiVersion,
                            uint32_t hostApiVersion) {
    Version pv;
    if (!ParseVersion(pluginApiVersion, pv)) return false;
    const uint32_t hostMajor = ApiVersionMajor(hostApiVersion);
    const uint32_t hostMinor = ApiVersionMinor(hostApiVersion);
    return pv.major == hostMajor && pv.minor <= hostMinor;
}

} // namespace sai2::host
