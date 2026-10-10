#pragma once
// ============================================================================
// PathUtil.h - UTF-8 字符串 → 文件系统路径
//
// 为什么需要它：Windows 上 std::filesystem::path 的**窄字符构造/赋值按 ANSI
// 代码页解释**（中文系统是 GBK）。本工程内部一律用 UTF-8 std::string 传路径，
// 一旦路径里有非 ASCII 字符（例如安装目录叫「SAI2插件测试包」），
// fs::exists / directory_iterator / ifstream 全部会失败——现象是"插件目录明明
// 有文件却扫不到、日志也不写"。
//
// 所以凡是"把 std::string 当路径用"的地方，都要先过一遍 PathFromUtf8()。
// ============================================================================

#include <filesystem>
#include <string>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#endif

namespace sai2::host {

#if defined(_WIN32)
// UTF-8（或已是宽字符）→ std::filesystem::path（宽字符）
inline std::filesystem::path PathFromUtf8(const std::string& s) {
    if (s.empty()) return std::filesystem::path();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                      static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::filesystem::path(s); // 非法 UTF-8：保持原样
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        w.data(), n);
    return std::filesystem::path(w);
}

inline std::filesystem::path PathFromUtf8(const std::wstring& s) {
    return std::filesystem::path(s);
}

// std::filesystem::path → UTF-8 std::string（日志/JSON 里显示用）
inline std::string PathToUtf8(const std::filesystem::path& p) {
    const std::wstring w = p.wstring();
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
                                      static_cast<int>(w.size()), nullptr, 0,
                                      nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                        out.data(), n, nullptr, nullptr);
    return out;
}
#else
inline std::filesystem::path PathFromUtf8(const std::string& s) {
    return std::filesystem::path(s);
}
inline std::filesystem::path PathFromUtf8(const std::wstring& s) {
    return std::filesystem::path(s);
}
inline std::string PathToUtf8(const std::filesystem::path& p) {
    return p.string();
}
#endif

} // namespace sai2::host
