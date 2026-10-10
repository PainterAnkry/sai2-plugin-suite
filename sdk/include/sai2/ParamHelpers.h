#pragma once
// ============================================================================
// ParamHelpers.h - 宿主参数通道读取辅助（Plugin API 1.1）
//
// 插件通过 IHostContext::GetParam 读取宿主下发的参数；本头文件把字符串
// 解析成常用类型，并在键缺失/格式非法时返回调用方给出的默认值。
// 这样"插件默认行为"与"宿主面板驱动"共用一条代码路径，缺少参数通道的
// 宿主（返回 nullptr）自然退化成默认参数。
// ============================================================================

#include "sai2/IHostContext.h"

#include <cstdlib>
#include <string>

namespace sai2 {

inline std::string GetParamString(IHostContext& host, const char* key,
                                  const char* def = "") {
    const char* v = host.GetParam(key);
    if (v && *v) return std::string(v);
    return std::string(def ? def : "");
}

inline float GetParamFloat(IHostContext& host, const char* key, float def) {
    const char* v = host.GetParam(key);
    if (!v || !*v) return def;
    char* end = nullptr;
    const double d = std::strtod(v, &end);
    if (end == v) return def;
    return static_cast<float>(d);
}

inline int GetParamInt(IHostContext& host, const char* key, int def) {
    const char* v = host.GetParam(key);
    if (!v || !*v) return def;
    char* end = nullptr;
    const long d = std::strtol(v, &end, 10);
    if (end == v) return def;
    return static_cast<int>(d);
}

inline bool GetParamBool(IHostContext& host, const char* key, bool def) {
    const char* v = host.GetParam(key);
    if (!v || !*v) return def;
    const std::string s = v;
    if (s == "0" || s == "false" || s == "no") return false;
    if (s == "1" || s == "true" || s == "yes") return true;
    return def;
}

} // namespace sai2
