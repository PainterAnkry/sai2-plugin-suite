// ============================================================================
// Diag.cpp - 诊断实现
// ============================================================================

#include "Diag.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <windows.h>

namespace sai2::bridge::ui {

namespace {

std::wstring g_path;       // 诊断日志全路径（宽字符，避免中文目录）
bool g_pathReady = false;
bool g_enabled = false;    // 默认关闭：只有 SAI2_BRIDGE_DIAG=1 才写文件
volatile LONG g_inPaint = 0;
char g_stage[64] = {0};

// 用宽字符 API 打开/写文件：路径可能含中文（见 host/PathUtil.h 的说明）
void AppendLine(const std::string& line) {
    if (!g_enabled || !g_pathReady || g_path.empty()) return;
    HANDLE f = CreateFileW(g_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[1024];
    const int n = std::snprintf(buf, sizeof(buf), "[%02u:%02u:%02u.%03u] %s\r\n",
                                st.wHour, st.wMinute, st.wSecond,
                                st.wMilliseconds, line.c_str());
    DWORD w = 0;
    if (n > 0)
        WriteFile(f, buf, static_cast<DWORD>(n < (int)sizeof(buf) ? n
                                                                 : (int)sizeof(buf) - 1),
                  &w, nullptr);
    CloseHandle(f);
}

void BuildPath(const std::string& exeDir) {
    if (g_pathReady) return;
    const std::string full = exeDir + "\\logs\\ui-diag.log";
    const int n = MultiByteToWideChar(CP_UTF8, 0, full.c_str(),
                                      static_cast<int>(full.size()), nullptr, 0);
    if (n > 0) {
        g_path.assign(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, full.c_str(),
                            static_cast<int>(full.size()), g_path.data(), n);
        // 目录不存在就建（同样走宽字符）
        std::wstring dir(g_path);
        const size_t p = dir.find_last_of(L'\\');
        if (p != std::wstring::npos) {
            dir.resize(p);
            CreateDirectoryW(dir.c_str(), nullptr);
        }
    }
    g_pathReady = true;
}

LONG WINAPI CrashHandler(EXCEPTION_POINTERS* info) {
    char buf[256];
    const DWORD code = info && info->ExceptionRecord
                           ? info->ExceptionRecord->ExceptionCode
                           : 0;
    const void* addr = info && info->ExceptionRecord
                           ? info->ExceptionRecord->ExceptionAddress
                           : nullptr;
    std::snprintf(buf, sizeof(buf),
                  "!! 未捕获异常 code=0x%08lX addr=%p inPaint=%ld stage=%s",
                  static_cast<unsigned long>(code), addr,
                  static_cast<long>(g_inPaint), g_stage);
    AppendLine(buf);
    return EXCEPTION_EXECUTE_HANDLER; // 落盘后按默认方式终止
}

void TerminateHandler() {
    AppendLine("!! std::terminate（未捕获的 C++ 异常 / 纯虚调用等）");
    std::abort();
}

} // namespace

void InstallCrashDiagnostics(const std::string& exeDir) {
    wchar_t flag[8] = {0};
    g_enabled = GetEnvironmentVariableW(L"SAI2_BRIDGE_DIAG", flag, 8) > 0 &&
                flag[0] == L'1';
    if (!g_enabled) return;
    BuildPath(exeDir);
    SetUnhandledExceptionFilter(CrashHandler);
    std::set_terminate(TerminateHandler);
    AppendLine("---- diagnostics installed ----");
}

void Diag(const std::string& line) { AppendLine(line); }

void DiagEnter(const char* stage) {
    InterlockedExchange(&g_inPaint, 1);
    std::snprintf(g_stage, sizeof(g_stage), "%s", stage ? stage : "?");
    AppendLine(std::string("enter ") + g_stage);
}

void DiagLeave(const char* stage) {
    AppendLine(std::string("leave ") + (stage ? stage : "?"));
    InterlockedExchange(&g_inPaint, 0);
    g_stage[0] = '\0';
}

bool InPaint() { return g_inPaint != 0; }

} // namespace sai2::bridge::ui
