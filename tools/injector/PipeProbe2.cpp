// 同步管道模式验证：服务端/客户端都不用 OVERLAPPED
#include <windows.h>
#include <cstdio>
#include <string>
#include <thread>

static HANDLE g_exit;

DWORD WINAPI ServerProc(LPVOID) {
    const wchar_t* name = L"\\\\.\\pipe\\probetest2";
    for (;;) {
        HANDLE pipe = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX,
                                       PIPE_TYPE_BYTE | PIPE_WAIT,
                                       PIPE_UNLIMITED_INSTANCES, 1 << 16,
                                       1 << 16, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            std::printf("[srv] create fail %lu\n", GetLastError());
            Sleep(300);
            continue;
        }
        BOOL connected = ConnectNamedPipe(pipe, nullptr);
        if (!connected && GetLastError() != ERROR_PIPE_CONNECTED) {
            CloseHandle(pipe);
            Sleep(100);
            continue;
        }
        std::printf("[srv] client connected\n");
        char buf[8192];
        DWORD n = 0;
        BOOL ok = ReadFile(pipe, buf, sizeof(buf) - 1, &n, nullptr);
        std::printf("[srv] read ok=%d n=%lu data=[%.*s]\n", (int)ok, n,
                    (int)(ok ? n : 0), buf);
        std::string resp = "{\"ok\":true,\"mode\":\"sync\"}\n";
        DWORD w = 0;
        ok = WriteFile(pipe, resp.data(), (DWORD)resp.size(), &w, nullptr);
        std::printf("[srv] write ok=%d\n", (int)ok);
        FlushFileBuffers(pipe);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
}

int main() {
    CreateThread(nullptr, 0, ServerProc, nullptr, 0, nullptr);
    Sleep(400);

    HANDLE h = CreateFileW(L"\\\\.\\pipe\\probetest2",
                           GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        std::printf("[cli] open fail %lu\n", GetLastError());
        return 1;
    }
    std::string wire = "{\"cmd\":\"ping\"}\n";
    DWORD w = 0;
    BOOL ok = WriteFile(h, wire.data(), (DWORD)wire.size(), &w, nullptr);
    std::printf("[cli] write ok=%d\n", (int)ok);
    char buf[65536];
    DWORD n = 0;
    ok = ReadFile(h, buf, sizeof(buf) - 1, &n, nullptr);
    std::printf("[cli] read ok=%d resp=[%.*s]\n", (int)ok, (int)(ok ? n : 0),
                buf);
    CloseHandle(h);
    return ok ? 0 : 1;
}
