#include "sai2/host/Json.h"

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace sai2::host {

// ---------------------------------------------------------------------------
// JsonValue
// ---------------------------------------------------------------------------
const JsonValue* JsonValue::Find(const std::string& key) const {
    if (type != Type::Object) return nullptr;
    for (const auto& kv : object)
        if (kv.first == key) return &kv.second;
    return nullptr;
}

std::string JsonValue::AsString(const std::string& def) const {
    return type == Type::String ? str : def;
}

double JsonValue::AsNumber(double def) const {
    return type == Type::Number ? number : def;
}

bool JsonValue::AsBool(bool def) const {
    return type == Type::Bool ? boolean : def;
}

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------
namespace {

struct Parser {
    const char* p = nullptr;
    const char* end = nullptr;
    std::string error;

    bool Fail(const std::string& msg) {
        if (error.empty()) error = msg;
        return false;
    }

    void SkipWs() {
        while (p < end &&
               (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
            ++p;
    }

    bool ParseValue(JsonValue& out, int depth) {
        if (depth > 64) return Fail("JSON 嵌套过深");
        SkipWs();
        if (p >= end) return Fail("JSON 意外结束");
        const char c = *p;
        if (c == '{') return ParseObject(out, depth);
        if (c == '[') return ParseArray(out, depth);
        if (c == '"') {
            out.type = JsonValue::Type::String;
            return ParseString(out.str);
        }
        if (c == 't') return ParseLiteral("true", out, true);
        if (c == 'f') return ParseLiteral("false", out, false);
        if (c == 'n') return ParseNull(out);
        return ParseNumber(out);
    }

    bool ParseLiteral(const char* lit, JsonValue& out, bool value) {
        for (const char* q = lit; *q; ++q, ++p) {
            if (p >= end || *p != *q) return Fail("JSON 字面量错误");
        }
        out.type = JsonValue::Type::Bool;
        out.boolean = value;
        return true;
    }

    bool ParseNull(JsonValue& out) {
        if (end - p < 4 || p[1] != 'u' || p[2] != 'l' || p[3] != 'l')
            return Fail("JSON null 错误");
        p += 4;
        out.type = JsonValue::Type::Null;
        return true;
    }

    bool ParseNumber(JsonValue& out) {
        const char* start = p;
        if (p < end && (*p == '-' || *p == '+')) ++p;
        bool digits = false;
        while (p < end && std::isdigit(static_cast<unsigned char>(*p))) {
            ++p; digits = true;
        }
        if (p < end && *p == '.') {
            ++p;
            while (p < end && std::isdigit(static_cast<unsigned char>(*p))) ++p;
        }
        if (p < end && (*p == 'e' || *p == 'E')) {
            ++p;
            if (p < end && (*p == '-' || *p == '+')) ++p;
            while (p < end && std::isdigit(static_cast<unsigned char>(*p))) ++p;
        }
        if (!digits) return Fail("JSON 数字错误");
        out.type = JsonValue::Type::Number;
        out.number = std::strtod(std::string(start, p).c_str(), nullptr);
        return true;
    }

    static void AppendUtf8(std::string& s, uint32_t cp) {
        if (cp < 0x80) {
            s.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            s.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool ParseHex4(uint32_t& out) {
        if (end - p < 4) return Fail("\\u 转义不完整");
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i, ++p) {
            const char c = *p;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
            else return Fail("\\u 转义非法");
        }
        out = v;
        return true;
    }

    bool ParseString(std::string& out) {
        if (p >= end || *p != '"') return Fail("JSON 字符串错误");
        ++p;
        out.clear();
        while (p < end) {
            const unsigned char c = static_cast<unsigned char>(*p);
            if (c == '"') { ++p; return true; }
            if (c == '\\') {
                ++p;
                if (p >= end) return Fail("转义不完整");
                switch (*p) {
                    case '"': out.push_back('"'); ++p; break;
                    case '\\': out.push_back('\\'); ++p; break;
                    case '/': out.push_back('/'); ++p; break;
                    case 'b': out.push_back('\b'); ++p; break;
                    case 'f': out.push_back('\f'); ++p; break;
                    case 'n': out.push_back('\n'); ++p; break;
                    case 'r': out.push_back('\r'); ++p; break;
                    case 't': out.push_back('\t'); ++p; break;
                    case 'u': {
                        ++p;
                        uint32_t cp = 0;
                        if (!ParseHex4(cp)) return false;
                        // 代理对
                        if (cp >= 0xD800 && cp <= 0xDBFF && end - p >= 6 &&
                            p[0] == '\\' && p[1] == 'u') {
                            p += 2;
                            uint32_t lo = 0;
                            if (!ParseHex4(lo)) return false;
                            if (lo >= 0xDC00 && lo <= 0xDFFF)
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        }
                        AppendUtf8(out, cp);
                        break;
                    }
                    default: return Fail("非法转义字符");
                }
            } else {
                out.push_back(static_cast<char>(c));
                ++p;
            }
        }
        return Fail("字符串未闭合");
    }

    bool ParseArray(JsonValue& out, int depth) {
        out.type = JsonValue::Type::Array;
        ++p; // '['
        SkipWs();
        if (p < end && *p == ']') { ++p; return true; }
        while (true) {
            JsonValue item;
            if (!ParseValue(item, depth + 1)) return false;
            out.array.push_back(std::move(item));
            SkipWs();
            if (p < end && *p == ',') { ++p; continue; }
            if (p < end && *p == ']') { ++p; return true; }
            return Fail("数组缺少 , 或 ]");
        }
    }

    bool ParseObject(JsonValue& out, int depth) {
        out.type = JsonValue::Type::Object;
        ++p; // '{'
        SkipWs();
        if (p < end && *p == '}') { ++p; return true; }
        while (true) {
            SkipWs();
            std::string key;
            if (!ParseString(key)) return false;
            SkipWs();
            if (p >= end || *p != ':') return Fail("对象缺少 :");
            ++p;
            JsonValue value;
            if (!ParseValue(value, depth + 1)) return false;
            out.object.emplace_back(std::move(key), std::move(value));
            SkipWs();
            if (p < end && *p == ',') { ++p; continue; }
            if (p < end && *p == '}') { ++p; return true; }
            return Fail("对象缺少 , 或 }");
        }
    }
};

} // namespace

bool JsonParser::Parse(const std::string& text, JsonValue& out,
                       std::string& error) {
    Parser parser;
    parser.p = text.data();
    parser.end = text.data() + text.size();
    const bool ok = parser.ParseValue(out, 0);
    if (ok) {
        parser.SkipWs();
        if (parser.p != parser.end) {
            error = "JSON 尾部有多余内容";
            return false;
        }
    } else {
        error = parser.error;
    }
    return ok;
}

std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (const unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
    return out;
}

} // namespace sai2::host
