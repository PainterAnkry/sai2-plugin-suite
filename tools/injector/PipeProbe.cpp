// 独立管道服务端：验证 AgentClient 是否正确（排除注入环境因素）
#include <windows.h>
#include <cstdio>
#include <string>

#include "../agent/AgentClient.h"

static HANDLE g_exit;

DWORD WINAPI ServerProc(LPVOID) {
    const wchar_t* name = L"\\\\.\\pipe\\probetest";
    for (;;) {
        HANDLE pipe = CreateNamedPipeW(
            name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES, 1 << 16, 1 << 16, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            std::printf("create fail %lu\n", GetLastError());
            Sleep(300);
            continue;
        }
        OVERLAPPED cov = {};
        cov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        BOOL connected = ConnectNamedPipe(pipe, &cov);
        if (!connected) {
            const DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                HANDLE waits[2] = {cov.hEvent, g_exit};
                WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            } else if (err != ERROR_PIPE_CONNECTED) {
                CloseHandle(cov.hEvent);
                CloseHandle(pipe);
                Sleep(100);
                continue;
            }
        }
        CloseHandle(cov.hEvent);
        std::printf("[srv] client connected\n");

        char buf[8192];
        OVERLAPPED rov = {};
        rov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        DWORD n = 0;
        BOOL ok = ReadFile(pipe, buf, sizeof(buf), nullptr, &rov);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            WaitForSingleObject(rov.hEvent, 5000);
            ok = GetOverlappedResult(pipe, &rov, &n, FALSE);
        }
        CloseHandle(rov.hEvent);
        std::printf("[srv] read ok=%d n=%lu data=[%.*s] gle=%lu\n", (int)ok, n,
                    (int)(ok ? n : 0), buf, ok ? 0 : GetLastError());

        std::string resp = "{\"ok\":true,\"agent\":\"probe\"}\n";
        OVERLAPPED wov = {};
        wov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        DWORD w = 0;
        ok = WriteFile(pipe, resp.data(), (DWORD)resp.size(), &w, &wov);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            WaitForSingleObject(wov.hEvent, 5000);
            ok = GetOverlappedResult(pipe, &wov, &w, FALSE);
        }
        CloseHandle(wov.hEvent);
        std::printf("[srv] write ok=%d\n", (int)ok);
        FlushFileBuffers(pipe);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
}

int main() {
    g_exit = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    CreateThread(nullptr, 0, ServerProc, nullptr, 0, nullptr);
    Sleep(400);

    sai2agent::AgentClient c;
    std::string resp;
    const bool ok = c.Connect() && c.Send("{\"cmd\":\"ping\"}", resp);
    std::printf("[cli] ok=%d lastErr=%lu resp=[%s]\n", (int)ok,
                (unsigned long)c.lastErr, resp.c_str());
    return ok ? 0 : 1;
}
