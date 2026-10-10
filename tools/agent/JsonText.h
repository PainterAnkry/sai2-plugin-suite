#pragma once
// ============================================================================
// JsonText.h - Agent 侧的极简 JSON 输出与解析（零依赖）
// ============================================================================

#include <cstdint>
#include <string>
#include <vector>

namespace sai2agent {

// ---- 输出 ----------------------------------------------------------------

inline void JsonEscapeInto(const std::wstring& s, std::string& out) {
    // wstring → UTF-8 → JSON 转义
    std::string u8;
    if (!s.empty()) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(),
                                          static_cast<int>(s.size()), nullptr,
                                          0, nullptr, nullptr);
        if (n > 0) {
            u8.resize(static_cast<size_t>(n));
            WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                                &u8[0], n, nullptr, nullptr);
        }
    }
    out += '"';
    for (unsigned char c : u8) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    out += '"';
}

// 仅用于窄字符串（已是 UTF-8 或 ASCII，比如路径）
inline void JsonEscapeInto(const std::string& s, std::string& out) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    out += '"';
}

inline void JsonNum(long long v, std::string& out) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld", v);
    out += buf;
}

// 提取 "key":[ ... ] 数组里的每个对象原文（子串），供逐个 JsonParseObject
inline std::vector<std::string> JsonArrayObjects(const std::string& json,
                                                 const char* key) {
    std::vector<std::string> out;
    const std::string pat = "\"" + std::string(key) + "\":[";
    size_t keyPos = json.find(pat);
    if (keyPos == std::string::npos) return out;
    const size_t arrBegin = keyPos + pat.size();
    size_t end = arrBegin;
    int depth = 1;
    bool inStr = false;
    for (size_t i = arrBegin; i < json.size(); ++i) {
        const char c = json[i];
        if (inStr) {
            if (c == '\\') ++i;
            else if (c == '"') inStr = false;
            continue;
        }
        if (c == '"') inStr = true;
        else if (c == '[') ++depth;
        else if (c == ']') {
            if (--depth == 0) { end = i; break; }
        }
    }
    size_t cur = arrBegin;
    while (cur < end) {
        while (cur < end && json[cur] != '{') ++cur;
        if (cur >= end) break;
        int d = 0;
        bool inS = false;
        size_t objEnd = cur;
        for (size_t i = cur; i < end; ++i) {
            const char c = json[i];
            if (inS) {
                if (c == '\\') ++i;
                else if (c == '"') inS = false;
                continue;
            }
            if (c == '"') inS = true;
            else if (c == '{') ++d;
            else if (c == '}') {
                if (--d == 0) { objEnd = i + 1; break; }
            }
        }
        out.push_back(json.substr(cur, objEnd - cur));
        cur = objEnd;
    }
    return out;
}

// UTF-8 → UTF-16
inline std::wstring U8ToWide(const std::string& s) {
    std::wstring out;
    if (s.empty()) return out;
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                      static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return out;
    out.resize(static_cast<size_t>(n));
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        &out[0], n);
    return out;
}

// ---- 解析（只支持一层对象的 string/number/bool，够命令协议用） -----------

struct JsonVal {
    bool isStr = false, isNum = false, isBool = false;
    std::wstring str;
    long long num = 0;
    bool boolean = false;
};

inline bool JsonParseObject(const std::string& json,
                            std::vector<std::pair<std::wstring, JsonVal>>& out) {
    size_t i = 0;
    const size_t n = json.size();
    auto skipWs = [&] {
        while (i < n && (json[i] == ' ' || json[i] == '\t' || json[i] == '\r' ||
                         json[i] == '\n'))
            ++i;
    };
    skipWs();
    if (i >= n || json[i] != '{') return false;
    ++i;
    skipWs();
    if (i < n && json[i] == '}') return true;
    while (i < n) {
        skipWs();
        if (i >= n || json[i] != '"') return false;
        ++i;
        std::string keyU8;
        while (i < n && json[i] != '"') {
            if (json[i] == '\\' && i + 1 < n) {
                ++i;
                switch (json[i]) {
                case 'n': keyU8 += '\n'; break;
                case 't': keyU8 += '\t'; break;
                case 'r': keyU8 += '\r'; break;
                case '"': keyU8 += '"'; break;
                case '\\': keyU8 += '\\'; break;
                case '/': keyU8 += '/'; break;
                case 'u': {
                    if (i + 4 < n) {
                        unsigned v = 0;
                        for (int k = 0; k < 4; ++k) {
                            const char h = json[++i];
                            v <<= 4;
                            if (h >= '0' && h <= '9') v |= unsigned(h - '0');
                            else if (h >= 'a' && h <= 'f') v |= unsigned(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') v |= unsigned(h - 'A' + 10);
                        }
                        if (v < 0x80) {
                            keyU8 += static_cast<char>(v);
                        } else if (v < 0x800) {
                            keyU8 += static_cast<char>(0xC0 | (v >> 6));
                            keyU8 += static_cast<char>(0x80 | (v & 0x3F));
                        } else {
                            keyU8 += static_cast<char>(0xE0 | (v >> 12));
                            keyU8 += static_cast<char>(0x80 | ((v >> 6) & 0x3F));
                            keyU8 += static_cast<char>(0x80 | (v & 0x3F));
                        }
                    }
                    break;
                }
                default: keyU8 += json[i]; break;
                }
                ++i;
            } else {
                keyU8 += json[i++];
            }
        }
        ++i; // 跳过收尾引号
        skipWs();
        if (i >= n || json[i] != ':') return false;
        ++i;
        skipWs();
        JsonVal v;
        if (i < n && json[i] == '"') {
            ++i;
            std::string valU8;
            while (i < n && json[i] != '"') {
                if (json[i] == '\\' && i + 1 < n) {
                    ++i;
                    switch (json[i]) {
                    case 'n': valU8 += '\n'; break;
                    case 't': valU8 += '\t'; break;
                    case 'r': valU8 += '\r'; break;
                    case '"': valU8 += '"'; break;
                    case '\\': valU8 += '\\'; break;
                    case '/': valU8 += '/'; break;
                    case 'u': {
                        if (i + 4 < n) {
                            unsigned v = 0;
                            for (int k = 0; k < 4; ++k) {
                                const char h = json[++i];
                                v <<= 4;
                                if (h >= '0' && h <= '9') v |= unsigned(h - '0');
                                else if (h >= 'a' && h <= 'f') v |= unsigned(h - 'a' + 10);
                                else if (h >= 'A' && h <= 'F') v |= unsigned(h - 'A' + 10);
                            }
                            if (v < 0x80) valU8 += static_cast<char>(v);
                            else if (v < 0x800) {
                                valU8 += static_cast<char>(0xC0 | (v >> 6));
                                valU8 += static_cast<char>(0x80 | (v & 0x3F));
                            } else {
                                valU8 += static_cast<char>(0xE0 | (v >> 12));
                                valU8 += static_cast<char>(0x80 | ((v >> 6) & 0x3F));
                                valU8 += static_cast<char>(0x80 | (v & 0x3F));
                            }
                        }
                        break;
                    }
                    default: valU8 += json[i]; break;
                    }
                    ++i;
                } else {
                    valU8 += json[i++];
                }
            }
            ++i;
            v.isStr = true;
            // UTF-8 → wstring
            const int wn = MultiByteToWideChar(
                CP_UTF8, 0, valU8.c_str(), static_cast<int>(valU8.size()),
                nullptr, 0);
            if (wn > 0) {
                v.str.resize(static_cast<size_t>(wn));
                MultiByteToWideChar(CP_UTF8, 0, valU8.c_str(),
                                    static_cast<int>(valU8.size()), &v.str[0],
                                    wn);
            }
        } else if (i < n && (json[i] == '-' ||
                             (json[i] >= '0' && json[i] <= '9'))) {
            const bool neg = json[i] == '-';
            if (neg) ++i;
            long long acc = 0;
            while (i < n && json[i] >= '0' && json[i] <= '9') {
                acc = acc * 10 + (json[i] - '0');
                ++i;
            }
            v.isNum = true;
            v.num = neg ? -acc : acc;
        } else if (i + 4 <= n && json.compare(i, 4, "true") == 0) {
            v.isBool = true; v.boolean = true; i += 4;
        } else if (i + 5 <= n && json.compare(i, 5, "false") == 0) {
            v.isBool = true; v.boolean = false; i += 5;
        } else {
            return false;
        }
        out.emplace_back(U8ToWide(keyU8), std::move(v));
        skipWs();
        if (i < n && json[i] == ',') { ++i; continue; }
        if (i < n && json[i] == '}') { ++i; return true; }
        return false;
    }
    return false;
}

} // namespace sai2agent
