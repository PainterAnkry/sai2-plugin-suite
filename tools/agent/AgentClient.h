#pragma once
// ============================================================================
// AgentClient.h - sai2_agent 管道客户端（injector 与 sai2_bridge 共用）
//
// 协议：短连接、单行 JSON 进 / 单行 JSON 出（见 docs/SAI2Injection.md）
// 同步 I/O：zig/clang(windows-gnu) 下 OVERLAPPED 管道实测异常，同步正常。
// 服务端单实例串行服务，连接遇 ERROR_PIPE_BUSY 时在超时内重试。
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <windows.h>

#include <mutex>
#include <string>
#include <vector>

namespace sai2agent {

inline const wchar_t* DefaultPipeName() { return L"\\\\.\\pipe\\sai2agent"; }

// ---------------------------------------------------------------------------
// 进程内串行化（关键修复）
//
// agent 的命名管道是**单实例同步服务**：服务端一次只服务一个连接，读完一整行
// 才回一整行。面板里有两条线程都会用它 —— UI 定时器（take.request / ping /
// layers，约 400ms 一次）和抓取线程（canvas.arm / state / save）。
// 两条线程各自 Connect 时，服务端的"请求↔响应"配对会错乱：抓取线程发出的
// canvas.save 可能轮不到自己的回复 → Query 返回 false，表现为
//   canvas ctx: locate failed (save failed: pipe busy ...)
// 实测证据见 docs/SAI2Injection.md §10.6.2 的管道日志（canvas.* 与 take.request
// 交错出现）。
//
// 因此进程内用一把互斥量把所有管道访问串起来：Connect 取锁、Close 放锁，
// 一个实例在其生命周期内独占管道，请求/响应永远成对。
// （跨进程仍可能争用，故 Connect 保留 ERROR_PIPE_BUSY 重试；诊断前先关面板。）
// ---------------------------------------------------------------------------
class AgentClient {
public:
    ~AgentClient() { Close(); }

    DWORD lastErr = 0; // 诊断：最近一次失败的 Win32 错误码

    // 全局"连接预算"（毫秒）。默认 3000：正常操作时给足重试窗口（服务端每服务
    // 完一个客户端都会重建管道名，存在"名字短暂不存在"的窗口）。
    // 但**启动首刷**要调小：面板刚显示时若 agent 还没注入，3 秒预算会白等，
    // 界面表现为"起来后一段时间不响应"。启动时先设 400ms → 没注入就快速失败
    // 并显示未连接；已注入则第一次 CreateFileW 就成功，等于零等待。
    static DWORD& DefaultConnectTimeout() {
        static DWORD ms = 3000;
        return ms;
    }
    static void SetDefaultConnectTimeout(DWORD ms) {
        DefaultConnectTimeout() = (ms == 0) ? 3000 : ms;
    }

    bool Connect(const wchar_t* pipeName = nullptr, DWORD timeoutMs = 0) {
        if (timeoutMs == 0) timeoutMs = DefaultConnectTimeout();
        Close();
        // 取进程内独占锁（UI 定时器单次查询是毫秒级，拿不到就等）
        lock_ = std::unique_lock<std::mutex>(PipeMutex());
        const wchar_t* name = pipeName ? pipeName : DefaultPipeName();
        const ULONGLONG t0 = GetTickCount64();
        for (;;) {
            handle_ = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0,
                                  nullptr, OPEN_EXISTING, 0, nullptr);
            if (handle_ != INVALID_HANDLE_VALUE) return true;
            lastErr = GetLastError();
            // 关键：服务端每服务完一个客户端就 DisconnectNamedPipe → 重新
            // CreateNamedPipe。在这两步之间**管道名短暂不存在**，此刻
            // CreateFileW 返回 ERROR_FILE_NOT_FOUND(2)（有时 ERROR_PIPE_NOT_CONNECTED(233)）。
            // 旧代码只把 ERROR_PIPE_BUSY(231) 当可重试，于是这种"恰好卡在重建窗口"
            // 的连接会**立刻失败**——表现为 canvas.save 明明刚发过 state 却连不上，
            // 日志里就是 save failed: pipe busy（其实根本不是 busy）。
            // 三种都属于"服务端暂不可用"，一律重试到超时。
            const bool retryable =
                lastErr == ERROR_PIPE_BUSY ||
                lastErr == ERROR_FILE_NOT_FOUND ||
                lastErr == ERROR_PIPE_NOT_CONNECTED;
            if (!retryable || GetTickCount64() - t0 > timeoutMs) {
                if (lock_.owns_lock()) lock_.unlock();
                return false;
            }
            if (lastErr == ERROR_PIPE_BUSY)
                WaitNamedPipeW(name, 100);
            else
                Sleep(15);   // 重建窗口，短暂退避
        }
    }

    // 发送一条命令并取回一行响应（同步阻塞；服务端命令处理为毫秒级，
    // 唯一慢命令 scan.strings 有 2s 自限）
    bool Send(const std::string& cmd, std::string& response) {
        if (handle_ == INVALID_HANDLE_VALUE) return false;

        std::string wire = cmd + "\n";
        DWORD written = 0;
        if (!WriteFile(handle_, wire.data(), static_cast<DWORD>(wire.size()),
                       &written, nullptr)) {
            lastErr = GetLastError();
            return false;
        }

        response.clear();
        char buf[65536];
        for (;;) {
            DWORD n = 0;
            if (!ReadFile(handle_, buf, sizeof(buf), &n, nullptr)) {
                lastErr = GetLastError();
                return false;
            }
            if (n == 0) { lastErr = ERROR_BROKEN_PIPE; return false; }
            response.append(buf, n);
            const size_t nl = response.find('\n');
            if (nl != std::string::npos) {
                response = response.substr(0, nl);
                return true;
            }
        }
    }

    // 便捷封装：连接 → 发命令 → 断开
    // budgetMs：本次连接的重试预算（毫秒）。0 = 用 DefaultConnectTimeout()。
    // 调用方**必须**按"agent 是否可能在线"传预算：服务端不在线时管道名
    // 不存在，Connect 会空转满整个预算 —— UI 线程上调 Q 一次就等于把
    // 界面卡死预算等长的时间（详见 BridgeApp.cpp RefreshLayers 的注释）。
    static bool Query(const std::string& cmd, std::string& response,
                      const wchar_t* pipeName = nullptr, DWORD budgetMs = 0) {
        AgentClient c;
        if (!c.Connect(pipeName, budgetMs)) return false;
        return c.Send(cmd, response);
    }

    void Close() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
        if (lock_.owns_lock()) lock_.unlock();   // 释放进程内独占
    }

private:
    static std::mutex& PipeMutex() {
        static std::mutex m;   // 函数内静态：C++11 起初始化线程安全
        return m;
    }

    HANDLE handle_ = INVALID_HANDLE_VALUE;
    std::unique_lock<std::mutex> lock_;   // 持有到 Close()
};

// ---------------------------------------------------------------------------
// 极简 JSON 字段/字符串数组提取（只服务本协议的已知结构，够用即可）
// ---------------------------------------------------------------------------

// 在 json 里找 `"key":"value"`，把 value 反转义进 out（UTF-8）
inline bool JsonFindString(const std::string& json, const char* key,
                           std::string& out) {
    const std::string pat = "\"" + std::string(key) + "\":\"";
    const size_t p = json.find(pat);
    if (p == std::string::npos) return false;
    size_t i = p + pat.size();
    out.clear();
    while (i < json.size() && json[i] != '"') {
        if (json[i] == '\\' && i + 1 < json.size()) {
            ++i;
            switch (json[i]) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'u': {
                if (i + 4 < json.size()) {
                    unsigned v = 0;
                    for (int k = 0; k < 4; ++k) {
                        const char h = json[++i];
                        v <<= 4;
                        if (h >= '0' && h <= '9') v |= unsigned(h - '0');
                        else if (h >= 'a' && h <= 'f') v |= unsigned(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') v |= unsigned(h - 'A' + 10);
                    }
                    if (v < 0x80) out += static_cast<char>(v);
                    else if (v < 0x800) {
                        out += static_cast<char>(0xC0 | (v >> 6));
                        out += static_cast<char>(0x80 | (v & 0x3F));
                    } else {
                        out += static_cast<char>(0xE0 | (v >> 12));
                        out += static_cast<char>(0x80 | ((v >> 6) & 0x3F));
                        out += static_cast<char>(0x80 | (v & 0x3F));
                    }
                }
                break;
            }
            default: out += json[i]; break;
            }
            ++i;
        } else {
            out += json[i++];
        }
    }
    return true;
}

inline bool JsonFindInt(const std::string& json, const char* key,
                        long long& out) {
    const std::string pat = "\"" + std::string(key) + "\":";
    const size_t p = json.find(pat);
    if (p == std::string::npos) return false;
    size_t i = p + pat.size();
    while (i < json.size() && json[i] == ' ') ++i;
    bool neg = false;
    if (i < json.size() && json[i] == '-') { neg = true; ++i; }
    if (i >= json.size() || json[i] < '0' || json[i] > '9') return false;
    long long acc = 0;
    while (i < json.size() && json[i] >= '0' && json[i] <= '9') {
        acc = acc * 10 + (json[i] - '0');
        ++i;
    }
    out = neg ? -acc : acc;
    return true;
}

// 提取所有 `"key":[ ... ]` 数组里每个对象中 `"field":"text"` 的文本。
// layers 协议里 groups[].rows 出现多次，因此要按全部出现的数组扫描。
inline std::vector<std::string> JsonFindArrayObjectsStringField(
    const std::string& json, const char* key, const char* field) {
    std::vector<std::string> out;
    const std::string pat = "\"" + std::string(key) + "\":[";
    const std::string fieldPat = "\"" + std::string(field) + "\":\"";
    size_t keyPos = 0;
    while ((keyPos = json.find(pat, keyPos)) != std::string::npos) {
        const size_t arrBegin = keyPos + pat.size();
        // 找到与该数组起点匹配的 ']'
        size_t end = arrBegin;
        int depth = 1;
        bool inStr = false;
        for (size_t i = arrBegin; i < json.size(); ++i) {
            const char c = json[i];
            if (inStr) {
                if (c == '\\') ++i;
                else if (c == '"') inStr = false;
                continue;
            }
            if (c == '"') inStr = true;
            else if (c == '[') ++depth;
            else if (c == ']') {
                if (--depth == 0) { end = i; break; }
            }
        }
        const std::string arr = json.substr(arrBegin, end - arrBegin);
        size_t pos = 0;
        while ((pos = arr.find(fieldPat, pos)) != std::string::npos) {
            size_t j = pos + fieldPat.size();
            std::string v;
            while (j < arr.size() && arr[j] != '"') {
                if (arr[j] == '\\' && j + 1 < arr.size()) {
                    ++j;
                    switch (arr[j]) {
                    case 'n': v += '\n'; break;
                    case 't': v += '\t'; break;
                    case 'r': v += '\r'; break;
                    case '"': v += '"'; break;
                    case '\\': v += '\\'; break;
                    case '/': v += '/'; break;
                    case 'u': {
                        if (j + 4 < arr.size()) {
                            unsigned vv = 0;
                            for (int k = 0; k < 4; ++k) {
                                const char h = arr[++j];
                                vv <<= 4;
                                if (h >= '0' && h <= '9') vv |= unsigned(h - '0');
                                else if (h >= 'a' && h <= 'f') vv |= unsigned(h - 'a' + 10);
                                else if (h >= 'A' && h <= 'F') vv |= unsigned(h - 'A' + 10);
                            }
                            if (vv < 0x80) v += static_cast<char>(vv);
                            else if (vv < 0x800) {
                                v += static_cast<char>(0xC0 | (vv >> 6));
                                v += static_cast<char>(0x80 | (vv & 0x3F));
                            } else {
                                v += static_cast<char>(0xE0 | (vv >> 12));
                                v += static_cast<char>(0x80 | ((vv >> 6) & 0x3F));
                                v += static_cast<char>(0x80 | (vv & 0x3F));
                            }
                        }
                        break;
                    }
                    default: v += arr[j]; break;
                    }
                    ++j;
                } else {
                    v += arr[j++];
                }
            }
            out.push_back(std::move(v));
            pos = j;
        }
        keyPos = end;
    }
    return out;
}

} // namespace sai2agent
