// ============================================================================
// InjectorMain.cpp - sai2_injector.exe
//
// 把 sai2_agent.dll 注入到目标进程（默认 sai2.exe），并通过命名管道与
// Agent 通信。所有"识别图层"的数据都从 Agent 查询得到。
//
// 命令：
//   inject  [--pid <n> | --exe <name>] [--launch <path>] [--dll <path>]
//   eject   [--exe <name> | --pid <n>]
//   status / layers / thumbs / text
//   cmd '<json>'                       —— 任意 Agent 命令
//   selftest                           —— FakeHost 端到端自检（不碰 SAI2）
//
// 注入方式：经典 LoadLibraryW 远程线程。SAI2 无任何自我保护（无反调试、
// 无代码完整性检查），此方式足够且最简单。卸载用远程 FreeLibrary，
// Agent 在 DllMain(DETACH) 里恢复 IAT 并等待工作线程收尾。
// ============================================================================

#include "../agent/AgentClient.h"

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>   // CommandLineToArgvW（取原始 Unicode 命令行参数）
#include <tlhelp32.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using sai2agent::AgentClient;

// ---------------------------------------------------------------------------
// 工具
// ---------------------------------------------------------------------------

std::wstring Utf8ToWide(const std::string& s) {
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

std::string WideToUtf8(const std::wstring& s) {
    std::string out;
    if (s.empty()) return out;
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(),
                                      static_cast<int>(s.size()), nullptr, 0,
                                      nullptr, nullptr);
    if (n <= 0) return out;
    out.resize(static_cast<size_t>(n));
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        &out[0], n, nullptr, nullptr);
    return out;
}

std::string ExeDir() {
    wchar_t buf[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring s(buf);
    const size_t p = s.find_last_of(L"\\/");
    if (p != std::wstring::npos) s = s.substr(0, p);
    return WideToUtf8(s);
}

DWORD FindProcessIdByName(const wchar_t* exeName) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, exeName) == 0) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

std::string LastWinErr(const char* what) {
    char buf[256];
    const DWORD e = GetLastError();
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, nullptr, e, 0, buf, sizeof(buf),
                   nullptr);
    for (char* p = buf; *p; ++p)
        if (*p == '\r' || *p == '\n') *p = ' ';
    std::string s = std::string(what) + " 失败 (Win32 " +
                    std::to_string(static_cast<unsigned long long>(e)) + "): " +
                    buf;
    return s;
}

// ---------------------------------------------------------------------------
// 注入 / 卸载
// ---------------------------------------------------------------------------

bool Inject(DWORD pid, const std::wstring& dllPath, std::string& error) {
    HANDLE proc = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
            PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE, pid);
    if (!proc) {
        error = LastWinErr("OpenProcess");
        return false;
    }

    // 已注入？（管道已在）——由调用方先行检查；这里只管注入
    const size_t bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT,
                                  PAGE_READWRITE);
    if (!remote) {
        error = LastWinErr("VirtualAllocEx");
        CloseHandle(proc);
        return false;
    }
    if (!WriteProcessMemory(proc, remote, dllPath.c_str(), bytes, nullptr)) {
        error = LastWinErr("WriteProcessMemory");
        CloseHandle(proc);
        return false;
    }

    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    FARPROC loadLib = GetProcAddress(k32, "LoadLibraryW");
    if (!loadLib) {
        error = LastWinErr("GetProcAddress(LoadLibraryW)");
        CloseHandle(proc);
        return false;
    }
    HANDLE thread = CreateRemoteThread(proc, nullptr, 0,
                                       reinterpret_cast<LPTHREAD_START_ROUTINE>(
                                           loadLib),
                                       remote, 0, nullptr);
    if (!thread) {
        error = LastWinErr("CreateRemoteThread");
        CloseHandle(proc);
        return false;
    }
    WaitForSingleObject(thread, 15000);
    // 关键校验：LoadLibraryW 远程线程的退出码就是 HMODULE，0 = DLL 在目标
    // 进程里加载失败（位数不匹配 / 被杀毒软件拦截 / 缺依赖）。线程本身创建
    // 成功不代表加载成功，不校验会误报"注入完成"然后管道永远等不来。
    DWORD exitCode = 0;
    GetExitCodeThread(thread, &exitCode);
    CloseHandle(thread);
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    CloseHandle(proc);
    if (exitCode == 0) {
        error = "sai2_agent.dll 在目标进程内加载失败（远程 LoadLibraryW 返回 0）。"
                "常见原因：杀毒软件拦截了 DLL（把插件目录加入白名单后重试）；"
                "目标 SAI2 不是 64 位版本。";
        return false;
    }
    return true;
}

bool Eject(DWORD pid, const wchar_t* dllName, std::string& error) {
    // 在目标模块列表里找我们的 DLL 基址
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (snap == INVALID_HANDLE_VALUE) {
        error = LastWinErr("CreateToolhelp32Snapshot(SNAPMODULE)");
        return false;
    }
    MODULEENTRY32W me = {};
    me.dwSize = sizeof(me);
    BYTE* base = nullptr;
    if (Module32FirstW(snap, &me)) {
        do {
            if (_wcsicmp(me.szModule, dllName) == 0) {
                base = me.modBaseAddr;
                break;
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    if (!base) {
        error = "目标进程里没有找到 " + WideToUtf8(dllName);
        return false;
    }

    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                  PROCESS_VM_OPERATION,
                              FALSE, pid);
    if (!proc) {
        error = LastWinErr("OpenProcess");
        return false;
    }
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    FARPROC freeLib = GetProcAddress(k32, "FreeLibrary");
    HANDLE thread = CreateRemoteThread(proc, nullptr, 0,
                                       reinterpret_cast<LPTHREAD_START_ROUTINE>(
                                           freeLib),
                                       base, 0, nullptr);
    if (!thread) {
        error = LastWinErr("CreateRemoteThread(FreeLibrary)");
        CloseHandle(proc);
        return false;
    }
    WaitForSingleObject(thread, 10000);
    CloseHandle(thread);
    CloseHandle(proc);
    return true;
}

// ---------------------------------------------------------------------------
// FakeHost 端到端自检
// ---------------------------------------------------------------------------

int g_checks = 0;
int g_fails = 0;

void Check(bool ok, const char* what) {
    ++g_checks;
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fails;
}

int RunSelfTest(const std::string& exeDirUtf8) {
    const std::wstring exeDir = Utf8ToWide(exeDirUtf8);
    const std::wstring dllPath = exeDir + L"\\sai2_agent.dll";
    const std::wstring fakePath = exeDir + L"\\sai2_fakehost.exe";

    // 1. 启动 FakeHost（独立管道名，避免与真 SAI2 里的 agent 抢名字）
    std::wstring testPipe = L"\\\\.\\pipe\\sai2agent_selftest";
    std::wstring env;
    {
        wchar_t tmp[MAX_PATH];
        if (GetSystemDirectoryW(tmp, MAX_PATH) > 0)
            env = L"SystemRoot=" + std::wstring(tmp);
        else
            env = L"SystemRoot=C:\\Windows";
        env += L'\0';
    }
    env += L"SAI2_AGENT_PIPE=" + testPipe;
    env += L'\0';
    env += L'\0';

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    std::wstring cmdline = L"\"" + fakePath + L"\" --autotest";
    std::vector<wchar_t> cmdBuf(cmdline.begin(), cmdline.end());
    cmdBuf.push_back(L'\0');
    std::vector<wchar_t> envBuf(env.begin(), env.end());
    envBuf.push_back(L'\0');
    if (!CreateProcessW(fakePath.c_str(), cmdBuf.data(), nullptr, nullptr,
                        FALSE, CREATE_UNICODE_ENVIRONMENT, envBuf.data(),
                        nullptr, &si, &pi)) {
        std::printf("FAIL 无法启动 FakeHost: %s\n",
                    LastWinErr("CreateProcessW").c_str());
        return 1;
    }
    CloseHandle(pi.hThread);
    const DWORD fakePid = pi.dwProcessId;

    auto KillFake = [&]() {
        TerminateProcess(pi.hProcess, 0);
        CloseHandle(pi.hProcess);
    };

    // 等 FakeHost 窗口就绪
    Sleep(900);
    HWND fakeWnd = FindWindowW(L"FakeSai2Main", nullptr);
    Check(fakeWnd != nullptr, "FakeHost 主窗口已创建");

    // 2. 注入
    std::string err;
    bool ok = Inject(fakePid, dllPath, err);
    if (!ok) Check(false, ("注入 Agent: " + err).c_str());

    // 3. 管道 ping
    std::string resp;
    ok = false;
    for (int i = 0; i < 40 && !ok; ++i) {
        ok = AgentClient::Query("{\"cmd\":\"ping\"}", resp, testPipe.c_str());
        if (!ok) Sleep(150);
    }
    Check(ok, "Agent 管道 ping");
    Check(ok && resp.find("\"ExtTextOutW\":true") != std::string::npos,
          "ExtTextOutW IAT 钩子已生效");

    // 4. 图层识别
    ok = false;
    std::string layersJson;
    for (int i = 0; i < 20 && !ok; ++i) {
        if (AgentClient::Query("{\"cmd\":\"layers\"}", layersJson, testPipe.c_str())) {
            if (layersJson.find("rows") != std::string::npos) ok = true;
        }
        if (!ok) Sleep(200);
    }
    Check(ok, "layers 命令返回行数据");

    const auto rows =
        sai2agent::JsonFindArrayObjectsStringField(layersJson, "rows", "text");
    bool all4 = true;
    for (const char* name : {"图层 1", "图层 2", "图层 3", "图层 4"}) {
        const std::string u8 = WideToUtf8(Utf8ToWide(name));
        bool found = false;
        for (const auto& r : rows)
            if (r == u8) found = true;
        if (!found) all4 = false;
    }
    Check(all4, "识别出图层 1~4（文字钩子 → 行重建）");

    // 5. 缩略图捕获（FakeHost 用 SetDIBitsToDevice 画 28x28 色块）
    ok = false;
    std::string thumbsJson;
    for (int i = 0; i < 10 && !ok; ++i) {
        if (AgentClient::Query("{\"cmd\":\"thumbs\"}", thumbsJson, testPipe.c_str())) {
            if (thumbsJson.find("\"captured\":true") != std::string::npos)
                ok = true;
        }
        if (!ok) Sleep(200);
    }
    Check(ok, "缩略图捕获（SetDIBitsToDevice 落盘 BMP）");

    // 6. 动态更新：让 FakeHost 重命名图层 1 → 描线，确认识别是"活的"
    if (fakeWnd) SendMessageW(fakeWnd, WM_USER + 2, 0, 0); // rename row0 → 描线
    Sleep(700);
    std::string layers2;
    AgentClient::Query("{\"cmd\":\"layers\"}", layers2, testPipe.c_str());
    const auto rows2 =
        sai2agent::JsonFindArrayObjectsStringField(layers2, "rows", "text");
    bool renamed = false;
    for (const auto& r : rows2)
        if (r == WideToUtf8(L"描线")) renamed = true;
    Check(renamed, "图层重命名后识别同步更新（描线）");

    // 7. 卸载
    ok = Eject(fakePid, L"sai2_agent.dll", err);
    if (!ok) Check(false, ("卸载 Agent: " + err).c_str());
    Sleep(500);
    bool pipeGone = !AgentClient::Query("{\"cmd\":\"ping\"}", resp, nullptr);
    Check(pipeGone, "卸载后管道不再响应");

    KillFake();

    std::printf("\n自检: %d 项, 失败 %d\n", g_checks, g_fails);
    return g_fails;
}

} // namespace

// ---------------------------------------------------------------------------
// 入口
// ---------------------------------------------------------------------------

// main() 的 argv 在 Windows 上来自 ANSI 命令行 —— 中文目录名会被转坏
// （例如 "SAI2插件测试包" 变成乱码，导致注入器找不到 agent）。
// 这里改用 CommandLineToArgvW 重新取一遍原始 Unicode 参数。
namespace {

std::vector<std::string> Utf8Args() {
    std::vector<std::string> out;
    int n = 0;
    LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n);
    if (!w) return out;
    for (int i = 0; i < n; ++i) out.push_back(WideToUtf8(w[i]));
    LocalFree(w);
    return out;
}

std::wstring WideArg(const std::vector<std::string>& a, size_t i) {
    return Utf8ToWide(a[i]);
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> wargs = Utf8Args();
    if (!wargs.empty()) argc = static_cast<int>(wargs.size());
    std::string action = "status";
    std::string pidStr, exeName = "sai2.exe", dllPathUtf8, launchPath;
    std::string cmdJson;

    std::string exeDir = ExeDir();
    std::string defaultDll = exeDir + "\\sai2_agent.dll";

    for (int i = 1; i < argc; ++i) {
        const std::string a = wargs[static_cast<size_t>(i)];
        auto next = [&](std::string& dst) {
            if (i + 1 < argc) {
                // 路径类参数直接用宽字符版本转 UTF-8，避免丢字符
                dst = WideToUtf8(WideArg(wargs, static_cast<size_t>(++i)));
            }
        };
        if (a == "inject" || a == "eject" || a == "status" || a == "layers" ||
            a == "thumbs" || a == "text" || a == "cmd" || a == "selftest")
            action = a;
        else if (a == "--pid") next(pidStr);
        else if (a == "--exe") next(exeName);
        else if (a == "--dll") next(dllPathUtf8);
        else if (a == "--launch") next(launchPath);
        else if (a == "--cmd") next(cmdJson);
    }
    const std::wstring dllPath =
        dllPathUtf8.empty() ? Utf8ToWide(defaultDll) : Utf8ToWide(dllPathUtf8);

    if (action == "selftest") return RunSelfTest(exeDir);

    // 选定目标进程
    DWORD pid = 0;
    if (!pidStr.empty()) {
        pid = static_cast<DWORD>(std::strtoul(pidStr.c_str(), nullptr, 10));
    } else {
        pid = FindProcessIdByName(Utf8ToWide(exeName).c_str());
    }

    if (action == "inject") {
        if (!pid) {
            if (launchPath.empty()) {
                std::printf("FAIL 未找到 %s（也没有 --launch）\n",
                            exeName.c_str());
                return 2;
            }
            STARTUPINFOW si = {};
            si.cb = sizeof(si);
            PROCESS_INFORMATION pi = {};
            std::wstring lp = Utf8ToWide(launchPath);
            std::vector<wchar_t> buf(lp.begin(), lp.end());
            buf.push_back(L'\0');
            if (!CreateProcessW(lp.c_str(), buf.data(), nullptr, nullptr, FALSE,
                                0, nullptr, nullptr, &si, &pi)) {
                std::printf("FAIL 启动失败: %s\n",
                            LastWinErr("CreateProcessW").c_str());
                return 2;
            }
            CloseHandle(pi.hThread);
            pid = pi.dwProcessId;
            std::printf("已启动目标进程 pid=%lu\n",
                        static_cast<unsigned long>(pid));
            Sleep(1200);
        }
        std::string pingResp;
        if (AgentClient::Query("{\"cmd\":\"ping\"}", pingResp)) {
            std::printf("SKIP Agent 已注入（管道已在）: %s\n", pingResp.c_str());
            return 0;
        }
        // 位数预检：32 位 SAI2 里加载不了 64 位 DLL，提前报清楚，
        // 别让用户对着"管道未就绪 lastErr=2"猜原因。
        {
            HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                      pid);
            if (proc) {
                BOOL wow64 = FALSE;
                if (IsWow64Process(proc, &wow64) && wow64) {
                    std::printf(
                        "FAIL 检测到 32 位版 SAI2 —— 本插件包只支持 64 位版"
                        "（请下载 64bit 版 sai2.exe 后重试）\n");
                    CloseHandle(proc);
                    return 1;
                }
                CloseHandle(proc);
            }
        }
        std::string err;
        if (!Inject(pid, dllPath, err)) {
            std::printf("FAIL 注入失败: %s\n", err.c_str());
            return 1;
        }
        // 等管道就绪（放宽到 10s：首次加载可能被杀软实时扫描拖慢）
        bool ready = false;
        DWORD lastCliErr = 0;
        for (int i = 0; i < 66 && !ready; ++i) {
            AgentClient probe;
            ready = probe.Connect() && probe.Send("{\"cmd\":\"ping\"}", pingResp);
            lastCliErr = probe.lastErr;
            if (!ready) Sleep(150);
        }
        if (!ready) {
            std::printf(
                "FAIL 注入完成但管道未就绪 (client lastErr=%lu)。常见原因：\n"
                "  1) 杀毒软件拦截了 sai2_agent.dll —— 把插件目录加入白名单后重试；\n"
                "  2) SAI2 不是 64 位版本；\n"
                "  3) 详细日志见插件目录 sai2_agent_*.log / sai2_agent_pipe_*.log\n",
                static_cast<unsigned long>(lastCliErr));
            return 1;
        }
        std::printf("OK 注入成功 pid=%lu\n%s\n",
                    static_cast<unsigned long>(pid), pingResp.c_str());
        return 0;
    }

    if (action == "eject") {
        if (!pid) {
            std::printf("FAIL 未找到目标进程\n");
            return 2;
        }
        // 从 --dll 路径取模块文件名（默认 sai2_agent.dll）
        std::wstring modName = dllPath;
        const size_t slash = modName.find_last_of(L"\\/");
        if (slash != std::wstring::npos) modName = modName.substr(slash + 1);
        std::string err;
        if (!Eject(pid, modName.c_str(), err)) {
            std::printf("FAIL 卸载失败: %s\n", err.c_str());
            return 1;
        }
        std::printf("OK 已卸载 Agent（IAT 已恢复）\n");
        return 0;
    }

    // 查询类命令
    std::string cmd;
    if (action == "status") cmd = "{\"cmd\":\"ping\"}";
    else if (action == "layers") cmd = "{\"cmd\":\"layers\"}";
    else if (action == "thumbs") cmd = "{\"cmd\":\"thumbs\"}";
    else if (action == "text") cmd = "{\"cmd\":\"text.recent\",\"n\":400}";
    else if (action == "cmd") cmd = cmdJson;
    if (cmd.empty()) {
        std::printf("用法: sai2_injector <inject|eject|status|layers|thumbs|text|cmd|selftest>\n");
        return 2;
    }

    std::string resp;
    AgentClient client;
    if (!client.Connect()) {
        std::printf("FAIL 连不上 Agent 管道（目标没注入？）pid=%lu\n",
                    static_cast<unsigned long>(pid));
        return 2;
    }
    if (!client.Send(cmd, resp)) {
        std::printf("FAIL 命令超时/失败\n");
        return 1;
    }
    std::printf("%s\n", resp.c_str());
    return 0;
}
