#pragma once
// ============================================================================
// Json.h - 零依赖的极简 JSON 解析（用于 manifest.json / registry.json / index.json）
//
// 支持：object / array / string（含 \uXXXX 转义 -> UTF-8）/ number / true / false / null
// ============================================================================

#include <string>
#include <utility>
#include <vector>

namespace sai2::host {

struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string str;
    std::vector<JsonValue> array;
    // 用 vector<pair> 保存，保持键顺序
    std::vector<std::pair<std::string, JsonValue>> object;

    const JsonValue* Find(const std::string& key) const;
    std::string AsString(const std::string& def = {}) const;
    double AsNumber(double def = 0.0) const;
    bool AsBool(bool def = false) const;

    bool IsNull() const { return type == Type::Null; }
    bool IsString() const { return type == Type::String; }
    bool IsNumber() const { return type == Type::Number; }
    bool IsObject() const { return type == Type::Object; }
    bool IsArray() const { return type == Type::Array; }
};

class JsonParser {
public:
    static bool Parse(const std::string& text, JsonValue& out, std::string& error);
};

// 字符串转义为 JSON 字符串字面量（含引号）
std::string JsonEscape(const std::string& s);

} // namespace sai2::host
