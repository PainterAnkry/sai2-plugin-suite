#pragma once
// ============================================================================
// GalleryParams.h - 解析 photocraft 的"参数记法"字符串
//
// 记法（来自 photocraft crates/algo/src/artistic.rs 的 gallery!{...}）：
//   {"pencilWidth":1..24=4,            -> Range  最小 1 最大 24 默认 4
//    "brushType":"simple|lightRough",  -> Choice 选项，**第一个是默认**
//    "invert":bool,                    -> Bool（默认 false）
//    "glowColor":json}                 -> 暂不支持（颜色/JSON），直接跳过
//
// 为什么用它：这套记法自带 min/max/默认值/枚举项，正好能自动生成插件侧的
// DefParam(key,label,def,min,max,isInt)，于是 47 个 Photoshop 滤镜**不需要
// 手写 47 个包装类** —— 一张规格表（GallerySpecs.h）+ 这个解析器 + 算法即可。
//
// 解析失败不抛异常：能解析出几个算几个，调用方可用 ParseGalleryParamsDiag
// 拿到诊断串（测试里断言"0 错误"）。
// ============================================================================

#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

namespace sai2::fg {

struct GalleryParamSpec {
    enum Kind { Range, Choice, Bool };
    Kind kind = Range;
    std::string key;             // 参数名（camelCase，与源一致）
    std::string label;           // 显示名；默认同 key（后续可本地化）
    // Range
    float min = 0.0f;
    float max = 0.0f;
    float def = 0.0f;            // Range 的默认值
    bool isInt = false;          // 记法里 min/max/默认都是整数 → 走整数滑块
    // Choice
    std::vector<std::string> choices;
    int choiceDefault = 0;       // 记法规定第一个选项为默认
    // Bool
    bool boolDefault = false;
};

namespace galleryparamdetail {

inline void SkipSpace(const std::string& s, size_t& i) {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
}

// 手写扫描而不是直接 strtof：C 的 strtof 会把 "1..24=4" 里的 "1." 当成合法浮点
// **连同那个点一起吃掉**，于是紧接着看到的是 ".24" 而不是 ".." → 误报语法错误。
// （这个坑是 tests/gallery_specs 抓出来的。）规则：可选的符号 → 数字 →
// 小数点后跟数字才当小数（后面是第二个 '.' 就停，交给 ".." 处理）。
inline bool ParseNumber(const std::string& s, size_t& i, float& out) {
    SkipSpace(s, i);
    const size_t start = i;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
    bool any = false;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
        ++i;
        any = true;
    }
    if (i < s.size() && s[i] == '.' &&
        !(i + 1 < s.size() && s[i + 1] == '.')) {
        ++i;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
            ++i;
            any = true;
        }
    }
    if (!any) {
        i = start;
        return false;
    }
    out = std::strtof(s.substr(start, i - start).c_str(), nullptr);
    return true;
}

inline bool LooksInt(const std::string& s, size_t from, size_t to) {
    for (size_t k = from; k < to; ++k) {
        const char c = s[k];
        if (c == '.' || c == 'e' || c == 'E') return false;
    }
    return true;
}

inline std::vector<std::string> SplitPipe(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        const size_t bar = s.find('|', start);
        if (bar == std::string::npos) {
            out.push_back(s.substr(start));
            break;
        }
        out.push_back(s.substr(start, bar - start));
        start = bar + 1;
    }
    return out;
}

} // namespace galleryparamdetail

// 解析一段参数记法；同时把"跳过了什么"写进 diag（可为 nullptr）。
inline std::vector<GalleryParamSpec> ParseGalleryParamsDiag(const std::string& s,
                                                            std::string* diag) {
    using namespace galleryparamdetail;
    std::vector<GalleryParamSpec> out;
    size_t i = s.find('{');
    if (i == std::string::npos) {
        if (diag) *diag += "no-brace;";
        return out;
    }
    ++i;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ',' || std::isspace(static_cast<unsigned char>(s[i]))))
            ++i;
        if (i >= s.size() || s[i] == '}') break;
        if (s[i] != '"') {
            if (diag) *diag += "bad-key-quote;";
            break;
        }
        const size_t k0 = ++i;
        const size_t k1 = s.find('"', k0);
        if (k1 == std::string::npos) {
            if (diag) *diag += "unterminated-key;";
            break;
        }
        GalleryParamSpec p;
        p.key = s.substr(k0, k1 - k0);
        p.label = p.key;
        i = k1 + 1;
        SkipSpace(s, i);
        if (i >= s.size() || s[i] != ':') {
            if (diag) *diag += "missing-colon:" + p.key + ";";
            break;
        }
        ++i;
        SkipSpace(s, i);

        if (s.compare(i, 4, "bool") == 0) {
            p.kind = GalleryParamSpec::Bool;
            p.boolDefault = false;
            i += 4;
            out.push_back(std::move(p));
            continue;
        }
        if (s.compare(i, 4, "json") == 0) {
            // 颜色等 JSON 参数：本插件暂无对应控件 → 跳过（记录以便统计）
            if (diag) *diag += "skip-json:" + p.key + ";";
            i += 4;
            continue;
        }
        if (i < s.size() && s[i] == '"') {
            const size_t c0 = ++i;
            const size_t c1 = s.find('"', c0);
            if (c1 == std::string::npos) {
                if (diag) *diag += "unterminated-choice:" + p.key + ";";
                break;
            }
            p.kind = GalleryParamSpec::Choice;
            p.choices = SplitPipe(s.substr(c0, c1 - c0));
            p.choiceDefault = 0;   // 源注释：第一个选项是默认
            i = c1 + 1;
            out.push_back(std::move(p));
            continue;
        }

        // 数值范围 min..max[=def]
        const size_t n0 = i;
        float lo = 0.0f, hi = 0.0f, d = 0.0f;
        if (!ParseNumber(s, i, lo)) {
            if (diag) *diag += "bad-min:" + p.key + ";";
            break;
        }
        const size_t n1 = i;
        SkipSpace(s, i);
        if (s.compare(i, 2, "..") != 0) {
            if (diag) *diag += "missing-dotdot:" + p.key + ";";
            break;
        }
        i += 2;
        if (!ParseNumber(s, i, hi)) {
            if (diag) *diag += "bad-max:" + p.key + ";";
            break;
        }
        const size_t n2 = i;
        d = lo;
        SkipSpace(s, i);
        if (i < s.size() && s[i] == '=') {
            ++i;
            if (!ParseNumber(s, i, d)) {
                if (diag) *diag += "bad-default:" + p.key + ";";
                break;
            }
        }
        const size_t n3 = i;
        p.kind = GalleryParamSpec::Range;
        p.min = lo;
        p.max = hi;
        p.def = d;
        p.isInt = LooksInt(s, n0, n1) && LooksInt(s, n1 + 2, n2) &&
                  LooksInt(s, n2, n3);
        out.push_back(std::move(p));
    }
    return out;
}

inline std::vector<GalleryParamSpec> ParseGalleryParams(const std::string& s) {
    return ParseGalleryParamsDiag(s, nullptr);
}

} // namespace sai2::fg
