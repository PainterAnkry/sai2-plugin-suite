#pragma once
// ============================================================================
// PluginAPI.h - SAI2 插件 API 版本定义与导出宏
//
// 所有插件与宿主共同依赖的基础约定。API 版本策略：
//   - 主版本(Major)变化 = 不兼容变更，宿主必须拒绝加载；
//   - 次版本(Minor)变化 = 向后兼容的新增接口，低次版本插件可运行在高次版本宿主上。
// ============================================================================

#include <cstdint>

#define SAI2_PLUGIN_API_VERSION_MAKE(major, minor) \
    ((static_cast<uint32_t>(major) << 16) | static_cast<uint32_t>(minor))

// 宿主当前支持的插件 API 版本
//   1.0 -> 1.1：新增 IHostContext::GetParam（宿主参数通道）。
//                虚函数追加在接口末尾且带默认实现，1.0 插件无需改动即可运行。
#define SAI2_PLUGIN_API_VERSION SAI2_PLUGIN_API_VERSION_MAKE(1, 1)

#if defined(_WIN32)
    #define SAI2_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
    #define SAI2_PLUGIN_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace sai2 {

inline constexpr uint32_t kPluginApiVersion = SAI2_PLUGIN_API_VERSION;

inline constexpr uint32_t MakeApiVersion(uint32_t major, uint32_t minor) {
    return (major << 16) | minor;
}
inline constexpr uint32_t ApiVersionMajor(uint32_t v) { return v >> 16; }
inline constexpr uint32_t ApiVersionMinor(uint32_t v) { return v & 0xFFFF; }

// 日志级别（宿主日志系统统一写入 logs/plugin.log）
enum class LogLevel : int {
    Debug = 0,
    Info = 1,
    Warning = 2,
    Error = 3,
};

inline const char* LogLevelName(LogLevel level) {
    switch (level) {
        case LogLevel::Debug:   return "DEBUG";
        case LogLevel::Info:    return "INFO";
        case LogLevel::Warning: return "WARN";
        case LogLevel::Error:   return "ERROR";
    }
    return "?";
}

} // namespace sai2
