// ============================================================================
// AgentMain.cpp - sai2_agent.dll 入口：DllMain + 命名管道服务线程
//
// 注入后（LoadLibraryW 远程线程）：
//   DLL_PROCESS_ATTACH → 起工作线程（不在 DllMain 里做任何重活）
//   工作线程：AgentCore::Startup（装 IAT 钩子）→ 管道循环
//   DLL_PROCESS_DETACH（FreeLibrary 卸载）：置退出标志 + 自连管道发 __exit
//   唤醒服务循环 → 等线程收尾 → AgentCore::Shutdown（恢复 IAT）→ 卸载
//
// 管道用【同步 I/O】：zig/clang(windows-gnu) 工具链下 OVERLAPPED 管道 I/O
// 实测异常（连接事件不触发、写入报 ERROR_PIPE_NOT_CONNECTED），同步模式
// 工作正常。协议是短连接（连上→发一行→收一行→断开），同步阻塞足够；
// 多客户端并发时后来者在 Connect 里等空闲实例（ERROR_PIPE_BUSY 重试）。
//
// 环境变量（injector 注入前设置，远程线程继承）：
//   SAI2_AGENT_OUT  日志/缩略图目录（默认 = DLL 所在目录）
//   SAI2_AGENT_PIPE 管道名（默认 \\.\pipe\sai2agent）
// ============================================================================

#include "AgentCore.h"
#include "IATHook.h"

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

EXTERN_C IMAGE_DOS_HEADER __ImageBase;

namespace sai2agent {
namespace {

AgentCore g_agent;
HANDLE g_workerThread = nullptr;
volatile LONG g_exitRequested = 0;

void PipeLog(const char* fmt, ...) {
    static FILE* f = nullptr;
    if (!f) {
        wchar_t path[MAX_PATH];
        wchar_t out[512] = {0};
        GetEnvironmentVariableW(L"SAI2_AGENT_OUT", out, 512);
        if (!out[0]) {
            GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), path,
                               MAX_PATH);
            wchar_t* slash = wcsrchr(path, L'\\');
            if (slash) *slash = L'\0';
            swprintf(out, 512, L"%s", path);
        }
        swprintf(path, MAX_PATH, L"%s\\sai2_agent_pipe_%lu.log", out,
                 GetCurrentProcessId());
        f = _wfopen(path, L"a");
        if (!f) return;
    }
    va_list ap;
    va_start(ap, fmt);
    char line[512];
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "[%02u:%02u:%02u.%03u] %s\n", st.wHour, st.wMinute, st.wSecond,
            st.wMilliseconds, line);
    fflush(f);
}

const wchar_t* PipeName() {
    static wchar_t name[256] = {0};
    if (!name[0]) {
        if (GetEnvironmentVariableW(L"SAI2_AGENT_PIPE", name, 256) == 0 ||
            !name[0]) {
            wcscpy_s(name, kDefaultPipeName);
        }
    }
    return name;
}

// 服务单个客户端连接；返回 false = 收到退出请求。
// 同步 I/O：客户端必然"发一行→收一行→断开"，阻塞读写即可。
bool ServeClient(HANDLE pipe) {
    std::string req;
    char buf[4096];
    DWORD n = 0;
    for (;;) {
        if (!ReadFile(pipe, buf, sizeof(buf), &n, nullptr)) return true;
        if (n == 0) return true;
        req.append(buf, n);
        const size_t nl = req.find('\n');
        if (nl != std::string::npos) {
            req = req.substr(0, nl);
            break;
        }
        if (req.size() > (1u << 20)) return true; // 异常客户端
    }

    PipeLog("cmd: %s", req.c_str());
    if (req.find("__exit") != std::string::npos) return false; // 卸载唤醒

    std::string resp;
    g_agent.HandleCommand(req, resp);
    if (req.find("__exit") == std::string::npos)
        g_agent.TouchBridgeSeen(); // 心跳：桥还活着（工具栏据此显隐）
    resp += '\n';
    DWORD w = 0;
    WriteFile(pipe, resp.data(), static_cast<DWORD>(resp.size()), &w, nullptr);
    FlushFileBuffers(pipe);
    return true;
}

void PipeServerLoop() {
    PipeLog("loop start, pipe name: %ls", PipeName());
    for (;;) {
        HANDLE pipe = CreateNamedPipeW(
            PipeName(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES, 1 << 16, 1 << 16, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            PipeLog("CreateNamedPipeW FAIL err=%lu", GetLastError());
            Sleep(300);
            continue;
        }
        const BOOL connected = ConnectNamedPipe(pipe, nullptr);
        if (!connected && GetLastError() != ERROR_PIPE_CONNECTED) {
            CloseHandle(pipe);
            Sleep(100);
            continue;
        }
        const bool keep = ServeClient(pipe);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
        if (!keep || g_agent.ExitRequested()) {
            PipeLog("loop exit");
            return;
        }
    }
}

std::wstring EnvOrEmpty(const wchar_t* key) {
    wchar_t buf[1024] = {0};
    if (GetEnvironmentVariableW(key, buf, 1024) > 0) return buf;
    return std::wstring();
}

DWORD WINAPI AgentThreadProc(LPVOID) {
    std::wstring error;
    if (!g_agent.Startup(EnvOrEmpty(L"SAI2_AGENT_OUT"), error)) {
        // 启动失败：没有钩子就没有数据，保留进程存活但不服务
        return 1;
    }
    g_agent.StartToolbar(); // SAI2 内浮动「插件」入口
    PipeServerLoop();
    g_agent.Shutdown();
    return 0;
}

// 唤醒阻塞在 ConnectNamedPipe 上的服务线程：自连一次并发 __exit
void WakeServerViaSelfConnect() {
    for (int i = 0; i < 30; ++i) {
        HANDLE h = CreateFileW(PipeName(), GENERIC_READ | GENERIC_WRITE, 0,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_PIPE_BUSY) {
                WaitNamedPipeW(PipeName(), 200);
                continue;
            }
            Sleep(50);
            continue;
        }
        const char* cmd = "{\"cmd\":\"__exit\"}\n";
        DWORD w = 0;
        WriteFile(h, cmd, static_cast<DWORD>(std::strlen(cmd)), &w, nullptr);
        // 服务端写回响应或直接断开都行；读一下避免其 FlushFileBuffers 阻塞
        char buf[256];
        DWORD n = 0;
        ReadFile(h, buf, sizeof(buf), &n, nullptr);
        CloseHandle(h);
        return;
    }
}

} // namespace
} // namespace sai2agent

using namespace sai2agent;

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(module);
        g_workerThread = CreateThread(nullptr, 0, AgentThreadProc, nullptr, 0,
                                      nullptr);
        break;

    case DLL_PROCESS_DETACH:
        if (reserved != nullptr) break; // 进程整体退出，无需清理
        InterlockedExchange(&g_exitRequested, 1);
        g_agent.RequestExit();
        g_agent.StopToolbarAndWait(); // 先停浮窗线程（它还在跑 DLL 代码）
        WakeServerViaSelfConnect();   // 唤醒阻塞的 ConnectNamedPipe
        // 等工作线程收尾（最多 6s），避免 FreeLibrary 后钩子代码被卸载
        if (g_workerThread) {
            const DWORD t0 = GetTickCount();
            while (WaitForSingleObject(g_workerThread, 50) == WAIT_TIMEOUT) {
                if (GetTickCount() - t0 > 6000) break;
            }
            CloseHandle(g_workerThread);
            g_workerThread = nullptr;
        }
        break;
    }
    return TRUE;
}
